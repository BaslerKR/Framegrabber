#pragma once

#include "AbstractSourceController.h"
#include "DeviceConnectionControl.h"
#include "DeviceParameterControl.h"
#include "FrameStorage.h"
#include "Framegrabber.h"
#include "ToolProvider.h"

#include <QFuture>
#include <QJsonObject>
#include <QString>

#include <atomic>
#include <map>
#include <memory>
#include <functional>
#include <string>

class FramegrabberGraphicsFrameStream;

/**
 * @brief Session source controller and the callable contract of the Framegrabber plugin.
 *
 * The standard connection lifecycle (`IDeviceConnectionControl`, declared by the host as
 * `device.*`), the board/applet `framegrabber.*` tools and the applet `parameters.*`
 * capability call the same Framegrabber authority as the Qt control panel. A connection
 * is made by loading an applet, so targets and connect are declared unavailable. Commands share the module's
 * control gate with the panel; at most one accepted request runs on the worker at a time.
 * Admission runs on the controller thread; completions run on the worker, exactly once,
 * before destruction completes.
 */
class FramegrabberSourceController final
    : public AbstractSourceController,
      public IToolProvider,
      public IDeviceConnectionControl,
      public IDeviceParameterControl,
      public IFrameStorageClient
{
    Q_OBJECT
    Q_INTERFACES(IToolProvider IDeviceConnectionControl IDeviceParameterControl IFrameStorageClient)

public:
    void setFrameStorageProvider(std::shared_ptr<IFrameStorageProvider> provider) override;
    using TargetsCompletion = IDeviceConnectionControl::TargetsCompletion;
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

    /** @brief Declares the board, applet, configuration and CXP camera tools (`framegrabber.*`). */
    QList<ToolDescriptor> toolDescriptors() const override;
    ApiResult<void> requestTool(const QString& name, int version, QJsonObject arguments,
                                IToolProvider::Completion completion) override;

    /** @brief Declares disconnect and Single; targets, connect and Live are unavailable with a reason. */
    DeviceConnectionCapabilities connectionCapabilities() const override;
    DeviceConnectionResult requestConnectionTargets(TargetsCompletion completion) override;
    /** @brief Copies the loaded board/applet connection; details stay in `framegrabber.state`. */
    DeviceConnectionResult requestConnectionState(IDeviceConnectionControl::Completion completion) override;
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

private:
    void registerCallbacks();
    void deregisterCallbacks();
    [[nodiscard]] bool workerBusy() const;
    [[nodiscard]] QString connectionId() const;
    /** @param fromWorker Excludes the calling worker job from the reported busy state. */
    [[nodiscard]] QJsonObject controlState(bool fromWorker = false) const;
    /** @brief Copies the canonical connection state from the loaded board/applet. */
    [[nodiscard]] DeviceConnectionState connectionState() const;
    /** @brief Admits one module or lifecycle control through the shared gate and worker. */
    ApiResult<void> admitControl(const QString& name, QJsonObject arguments,
                                 std::function<void(ApiResult<QJsonValue>)> deliver);
    DeviceConnectionResult admitLifecycle(const QString& name, std::string connectionId,
                                          IDeviceConnectionControl::Completion completion);

    DeviceParameterResult requestParameter(DeviceParameterTarget target,
        std::optional<DeviceParameterValue> value, std::optional<std::uint64_t> revision,
        bool mutate, IDeviceParameterControl::Completion completion);
    DeviceParameterDescriptor observeParameter(const Framegrabber::AppletFeatureNode& node,
                                               unsigned int dmaIndex,
                                               const std::string& connection);

    Framegrabber* _framegrabber = nullptr;
    FrameConsumer _frameConsumer;
    std::atomic<bool> _isGrabbing{false};
    const QString _instanceId;

    Framegrabber::CallbackId _statusCallbackId = 0;
    std::unique_ptr<FramegrabberGraphicsFrameStream> _graphicsStream;

    QFuture<void> _work;
    // Revisions change only when an observed descriptor differs; owned by the worker.
    std::map<std::string, DeviceParameterDescriptor> _observations;
    std::uint64_t _revision = 0;
};
