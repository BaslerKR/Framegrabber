#include "FramegrabberGraphicsFrameAdapter.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace
{

class TestCopyGate final : public FramegrabberDetail::CaptureCopyGate
{
public:
    explicit TestCopyGate(std::vector<std::uint8_t> source, const bool rejectOverwrite = false)
        : _source(std::move(source))
        , _rejectOverwrite(rejectOverwrite)
    {
    }

    bool copyAndVerify(std::uint8_t* destination, std::size_t capacity) noexcept override
    {
        ++copyAttempts;
        if (!destination || capacity < _source.size())
        {
            return false;
        }
        if (!_source.empty())
        {
            std::memcpy(destination, _source.data(), _source.size());
        }
        if (cancelOnCopy) cancelOnCopy->store(true);
        return !_rejectOverwrite;
    }

    std::atomic<bool>* cancelOnCopy = nullptr;
    int copyAttempts = 0;

private:
    std::vector<std::uint8_t> _source;
    bool _rejectOverwrite = false;
};

} // namespace

int main()
{
    GraphicsLocalFrameContextSource localSource;

    {
        FramegrabberDetail::CaptureFrameDescriptor descriptor;
        descriptor.size = 4U;
        descriptor.width = 2;
        descriptor.height = 2;
        descriptor.stride = 2;
        descriptor.bitsPerPixel = 8;
        descriptor.pixelFormat = Framegrabber::PixelFormat::Mono8;
        descriptor.bitAlignment = Framegrabber::BitAlignment::Packed;
        descriptor.frameSeq = 9U;

        TestCopyGate gate({1U, 2U, 3U, 4U});
        GraphicsImageFailure failure = GraphicsImageFailure::None;
        GraphicsImage rejected;
        auto result = FramegrabberGraphicsFrameAdapter::captureFrame(
            localSource, descriptor, gate, &failure, &rejected);
        assert(result.hasValue());
        assert(failure == GraphicsImageFailure::None);
        assert(gate.copyAttempts == 1);

        const GraphicsFrame frame = std::move(result).value();
        assert(frame.isValid());
        assert(frame.firstImage() != nullptr);
        const GraphicsImage& image = frame.firstImage()->payload;
        assert(image.isValid());
        assert(image.pixelFormat == GraphicsImagePixelFormat::Mono8);
        assert(image.frameSequence == 9U);
        assert(image.pixels.size() == 4U);
        assert(image.data() != nullptr);
        assert(image.data()[0] == 1U);
        assert(image.data()[3] == 4U);
        assert(image.pixels.allocation());
        assert(image.pixels.allocation()->origin() == GraphicsAllocationOrigin::LocalHeap);
        assert(FramegrabberGraphicsFrameAdapter::imageFailure(image) == GraphicsImageFailure::None);
        assert(frame.metadata().sourceName == "Framegrabber");
        assert(frame.metadata().frameIndex == 9U);

        auto checked = image;
        checked.stride = 1;
        assert(FramegrabberGraphicsFrameAdapter::imageFailure(checked) == GraphicsImageFailure::StrideBelowMinimum);
        checked.stride = 2;
        checked.pixels = checked.pixels.subspan(0U, 3U);
        assert(FramegrabberGraphicsFrameAdapter::imageFailure(checked) == GraphicsImageFailure::TruncatedBytes);
        checked.pixels = {};
        assert(FramegrabberGraphicsFrameAdapter::imageFailure(checked) == GraphicsImageFailure::MissingBytes);
    }

    {
        FramegrabberDetail::CaptureFrameDescriptor descriptor;
        descriptor.size = 3U;
        descriptor.width = 2;
        descriptor.height = 2;
        descriptor.stride = 2;
        descriptor.bitsPerPixel = 8;
        descriptor.pixelFormat = Framegrabber::PixelFormat::Mono8;
        descriptor.frameSeq = 10U;

        TestCopyGate gate({1U, 2U, 3U});
        GraphicsImageFailure failure = GraphicsImageFailure::None;
        GraphicsImage rejected;
        auto result = FramegrabberGraphicsFrameAdapter::captureFrame(
            localSource, descriptor, gate, &failure, &rejected);
        assert(!result.hasValue());
        assert(failure == GraphicsImageFailure::TruncatedBytes);
        assert(gate.copyAttempts == 0);
        assert(rejected.frameSequence == 10U);
    }

    {
        FramegrabberDetail::CaptureFrameDescriptor descriptor;
        descriptor.size = 0U;
        descriptor.width = 2;
        descriptor.height = 2;
        descriptor.stride = 2;
        descriptor.bitsPerPixel = 8;
        descriptor.pixelFormat = Framegrabber::PixelFormat::Mono8;
        descriptor.frameSeq = 11U;

        TestCopyGate gate({});
        GraphicsImageFailure failure = GraphicsImageFailure::None;
        auto result = FramegrabberGraphicsFrameAdapter::captureFrame(
            localSource, descriptor, gate, &failure);
        assert(!result.hasValue());
        assert(failure == GraphicsImageFailure::MissingBytes);
        assert(gate.copyAttempts == 0);
    }

    {
        FramegrabberDetail::CaptureFrameDescriptor descriptor;
        descriptor.size = 4U;
        descriptor.width = 2;
        descriptor.height = 2;
        descriptor.stride = 0;
        descriptor.bitsPerPixel = 8;
        descriptor.pixelFormat = Framegrabber::PixelFormat::Mono8;

        GraphicsImageFailure failure = GraphicsImageFailure::None;
        auto plan = FramegrabberGraphicsFrameAdapter::planImage(descriptor, &failure);
        assert(!plan.hasValue());
        assert(failure == GraphicsImageFailure::InvalidStride);
    }

    {
        FramegrabberDetail::CaptureFrameDescriptor descriptor;
        descriptor.size = 4; descriptor.width = 2; descriptor.height = 2;
        descriptor.stride = 2; descriptor.bitsPerPixel = 8;
        descriptor.pixelFormat = Framegrabber::PixelFormat::Mono8;
        std::atomic<bool> localCancel{false}, externalCancel{false};
        TestCopyGate gate({1, 2, 3, 4});
        gate.cancelOnCopy = &localCancel;
        auto result = FramegrabberGraphicsFrameAdapter::captureFrame(
            localSource, descriptor, gate, nullptr, nullptr, &localCancel, &externalCancel);
        assert(!result.hasValue());
        assert(result.error() == GraphicsFrameError::Disconnected);
        assert(gate.copyAttempts == 1);
    }

    {
        FramegrabberDetail::CaptureFrameDescriptor descriptor;
        descriptor.size = 4; descriptor.width = 2; descriptor.height = 2;
        descriptor.stride = 2; descriptor.bitsPerPixel = 8;
        descriptor.pixelFormat = Framegrabber::PixelFormat::Mono8;
        TestCopyGate gate({1, 2, 3, 4}, true);
        auto result = FramegrabberGraphicsFrameAdapter::captureFrame(localSource, descriptor, gate);
        assert(!result.hasValue());
        assert(gate.copyAttempts == 1);
    }

    auto captureNative = [&localSource](const Framegrabber::PixelFormat pixelFormat,
                                        const GraphicsImagePixelFormat expected,
                                        const int width,
                                        const int height,
                                        const int stride,
                                        const int bitsPerPixel)
    {
        const std::size_t bytes = static_cast<std::size_t>(stride) * static_cast<std::size_t>(height);
        std::vector<std::uint8_t> payload(bytes);
        for (std::size_t i = 0; i < bytes; ++i)
        {
            payload[i] = static_cast<std::uint8_t>(i & 0xFFU);
        }

        FramegrabberDetail::CaptureFrameDescriptor input;
        input.size = bytes;
        input.width = width;
        input.height = height;
        input.stride = stride;
        input.bitsPerPixel = bitsPerPixel;
        input.pixelFormat = pixelFormat;
        input.bitAlignment = Framegrabber::BitAlignment::Packed;
        input.frameSeq = 3U;

        TestCopyGate gate(payload);
        GraphicsImageFailure nativeFailure = GraphicsImageFailure::None;
        auto result = FramegrabberGraphicsFrameAdapter::captureFrame(
            localSource, input, gate, &nativeFailure);
        assert(result.hasValue());
        assert(nativeFailure == GraphicsImageFailure::None);
        assert(gate.copyAttempts == 1);
        const GraphicsFrame frame = std::move(result).value();
        assert(frame.isValid());
        assert(frame.firstImage() != nullptr);
        const GraphicsImage& wrapped = frame.firstImage()->payload;
        assert(wrapped.isValid());
        assert(wrapped.pixelFormat == expected);
        assert(wrapped.pixels.size() == bytes);
        assert(std::equal(payload.begin(), payload.end(), wrapped.data()));
        assert(wrapped.width == width);
        assert(wrapped.height == height);
        assert(wrapped.stride == stride);
    };

    captureNative(
        Framegrabber::PixelFormat::BayerRG8,
        GraphicsImagePixelFormat::BayerRG8,
        4,
        4,
        4,
        8);
    captureNative(
        Framegrabber::PixelFormat::YCbCr422_8,
        GraphicsImagePixelFormat::YCbCr422_8,
        4,
        2,
        8,
        16);
    captureNative(
        Framegrabber::PixelFormat::RGB10Packed,
        GraphicsImagePixelFormat::RGB10Packed,
        4,
        2,
        15,
        30);
    return 0;
}
