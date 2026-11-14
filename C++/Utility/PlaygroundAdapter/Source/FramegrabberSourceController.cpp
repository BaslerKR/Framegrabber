#include "FramegrabberSourceController.h"

#include "Utility/PlaygroundAdapter/FramegrabberGraphicsFrameStream.h"
#include "SessionFrame.h"

#include <QDebug>
#include <QJsonArray>
#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <thread>
#include <utility>

namespace
{
// Single acquisition waits for one frame per DMA; a trigger source may be idle.
constexpr auto kSingleAcquisitionTimeout = std::chrono::seconds(30);
// Host-declared lifecycle operations routed through the module's shared control path; not tool names.
const QLatin1String kDisconnect("lifecycle.disconnect");
const QLatin1String kSingle("lifecycle.single");

QJsonObject schema(QJsonObject properties, QJsonArray required)
{
    return {{"type", "object"}, {"properties", properties}, {"required", required},
            {"additionalProperties", false}};
}

const QJsonObject& text()
{
    static const QJsonObject value{{"type", "string"}, {"minLength", 1}};
    return value;
}

const QJsonObject& anyText()
{
    static const QJsonObject value{{"type", "string"}};
    return value;
}

const QJsonObject& flag()
{
    static const QJsonObject value{{"type", "boolean"}};
    return value;
}

const QJsonObject& integer()
{
    static const QJsonObject value{{"type", "integer"}};
    return value;
}

QJsonObject stateSchema()
{
    const auto applet = schema({{"name", anyText()}, {"path", anyText()}, {"version", anyText()}},
        {"name", "path", "version"});
    const auto error = schema({{"code", integer()}, {"message", anyText()}}, {"code", "message"});
    const auto discovery = schema({{"result", integer()}, {"message", anyText()},
        {"topologyMismatch", flag()}}, {"result", "message", "topologyMismatch"});
    const auto statistics = schema({{"dma", integer()}, {"delivered", anyText()},
        {"skipped", anyText()}, {"overwritten", anyText()}, {"rejected", anyText()}},
        {"dma", "delivered", "skipped", "overwritten", "rejected"});
    return schema({{"connected", flag()}, {"live", flag()}, {"busy", flag()},
        {"connectionId", anyText()}, {"board", anyText()}, {"applet", applet},
        {"configuration", anyText()}, {"dmaCount", integer()}, {"lastError", error},
        {"cameraDiscovery", discovery},
        {"statistics", QJsonObject{{"type", "array"}, {"items", statistics}}}},
        {"connected", "live", "busy", "connectionId", "board", "applet", "configuration",
         "dmaCount", "lastError", "cameraDiscovery", "statistics"});
}

QJsonObject appletsSchema()
{
    const auto entry = schema({{"path", text()}, {"name", anyText()}, {"file", anyText()},
        {"version", anyText()}, {"description", anyText()}, {"dmaCount", integer()},
        {"cameraCount", integer()}, {"active", flag()}, {"powerUp", flag()},
        {"cameraControl", flag()}, {"loaded", flag()}},
        {"path", "name", "file", "version", "description", "dmaCount", "cameraCount",
         "active", "powerUp", "cameraControl", "loaded"});
    return schema({{"applets", QJsonObject{{"type", "array"}, {"items", entry}}}}, {"applets"});
}

QJsonObject camerasSchema()
{
    const auto entry = schema({{"dma", integer()}, {"vendor", anyText()}, {"model", anyText()},
        {"serial", anyText()}, {"connected", flag()}},
        {"dma", "vendor", "model", "serial", "connected"});
    const auto discovery = schema({{"result", integer()}, {"message", anyText()},
        {"topologyMismatch", flag()}}, {"result", "message", "topologyMismatch"});
    return schema({{"cameras", QJsonObject{{"type", "array"}, {"items", entry}}},
        {"discovery", discovery}}, {"cameras", "discovery"});
}

QJsonObject boardsSchema()
{
    const auto entry = schema({{"boardId", text()}, {"index", integer()}, {"name", anyText()},
        {"serial", anyText()}, {"firmware", anyText()}, {"driver", anyText()},
        {"hardware", anyText()}, {"connected", flag()}},
        {"boardId", "index", "name", "serial", "firmware", "driver", "hardware", "connected"});
    return schema({{"boards", QJsonObject{{"type", "array"}, {"items", entry}}}}, {"boards"});
}

QJsonObject discoveryJson(const Framegrabber::CameraDiscovery& discovery)
{
    return {{"result", discovery.sdkResult}, {"message", QString::fromStdString(discovery.message)},
            {"topologyMismatch", discovery.topologyMismatch}};
}

QJsonObject appletsJson(const Framegrabber& framegrabber)
{
    const std::string loaded = framegrabber.isOpened() ? framegrabber.appletPath() : std::string{};
    QJsonArray applets;
    for (const Framegrabber::AppletInfo& applet : framegrabber.appletCatalog())
    {
        applets.append(QJsonObject{{"path", QString::fromStdString(applet.path)},
            {"name", QString::fromStdString(applet.name)}, {"file", QString::fromStdString(applet.file)},
            {"version", QString::fromStdString(applet.version)},
            {"description", QString::fromStdString(applet.description)},
            {"dmaCount", applet.dmaCount}, {"cameraCount", applet.cameraCount},
            {"active", applet.active}, {"powerUp", applet.powerUp},
            {"cameraControl", applet.supportsCameraControl},
            {"loaded", !loaded.empty() && loaded == applet.path}});
    }
    return {{"applets", applets}};
}

QJsonObject camerasJson(const Framegrabber& framegrabber)
{
    QJsonArray cameras;
    for (const Framegrabber::CameraInfo& camera :
         framegrabber.getCachedCameraList(Framegrabber::CameraTransport::CoaXPress))
    {
        cameras.append(QJsonObject{{"dma", static_cast<int>(camera.dmaIndex)},
            {"vendor", QString::fromStdString(camera.vendor)},
            {"model", QString::fromStdString(camera.model)},
            {"serial", QString::fromStdString(camera.serial)}, {"connected", camera.connected}});
    }
    return {{"cameras", cameras}, {"discovery", discoveryJson(framegrabber.lastCameraDiscovery())}};
}

bool boardMatches(const FramegrabberSystem::BoardInfo& board, const QString& reference)
{
    const std::string value = reference.toStdString();
    return board.displayName() == value || board.name == value || board.serial == value;
}

/** @brief Returns the catalog path a reference resolves to, or empty outside the catalog. */
std::string catalogApplet(const Framegrabber& framegrabber, const std::string& reference)
{
    const std::string resolved = framegrabber.resolveAppletReference(reference);
    if (resolved.empty())
    {
        return {};
    }
    for (const auto& applet : framegrabber.appletCatalog())
    {
        std::error_code error;
        if (applet.path == resolved
            || (std::filesystem::equivalent(applet.path, resolved, error) && !error))
        {
            return applet.path;
        }
    }
    return {};
}
}

