#include "FramegrabberGraphicsFrameStream.h"

#include "FramegrabberCaptureTransaction.h"
#include "FramegrabberGraphicsFrameAdapter.h"

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
    FramegrabberDetail::CaptureTransactionAccess::setCaptureTransaction(
        *_framegrabber,
        [this, callbackToken, rejectedImage = std::move(rejectedImage)](
            const FramegrabberDetail::CaptureFrameDescriptor& descriptor,
            FramegrabberDetail::CaptureCopyGate& copyGate)
            -> FramegrabberDetail::CaptureTransactionOutcome
        {
            using Outcome = FramegrabberDetail::CaptureTransactionOutcome;
            GraphicsFrameCallbackGate::Lease lease(callbackToken);
            if (!lease || isCancelled())
            {
                return Outcome::AbortedStop;
            }
            GraphicsImageFailure imageFailure = GraphicsImageFailure::None;
            GraphicsImage rejected;
            auto frame = FramegrabberGraphicsFrameAdapter::captureFrame(
                *_contextSource,
                descriptor,
                copyGate,
                &imageFailure,
                rejectedImage ? &rejected : nullptr,
                &_localCancelled,
                _externalCancelled.get());
            if (frame.hasValue())
            {
                if (isCancelled())
                {
                    return Outcome::AbortedStop;
                }
                _callback(std::move(frame).value(), descriptor.dmaIndex);
                return Outcome::Delivered;
            }

            const GraphicsFrameError error = frame.error();
            if (isCancelled())
            {
                return Outcome::AbortedStop;
            }
            if (error == GraphicsFrameError::NoReadyCredit)
            {
                if (_storageOutcome) _storageOutcome(error);
                return Outcome::DroppedNoCredit;
            }
            if (error == GraphicsFrameError::ClassRefused || error == GraphicsFrameError::Capacity)
            {
                if (_storageOutcome) _storageOutcome(error);
                return Outcome::DroppedStorageRefused;
            }
            if (error == GraphicsFrameError::Disconnected)
            {
                return Outcome::AbortedStop;
            }
            if (error == GraphicsFrameError::ClassNotPrepared)
            {
                if (_storageOutcome) _storageOutcome(error);
                return Outcome::DroppedStorageRefused;
            }
            if (error == GraphicsFrameError::Allocation && _storageOutcome)
            {
                _storageOutcome(error);
            }
            if (rejectedImage && imageFailure != GraphicsImageFailure::None)
            {
                rejectedImage(rejected, descriptor.dmaIndex, imageFailure, descriptor.size);
            }
            return Outcome::DroppedInvalidOrFailed;
        });
}

FramegrabberGraphicsFrameStream::Impl::~Impl()
{
    _localCancelled.store(true, std::memory_order_release);
    _callbackGate.beginShutdown();
    if (_framegrabber)
    {
        FramegrabberDetail::CaptureTransactionAccess::clearCaptureTransaction(*_framegrabber);
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
