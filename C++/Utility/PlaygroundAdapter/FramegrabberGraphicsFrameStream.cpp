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
            GraphicsFrameBuilder builder;
            GraphicsImageFailure failure = GraphicsImageFailure::None;
            GraphicsImage graphicsImage =
                FramegrabberGraphicsFrameAdapter::wrapImage(builder, image, sequence, &failure);
            if (failure != GraphicsImageFailure::None) {
                if (rejectedImage) rejectedImage(graphicsImage, image.dmaIndex, failure);
                throw std::runtime_error("Invalid GraphicsImage conversion.");
            }

            builder.addImage("image", std::move(graphicsImage), "Image");
            GraphicsFrameMetadata metadata;
            metadata.sourceName = "Framegrabber";
            metadata.frameIndex = sequence;
            builder.setMetadata(std::move(metadata));
            auto published = std::move(builder).publish();
            if (!published) {
                if (rejectedImage) {
                    GraphicsImage rejected;
                    rejected.width = image.width;
                    rejected.height = image.height;
                    rejected.stride = image.stride;
                    rejected.sourceFormat = image.fgFormat;
                    rejected.frameSequence = sequence;
                    const GraphicsImageFailure publishFailure =
                        (published.error() == GraphicsFrameError::InvalidInput
                         || published.error() == GraphicsFrameError::UnsealedWrite
                         || published.error() == GraphicsFrameError::None)
                        ? GraphicsImageFailure::InvalidDimensions
                        : GraphicsImageFailure::StorageRefused;
                    rejectedImage(rejected, image.dmaIndex, publishFailure);
                }
                throw std::runtime_error("Invalid GraphicsFrame publish.");
            }
            _callback(std::move(published).value(), image.dmaIndex);
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
