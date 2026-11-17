#include "FramegrabberSourceController.h"

#include "Utility/PlaygroundAdapter/FramegrabberGraphicsFrameStream.h"
#include "DeviceRuntime/Contract/SessionFrame.h"

#include <QDebug>
#include <QFileInfo>
#include <QJsonArray>
#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <chrono>
#include <cctype>
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
// Interface label copied into the target/state record; it is not an admission rule.
const char* const kInterfaceType = "FrameGrabber";
const char* const kInterfaceReason =
    "A Basler frame grabber board is one exclusive resource; one connection owns its DMA sources.";

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

QJsonObject promptText(const char* format)
{
    return {{"type", "string"}, {"minLength", 1}, {"maxLength", 4096}, {"format", format}};
}

/** @brief A Host-owned file-open input: the Host dialog supplies an absolute path. */
QJsonObject hostFileText(const QString& title, const QStringList& nameFilters)
{
    return {{"type", "string"}, {"minLength", 1}, {"maxLength", 4096},
            {"format", "file-open"}, {"title", title},
            {"nameFilters", QJsonArray::fromStringList(nameFilters)}};
}

/** @brief Native applet libraries the SDK accepts for Fg_Init, case-insensitively. */
bool isAllowedAppletFile(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return extension == ".hap" || extension == ".dll" || extension == ".so";
}

/** @brief Filesystem checks need the native wide path on Windows; QString::toStdString() is UTF-8. */
std::filesystem::path nativeFileSystemPath(const QString& value)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(value.toStdWString());
#else
    return std::filesystem::path(value.toStdString());
#endif
}

/** @brief The narrow Fg_Init(const char*) path uses the process ANSI code page on Windows.
 *  @return Empty with an actionable reason when the path cannot be represented losslessly.
 */
std::string sdkFilePath(const QString& value, QString& reason)
{
#ifdef Q_OS_WIN
    const QByteArray local = value.toLocal8Bit();
    if (QString::fromLocal8Bit(local) != value) {
        reason = QStringLiteral("The applet path cannot be represented in the current Windows code page; use an ASCII-only directory.");
        return {};
    }
    return std::string(local.constData(), static_cast<std::size_t>(local.size()));
#else
    Q_UNUSED(reason);
    return value.toStdString();
#endif
}