void FramegrabberSourceController::setFrameStorageProvider(
    std::shared_ptr<IFrameStorageProvider> provider)
{
    if (_framegrabber == nullptr)
    {
        return;
    }
    if (!provider)
    {
        _framegrabber->setFrameStorage({});
        return;
    }
    _framegrabber->setFrameStorage([provider](const std::size_t bytes)
    {
        return provider->acquire(bytes);
    });
}

FramegrabberSourceController::FramegrabberSourceController(
    Framegrabber* framegrabber,
    QObject* parent)
    : AbstractSourceController(parent),
      _framegrabber(framegrabber),
      _instanceId(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
    registerCallbacks();
}

FramegrabberSourceController::~FramegrabberSourceController()
{
    // Accepted tool and parameter work completes before the Framegrabber is released.
    _work.waitForFinished();
    stop();
    deregisterCallbacks();
}

void FramegrabberSourceController::start()
{
    if (_framegrabber)
    {
        publishSourceDescriptors();
        _framegrabber->grab();
    }
}

void FramegrabberSourceController::stop()
{
    if (_framegrabber)
    {
        _framegrabber->stop();
    }
    _isGrabbing.store(false, std::memory_order_release);
}

std::vector<GraphicsSourceDescriptor> FramegrabberSourceController::sourceDescriptors() const
{
    std::vector<GraphicsSourceDescriptor> sources;
    const int count = _framegrabber ? _framegrabber->getDMACount() : 0;
    sources.reserve(count > 0 ? static_cast<std::size_t>(count) : 1U);
    for (int index = 0; index < count; ++index)
    {
        const std::string id = "dma." + std::to_string(index);
        sources.push_back({static_cast<unsigned int>(index), id,
                           "DMA " + std::to_string(index)});
    }
    if (sources.empty()) sources.push_back({0U, "dma.0", "DMA 0"});
    return sources;
}

