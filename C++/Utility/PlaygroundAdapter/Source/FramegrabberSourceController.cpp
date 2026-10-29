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
        {"device.connection", 1, "Frame grabber state",
            "Observe the loaded board/applet, connection generation, last SDK error and per-DMA acquisition counters.",
            ToolEffect::Observe, true, {}, empty, state},
        {"device.boards", 1, "Frame grabber boards", "Copy the boards discovered by the SDK for this session.",
            ToolEffect::Observe, true, {}, empty, boardsSchema()},
        {"device.applets", 1, "Loadable applets",
            "Copy the applets the SDK enumerated as loadable (FG_AIS_FILESYSTEM, FG_AF_IS_LOADABLE).",
            ToolEffect::Observe, true, {}, empty, appletsSchema()},
        {"device.applets-refresh", 1, "Enumerate applets",
            "Re-enumerate loadable applets for a board; refused while an applet is loaded.",
            ToolEffect::Observe, true, {}, schema({{"board", text()}}, {}), appletsSchema()},
        {"device.applet-load", 1, "Load applet",
            "Initialize the board with an applet path or unique applet name (Fg_Init). Replacing a loaded applet requires its connectionId; acquisition must be stopped.",
            ToolEffect::Device, true, {},
            schema({{"board", text()}, {"applet", text()}, {"connectionId", text()}}, {"board", "applet"}), state},
        {"device.configuration-load", 1, "Load configuration",
            "Apply an .mcf file: Fg_InitConfig, or Fg_loadConfig when its applet is loaded. An applet argument replaces the MCF applet reference.",
            ToolEffect::Device, true, {},
            schema({{"board", text()}, {"configuration", text()}, {"applet", text()}, {"connectionId", text()}},
                {"board", "configuration"}), state},
        {"device.configuration-save", 1, "Save configuration",
            "Write the current applet parameters to an .mcf file (Fg_saveConfig) on the host file system.",
            ToolEffect::Device, true, {},
            schema({{"connectionId", text()}, {"configuration", text()}}, {"connectionId", "configuration"}), state},
        {"device.disconnect", 1, "Release frame grabber",
            "Stop acquisition and release the exact connection generation (Fg_FreeGrabber).",
            ToolEffect::Device, true, {}, connection, state},
        {"device.cameras", 1, "CoaXPress cameras", "Copy the cameras found by the last CXP discovery.",
            ToolEffect::Observe, true, {}, empty, camerasSchema()},
        {"device.cameras-refresh", 1, "Rediscover cameras",
            "Restart CXP discovery and reconnect cameras on the exact connection; topology mismatches are reported.",
            ToolEffect::Device, true, {}, connection, camerasSchema()},
        {"device.single", 1, "Acquire one frame per DMA",
            "Acquire one frame on every DMA channel and wait up to 30 s; query the viewer separately for presentation.",
            ToolEffect::Pipeline, true, {}, connection, state}};
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

    const auto deliver = [completion = std::move(completion)](Result result) noexcept {
        try { completion(std::move(result)); }
        catch (...) { qWarning() << "[Framegrabber] tool consumer callback threw."; }
    };
    if (name == "device.connection") { deliver(Result::success(controlState())); return Admission::success(); }
    if (name == "device.applets") { deliver(Result::success(appletsJson(*_framegrabber))); return Admission::success(); }
    if (name == "device.cameras") { deliver(Result::success(camerasJson(*_framegrabber))); return Admission::success(); }
    if (name == "device.boards") {
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
    const bool needsConnection = name == "device.configuration-save" || name == "device.disconnect"
        || name == "device.cameras-refresh" || name == "device.single";
    if (needsConnection) {
        if (!connected) return Admission::failure(ApiErrorCode::Unavailable, "No applet is loaded.");
        if (requestedConnection != currentConnection)
            return Admission::failure(ApiErrorCode::Conflict, "Connection generation changed.");
    }
    std::string boardName;
    if (arguments.contains("board")) {
        const QString board = arguments.value("board").toString();
        bool found = false;
        for (const auto& candidate : _framegrabber->getCachedBoardInfo())
            if (boardMatches(candidate, board)) { found = true; boardName = candidate.displayName(); }
        if (!found) return Admission::failure(ApiErrorCode::NotFound, "Exact declared board was not found.");
    }
    if (name == "device.applets-refresh" && connected)
        return Admission::failure(ApiErrorCode::Conflict, "Release the board before enumerating applets.");
    // Applets contain native libraries. Callable requests load only applets the SDK
    // enumerated from its standard locations; arbitrary files remain a GUI choice.
    if (arguments.contains("applet")
        && catalogApplet(*_framegrabber, arguments.value("applet").toString().toStdString()).empty())
        return Admission::failure(ApiErrorCode::PermissionDenied,
            "Only applets listed by device.applets can be loaded through the API.");
    if (name == "device.applet-load" || name == "device.configuration-load") {
        if (connected && requestedConnection.isEmpty())
            return Admission::failure(ApiErrorCode::Conflict, "Pass the current connectionId to replace the loaded applet.");
        if (connected && requestedConnection != currentConnection)
            return Admission::failure(ApiErrorCode::Conflict, "Connection generation changed.");
        if (connected && boardName != _framegrabber->getConnectedFramegrabberName())
            return Admission::failure(ApiErrorCode::Conflict, "Release the loaded board before selecting another board.");
    }
    if (_framegrabber->isGrabbing() && name != "device.disconnect")
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
            if (name == "device.applets-refresh") {
                if (framegrabber->refreshAppletCatalog(boardName)) payload = Payload::Applets;
                else failure = Result::failure(ApiErrorCode::Conflict, "Applet enumeration requires a released board.");
            } else if (name == "device.applet-load") {
                if (framegrabber->loadApplet(arguments.value("applet").toString().toStdString(), boardName))
                    payload = Payload::State;
                else failure = failed("Failed to load the applet.");
            } else if (name == "device.configuration-load") {
                const std::string configuration = arguments.value("configuration").toString().toStdString();
                const std::string applet = arguments.value("applet").toString().toStdString();
                // The MCF names its own applet; apply the same catalog restriction to it.
                const std::string configured = applet.empty()
                    ? framegrabber->appletPathFromConfiguration(configuration, boardName) : applet;
                if (catalogApplet(*framegrabber, configured).empty())
                    failure = Result::failure(ApiErrorCode::PermissionDenied,
                        "The configuration does not reference an applet listed by device.applets; pass one as applet.");
                else if (framegrabber->loadAppletConfiguration(applet, configuration, boardName))
                    payload = Payload::State;
                else failure = failed("Failed to apply the configuration.");
            } else if (name == "device.configuration-save") {
                if (framegrabber->saveConfiguration(arguments.value("configuration").toString().toStdString()))
                    payload = Payload::State;
                else failure = failed("Failed to save the configuration.");
            } else if (name == "device.disconnect") {
                framegrabber->close();
                if (!framegrabber->isOpened()) payload = Payload::State;
                else failure = failed("The frame grabber could not be released.");
            } else if (name == "device.cameras-refresh") {
                const bool connected = framegrabber->refreshCameras(Framegrabber::CameraTransport::CoaXPress);
                const auto discovery = framegrabber->lastCameraDiscovery();
                if (connected || discovery.topologyMismatch) payload = Payload::Cameras;
                else failure = Result::failure(ApiErrorCode::Unavailable,
                    discovery.message.empty() ? QStringLiteral("CXP camera discovery failed.")
                                              : QString::fromStdString(discovery.message),
                    QJsonObject{{"sdkResult", discovery.sdkResult}});
            } else if (name == "device.single") {
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
