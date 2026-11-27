#include "FramegrabberGraphicsFrameStream.h"

#include "FramegrabberGraphicsFrameAdapter.h"

#include <stdexcept>
#include <memory>
#include <utility>

class FramegrabberGraphicsFrameStream::Impl final
{
public:
    Impl(Framegrabber* framegrabber,
         std::shared_ptr<GraphicsFrameContextSource> contextSource,
         GraphicsFrameCallback callback,
         RejectedImage rejectedImage,
         StorageOutcome storageOutcome,
         std::shared_ptr<const std::atomic<bool>> cancelled);
    ~Impl();

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void cancel() noexcept
    {
        _localCancelled.store(true, std::memory_order_release);
    }

private:
    [[nodiscard]] bool isCancelled() const noexcept
    {
        return _localCancelled.load(std::memory_order_acquire)
            || (_externalCancelled && _externalCancelled->load(std::memory_order_acquire));
    }

    Framegrabber* _framegrabber = nullptr;
    std::shared_ptr<GraphicsFrameContextSource> _contextSource;
    GraphicsFrameCallback _callback;
    StorageOutcome _storageOutcome;
    std::shared_ptr<const std::atomic<bool>> _externalCancelled;
    std::atomic<bool> _localCancelled{false};
    GraphicsFrameCallbackGate _callbackGate;
    Framegrabber::CallbackId _grabCallbackId = 0;
};

FramegrabberGraphicsFrameStream::Impl::Impl(
    Framegrabber* framegrabber,
         std::shared_ptr<GraphicsFrameContextSource> contextSource,
    GraphicsFrameCallback callback,
    RejectedImage rejectedImage,
    StorageOutcome storageOutcome,
    std::shared_ptr<const std::atomic<bool>> cancelled)
    : _framegrabber(framegrabber),
      _contextSource(std::move(contextSource)),
      _callback(std::move(callback)),
      _storageOutcome(std::move(storageOutcome)),
      _externalCancelled(std::move(cancelled))
{
    if (!_framegrabber || !_contextSource || !_callback)
    {
        return;
    }

    const auto callbackToken = _callbackGate.token();
    _grabCallbackId = _framegrabber->registerGrabCallback(
        [this, callbackToken, rejectedImage = std::move(rejectedImage)](
            const Framegrabber::Image& image, const std::size_t sequence) {
            GraphicsFrameCallbackGate::Lease lease(callbackToken);
            if (!lease || isCancelled()) return;
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
            if (isCancelled()) return;
            _callback(std::move(published).value(), image.dmaIndex);
        });
}

FramegrabberGraphicsFrameStream::Impl::~Impl()
{
    _localCancelled.store(true, std::memory_order_release);
    _callbackGate.beginShutdown();
    if (_framegrabber && _grabCallbackId != 0U)
    {
        _framegrabber->deregisterGrabCallback(_grabCallbackId);
    }
    _callbackGate.waitForDrain();
}

FramegrabberGraphicsFrameStream::FramegrabberGraphicsFrameStream(
    Framegrabber* framegrabber,
    std::shared_ptr<GraphicsFrameContextSource> contextSource,
    GraphicsFrameCallback callback,
    RejectedImage rejectedImage,
    StorageOutcome storageOutcome,
    std::shared_ptr<const std::atomic<bool>> cancelled)
    : _impl(std::make_unique<Impl>(
        framegrabber,
        std::move(contextSource),
        std::move(callback),
        std::move(rejectedImage),
        std::move(storageOutcome),
        std::move(cancelled)))
{
}

FramegrabberGraphicsFrameStream::~FramegrabberGraphicsFrameStream() = default;

void FramegrabberGraphicsFrameStream::cancel() noexcept
{
    if (_impl)
    {
        _impl->cancel();
    }
}