bool FramegrabberSourceController::isGrabbing() const
{
    return _isGrabbing.load(std::memory_order_acquire);
}

void FramegrabberSourceController::setFrameConsumer(FrameConsumer consumer)
{
    _frameConsumer = std::move(consumer);
}

void FramegrabberSourceController::registerCallbacks()
{
    if (!_framegrabber)
    {
        return;
    }

    const QPointer<FramegrabberSourceController> guard(this);
    _statusCallbackId = _framegrabber->registerStatusCallback(
        [this, guard](const Framegrabber::Status status, const bool on)
        {
            if (status == Framegrabber::GrabbingStatus)
            {
                _isGrabbing.store(on, std::memory_order_release);
                emit diagnosticAcquisitionChanged(on, QStringLiteral("framegrabber"));
            }
            else if (status == Framegrabber::ConnectionStatus && on && guard)
            {
                // A newly loaded applet can change the DMA topology.
                QMetaObject::invokeMethod(this, [guard]
                {
                    if (guard) guard->publishSourceDescriptors();
                }, Qt::QueuedConnection);
            }
        });

    _graphicsStream = std::make_unique<FramegrabberGraphicsFrameStream>(
        _framegrabber,
        [this](GraphicsFrame&& payload, const unsigned int sourceIndex)
        {
            if (!_frameConsumer) return;
            SessionFrame frame;
            frame.payload = std::move(payload);
            frame.frameSeq = frame.payload.metadata.frameIndex;
            _frameConsumer(std::move(frame), sourceIndex);
        });
}

void FramegrabberSourceController::deregisterCallbacks()
{
    if (!_framegrabber)
    {
        return;
    }
    _graphicsStream.reset();
    if (_statusCallbackId != 0)
    {
        _framegrabber->deregisterStatusCallback(_statusCallbackId);
        _statusCallbackId = 0;
    }
}

bool FramegrabberSourceController::workerBusy() const
{
    return _work.isStarted() && !_work.isFinished();
}

QString FramegrabberSourceController::connectionId() const
{
    const std::uint64_t generation = _framegrabber ? _framegrabber->connectionGeneration() : 0;
    return generation == 0 ? QString() : _instanceId + QLatin1Char(':') + QString::number(generation);
}

