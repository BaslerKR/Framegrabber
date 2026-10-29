#pragma once

#include "AbstractSourceController.h"
#include "DeviceParameterControl.h"
#include "Framegrabber.h"
#include "ToolProvider.h"

#include <QFuture>
#include <QJsonObject>
#include <QString>

#include <atomic>
#include <map>
#include <memory>
#include <string>

class FramegrabberGraphicsFrameStream;

/**
 * @brief Session source controller and the callable contract of the Framegrabber plugin.
 *
 * The declared `device.*` tools and the applet `parameters.*` capability call the same
 * Framegrabber authority as the Qt control panel. Lifecycle commands share the module's
 * control gate with the panel; at most one accepted request runs on the worker at a time.
 * Admission runs on the controller thread; completions run on the worker, exactly once,
 * before destruction completes.
 */
class FramegrabberSourceController final
    : public AbstractSourceController,
      public IToolProvider,
      public IDeviceParameterControl
{
    Q_OBJECT
    Q_INTERFACES(IToolProvider IDeviceParameterControl)

public:
    explicit FramegrabberSourceController(Framegrabber* framegrabber,
                                           QObject* parent = nullptr);
    ~FramegrabberSourceController() override;

    void start() override;
    void stop() override;
    bool isGrabbing() const override;
    void setFrameConsumer(FrameConsumer consumer) override;
    [[nodiscard]] std::vector<GraphicsSourceDescriptor> sourceDescriptors() const override;
    bool supports3D() const override { return false; }

    QList<ToolDescriptor> toolDescriptors() const override;
    ApiResult<void> requestTool(const QString& name, int version, QJsonObject arguments,
                                IToolProvider::Completion completion) override;

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