QJsonObject enumText(const QStringList& values)
{
    QJsonArray allowed;
    for (const auto& value : values)
    {
        if (allowed.size() == 64) break;
        allowed.append(value.left(4096));
    }
    return {{"type", "string"}, {"minLength", 1}, {"maxLength", 4096},
            {"format", "enum"}, {"enum", allowed}};
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

template<class Backend>
QJsonObject appletsJson(const Backend& backend)
{
    const std::string loaded = backend.isOpened ? (backend.isOpened() ? backend.appletPath() : std::string{})
                                                : std::string{};
    QJsonArray applets;
    if (backend.appletCatalog)
    {
        for (const Framegrabber::AppletInfo& applet : backend.appletCatalog())
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
    }
    return {{"applets", applets}};
}

template<class Backend>
QJsonObject camerasJson(const Backend& backend)
{
    QJsonArray cameras;
    if (backend.cachedCameras)
    {
        for (const Framegrabber::CameraInfo& camera :
             backend.cachedCameras(Framegrabber::CameraTransport::CoaXPress))
        {
            cameras.append(QJsonObject{{"dma", static_cast<int>(camera.dmaIndex)},
                {"vendor", QString::fromStdString(camera.vendor)},
                {"model", QString::fromStdString(camera.model)},
                {"serial", QString::fromStdString(camera.serial)}, {"connected", camera.connected}});
        }
    }
    const auto discovery = backend.cameraDiscovery ? backend.cameraDiscovery()
                                                   : Framegrabber::CameraDiscovery{};
    return {{"cameras", cameras}, {"discovery", discoveryJson(discovery)}};
}

bool boardMatches(const FramegrabberSystem::BoardInfo& board, const QString& reference)
{
    const std::string value = reference.toStdString();
    return board.displayName() == value || board.name == value || board.serial == value;
}

/** @brief Returns the catalog path a reference resolves to, or empty outside the catalog. */
template<class Backend>
std::string catalogApplet(const Backend& backend, const std::string& reference)
{
    if (!backend.resolveApplet || !backend.appletCatalog) return {};
    const std::string resolved = backend.resolveApplet(reference);
    if (resolved.empty())
    {
        return {};
    }
    for (const auto& applet : backend.appletCatalog())
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

/** @brief Copies the bounded enum options the installed catalog offers. */
template<class Backend>
QStringList appletOptions(const Backend& backend)
{
    QStringList options;
    if (!backend.appletCatalog) return options;
    const auto catalog = backend.appletCatalog();
    std::map<std::string, int> nameCounts;
    for (const auto& applet : catalog)
        if (!applet.name.empty()) ++nameCounts[applet.name];
    for (const auto& applet : catalog)
    {
        if (options.size() == 64) break;
        // A unique name resolves unambiguously; a duplicated or empty name falls back to the
        // exact catalog path so every offered value can still be loaded.
        QString option;
        if (!applet.name.empty() && nameCounts[applet.name] == 1)
            option = QString::fromStdString(applet.name);
        else
            option = QString::fromStdString(applet.path.empty() ? applet.file : applet.path);
        if (!option.isEmpty() && !options.contains(option)) options.append(option);
    }
    return options;
}

QString actionGroup()
{
    return QStringLiteral("framegrabber");
}

bool isActionName(const QString& name)
{
    return name == QLatin1String("framegrabber.applets-refresh")
        || name == QLatin1String("framegrabber.applet-load")
        || name == QLatin1String("framegrabber.applet-load-file")
        || name == QLatin1String("framegrabber.configuration-load")
        || name == QLatin1String("framegrabber.configuration-save")
        || name == QLatin1String("framegrabber.cameras-refresh");
}

QString loadPhase(const QString& name)
{
    return name == QLatin1String("framegrabber.configuration-load")
        ? QStringLiteral("configuration-load") : QStringLiteral("applet-load");
}

QString loadMessage(const QString& name, const QString& argument)
{
    // ApiProgress bounds messages to 256 characters; a longer report is dropped. Keep the full
    // path in the result error, but bound the progress detail so the panel always updates.
    return (name == QLatin1String("framegrabber.configuration-load")
        ? QStringLiteral("Applying configuration '%1'.").arg(argument)
        : QStringLiteral("Loading applet '%1'.").arg(argument)).left(256);
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
      _notifications([this](const std::string& connectionId, const std::string& subscriptionId) {
          return snapshotChange(connectionId, subscriptionId);
      }),
      _instanceId(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
    bindBackend();
    _sessionAvailable.store(_framegrabber != nullptr, std::memory_order_release);
    startObservationThread();
    registerCallbacks();
}

FramegrabberSourceController::~FramegrabberSourceController()
{
    // The observation thread and accepted tool/parameter work complete before the
    // Framegrabber is released; subscribing listeners never outlive this controller.
    stopObservationThread();
    _notifications.close();
    _work.waitForFinished();
    stop();
    deregisterCallbacks();
}

void FramegrabberSourceController::bindBackend()
{
    Framegrabber* const framegrabber = _framegrabber;
    if (!framegrabber) return;
    _backend.boards = [framegrabber] { return framegrabber->getCachedBoardInfo(); };
    _backend.isOpened = [framegrabber] { return framegrabber->isOpened(); };
    _backend.connectedBoard = [framegrabber] { return framegrabber->getConnectedFramegrabberName(); };
    _backend.loadedAppletName = [framegrabber] { return framegrabber->getLoadedAppletName(); };
    _backend.loadedAppletVersion = [framegrabber] { return framegrabber->getLoadedAppletVersion(); };
    _backend.appletPath = [framegrabber] { return framegrabber->appletPath(); };
    _backend.configurationPath = [framegrabber] { return framegrabber->configurationPath(); };
    _backend.connectionGeneration = [framegrabber] { return framegrabber->connectionGeneration(); };
    _backend.refreshAppletCatalog = [framegrabber](const std::string& board) {
        return framegrabber->refreshAppletCatalog(board);
    };
    _backend.appletCatalog = [framegrabber] { return framegrabber->appletCatalog(); };
    _backend.resolveApplet = [framegrabber](const std::string& reference) {
        return framegrabber->resolveAppletReference(reference);
    };
    _backend.loadApplet = [framegrabber](const std::string& reference, const std::string& board) {
        return framegrabber->loadApplet(reference, board);
    };
    _backend.appletPathFromConfiguration =
        [framegrabber](const std::string& configuration, const std::string& board) {
            return framegrabber->appletPathFromConfiguration(configuration, board);
        };
    _backend.loadAppletConfiguration =
        [framegrabber](const std::string& applet, const std::string& configuration,
                       const std::string& board) {
            return framegrabber->loadAppletConfiguration(applet, configuration, board);
        };
    _backend.saveConfiguration = [framegrabber](const std::string& configuration) {
        return framegrabber->saveConfiguration(configuration);
    };
    _backend.close = [framegrabber] { framegrabber->close(); };
    _backend.isGrabbing = [framegrabber] { return framegrabber->isGrabbing(); };
    _backend.dmaCount = [framegrabber] { return framegrabber->getDMACount(); };
    _backend.tryBeginControl = [framegrabber] { return framegrabber->tryBeginControl(); };
    _backend.endControl = [framegrabber] { framegrabber->endControl(); };
    _backend.controlBusy = [framegrabber] { return framegrabber->controlBusy(); };
    _backend.lastError = [framegrabber] { return framegrabber->lastError(); };
    _backend.acquisitionStatistics = [framegrabber] { return framegrabber->acquisitionStatistics(); };
    _backend.cameraDiscovery = [framegrabber] { return framegrabber->lastCameraDiscovery(); };
    _backend.refreshCameras = [framegrabber](const Framegrabber::CameraTransport transport) {
        return framegrabber->refreshCameras(transport);
    };
    _backend.cachedCameras = [framegrabber](const Framegrabber::CameraTransport transport) {
        return framegrabber->getCachedCameraList(transport);
    };
    _backend.runtimeDiagnostics = [framegrabber](const std::optional<unsigned> dma) {
        return framegrabber->runtimeDiagnostics(dma);
    };
    _backend.grab = [framegrabber](const std::size_t frames) { framegrabber->grab(frames); };
    _backend.stop = [framegrabber] { framegrabber->stop(); };
    _backend.appletFeatureModel = [framegrabber](const unsigned dma) {
        return framegrabber->getAppletFeatureModel(dma);
    };
    _backend.appletParameterLimits =
        [framegrabber](const std::int64_t id, const unsigned dma) {
            return framegrabber->getAppletParameterLimits(id, dma);
        };
    _backend.getAppletParameter =
        [framegrabber](const std::int64_t id, const unsigned dma,
                       Framegrabber::ParameterValue& value, const bool useCache) {
            return framegrabber->getAppletParameterById(id, dma, value, useCache);
        };
    _backend.appletParameterPrototype = [framegrabber](const std::int64_t id, const unsigned dma) {
        return framegrabber->appletParameterPrototype(id, dma);
    };
    _backend.setAppletParameter =
        [framegrabber](const std::int64_t id, const unsigned dma,
                       const Framegrabber::ParameterValue& value, const bool verify,
                       const bool invalidate, int* result) {
            return framegrabber->setAppletParameterById(id, dma, value, verify, invalidate, result);
        };
    _backend.executeAppletCommand = [framegrabber](const unsigned dma, const std::int64_t id,
                                                   int* result) {
        return framegrabber->executeAppletCommandById(dma, id, result);
    };
    _backend.cameraCapabilities = [framegrabber] { return framegrabber->cameraControlCapabilities(); };
    _backend.cameraFeatureXml = [framegrabber](const Framegrabber::CameraTransport transport,
                                               const unsigned dma) {
        return framegrabber->getCameraFeatureXml(transport, dma);
    };
    _backend.getCameraFeature = [framegrabber](const Framegrabber::CameraTransport transport,
                                               const unsigned dma, const std::string& name,
                                               Framegrabber::ParameterValue& value) {
        return framegrabber->getCameraFeature(transport, dma, name, value);
    };
    _backend.cameraFeatureLimits =
        [framegrabber](const Framegrabber::CameraTransport transport, const unsigned dma,
                       const std::string& name, const bool floatingPoint) {
            return framegrabber->getCameraFeatureLimits(transport, dma, name, floatingPoint);
        };
    _backend.setCameraFeature =
        [framegrabber](const Framegrabber::CameraTransport transport, const unsigned dma,
                       const std::string& name, const Framegrabber::ParameterValue& value,
                       const bool verify) {
            return framegrabber->setCameraFeature(transport, dma, name, value, verify);
        };
    _backend.executeCameraCommand =
        [framegrabber](const Framegrabber::CameraTransport transport, const unsigned dma,
                       const std::string& name) {
            return framegrabber->executeCameraCommand(transport, dma, name);
        };
}

void FramegrabberSourceController::start()
{
    if (!_sessionAvailable.load(std::memory_order_acquire))
    {
        return;
    }
    publishSourceDescriptors();
    if (_backend.grab) _backend.grab(0);
}

void FramegrabberSourceController::stop()
{
    if (_sessionAvailable.load(std::memory_order_acquire) && _backend.stop) _backend.stop();
    _isGrabbing.store(false, std::memory_order_release);
}

std::vector<GraphicsSourceDescriptor> FramegrabberSourceController::sourceDescriptors() const
{
    std::vector<GraphicsSourceDescriptor> sources;
    const int count = _sessionAvailable.load(std::memory_order_acquire) && _backend.dmaCount
        ? _backend.dmaCount() : 0;
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
                // Writability is re-observed after acquisition changes; never cached from a
                // locked state and never deferred to a GUI timer.
                wakeObservation(false);
            }
            else if (status == Framegrabber::ConnectionStatus && on && guard)
            {
                // A newly loaded applet can change the DMA topology.
                QMetaObject::invokeMethod(this, [guard]
                {
                    if (guard) guard->publishSourceDescriptors();
                }, Qt::QueuedConnection);
                wakeObservation(false);
            }
            else if (status == Framegrabber::ConnectionStatus
                     || status == Framegrabber::ConfigurationStatus)
            {
                wakeObservation(false);
            }
        });

    // The panel and the API observe the same node replacements; reopen is not required.
    _nodeCallbackId = _framegrabber->registerNodeUpdatedCallback(
        [this](Framegrabber::FeatureSource, Framegrabber::CameraTransport,
               unsigned int, const std::string&)
        {
            wakeObservation(false);
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
    if (_nodeCallbackId != 0)
    {
        _framegrabber->deregisterNodeUpdatedCallback(_nodeCallbackId);
        _nodeCallbackId = 0;
    }
}

bool FramegrabberSourceController::workerBusy() const
{
    return _work.isStarted() && !_work.isFinished();
}

QString FramegrabberSourceController::connectionId() const
{
    const std::uint64_t generation = _sessionAvailable.load(std::memory_order_acquire)
        && _backend.connectionGeneration ? _backend.connectionGeneration() : 0;
    return generation == 0 ? QString() : _instanceId + QLatin1Char(':') + QString::number(generation);
}

QJsonObject FramegrabberSourceController::controlState(const bool fromWorker) const
{
    const bool available = _sessionAvailable.load(std::memory_order_acquire);
    const bool connected = available && _backend.isOpened && _backend.isOpened();
    const Framegrabber::OperationError error =
        available && _backend.lastError ? _backend.lastError() : Framegrabber::OperationError{};
    QJsonArray statistics;
    if (available && _backend.acquisitionStatistics)
    {
        for (const auto& channel : _backend.acquisitionStatistics())
        {
            statistics.append(QJsonObject{{"dma", static_cast<int>(channel.dmaIndex)},
                {"delivered", QString::number(channel.delivered)},
                {"skipped", QString::number(channel.skipped)},
                {"overwritten", QString::number(channel.overwritten)},
                {"rejected", QString::number(channel.rejected)}});
        }
    }
    const auto stringOrEmpty = [connected](const std::function<std::string()>& getter) {
        return connected && getter ? QString::fromStdString(getter()) : QString();
    };
    const auto discovery = available && _backend.cameraDiscovery
        ? _backend.cameraDiscovery() : Framegrabber::CameraDiscovery{};
    return {{"connected", connected},
        {"live", connected && _backend.isGrabbing && _backend.isGrabbing()},
        {"busy", (available && _backend.controlBusy && _backend.controlBusy())
            || (!fromWorker && workerBusy())},
        {"connectionId", connectionId()},
        {"board", stringOrEmpty(_backend.connectedBoard)},
        {"applet", QJsonObject{
            {"name", stringOrEmpty(_backend.loadedAppletName)},
            {"path", stringOrEmpty(_backend.appletPath)},
            {"version", stringOrEmpty(_backend.loadedAppletVersion)}}},
        {"configuration", stringOrEmpty(_backend.configurationPath)},
        {"dmaCount", connected && _backend.dmaCount ? _backend.dmaCount() : 0},
        {"lastError", QJsonObject{{"code", error.code}, {"message", QString::fromStdString(error.message)}}},
        {"cameraDiscovery", discoveryJson(discovery)},
        {"statistics", statistics}};
}

QList<ToolDescriptor> FramegrabberSourceController::toolDescriptors() const
{
    const auto empty = schema({}, {});
    const auto connection = schema({{"connectionId", text()}}, {"connectionId"});
    const auto state = stateSchema();
    const QStringList options = appletOptions(_backend);
    const QJsonObject applet = options.isEmpty() ? text() : enumText(options);
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
            "Initialize the board with an installed applet selected by an enumerated unique name or path (Fg_Init). An omitted board uses the session's connected board. Replacing a loaded applet requires its connectionId; acquisition must be stopped.",
            ToolEffect::Device, ToolEgress::Metadata, true, {},
            schema({{"board", text()}, {"applet", applet}, {"connectionId", text()}}, {"applet"}), state},
        {"framegrabber.applet-load-file", 1, "Load applet file",
            "Initialize the board with an applet file chosen in the Host file dialog (Fg_Init). The path must exist and end in .hap, .dll or .so. An omitted board uses the session's connected board. Replacing a loaded applet requires its connectionId; acquisition must be stopped.",
            ToolEffect::Device, ToolEgress::Metadata, true, {},
            schema({{"board", text()}, {"connectionId", text()},
                    {"appletFile", hostFileText(QStringLiteral("Applet file"),
                        {QStringLiteral("Applet libraries (*.hap *.dll *.so)"),
                         QStringLiteral("All files (*)")})}},
                {"appletFile"}), state},
        {"framegrabber.configuration-load", 1, "Load configuration",
            "Apply an .mcf file: Fg_InitConfig, or Fg_loadConfig when its applet is loaded. An omitted board uses the session's connected board. An applet argument selects an installed applet that replaces the MCF applet reference.",
            ToolEffect::Device, ToolEgress::Metadata, true, {},
            schema({{"board", text()}, {"configuration", promptText("file-open")},
                    {"applet", applet}, {"connectionId", text()}},
                {"configuration"}), state},
        {"framegrabber.configuration-save", 1, "Save configuration",
            "Write the current applet parameters to an .mcf file (Fg_saveConfig) on the host file system.",
            ToolEffect::Device, ToolEgress::Metadata, true, {},
            schema({{"connectionId", text()}, {"configuration", promptText("file-save")}},
                {"connectionId", "configuration"}), state},
        {"framegrabber.cameras", 1, "CoaXPress cameras", "Copy the cameras found by the last CXP discovery.",
            ToolEffect::Observe, ToolEgress::Metadata, true, {}, empty, camerasSchema()},
        {"framegrabber.cameras-refresh", 1, "Rediscover cameras",
            "Restart CXP discovery and reconnect cameras on the exact connection; topology mismatches are reported.",
            ToolEffect::Device, ToolEgress::Metadata, true, {}, connection, camerasSchema()}};
    // Only user actions carry presentation metadata; observation tools stay API-only.
    for (auto& descriptor : result)
    {
        if (!isActionName(descriptor.name)) continue;
        descriptor.presentation.actionId = descriptor.name;
        descriptor.presentation.group = actionGroup();
    }
    const auto present = [&result](const char* name, const char* icon, const int order) {
        for (auto& descriptor : result)
        {
            if (descriptor.name != QLatin1String(name)) continue;
            descriptor.presentation.iconId = QString::fromLatin1(icon);
            descriptor.presentation.order = order;
        }
    };
    present("framegrabber.applets-refresh", "refresh", 10);
    present("framegrabber.applet-load", "plug-connected", 20);
    present("framegrabber.applet-load-file", "opened-folder", 25);
    present("framegrabber.configuration-load", "opened-folder", 30);
    present("framegrabber.configuration-save", "save", 40);
    present("framegrabber.cameras-refresh", "cameras", 50);
    if (!_sessionAvailable.load(std::memory_order_acquire))
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
    return requestToolInternal(name, version, std::move(arguments), {}, std::move(completion));
}