QJsonObject FramegrabberSourceController::controlState(const bool fromWorker) const
{
    const bool connected = _framegrabber && _framegrabber->isOpened();
    const Framegrabber::OperationError error =
        _framegrabber ? _framegrabber->lastError() : Framegrabber::OperationError{};
    QJsonArray statistics;
    if (_framegrabber)
    {
        for (const auto& channel : _framegrabber->acquisitionStatistics())
        {
            statistics.append(QJsonObject{{"dma", static_cast<int>(channel.dmaIndex)},
                {"delivered", QString::number(channel.delivered)},
                {"skipped", QString::number(channel.skipped)},
                {"overwritten", QString::number(channel.overwritten)},
                {"rejected", QString::number(channel.rejected)}});
        }
    }
    return {{"connected", connected},
        {"live", _framegrabber && _framegrabber->isGrabbing()},
        {"busy", (_framegrabber && _framegrabber->controlBusy()) || (!fromWorker && workerBusy())},
        {"connectionId", connectionId()},
        {"board", connected ? QString::fromStdString(_framegrabber->getConnectedFramegrabberName()) : QString()},
        {"applet", QJsonObject{
            {"name", connected ? QString::fromStdString(_framegrabber->getLoadedAppletName()) : QString()},
            {"path", connected ? QString::fromStdString(_framegrabber->appletPath()) : QString()},
            {"version", connected ? QString::fromStdString(_framegrabber->getLoadedAppletVersion()) : QString()}}},
        {"configuration", connected ? QString::fromStdString(_framegrabber->configurationPath()) : QString()},
        {"dmaCount", connected ? _framegrabber->getDMACount() : 0},
        {"lastError", QJsonObject{{"code", error.code}, {"message", QString::fromStdString(error.message)}}},
        {"cameraDiscovery", discoveryJson(_framegrabber
            ? _framegrabber->lastCameraDiscovery() : Framegrabber::CameraDiscovery{})},
        {"statistics", statistics}};
}

QList<ToolDescriptor> FramegrabberSourceController::toolDescriptors() const
{
    const auto empty = schema({}, {});
    const auto connection = schema({{"connectionId", text()}}, {"connectionId"});
    const auto state = stateSchema();
    QList<ToolDescriptor> result{
        {"framegrabber.state", 1, "Frame grabber state",
            "Observe the loaded board/applet, connection generation, last SDK error and per-DMA acquisition counters.",
            ToolEffect::Observe, ToolEgress::Metadata, true, {}, empty, state},
        {"framegrabber.boards", 1, "Frame grabber boards", "Copy the boards discovered by the SDK for this session.",
            ToolEffect::Observe, ToolEgress::Metadata, true, {}, empty, boardsSchema()},
        {"framegrabber.applets", 1, "Loadable applets",
            "Copy the applets the SDK enumerated as loadable (FG_AIS_FILESYSTEM, FG_AF_IS_LOADABLE).",
            ToolEffect::Observe, ToolEgress::Metadata, true, {}, empty, appletsSchema()},
        {"framegrabber.applets-refresh", 1, "Enumerate applets",
            "Re-enumerate loadable applets for a board; refused while an applet is loaded.",
            ToolEffect::Device, ToolEgress::Metadata, true, {}, schema({{"board", text()}}, {}), appletsSchema()},
        {"framegrabber.applet-load", 1, "Load applet",
            "Initialize the board with an applet path or unique applet name (Fg_Init). Replacing a loaded applet requires its connectionId; acquisition must be stopped.",
            ToolEffect::Device, ToolEgress::Metadata, true, {},
            schema({{"board", text()}, {"applet", text()}, {"connectionId", text()}}, {"board", "applet"}), state},
        {"framegrabber.configuration-load", 1, "Load configuration",
            "Apply an .mcf file: Fg_InitConfig, or Fg_loadConfig when its applet is loaded. An applet argument replaces the MCF applet reference.",
            ToolEffect::Device, ToolEgress::Metadata, true, {},
            schema({{"board", text()}, {"configuration", text()}, {"applet", text()}, {"connectionId", text()}},
                {"board", "configuration"}), state},
        {"framegrabber.configuration-save", 1, "Save configuration",
            "Write the current applet parameters to an .mcf file (Fg_saveConfig) on the host file system.",
            ToolEffect::Device, ToolEgress::Metadata, true, {},
            schema({{"connectionId", text()}, {"configuration", text()}}, {"connectionId", "configuration"}), state},
        {"framegrabber.cameras", 1, "CoaXPress cameras", "Copy the cameras found by the last CXP discovery.",
            ToolEffect::Observe, ToolEgress::Metadata, true, {}, empty, camerasSchema()},
        {"framegrabber.cameras-refresh", 1, "Rediscover cameras",
            "Restart CXP discovery and reconnect cameras on the exact connection; topology mismatches are reported.",
            ToolEffect::Device, ToolEgress::Metadata, true, {}, connection, camerasSchema()}};
    if (!_framegrabber)
    {
        for (auto& descriptor : result)
        {
            descriptor.available = false;
            descriptor.unavailableReason = "The frame grabber session has ended.";
        }
    }
    return result;
}

