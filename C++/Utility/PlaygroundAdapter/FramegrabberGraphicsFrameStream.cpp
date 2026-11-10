#include "FramegrabberGraphicsFrameStream.h"

#include "FramegrabberGraphicsFrameAdapter.h"
#include "FramegrabberSystem.h"

#include <exception>
#include <memory>
#include <string>
#include <utility>

class FramegrabberGraphicsFrameStream::Impl final
{
public:
    Impl(Framegrabber* framegrabber, FramegrabberFrameCallback callback);
    ~Impl();

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

private:
    Framegrabber* _framegrabber = nullptr;
    FramegrabberFrameCallback _callback;
    GraphicsFrameCallbackGate _callbackGate;
    Framegrabber::CallbackId _grabCallbackId = 0;
};

FramegrabberGraphicsFrameStream::Impl::Impl(
    Framegrabber* framegrabber,
    FramegrabberFrameCallback callback)
    : _framegrabber(framegrabber), _callback(std::move(callback))
{
    if (!_framegrabber || !_callback)
    {
        return;
    }

    const auto callbackToken = _callbackGate.token();
    _grabCallbackId = _framegrabber->registerGrabCallback(
        [this, callbackToken](const Framegrabber::Image& image, const std::size_t sequence) {
            GraphicsFrameCallbackGate::Lease lease(callbackToken);
            if (!lease) return;
            try
            {
                GraphicsImage graphicsImage =
                    FramegrabberGraphicsFrameAdapter::wrapImage(image, sequence);
                if (!graphicsImage.isValid()) return;

                GraphicsFrame frame;
                frame.addImage("image", std::move(graphicsImage), "Image");
                frame.metadata.sourceName = "Framegrabber";
                frame.metadata.frameIndex = sequence;
                _callback(
                    std::move(frame),
                    image.dmaIndex,
                    FramegrabberFrameCapture{image.captureGeneration, image.picture});
            }
            catch (const std::exception& error)
            {
                FramegrabberSystem::syslog(
                    std::string("Framegrabber GraphicsFrame callback failed: ") + error.what(), true);
            }
            catch (...)
            {
                FramegrabberSystem::syslog(
                    "Framegrabber GraphicsFrame callback failed with an unknown exception.", true);
            }
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
    FramegrabberFrameCallback callback)
    : _impl(std::make_unique<Impl>(framegrabber, std::move(callback)))
{
}

FramegrabberGraphicsFrameStream::~FramegrabberGraphicsFrameStream() = default;