ApiResult<void> FramegrabberSourceController::requestToolWithProgress(
    const QString& callId, const QString& name, const int version, QJsonObject arguments,
    Progress progress, IToolProvider::Completion completion)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (callId.isEmpty())
        return ApiResult<void>::failure(ApiErrorCode::InvalidArgument, "An exact call identity is required.");
    {
        std::lock_guard<std::mutex> lock(_activeCallMutex);
        if (!_activeCallId.isEmpty())
            return ApiResult<void>::failure(ApiErrorCode::Busy,
                "Another accepted call already holds the module worker.");
        _activeCallId = callId;
    }
    const auto clear = [this, callId] {
        std::lock_guard<std::mutex> lock(_activeCallMutex);
        if (_activeCallId == callId) _activeCallId.clear();
    };
    const auto admitted = requestToolInternal(name, version, std::move(arguments), progress,
        [clear, completion = std::move(completion)](ApiResult<QJsonValue> result) noexcept
        {
            clear();
            try { completion(std::move(result)); }
            catch (...) { qWarning() << "[Framegrabber] tool consumer callback threw."; }
        });
    if (!admitted.succeeded()) clear();
    return admitted;
}

ApiResult<void> FramegrabberSourceController::cancelTool(const QString& callId)
{
    Q_ASSERT(QThread::currentThread() == thread());
    std::lock_guard<std::mutex> lock(_activeCallMutex);
    if (callId.isEmpty() || _activeCallId != callId)
        return ApiResult<void>::failure(ApiErrorCode::NotFound,
            "No accepted call holds that identity.");
    // The Basler Frame Grabber SDK exposes no cancellation for Fg_Init/Fg_InitConfig; the
    // accepted call completes normally and its completion reports the outcome.
    return ApiResult<void>::failure(ApiErrorCode::Unsupported,
        "The Basler Frame Grabber SDK cannot cancel an applet or configuration load.");
}