ApiResult<void> FramegrabberSourceController::requestTool(const QString& name,
                                                          const int version,
                                                          QJsonObject arguments,
                                                          IToolProvider::Completion completion)
{
    Q_ASSERT(QThread::currentThread() == thread());
    using Result = ApiResult<QJsonValue>;
    using Admission = ApiResult<void>;
    if (!completion) return Admission::failure(ApiErrorCode::InvalidArgument, "Completion is required.");
    std::optional<ToolDescriptor> descriptor;
    for (const auto& candidate : toolDescriptors()) if (candidate.name == name) descriptor = candidate;
    if (!descriptor) return Admission::failure(ApiErrorCode::NotFound, "Tool is not declared.");
    if (version != descriptor->version) return Admission::failure(ApiErrorCode::Unsupported, "Unsupported tool version.");
    if (!_framegrabber) return Admission::failure(ApiErrorCode::Unavailable, descriptor->unavailableReason);
    // The registry validates schemas; direct callers get the same strict check here.
    const QJsonObject properties = descriptor->inputSchema.value("properties").toObject();
    for (auto it = arguments.begin(); it != arguments.end(); ++it)
        if (!properties.contains(it.key()) || !it.value().isString() || it.value().toString().isEmpty()
            || it.value().toString().contains(QChar::Null))
            return Admission::failure(ApiErrorCode::InvalidArgument, "Exact typed arguments are required.");
    for (const auto required : descriptor->inputSchema.value("required").toArray())
        if (!arguments.contains(required.toString()))
            return Admission::failure(ApiErrorCode::InvalidArgument, "Exact typed arguments are required.");

    return admitControl(name, std::move(arguments), [completion = std::move(completion)](Result result) noexcept {
        try { completion(std::move(result)); }
        catch (...) { qWarning() << "[Framegrabber] tool consumer callback threw."; }
    });
}

