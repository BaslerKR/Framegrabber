#include "FramegrabberGraphicsFrameStream.h"

#include "FramegrabberGraphicsFrameAdapter.h"

#include <stdexcept>
#include <memory>
#include <utility>

class FramegrabberGraphicsFrameStream::Impl final
{
public:
    Impl(Framegrabber* framegrabber, GraphicsFrameCallback callback, RejectedImage rejectedImage);
    ~Impl();

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

private:
    Framegrabber* _framegrabber = nullptr;
    GraphicsFrameCallback _callback;
    GraphicsFrameCallbackGate _callbackGate;
    Framegrabber::CallbackId _grabCallbackId = 0;
};

FramegrabberGraphicsFrameStream::Impl::Impl(
    Framegrabber* framegrabber,
    GraphicsFrameCallback callback,
    RejectedImage rejectedImage)
    : _framegrabber(framegrabber), _callback(std::move(callback))
{
    if (!_framegrabber || !_callback)
    {
        return;
    }

    const auto callbackToken = _callbackGate.token();
    _grabCallbackId = _framegrabber->registerGrabCallback(
        [this, callbackToken, rejectedImage = std::move(rejectedImage)](
            const Framegrabber::Image& image, const std::size_t sequence) {
            GraphicsFrameCallbackGate::Lease lease(callbackToken);
            if (!lease) return;
            // The DMA callback boundary counts and logs any conversion/consumer failure once.
            GraphicsImage graphicsImage = FramegrabberGraphicsFrameAdapter::wrapImage(image, sequence);
            const auto failure = FramegrabberGraphicsFrameAdapter::imageFailure(graphicsImage);
            if (failure != GraphicsImageFailure::None) {
                if (rejectedImage) rejectedImage(graphicsImage, image.dmaIndex, failure);
                throw std::runtime_error("Invalid GraphicsImage conversion.");
            }

            GraphicsFrame frame;
            frame.addImage("image", std::move(graphicsImage), "Image");
            frame.metadata.sourceName = "Framegrabber";
            frame.metadata.frameIndex = sequence;
            _callback(std::move(frame), image.dmaIndex);
        });
}

FramegrabberGraphicsFrameStream::Impl::~Impl()
{
    _callbackGate.beginShutdown();
    if (_framegrabber && _grabCallbackId != 0U)
    {
        _framegrabber->deregisterGrabCallback(_grabCallbackId);
    }
    _callbackGate.waitForDrain();
}

FramegrabberGraphicsFrameStream::FramegrabberGraphicsFrameStream(
    Framegrabber* framegrabber,
    GraphicsFrameCallback callback,
    RejectedImage rejectedImage)
    : _impl(std::make_unique<Impl>(framegrabber, std::move(callback), std::move(rejectedImage)))
{
}

FramegrabberGraphicsFrameStream::~FramegrabberGraphicsFrameStream() = default;