ApiResult<void> FramegrabberSourceController::requestToolInternal(
    const QString& name, const int version, QJsonObject arguments,
    std::function<void(const ApiProgress&)> progress, IToolProvider::Completion completion)
{
    Q_ASSERT(QThread::currentThread() == thread());
    using Result = ApiResult<QJsonValue>;
    using Admission = ApiResult<void>;
    if (!completion) return Admission::failure(ApiErrorCode::InvalidArgument, "Completion is required.");
    std::optional<ToolDescriptor> descriptor;
    for (const auto& candidate : toolDescriptors()) if (candidate.name == name) descriptor = candidate;
    if (!descriptor) return Admission::failure(ApiErrorCode::NotFound, "Tool is not declared.");
    if (version != descriptor->version) return Admission::failure(ApiErrorCode::Unsupported, "Unsupported tool version.");
    if (!_sessionAvailable.load(std::memory_order_acquire))
        return Admission::failure(ApiErrorCode::Unavailable, descriptor->unavailableReason);
    // The registry validates schemas; direct callers get the same strict check here.
    const QJsonObject properties = descriptor->inputSchema.value("properties").toObject();
    for (auto it = arguments.begin(); it != arguments.end(); ++it)
        if (!properties.contains(it.key()) || !it.value().isString() || it.value().toString().isEmpty()
            || it.value().toString().contains(QChar::Null))
            return Admission::failure(ApiErrorCode::InvalidArgument, "Exact typed arguments are required.");
    // An omitted board means the session's connected board, so the host panel never needs a
    // field-name rule. Resolve it before prompting; a session without a board refuses first.
    const bool loadAction = name == QLatin1String("framegrabber.applet-load")
        || name == QLatin1String("framegrabber.applet-load-file")
        || name == QLatin1String("framegrabber.configuration-load");
    if (loadAction && !arguments.contains("board"))
    {
        const std::string connected =
            _backend.connectedBoard ? _backend.connectedBoard() : std::string{};
        if (connected.empty())
            return Admission::failure(ApiErrorCode::Unavailable,
                "No board is connected; pass board explicitly or connect the session board first.");
        arguments.insert(QStringLiteral("board"), QString::fromStdString(connected));
    }

    const QStringList options = appletOptions(_backend);
    for (const auto required : descriptor->inputSchema.value("required").toArray())
    {
        if (arguments.contains(required.toString())) continue;
        const QString field = required.toString();
        if (field == QLatin1String("applet") && !options.isEmpty())
        {
            // A missing installed applet is a retryable prompt, never an effect.
            try { completion(argumentRequiredFailure("applet", "enum",
                "Choose one of the applets installed for this board.", options)); }
            catch (...) { qWarning() << "[Framegrabber] tool consumer callback threw."; }
            return Admission::success();
        }
        if (field == QLatin1String("configuration"))
        {
            const bool save = name == QLatin1String("framegrabber.configuration-save");
            try { completion(argumentRequiredFailure("configuration",
                save ? "file-save" : "file-open",
                save ? "Choose the .mcf file to write." : "Choose the .mcf file to apply.")); }
            catch (...) { qWarning() << "[Framegrabber] tool consumer callback threw."; }
            return Admission::success();
        }
        return Admission::failure(ApiErrorCode::InvalidArgument, "Exact typed arguments are required.");
    }

    // Q3: the callable route loads only applets inside the installed catalog. The caller is
    // prompted with that catalog instead of receiving a native-library path permission.
    if (arguments.contains("applet"))
    {
        const std::string requested = arguments.value("applet").toString().toStdString();
        if (catalogApplet(_backend, requested).empty())
        {
            if (options.isEmpty())
                return Admission::failure(ApiErrorCode::PermissionDenied,
                    "Only applets listed by framegrabber.applets can be loaded through the API; refresh the catalog first.");
            try { completion(argumentRequiredFailure("applet", "enum",
                "Only applets listed by framegrabber.applets can be loaded through the API.", options)); }
            catch (...) { qWarning() << "[Framegrabber] tool consumer callback threw."; }
            return Admission::success();
        }
    }
    if (name == QLatin1String("framegrabber.configuration-load") && !arguments.contains("applet"))
    {
        const std::string configuration = arguments.value("configuration").toString().toStdString();
        const QString board = arguments.value("board").toString();
        std::string boardName;
        if (_backend.boards)
        {
            for (const auto& candidate : _backend.boards())
                if (boardMatches(candidate, board)) boardName = candidate.displayName();
        }
        if (_backend.appletPathFromConfiguration)
        {
            const std::string configured = _backend.appletPathFromConfiguration(configuration, boardName);
            // F3: an MCF whose applet is not installed offers only installed applets.
            if (!configured.empty() && catalogApplet(_backend, configured).empty())
            {
                if (options.isEmpty())
                    return Admission::failure(ApiErrorCode::PermissionDenied,
                        "The configuration does not reference an applet listed by framegrabber.applets; refresh the catalog first.");
                try { completion(argumentRequiredFailure("applet", "enum",
                    "The configuration references an applet that is not installed; choose an installed applet.",
                    options)); }
                catch (...) { qWarning() << "[Framegrabber] tool consumer callback threw."; }
                return Admission::success();
            }
        }
    }

    return admitControl(name, std::move(arguments), std::move(progress),
        [completion = std::move(completion)](Result result) noexcept {
            try { completion(std::move(result)); }
            catch (...) { qWarning() << "[Framegrabber] tool consumer callback threw."; }
        });
}

