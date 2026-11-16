#pragma once

#include "AbstractSourceController.h"
#include "DeviceConnectionControl.h"
#include "DeviceParameterControl.h"
#include "FrameStorage.h"
#include "Framegrabber.h"
#include "FramegrabberParameterNotifications.h"
#include "ToolProvider.h"

#include <QFuture>
#include <QJsonObject>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class FramegrabberGraphicsFrameStream;
struct FramegrabberControllerTestAccess;

/** @brief One flattened camera feature declaration copied from the SDK feature XML. */
struct FramegrabberCameraFeatureDeclaration {
    std::string name;
    std::string tag;
    std::string displayName;
    std::string description;
    std::string unit;
    std::string accessMode;
    DeviceParameterVisibility visibility = DeviceParameterVisibility::Primary;
    std::string lockedFeature;
    std::string availableFeature;
    std::vector<std::string> categoryPath;
    std::vector<std::string> enumEntries;
};

/**
 * @brief Session source controller and the callable contract of the Framegrabber plugin.
 *
 * The standard connection lifecycle (`IDeviceConnectionControl`, declared by the host as
 * `device.*`), the board/applet `framegrabber.*` actions and the applet/camera `parameters.*`
 * capability call the same Framegrabber authority as the Qt control panel. One board
 * connection owns the DMA-channel sources; a connect loads the board's active/power-up
 * applet, so the standard target catalog lists boards and `requestConnect` runs that opening
 * action. Commands share the module's control gate with the panel; at most one accepted
 * request runs on the worker at a time. Admission runs on the controller thread; completions
 * run on the worker, exactly once, before destruction completes.
 */
