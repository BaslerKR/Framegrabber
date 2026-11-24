#include "FramegrabberGraphicsFrameAdapter.h"
#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

int main()
{
    auto storage = std::make_shared<std::vector<std::uint8_t>>(4U);
    (*storage)[0] = 1U;
    (*storage)[3] = 4U;

    Framegrabber::Image source;
    source.storage = std::shared_ptr<const std::uint8_t>(storage, storage->data());
    source.size = storage->size();
    source.width = 2;
    source.height = 2;
    source.stride = 2;
    source.bitsPerPixel = 8;
    source.pixelFormat = Framegrabber::PixelFormat::Mono8;
    source.bitAlignment = Framegrabber::BitAlignment::Packed;

    const GraphicsImage image = FramegrabberGraphicsFrameAdapter::wrapImage(source, 9U);
    assert(image.isValid());
    assert(image.pixelFormat == GraphicsImagePixelFormat::Mono8);
    assert(image.frameSequence == 9U);
    assert(image.data() == storage->data());
    assert(image.data()[3] == 4U);
    assert(FramegrabberGraphicsFrameAdapter::imageFailure(image) == GraphicsImageFailure::None);

    auto truncated = source;
    truncated.size = 3;
    assert(!truncated.isValid());
    truncated = source;
    truncated.height = 0;
    assert(!truncated.isValid());
    truncated = source;
    truncated.stride = 0;
    assert(!truncated.isValid());
    truncated = source;
    truncated.storage.reset();
    assert(!truncated.isValid());
    truncated = source;
    truncated.stride = 1;
    truncated.size = 2;
    assert(!truncated.isValid());
    truncated = source;
    truncated.bitsPerPixel = 0;
    assert(!truncated.isValid());
    truncated = source;
    truncated.width = (std::numeric_limits<int>::max)();
    truncated.height = (std::numeric_limits<int>::max)();
    truncated.stride = (std::numeric_limits<int>::max)();
    truncated.bitsPerPixel = (std::numeric_limits<int>::max)();
    truncated.size = (std::numeric_limits<std::size_t>::max)();
    assert(!truncated.isValid());

    auto rejected = image;
    rejected.stride = 1;
    assert(FramegrabberGraphicsFrameAdapter::imageFailure(rejected) == GraphicsImageFailure::StrideBelowMinimum);
    rejected.stride = 2;
    rejected.size = 3;
    assert(FramegrabberGraphicsFrameAdapter::imageFailure(rejected) == GraphicsImageFailure::TruncatedBytes);
    rejected.storage.reset();
    assert(FramegrabberGraphicsFrameAdapter::imageFailure(rejected) == GraphicsImageFailure::MissingBytes);

    auto wrapNative = [](const Framegrabber::PixelFormat pixelFormat,
                         const GraphicsImagePixelFormat expected,
                         const int width,
                         const int height,
                         const int stride,
                         const int bitsPerPixel)
    {
        auto bytes = std::make_shared<std::vector<std::uint8_t>>(
            static_cast<std::size_t>(stride) * static_cast<std::size_t>(height));
        Framegrabber::Image input;
        input.storage = std::shared_ptr<const std::uint8_t>(bytes, bytes->data());
        input.size = bytes->size();
        input.width = width;
        input.height = height;
        input.stride = stride;
        input.bitsPerPixel = bitsPerPixel;
        input.pixelFormat = pixelFormat;
        input.bitAlignment = Framegrabber::BitAlignment::Packed;
        assert(input.isValid());

        const GraphicsImage wrapped = FramegrabberGraphicsFrameAdapter::wrapImage(input, 3U);
        assert(wrapped.isValid());
        assert(wrapped.pixelFormat == expected);
        assert(wrapped.data() == bytes->data());
        assert(wrapped.width == width);
        assert(wrapped.height == height);
        assert(wrapped.stride == stride);
    };

    wrapNative(
        Framegrabber::PixelFormat::BayerRG8,
        GraphicsImagePixelFormat::BayerRG8,
        4,
        4,
        4,
        8);
    wrapNative(
        Framegrabber::PixelFormat::YCbCr422_8,
        GraphicsImagePixelFormat::YCbCr422_8,
        4,
        2,
        8,
        16);
    wrapNative(
        Framegrabber::PixelFormat::RGB10Packed,
        GraphicsImagePixelFormat::RGB10Packed,
        4,
        2,
        15,
        30);
    return 0;
}
