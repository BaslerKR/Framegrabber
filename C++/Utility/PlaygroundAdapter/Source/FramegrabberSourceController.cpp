#include "FramegrabberSourceController.h"

#include "Utility/PlaygroundAdapter/FramegrabberGraphicsFrameStream.h"
#include "SessionFrame.h"

#include <QDebug>

#include <cstddef>
#include <utility>

FramegrabberSourceController::FramegrabberSourceController(
    Framegrabber* framegrabber,
    QObject* parent)
    : AbstractSourceController(parent),
      _framegrabber(framegrabber)
{
    registerCallbacks();
}

FramegrabberSourceController::~FramegrabberSourceController()
{
    stop();
    deregisterCallbacks();
}

void FramegrabberSourceController::start()
{
    if (_framegrabber)
    {
        publishSourceDescriptors();
        _framegrabber->grab();
    }
}

void FramegrabberSourceController::stop()
{
    if (_framegrabber)
    {
        _framegrabber->stop();
    }
    _isGrabbing.store(false, std::memory_order_release);
}

std::vector<GraphicsSourceDescriptor> FramegrabberSourceController::sourceDescriptors() const
{
    std::vector<GraphicsSourceDescriptor> sources;
    const int count = _framegrabber ? _framegrabber->getDMACount() : 0;
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

    _statusCallbackId = _framegrabber->registerStatusCallback(
        [this](const Framegrabber::Status status, const bool on)
        {
            if (status == Framegrabber::GrabbingStatus)
            {
                _isGrabbing.store(on, std::memory_order_release);
                emit diagnosticAcquisitionChanged(on, QStringLiteral("framegrabber"));
            }
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
}