class FramegrabberSourceController final
    : public AbstractSourceController,
      public IToolProvider,
      public IToolExecution,
      public IDeviceConnectionControl,
      public IDeviceParameterControl,
      public IFrameStorageClient
{
    Q_OBJECT
    Q_INTERFACES(IToolProvider IToolExecution IDeviceConnectionControl IDeviceParameterControl IFrameStorageClient)

public:
    void setFrameStorageProvider(std::shared_ptr<IFrameStorageProvider> provider) override;
    using TargetsCompletion = IDeviceConnectionControl::TargetsCompletion;
    using ChangeListener = IDeviceParameterControl::ChangeListener;
    explicit FramegrabberSourceController(Framegrabber* framegrabber,
                                           QObject* parent = nullptr);
    ~FramegrabberSourceController() override;

    /** @brief Copies producer diagnostics for the selected DMA channel. */
    std::optional<SourceRuntimeDiagnostics> sourceRuntimeDiagnostics(unsigned sourceIndex) const override;
    void start() override;
    void stop() override;
    bool isGrabbing() const override;
    void setFrameConsumer(FrameConsumer consumer) override;
    [[nodiscard]] std::vector<GraphicsSourceDescriptor> sourceDescriptors() const override;
    bool supports3D() const override { return false; }

    /** @brief Declares the board, applet, configuration and CXP camera actions (`framegrabber.*`). */
    QList<ToolDescriptor> toolDescriptors() const override;
    ApiResult<void> requestTool(const QString& name, int version, QJsonObject arguments,
                                IToolProvider::Completion completion) override;
    /** @brief Admits one action with bounded progress; the SDK offers no cancellation. */
    ApiResult<void> requestToolWithProgress(const QString& callId, const QString& name, int version,
                                            QJsonObject arguments, Progress progress,
                                            IToolProvider::Completion completion) override;
    /** @brief Answers `Unsupported` while an accepted call cannot be stopped cooperatively. */
    ApiResult<void> cancelTool(const QString& callId) override;

    /** @brief Declares a hierarchical session: one exclusive board connection owns its DMA sources. */
    DeviceConnectionCapabilities connectionCapabilities() const override;
    /** @brief Copies the cached board catalog as exact connection targets. */
    DeviceConnectionResult requestConnectionTargets(TargetsCompletion completion) override;
    /** @brief Copies the loaded board/applet connection with its DMA sources. */
    DeviceConnectionResult requestConnectionState(IDeviceConnectionControl::Completion completion) override;
    /** @brief Loads the board's active/power-up applet as the opening action. */
    DeviceConnectionResult requestConnect(std::string targetId, IDeviceConnectionControl::Completion completion) override;
    /** @brief Stops acquisition and releases the exact connection (Fg_FreeGrabber). */
    DeviceConnectionResult requestDisconnect(std::string connectionId, IDeviceConnectionControl::Completion completion) override;
    /** @brief Acquires one frame on every DMA channel and waits up to 30 s. */
    DeviceConnectionResult requestSingle(std::string connectionId, IDeviceConnectionControl::Completion completion) override;

    DeviceParameterResult requestParameters(InventoryCompletion completion) override;
    DeviceParameterResult requestReadParameter(DeviceParameterTarget target,
                                               IDeviceParameterControl::Completion completion) override;
    DeviceParameterResult requestWriteParameter(DeviceParameterTarget target,
        DeviceParameterValue value, std::optional<std::uint64_t> expectedRevision,
        IDeviceParameterControl::Completion completion) override;
    DeviceParameterResult requestExecuteParameter(DeviceParameterTarget target,
        std::optional<std::uint64_t> expectedRevision,
        IDeviceParameterControl::Completion completion) override;
    /** @brief Starts one module-owned observation subscription and its initial snapshot. */
    DeviceParameterResult requestParameterChanges(std::string connectionId,
        std::string subscriptionId, ChangeListener delivery) override;
    /** @brief Stops delivery for one subscription and completes after the module drained it. */
    DeviceParameterResult releaseParameterChanges(std::string connectionId,
        std::string subscriptionId, IDeviceParameterControl::Completion completion) override;

private:
    friend struct FramegrabberControllerTestAccess;
    friend struct FramegrabberTestBackend;

    /** @brief Copies the SDK and widget-facing module surface used by this adapter. */
    struct Backend {
        std::function<std::vector<FramegrabberSystem::BoardInfo>()> boards;
        std::function<bool()> isOpened;
        std::function<std::string()> connectedBoard;
        std::function<std::string()> loadedAppletName;
        std::function<std::string()> loadedAppletVersion;
        std::function<std::string()> appletPath;
        std::function<std::string()> configurationPath;
        std::function<std::uint64_t()> connectionGeneration;
        std::function<bool(const std::string&)> refreshAppletCatalog;
        std::function<std::vector<Framegrabber::AppletInfo>()> appletCatalog;
        std::function<std::string(const std::string&)> resolveApplet;
        std::function<bool(const std::string&, const std::string&)> loadApplet;
        std::function<std::string(const std::string&, const std::string&)> appletPathFromConfiguration;
        std::function<bool(const std::string&, const std::string&, const std::string&)> loadAppletConfiguration;
        std::function<bool(const std::string&)> saveConfiguration;
        std::function<void()> close;
        std::function<bool()> isGrabbing;
        std::function<int()> dmaCount;
        std::function<bool()> tryBeginControl;
        std::function<void()> endControl;
        std::function<bool()> controlBusy;
        std::function<Framegrabber::OperationError()> lastError;
        std::function<std::vector<Framegrabber::ChannelStatistics>()> acquisitionStatistics;
        std::function<Framegrabber::CameraDiscovery()> cameraDiscovery;
        std::function<bool(Framegrabber::CameraTransport)> refreshCameras;
        std::function<std::vector<Framegrabber::CameraInfo>(Framegrabber::CameraTransport)> cachedCameras;
        std::function<Framegrabber::RuntimeDiagnostics(std::optional<unsigned>)> runtimeDiagnostics;
        std::function<void(std::size_t)> grab;
        std::function<void()> stop;
        std::function<std::vector<Framegrabber::AppletFeatureNode>(unsigned)> appletFeatureModel;
        std::function<Framegrabber::AppletParameterLimits(std::int64_t, unsigned)> appletParameterLimits;
        std::function<bool(std::int64_t, unsigned, Framegrabber::ParameterValue&, bool)> getAppletParameter;
        std::function<std::optional<Framegrabber::ParameterValue>(std::int64_t, unsigned)> appletParameterPrototype;
        std::function<bool(std::int64_t, unsigned, const Framegrabber::ParameterValue&, bool, bool, int*)> setAppletParameter;
        std::function<bool(unsigned, std::int64_t, int*)> executeAppletCommand;
        std::function<std::vector<Framegrabber::CameraControlCapability>()> cameraCapabilities;
        std::function<std::string(Framegrabber::CameraTransport, unsigned)> cameraFeatureXml;
        std::function<bool(Framegrabber::CameraTransport, unsigned, const std::string&, Framegrabber::ParameterValue&)> getCameraFeature;
        std::function<Framegrabber::AppletParameterLimits(Framegrabber::CameraTransport, unsigned, const std::string&, bool)> cameraFeatureLimits;
        std::function<bool(Framegrabber::CameraTransport, unsigned, const std::string&, const Framegrabber::ParameterValue&, bool)> setCameraFeature;
        std::function<bool(Framegrabber::CameraTransport, unsigned, const std::string&)> executeCameraCommand;
    };

    /** @brief One observation pass: replaced descriptors and targets that disappeared. */
    struct Observation {
        std::vector<DeviceParameterDescriptor> changed;
        std::vector<DeviceParameterTarget> invalidated;
        std::vector<DeviceParameterDescriptor> descriptors;
        std::string error;
    };

    void bindBackend();
    void registerCallbacks();
    void deregisterCallbacks();
    [[nodiscard]] bool workerBusy() const;
    [[nodiscard]] QString connectionId() const;
    /** @param fromWorker Excludes the calling worker job from the reported busy state. */
    [[nodiscard]] QJsonObject controlState(bool fromWorker = false) const;
    /** @brief Copies the canonical connection state from the loaded board/applet. */
    [[nodiscard]] DeviceConnectionState connectionState() const;
    /** @brief Copies the cached board catalog as targets without SDK waits. */
    [[nodiscard]] DeviceConnectionTargets connectionTargets() const;
    /** @brief Admits one module or lifecycle control through the shared gate and worker. */
    ApiResult<void> admitControl(const QString& name, QJsonObject arguments,
                                 std::function<void(const ApiProgress&)> progress,
                                 std::function<void(ApiResult<QJsonValue>)> deliver);
    DeviceConnectionResult admitLifecycle(const QString& name, std::string connectionId,
                                          IDeviceConnectionControl::Completion completion);
    ApiResult<void> requestToolInternal(const QString& name, int version, QJsonObject arguments,
                                        std::function<void(const ApiProgress&)> progress,
                                        IToolProvider::Completion completion);

    DeviceParameterResult requestParameter(DeviceParameterTarget target,
        std::optional<DeviceParameterValue> value, std::optional<std::uint64_t> revision,
        bool mutate, IDeviceParameterControl::Completion completion);
    DeviceParameterDescriptor observeParameter(const Framegrabber::AppletFeatureNode& node,
                                               unsigned int dmaIndex,
                                               const std::string& connection,
                                               const std::vector<std::string>& categoryPath);
    /** @brief Observes one typed camera feature from its XML declaration. */
    DeviceParameterDescriptor observeCameraParameter(
        const FramegrabberCameraFeatureDeclaration& declaration,
        Framegrabber::CameraTransport transport, unsigned int dmaIndex,
        const std::string& connection);
    /** @brief Observes every applet and camera parameter for one connection. */
    Observation observeAll(const std::string& connection, bool purge);
    /** @brief Copies the cached camera declarations, parsing the XML once per generation. */
    std::vector<FramegrabberCameraFeatureDeclaration> cameraDeclarations(
        Framegrabber::CameraTransport transport, unsigned int dmaIndex,
        const std::string& connection);
    /** @brief Copies one complete snapshot from the current observations. */
    DeviceParameterChange snapshotChange(const std::string& connection,
                                         const std::string& subscriptionId) const;
    /** @brief Returns whether any parameter of one connection has been observed. */
    [[nodiscard]] bool hasObservations(const std::string& connection) const;

    /** @brief Requests one observation pass; a snapshot replaces pending deltas. */
    void wakeObservation(bool snapshot);
    void startObservationThread();
    /** @brief Stops and joins the observation thread; safe to call more than once. */
    void stopObservationThread();
    void observationLoop();
    void observeAndPublish(bool snapshot);
    /** @brief Publishes differences or a snapshot for the current connection. */
    void publishObservation(const std::string& connection, bool snapshot,
                            const Observation& observation);

    Framegrabber* _framegrabber = nullptr;
    Backend _backend;
    std::atomic<bool> _sessionAvailable{false};
    FrameConsumer _frameConsumer;
    std::atomic<bool> _isGrabbing{false};
    const QString _instanceId;

    Framegrabber::CallbackId _statusCallbackId = 0;
    Framegrabber::CallbackId _nodeCallbackId = 0;
    std::unique_ptr<FramegrabberGraphicsFrameStream> _graphicsStream;

    QFuture<void> _work;
    mutable std::mutex _observationMutex;
    // Revisions change only when an observed descriptor differs.
    std::map<std::string, DeviceParameterDescriptor> _observations;
    /** @brief Descriptors as last delivered to subscriptions; never the request-path cache. */
    std::map<std::string, DeviceParameterDescriptor> _publishedObservations;
    /** @brief Parsed camera feature XML per connection generation and DMA channel. */
    std::map<std::string, std::vector<FramegrabberCameraFeatureDeclaration>> _cameraDeclarations;
    std::atomic<std::uint64_t> _revision{0};
    /** @brief Serializes observation passes independently of the shared control gate. */
    std::mutex _observationCycleMutex;
    /** @brief Connection whose descriptors are published; empty while disconnected. */
    std::string _notificationConnectionId;

    framegrabber::ParameterNotificationHub _notifications;
    std::thread _observerThread;
    std::mutex _observerMutex;
    std::condition_variable _observerWake;
    bool _observerStop = false;
    bool _observerPending = false;
    bool _observerSnapshot = false;

    std::mutex _activeCallMutex;
    QString _activeCallId;
};