ApiResult<void> FramegrabberSourceController::admitControl(
    const QString& name, QJsonObject arguments, std::function<void(const ApiProgress&)> progress,
    std::function<void(ApiResult<QJsonValue>)> deliver)
{
    Q_ASSERT(QThread::currentThread() == thread());
    using Result = ApiResult<QJsonValue>;
    using Admission = ApiResult<void>;
    if (!_sessionAvailable.load(std::memory_order_acquire))
        return Admission::failure(ApiErrorCode::Unavailable, "The frame grabber session has ended.");
    if (name == "framegrabber.state") { deliver(Result::success(controlState())); return Admission::success(); }
    if (name == "framegrabber.applets") { deliver(Result::success(appletsJson(_backend))); return Admission::success(); }
    if (name == "framegrabber.cameras") { deliver(Result::success(camerasJson(_backend))); return Admission::success(); }
    if (name == "framegrabber.boards") {
        QJsonArray boards;
        const std::string connected = _backend.isOpened && _backend.isOpened()
            ? _backend.connectedBoard() : std::string{};
        if (_backend.boards)
        {
            for (const auto& board : _backend.boards())
                boards.append(QJsonObject{{"boardId", QString::fromStdString(board.displayName())},
                    {"index", static_cast<int>(board.index)}, {"name", QString::fromStdString(board.name)},
                    {"serial", QString::fromStdString(board.serial)}, {"firmware", QString::fromStdString(board.firmware)},
                    {"driver", QString::fromStdString(board.driver)}, {"hardware", QString::fromStdString(board.hardware)},
                    {"connected", !connected.empty() && connected == board.displayName()}});
        }
        deliver(Result::success(QJsonObject{{"boards", boards}}));
        return Admission::success();
    }

    if (workerBusy() || (_backend.controlBusy && _backend.controlBusy()))
        return Admission::failure(ApiErrorCode::Busy, "Another frame grabber control operation is active.");

    const bool connected = _backend.isOpened && _backend.isOpened();
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
        if (_backend.boards)
        {
            for (const auto& candidate : _backend.boards())
                if (boardMatches(candidate, board)) { found = true; boardName = candidate.displayName(); }
        }
        if (!found) return Admission::failure(ApiErrorCode::NotFound, "Exact declared board was not found.");
    }
    if (name == "framegrabber.applets-refresh" && connected)
        return Admission::failure(ApiErrorCode::Conflict, "Release the board before enumerating applets.");
    if (name == "framegrabber.applet-load" || name == "framegrabber.applet-load-file"
        || name == "framegrabber.configuration-load") {
        if (connected && requestedConnection.isEmpty())
            return Admission::failure(ApiErrorCode::Conflict, "Pass the current connectionId to replace the loaded applet.");
        if (connected && requestedConnection != currentConnection)
            return Admission::failure(ApiErrorCode::Conflict, "Connection generation changed.");
        if (connected && boardName != (connected ? _backend.connectedBoard() : std::string{}))
            return Admission::failure(ApiErrorCode::Conflict, "Release the loaded board before selecting another board.");
    }
    if (_backend.isGrabbing && _backend.isGrabbing() && name != kDisconnect)
        return Admission::failure(ApiErrorCode::Busy, "Stop acquisition explicitly first.");
    // The same gate serializes the host device panel; it never implicitly stops acquisition.
    if (!_backend.tryBeginControl || !_backend.tryBeginControl())
        return Admission::failure(ApiErrorCode::Busy, "Another frame grabber control operation is active.");

    Backend* const backend = &_backend;
    _work = QtConcurrent::run([this, backend, name, arguments, boardName, deliver,
                               progress = std::move(progress)] {
        // Results are copied after the gate is released, so they report the settled state.
        enum class Payload { State, Applets, Cameras, None };
        Payload payload = Payload::None;
        std::optional<Result> failure;
        const auto failed = [backend](const QString& fallback) {
            const auto error = backend->lastError ? backend->lastError() : Framegrabber::OperationError{};
            return Result::failure(ApiErrorCode::Unavailable,
                error.message.empty() ? fallback : QString::fromStdString(error.message),
                QJsonObject{{"sdkResult", error.code}});
        };
        const auto report = [&progress](const ApiProgress& value) {
            if (!progress) return;
            try { progress(value); } catch (...) {}
        };
        try {
            if (name == "framegrabber.applets-refresh") {
                if (backend->refreshAppletCatalog && backend->refreshAppletCatalog(boardName)) payload = Payload::Applets;
                else failure = Result::failure(ApiErrorCode::Conflict, "Applet enumeration requires a released board.");
            } else if (name == "framegrabber.applet-load") {
                const QString applet = arguments.value("applet").toString();
                report({loadPhase(name), loadMessage(name, applet), 0, 1, false});
                if (backend->loadApplet && backend->loadApplet(applet.toStdString(), boardName))
                    payload = Payload::State;
                else failure = failed("Failed to load the applet.");
            } else if (name == "framegrabber.applet-load-file") {
                const QString appletFile = arguments.value("appletFile").toString();
                report({loadPhase(name), loadMessage(name, appletFile), 0, 1, false});
                const std::filesystem::path path = nativeFileSystemPath(appletFile);
                std::error_code existsError;
                const bool exists = std::filesystem::is_regular_file(path, existsError) && !existsError;
                QString sdkReason;
                const std::string sdkPath = sdkFilePath(appletFile, sdkReason);
                if (!isAllowedAppletFile(path)) {
                    failure = Result::failure(ApiErrorCode::InvalidArgument,
                        QStringLiteral("Unsupported applet file extension: use .hap, .dll or .so."));
                } else if (!exists) {
                    failure = Result::failure(ApiErrorCode::InvalidArgument,
                        QStringLiteral("The applet file does not exist: %1.").arg(appletFile));
                } else if (!sdkReason.isEmpty()) {
                    failure = Result::failure(ApiErrorCode::InvalidArgument, sdkReason);
                } else if (backend->loadApplet && backend->loadApplet(sdkPath, boardName)) {
                    payload = Payload::State;
                } else failure = failed(QStringLiteral("Failed to load the applet from '%1'.").arg(appletFile));
            } else if (name == "framegrabber.configuration-load") {
                const QString configurationText = arguments.value("configuration").toString();
                QString configReason;
                const std::string configuration = sdkFilePath(configurationText, configReason);
                const std::string applet = arguments.value("applet").toString().toStdString();
                report({loadPhase(name), loadMessage(name, configurationText), 0, 1, false});
                std::error_code configError;
                const bool missing = QFileInfo(configurationText).isAbsolute()
                    && !std::filesystem::is_regular_file(nativeFileSystemPath(configurationText), configError);
                // The MCF names its own applet; apply the same catalog restriction to it.
                const std::string configured = applet.empty() && backend->appletPathFromConfiguration
                    ? backend->appletPathFromConfiguration(configuration, boardName) : applet;
                if (!configReason.isEmpty())
                    failure = Result::failure(ApiErrorCode::InvalidArgument, configReason);
                else if (missing)
                    failure = Result::failure(ApiErrorCode::InvalidArgument,
                        QStringLiteral("The configuration file does not exist: %1.").arg(configurationText));
                else if (catalogApplet(*backend, configured).empty())
                    failure = Result::failure(ApiErrorCode::PermissionDenied,
                        "The configuration does not reference an applet listed by framegrabber.applets; pass one as applet.");
                else if (backend->loadAppletConfiguration
                    && backend->loadAppletConfiguration(applet, configuration, boardName))
                    payload = Payload::State;
                else failure = failed("Failed to apply the configuration.");
            } else if (name == "framegrabber.configuration-save") {
                const QString saveText = arguments.value("configuration").toString();
                QString saveReason;
                const std::string savePath = sdkFilePath(saveText, saveReason);
                if (!saveReason.isEmpty())
                    failure = Result::failure(ApiErrorCode::InvalidArgument, saveReason);
                else if (backend->saveConfiguration && backend->saveConfiguration(savePath))
                    payload = Payload::State;
                else failure = failed("Failed to save the configuration.");
            } else if (name == kDisconnect) {
                if (backend->close) backend->close();
                if (!(backend->isOpened && backend->isOpened())) payload = Payload::State;
                else failure = failed("The frame grabber could not be released.");
            } else if (name == "framegrabber.cameras-refresh") {
                const bool refreshed = backend->refreshCameras
                    && backend->refreshCameras(Framegrabber::CameraTransport::CoaXPress);
                const auto discovery = backend->cameraDiscovery ? backend->cameraDiscovery()
                                                                : Framegrabber::CameraDiscovery{};
                if (refreshed || discovery.topologyMismatch) payload = Payload::Cameras;
                else failure = Result::failure(ApiErrorCode::Unavailable,
                    discovery.message.empty() ? QStringLiteral("CXP camera discovery failed.")
                                              : QString::fromStdString(discovery.message),
                    QJsonObject{{"sdkResult", discovery.sdkResult}});
            } else if (name == kSingle) {
                if (backend->grab) backend->grab(1);
                const auto deadline = std::chrono::steady_clock::now() + kSingleAcquisitionTimeout;
                const auto grabbing = [backend] {
                    return backend->isGrabbing && backend->isGrabbing();
                };
                while (grabbing() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                if (grabbing()) {
                    if (backend->stop) backend->stop();
                    failure = Result::failure(ApiErrorCode::Timeout,
                        "Not every DMA channel delivered a frame within 30 s; acquisition was stopped.");
                } else {
                    const int dmaCount = backend->dmaCount ? backend->dmaCount() : 0;
                    int delivered = 0;
                    if (backend->acquisitionStatistics)
                    {
                        for (const auto& channel : backend->acquisitionStatistics())
                            if (channel.delivered >= 1) ++delivered;
                    }
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
        if (progress)
        {
            if (name == QLatin1String("framegrabber.applet-load"))
                report({loadPhase(name), loadMessage(name, arguments.value("applet").toString()), 1, 1, false});
            else if (name == QLatin1String("framegrabber.applet-load-file"))
                report({loadPhase(name), loadMessage(name, arguments.value("appletFile").toString()), 1, 1, false});
            else if (name == QLatin1String("framegrabber.configuration-load"))
                report({loadPhase(name),
                    loadMessage(name, arguments.value("configuration").toString()), 1, 1, false});
        }
        if (backend->endControl) backend->endControl();
        if (!failure
            && (name == QLatin1String("framegrabber.applet-load")
                || name == QLatin1String("framegrabber.applet-load-file")
                || name == QLatin1String("framegrabber.configuration-load")
                || name == QLatin1String("framegrabber.cameras-refresh"))) {
            if (name == QLatin1String("framegrabber.cameras-refresh")) {
                // A refresh may replace the connected camera or its feature list under the same
                // connection generation; drop both XML and parsed declarations.
                std::lock_guard<std::mutex> lock(_observationMutex);
                _cameraDeclarations.clear();
                _cameraFeatureXml.clear();
            }
            wakeObservation(false);
        }
        if (failure) { deliver(std::move(*failure)); return; }
        switch (payload) {
        case Payload::State: deliver(Result::success(controlState(true))); break;
        case Payload::Applets: deliver(Result::success(appletsJson(*backend))); break;
        case Payload::Cameras: deliver(Result::success(camerasJson(*backend))); break;
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

DeviceConnectionInterfaceInfo interfaceInfo(const std::string& hardware)
{
    DeviceConnectionInterfaceInfo info;
    info.type = hardware.empty() ? std::string(kInterfaceType) : hardware;
    if (info.type.size() > 64) info.type.resize(64);
    info.exclusive = true;
    info.reason = std::string(kInterfaceReason);
    return info;
}

template<class Callback, class Value>
void completeConnection(const Callback& callback, Value value) noexcept
{
    try { callback(std::move(value)); } catch (...) {}
}
}

DeviceConnectionState FramegrabberSourceController::connectionState() const
{
    const bool connected = _sessionAvailable.load(std::memory_order_acquire)
        && _backend.isOpened && _backend.isOpened();
    if (!connected) return {};
    const std::string board = _backend.connectedBoard ? _backend.connectedBoard() : std::string{};
    const std::string applet = _backend.loadedAppletName ? _backend.loadedAppletName() : std::string{};
    DeviceConnectionState state;
    state.connected = true;
    state.live = _backend.isGrabbing && _backend.isGrabbing();
    state.connectionId = connectionId().toStdString();
    state.targetId = board;
    state.displayName = applet.empty() ? board : board + " / " + applet;
    state.kind = DeviceConnectionTargetKind::Interface;
    std::uint64_t delivered = 0;
    if (_backend.acquisitionStatistics)
        for (const auto& channel : _backend.acquisitionStatistics()) delivered += channel.delivered;
    state.deliveredFrames = delivered;
    std::string hardware;
    if (_backend.boards)
        for (const auto& candidate : _backend.boards())
            if (candidate.displayName() == board) hardware = candidate.hardware;
    state.interfaceInfo = interfaceInfo(hardware);
    for (const auto& source : sourceDescriptors())
    {
        if (source.id.empty()) continue;
        state.sources.push_back({source.id, source.index});
    }
    return state;
}

DeviceConnectionCapabilities FramegrabberSourceController::connectionCapabilities() const
{
    if (!_sessionAvailable.load(std::memory_order_acquire)) {
        const DeviceConnectionAvailability ended{false, "The frame grabber session has ended."};
        DeviceConnectionCapabilities capabilities{ended, ended, ended, ended, ended, ended};
        capabilities.connectionModel = DeviceConnectionModel::Hierarchical;
        return capabilities;
    }
    const DeviceConnectionAvailability yes{true, {}};
    DeviceConnectionCapabilities capabilities{
        yes,
        {false, "Board discovery runs when the session is created; observe the cached device.targets."},
        yes, yes, yes,
        {false, "Live acquisition uses source.start and source.stop."}};
    // One board connection owns its DMA-channel sources.
    capabilities.connectionModel = DeviceConnectionModel::Hierarchical;
    return capabilities;
}

DeviceConnectionTargets FramegrabberSourceController::connectionTargets() const
{
    DeviceConnectionTargets targets;
    if (!_sessionAvailable.load(std::memory_order_acquire)) {
        targets.result = {DeviceConnectionError::Unavailable, "The frame grabber session has ended."};
        return targets;
    }
    if (_backend.boards)
    {
        for (const auto& board : _backend.boards())
        {
            DeviceConnectionTarget target;
            target.targetId = board.displayName();
            target.displayName = board.displayName();
            target.kind = DeviceConnectionTargetKind::Interface;
            target.interfaceInfo = interfaceInfo(board.hardware);
            targets.targets.push_back(std::move(target));
        }
    }
    return targets;
}

DeviceConnectionResult FramegrabberSourceController::requestConnectionTargets(TargetsCompletion completion)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!completion) return {DeviceConnectionError::InvalidArgument, "Completion is required."};
    completeConnection(completion, connectionTargets());
    return {};
}

DeviceConnectionResult FramegrabberSourceController::requestConnect(std::string targetId,
                                                                    IDeviceConnectionControl::Completion completion)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!completion) return {DeviceConnectionError::InvalidArgument, "Completion is required."};
    if (!_sessionAvailable.load(std::memory_order_acquire))
        return {DeviceConnectionError::Unavailable, "The frame grabber session has ended."};
    if (targetId.empty() || targetId.find('\0') != std::string::npos)
        return {DeviceConnectionError::InvalidArgument, "An exact board targetId is required."};
    if (_backend.isOpened && _backend.isOpened())
        return {DeviceConnectionError::Conflict, "Disconnect before connecting another board."};
    if ((_backend.isGrabbing && _backend.isGrabbing()) || workerBusy()
        || (_backend.controlBusy && _backend.controlBusy()))
        return {DeviceConnectionError::Busy, "Another frame grabber control operation is active."};
    const QString reference = QString::fromStdString(targetId);
    std::string boardName;
    if (_backend.boards)
    {
        for (const auto& board : _backend.boards())
            if (boardMatches(board, reference)) boardName = board.displayName();
    }
    if (boardName.empty())
        return {DeviceConnectionError::NotFound, "Exact declared board was not found."};
    if (!_backend.tryBeginControl || !_backend.tryBeginControl())
        return {DeviceConnectionError::Busy, "Another frame grabber control operation is active."};

    Backend* const backend = &_backend;
    _work = QtConcurrent::run([this, backend, boardName, completion = std::move(completion)] {
        DeviceConnectionResult result;
        try {
            if (backend->refreshAppletCatalog) backend->refreshAppletCatalog(boardName);
            std::string applet;
            if (backend->appletCatalog)
            {
                std::string powerUp;
                for (const auto& candidate : backend->appletCatalog())
                {
                    if (candidate.active) { applet = candidate.path; break; }
                    if (powerUp.empty() && candidate.powerUp) powerUp = candidate.path;
                }
                if (applet.empty()) applet = powerUp;
            }
            if (applet.empty()) {
                result = {DeviceConnectionError::NotFound,
                    "The board reports no active or power-up applet to load.",
                    DeviceConnectionEffect::None, connectionState()};
            } else if (backend->loadApplet && backend->loadApplet(applet, boardName)) {
                result = {{}, {}, DeviceConnectionEffect::Applied, connectionState()};
            } else {
                const auto error = backend->lastError ? backend->lastError()
                                                      : Framegrabber::OperationError{};
                result = {DeviceConnectionError::BackendFailure,
                    error.message.empty() ? "The board's opening applet could not be loaded."
                                          : error.message,
                    DeviceConnectionEffect::None, connectionState()};
            }
        } catch (const std::exception& error) {
            result = {DeviceConnectionError::BackendFailure, error.what(),
                DeviceConnectionEffect::Unknown, std::nullopt};
        } catch (...) {
            result = {DeviceConnectionError::BackendFailure,
                "Unexpected frame grabber opening failure.", DeviceConnectionEffect::Unknown, std::nullopt};
        }
        if (backend->endControl) backend->endControl();
        wakeObservation(true);
        completeConnection(completion, std::move(result));
    });
    return {};
}

DeviceConnectionResult FramegrabberSourceController::requestConnectionState(IDeviceConnectionControl::Completion completion)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!completion) return {DeviceConnectionError::InvalidArgument, "Completion is required."};
    if (!_sessionAvailable.load(std::memory_order_acquire))
        return {DeviceConnectionError::Unavailable, "The frame grabber session has ended."};
    completeConnection(completion, DeviceConnectionResult{{}, {}, {}, connectionState()});
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
    if (!_sessionAvailable.load(std::memory_order_acquire))
        return {DeviceConnectionError::Unavailable, "The frame grabber session has ended."};
    if (_backend.isOpened && !_backend.isOpened())
        return {DeviceConnectionError::Disconnected, "No applet is loaded."};
    // The shared control path keeps one gate and one worker for GUI, module tools and lifecycle calls.
    // The controller drains its worker before destruction, so the completion may read it.
    const auto admitted = admitControl(name, QJsonObject{{"connectionId", QString::fromStdString(connectionId)}},
        {}, [this, completion = std::move(completion)](ApiResult<QJsonValue> result) noexcept {
            DeviceConnectionResult copied;
            if (result.succeeded()) {
                copied = {{}, {}, DeviceConnectionEffect::Applied, connectionState()};
            } else {
                // Only accepted SDK work fails here, so its effect on the device is uncertain.
                copied = {lifecycleError(result.error()->code), result.error()->message.toStdString(),
                          DeviceConnectionEffect::Unknown};
            }
            wakeObservation(false);
            try { completion(std::move(copied)); }
            catch (...) { qWarning() << "[Framegrabber] lifecycle consumer callback threw."; }
        });
    if (admitted.succeeded()) return {};
    return {lifecycleError(admitted.error()->code), admitted.error()->message.toStdString()};
}