ApiResult<void> FramegrabberSourceController::admitControl(const QString& name, QJsonObject arguments,
                                                           std::function<void(ApiResult<QJsonValue>)> deliver)
{
    Q_ASSERT(QThread::currentThread() == thread());
    using Result = ApiResult<QJsonValue>;
    using Admission = ApiResult<void>;
    if (!_framegrabber) return Admission::failure(ApiErrorCode::Unavailable, "The frame grabber session has ended.");
    if (name == "framegrabber.state") { deliver(Result::success(controlState())); return Admission::success(); }
    if (name == "framegrabber.applets") { deliver(Result::success(appletsJson(*_framegrabber))); return Admission::success(); }
    if (name == "framegrabber.cameras") { deliver(Result::success(camerasJson(*_framegrabber))); return Admission::success(); }
    if (name == "framegrabber.boards") {
        QJsonArray boards;
        const std::string connected = _framegrabber->isOpened() ? _framegrabber->getConnectedFramegrabberName() : std::string{};
        for (const auto& board : _framegrabber->getCachedBoardInfo())
            boards.append(QJsonObject{{"boardId", QString::fromStdString(board.displayName())},
                {"index", static_cast<int>(board.index)}, {"name", QString::fromStdString(board.name)},
                {"serial", QString::fromStdString(board.serial)}, {"firmware", QString::fromStdString(board.firmware)},
                {"driver", QString::fromStdString(board.driver)}, {"hardware", QString::fromStdString(board.hardware)},
                {"connected", !connected.empty() && connected == board.displayName()}});
        deliver(Result::success(QJsonObject{{"boards", boards}}));
        return Admission::success();
    }

    if (workerBusy() || _framegrabber->controlBusy())
        return Admission::failure(ApiErrorCode::Busy, "Another frame grabber control operation is active.");
    const bool connected = _framegrabber->isOpened();
    const QString currentConnection = connectionId();
    const QString requestedConnection = arguments.value("connectionId").toString();
    const bool needsConnection = name == "framegrabber.configuration-save" || name == kDisconnect
        || name == "framegrabber.cameras-refresh" || name == kSingle;
    if (needsConnection) {
        if (!connected) return Admission::failure(ApiErrorCode::Unavailable, "No applet is loaded.");
        if (requestedConnection != currentConnection)
            return Admission::failure(ApiErrorCode::Conflict, "The connection changed; observe it again.");
    }
    std::string boardName;
    if (arguments.contains("board")) {
        const QString board = arguments.value("board").toString();
        bool found = false;
        for (const auto& candidate : _framegrabber->getCachedBoardInfo())
            if (boardMatches(candidate, board)) { found = true; boardName = candidate.displayName(); }
        if (!found) return Admission::failure(ApiErrorCode::NotFound, "Exact declared board was not found.");
    }
    if (name == "framegrabber.applets-refresh" && connected)
        return Admission::failure(ApiErrorCode::Conflict, "Release the board before enumerating applets.");
    // Applets contain native libraries. Callable requests load only applets the SDK
    // enumerated from its standard locations; arbitrary files remain a GUI choice.
    if (arguments.contains("applet")
        && catalogApplet(*_framegrabber, arguments.value("applet").toString().toStdString()).empty())
        return Admission::failure(ApiErrorCode::PermissionDenied,
            "Only applets listed by framegrabber.applets can be loaded through the API.");
    if (name == "framegrabber.applet-load" || name == "framegrabber.configuration-load") {
        if (connected && requestedConnection.isEmpty())
            return Admission::failure(ApiErrorCode::Conflict, "Pass the current connectionId to replace the loaded applet.");
        if (connected && requestedConnection != currentConnection)
            return Admission::failure(ApiErrorCode::Conflict, "Connection generation changed.");
        if (connected && boardName != _framegrabber->getConnectedFramegrabberName())
            return Admission::failure(ApiErrorCode::Conflict, "Release the loaded board before selecting another board.");
    }
    if (_framegrabber->isGrabbing() && name != kDisconnect)
        return Admission::failure(ApiErrorCode::Busy, "Stop acquisition explicitly first.");
    // The same gate serializes the control panel; it never implicitly stops acquisition.
    if (!_framegrabber->tryBeginControl())
        return Admission::failure(ApiErrorCode::Busy, "Another frame grabber control operation is active.");

    Framegrabber* framegrabber = _framegrabber;
    _work = QtConcurrent::run([this, framegrabber, name, arguments, boardName, deliver] {
        // Results are copied after the gate is released, so they report the settled state.
        enum class Payload { State, Applets, Cameras, None };
        Payload payload = Payload::None;
        std::optional<Result> failure;
        const auto failed = [framegrabber](const QString& fallback) {
            const auto error = framegrabber->lastError();
            return Result::failure(ApiErrorCode::Unavailable,
                error.message.empty() ? fallback : QString::fromStdString(error.message),
                QJsonObject{{"sdkResult", error.code}});
        };
        try {
            if (name == "framegrabber.applets-refresh") {
                if (framegrabber->refreshAppletCatalog(boardName)) payload = Payload::Applets;
                else failure = Result::failure(ApiErrorCode::Conflict, "Applet enumeration requires a released board.");
            } else if (name == "framegrabber.applet-load") {
                if (framegrabber->loadApplet(arguments.value("applet").toString().toStdString(), boardName))
                    payload = Payload::State;
                else failure = failed("Failed to load the applet.");
            } else if (name == "framegrabber.configuration-load") {
                const std::string configuration = arguments.value("configuration").toString().toStdString();
                const std::string applet = arguments.value("applet").toString().toStdString();
                // The MCF names its own applet; apply the same catalog restriction to it.
                const std::string configured = applet.empty()
                    ? framegrabber->appletPathFromConfiguration(configuration, boardName) : applet;
                if (catalogApplet(*framegrabber, configured).empty())
                    failure = Result::failure(ApiErrorCode::PermissionDenied,
                        "The configuration does not reference an applet listed by framegrabber.applets; pass one as applet.");
                else if (framegrabber->loadAppletConfiguration(applet, configuration, boardName))
                    payload = Payload::State;
                else failure = failed("Failed to apply the configuration.");
            } else if (name == "framegrabber.configuration-save") {
                if (framegrabber->saveConfiguration(arguments.value("configuration").toString().toStdString()))
                    payload = Payload::State;
                else failure = failed("Failed to save the configuration.");
            } else if (name == kDisconnect) {
                framegrabber->close();
                if (!framegrabber->isOpened()) payload = Payload::State;
                else failure = failed("The frame grabber could not be released.");
            } else if (name == "framegrabber.cameras-refresh") {
                const bool connected = framegrabber->refreshCameras(Framegrabber::CameraTransport::CoaXPress);
                const auto discovery = framegrabber->lastCameraDiscovery();
                if (connected || discovery.topologyMismatch) payload = Payload::Cameras;
                else failure = Result::failure(ApiErrorCode::Unavailable,
                    discovery.message.empty() ? QStringLiteral("CXP camera discovery failed.")
                                              : QString::fromStdString(discovery.message),
                    QJsonObject{{"sdkResult", discovery.sdkResult}});
            } else if (name == kSingle) {
                framegrabber->grab(1);
                const auto deadline = std::chrono::steady_clock::now() + kSingleAcquisitionTimeout;
                while (framegrabber->isGrabbing() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                if (framegrabber->isGrabbing()) {
                    framegrabber->stop();
                    failure = Result::failure(ApiErrorCode::Timeout,
                        "Not every DMA channel delivered a frame within 30 s; acquisition was stopped.");
                } else {
                    const int dmaCount = framegrabber->getDMACount();
                    int delivered = 0;
                    for (const auto& channel : framegrabber->acquisitionStatistics())
                        if (channel.delivered >= 1) ++delivered;
                    if (dmaCount > 0 && delivered >= dmaCount) payload = Payload::State;
                    else failure = Result::failure(ApiErrorCode::Unavailable,
                        "Acquisition ended before every DMA channel delivered a frame.");
                }
            }
        } catch (const std::exception& error) {
            failure = Result::failure(ApiErrorCode::Unavailable, QString::fromLocal8Bit(error.what()));
        } catch (...) {
            failure = Result::failure(ApiErrorCode::Internal, "Unexpected frame grabber backend failure.");
        }
        framegrabber->endControl();
        if (failure) { deliver(std::move(*failure)); return; }
        switch (payload) {
        case Payload::State: deliver(Result::success(controlState(true))); break;
        case Payload::Applets: deliver(Result::success(appletsJson(*framegrabber))); break;
        case Payload::Cameras: deliver(Result::success(camerasJson(*framegrabber))); break;
        case Payload::None: deliver(Result::failure(ApiErrorCode::Internal, "Frame grabber operation did not complete.")); break;
        }
    });
    return Admission::success();
}

// --- Standard connection lifecycle ---------------------------------------

namespace {
DeviceConnectionError lifecycleError(const ApiErrorCode code)
{
    switch (code) {
    case ApiErrorCode::InvalidArgument: return DeviceConnectionError::InvalidArgument;
    case ApiErrorCode::NotFound: return DeviceConnectionError::NotFound;
    case ApiErrorCode::Conflict: return DeviceConnectionError::Conflict;
    case ApiErrorCode::Busy: return DeviceConnectionError::Busy;
    case ApiErrorCode::Unsupported: return DeviceConnectionError::Unsupported;
    case ApiErrorCode::Unavailable: return DeviceConnectionError::Unavailable;
    case ApiErrorCode::Timeout: return DeviceConnectionError::Timeout;
    default: return DeviceConnectionError::BackendFailure;
    }
}
}

DeviceConnectionState FramegrabberSourceController::connectionState() const
{
    const bool connected = _framegrabber && _framegrabber->isOpened();
    if (!connected) return {};
    const std::string board = _framegrabber->getConnectedFramegrabberName();
    const std::string applet = _framegrabber->getLoadedAppletName();
    return {true, _framegrabber->isGrabbing(), connectionId().toStdString(), board,
            applet.empty() ? board : board + " / " + applet, std::nullopt};
}

DeviceConnectionCapabilities FramegrabberSourceController::connectionCapabilities() const
{
    if (!_framegrabber) {
        const DeviceConnectionAvailability ended{false, "The frame grabber session has ended."};
        return {ended, ended, ended, ended, ended, ended};
    }
    const DeviceConnectionAvailability yes{true, {}};
    const DeviceConnectionAvailability applet{false,
        "A frame grabber connection is made by loading an applet with framegrabber.applet-load or framegrabber.configuration-load."};
    return {applet, applet, applet, yes, yes, {false, "Live acquisition uses source.start and source.stop."}};
}

DeviceConnectionResult FramegrabberSourceController::requestConnectionTargets(TargetsCompletion)
{
    return {DeviceConnectionError::Unsupported, connectionCapabilities().targets.reason};
}

DeviceConnectionResult FramegrabberSourceController::requestConnect(std::string, IDeviceConnectionControl::Completion)
{
    return {DeviceConnectionError::Unsupported, connectionCapabilities().connect.reason};
}

DeviceConnectionResult FramegrabberSourceController::requestConnectionState(IDeviceConnectionControl::Completion completion)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!completion) return {DeviceConnectionError::InvalidArgument, "Completion is required."};
    if (!_framegrabber) return {DeviceConnectionError::Unavailable, "The frame grabber session has ended."};
    try { completion({{}, {}, {}, connectionState()}); }
    catch (...) { qWarning() << "[Framegrabber] lifecycle consumer callback threw."; }
    return {};
}

