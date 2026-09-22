#pragma once

#include "AbstractSourceController.h"
#include "Framegrabber.h"

#include <atomic>
#include <memory>

class FramegrabberGraphicsFrameStream;

class FramegrabberSourceController final : public AbstractSourceController
{
    Q_OBJECT

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

private:
    void registerCallbacks();
    void deregisterCallbacks();

    Framegrabber* _framegrabber = nullptr;
    FrameConsumer _frameConsumer;
    std::atomic<bool> _isGrabbing{false};

    Framegrabber::CallbackId _statusCallbackId = 0;
    std::unique_ptr<FramegrabberGraphicsFrameStream> _graphicsStream;
};