void FramegrabberSourceController::startObservationThread()
{
    std::lock_guard<std::mutex> lock(_observerMutex);
    if (_observerThread.joinable()) return;
    _observerStop = false;
    _observerThread = std::thread([this] { observationLoop(); });
}

void FramegrabberSourceController::stopObservationThread()
{
    {
        std::lock_guard<std::mutex> lock(_observerMutex);
        _observerStop = true;
    }
    _observerWake.notify_all();
    if (_observerThread.joinable()) _observerThread.join();
}

void FramegrabberSourceController::wakeObservation(const bool snapshot)
{
    std::lock_guard<std::mutex> lock(_observerMutex);
    if (_observerStop) return;
    _observerPending = true;
    _observerSnapshot = _observerSnapshot || snapshot;
    _observerWake.notify_one();
}

void FramegrabberSourceController::observationLoop()
{
    for (;;)
    {
        bool snapshot = false;
        {
            std::unique_lock<std::mutex> lock(_observerMutex);
            _observerWake.wait(lock, [this] { return _observerStop || _observerPending; });
            if (_observerStop) return;
            _observerPending = false;
            snapshot = _observerSnapshot;
            _observerSnapshot = false;
        }
        if (!_notifications.active()) continue;

        // Observation has its own serialization. It waits for an accepted control operation
        // to settle but never holds the shared gate, so a lifecycle or parameter admission is
        // not refused as busy just because a subscription is refreshing the tree.
        bool ready = true;
        for (;;)
        {
            {
                std::lock_guard<std::mutex> lock(_observerMutex);
                if (_observerStop) return;
            }
            if (!_sessionAvailable.load(std::memory_order_acquire)) { ready = false; break; }
            if (!_backend.controlBusy || !_backend.controlBusy()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!ready) continue;

        std::lock_guard<std::mutex> cycle(_observationCycleMutex);
        const std::string before = connectionId().toStdString();
        try { observeAndPublish(snapshot); }
        catch (...) { qWarning() << "[Framegrabber] parameter observation failed."; }
        _notifications.drain();
        // A lifecycle command may have changed the generation during the pass; observe again
        // from the new generation instead of publishing a mixed tree.
        if (connectionId().toStdString() != before) wakeObservation(true);
    }
}

void FramegrabberSourceController::observeAndPublish(const bool snapshot)
{
    if (!_notifications.active()) return;
    const bool opened = _sessionAvailable.load(std::memory_order_acquire)
        && _backend.isOpened && _backend.isOpened();
    std::string previous;
    {
        std::lock_guard<std::mutex> lock(_observationMutex);
        previous = _notificationConnectionId;
    }
    if (!opened)
    {
        if (previous.empty()) return;
        _notifications.publishTerminal(previous, DeviceParameterError::Disconnected,
            "The frame grabber connection closed.");
        std::lock_guard<std::mutex> lock(_observationMutex);
        _notificationConnectionId.clear();
        _observations.clear();
        _publishedObservations.clear();
        _cameraDeclarations.clear();
        _cameraFeatureXml.clear();
        return;
    }
    const std::string current = connectionId().toStdString();
    if (current.empty()) return;
    bool snapshotRequired = snapshot;
    if (previous.empty())
    {
        // Adopt the current generation without discarding the revisions a caller already
        // observed through the request path; the first subscription still gets a snapshot.
        std::lock_guard<std::mutex> lock(_observationMutex);
        _notificationConnectionId = current;
        snapshotRequired = true;
    }
    else if (previous != current)
    {
        _notifications.publishTerminal(previous, DeviceParameterError::Disconnected,
            "The frame grabber connection changed.");
        std::lock_guard<std::mutex> lock(_observationMutex);
        _notificationConnectionId = current;
        _observations.clear();
        _publishedObservations.clear();
        _cameraDeclarations.clear();
        _cameraFeatureXml.clear();
        snapshotRequired = true;
    }
    const Observation observed = observeAll(current, true);
    if (!observed.error.empty())
    {
        _notifications.publishTerminal(current, DeviceParameterError::BackendFailure, observed.error);
        return;
    }
    publishObservation(current, snapshotRequired, observed);
}

std::optional<SourceRuntimeDiagnostics> FramegrabberSourceController::sourceRuntimeDiagnostics(unsigned sourceIndex) const
{
    if (!_sessionAvailable.load(std::memory_order_acquire) || !_backend.runtimeDiagnostics)
        return std::nullopt;
    if (sourceIndex >= static_cast<unsigned>(_backend.dmaCount ? _backend.dmaCount() : 0))
        return std::nullopt;
    const auto value = _backend.runtimeDiagnostics(sourceIndex);
    return SourceRuntimeDiagnostics{value.grabbing, value.delivered, value.failed};
}