DeviceConnectionResult FramegrabberSourceController::requestDisconnect(std::string connectionId,
                                                                       IDeviceConnectionControl::Completion completion)
{
    return admitLifecycle(kDisconnect, std::move(connectionId), std::move(completion));
}

DeviceConnectionResult FramegrabberSourceController::requestSingle(std::string connectionId,
                                                                   IDeviceConnectionControl::Completion completion)
{
    return admitLifecycle(kSingle, std::move(connectionId), std::move(completion));
}

DeviceConnectionResult FramegrabberSourceController::admitLifecycle(const QString& name, std::string connectionId,
                                                                    IDeviceConnectionControl::Completion completion)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!completion) return {DeviceConnectionError::InvalidArgument, "Completion is required."};
    if (_framegrabber && !_framegrabber->isOpened())
        return {DeviceConnectionError::Disconnected, "No applet is loaded."};
    // The shared control path keeps one gate and one worker for GUI, module tools and lifecycle calls.
    // The controller drains its worker before destruction, so the completion may read it.
    const auto admitted = admitControl(name, QJsonObject{{"connectionId", QString::fromStdString(connectionId)}},
        [this, completion = std::move(completion)](ApiResult<QJsonValue> result) noexcept {
            DeviceConnectionResult copied;
            if (result.succeeded()) {
                copied = {{}, {}, DeviceConnectionEffect::Applied, connectionState()};
            } else {
                // Only accepted SDK work fails here, so its effect on the device is uncertain.
                copied = {lifecycleError(result.error()->code), result.error()->message.toStdString(),
                          DeviceConnectionEffect::Unknown};
            }
            try { completion(std::move(copied)); }
            catch (...) { qWarning() << "[Framegrabber] lifecycle consumer callback threw."; }
        });
    if (admitted.succeeded()) return {};
    return {lifecycleError(admitted.error()->code), admitted.error()->message.toStdString()};
}

std::optional<SourceRuntimeDiagnostics> FramegrabberSourceController::sourceRuntimeDiagnostics(unsigned sourceIndex) const
{
    if (!_framegrabber || sourceIndex >= static_cast<unsigned>(_framegrabber->getDMACount())) return std::nullopt;
    const auto value = _framegrabber->runtimeDiagnostics(sourceIndex);
    return SourceRuntimeDiagnostics{value.grabbing, value.delivered, value.failed};
}
