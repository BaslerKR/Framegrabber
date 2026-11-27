#include "FramegrabberGraphicsFrameAdapter.h"

namespace
{
[[nodiscard]] GraphicsImagePixelFormat graphicsPixelFormat(
    const Framegrabber::PixelFormat format) noexcept
{
#define MAP_PIXEL_FORMAT(name) \
    case Framegrabber::PixelFormat::name: return GraphicsImagePixelFormat::name
    switch (format)
    {
    MAP_PIXEL_FORMAT(Unknown);
    MAP_PIXEL_FORMAT(Mono8);
    MAP_PIXEL_FORMAT(Mono10Packed);
    MAP_PIXEL_FORMAT(Mono12Packed);
    MAP_PIXEL_FORMAT(Mono14Packed);
    MAP_PIXEL_FORMAT(Mono16);
    MAP_PIXEL_FORMAT(Mono32);
    MAP_PIXEL_FORMAT(Binary);
    MAP_PIXEL_FORMAT(RGB24);
    MAP_PIXEL_FORMAT(BGR24);
    MAP_PIXEL_FORMAT(YCbCr422_8);
    MAP_PIXEL_FORMAT(YCbCr422_8_CbYCrY);
    MAP_PIXEL_FORMAT(RGBA32);
    MAP_PIXEL_FORMAT(RGB30);
    MAP_PIXEL_FORMAT(RGB48);
    MAP_PIXEL_FORMAT(BGR48);
    MAP_PIXEL_FORMAT(RGB10Packed);
    MAP_PIXEL_FORMAT(RGB12Packed);
    MAP_PIXEL_FORMAT(RGB14Packed);
    MAP_PIXEL_FORMAT(BGR10Packed);
    MAP_PIXEL_FORMAT(BGR12Packed);
    MAP_PIXEL_FORMAT(BGR14Packed);
    MAP_PIXEL_FORMAT(RGBA10Packed);
    MAP_PIXEL_FORMAT(RGBA12Packed);
    MAP_PIXEL_FORMAT(RGBA14Packed);
    MAP_PIXEL_FORMAT(RGBA64);
    MAP_PIXEL_FORMAT(BGRA32);
    MAP_PIXEL_FORMAT(BGRA10Packed);
    MAP_PIXEL_FORMAT(BGRA12Packed);
    MAP_PIXEL_FORMAT(BGRA14Packed);
    MAP_PIXEL_FORMAT(BGRA64);
    MAP_PIXEL_FORMAT(RGBX32);
    MAP_PIXEL_FORMAT(RGBX10Packed);
    MAP_PIXEL_FORMAT(RGBX12Packed);
    MAP_PIXEL_FORMAT(RGBX14Packed);
    MAP_PIXEL_FORMAT(RGBX64);
    MAP_PIXEL_FORMAT(BayerGR8);
    MAP_PIXEL_FORMAT(BayerGR10);
    MAP_PIXEL_FORMAT(BayerGR12);
    MAP_PIXEL_FORMAT(BayerGR14);
    MAP_PIXEL_FORMAT(BayerGR16);
    MAP_PIXEL_FORMAT(BayerRG8);
    MAP_PIXEL_FORMAT(BayerRG10);
    MAP_PIXEL_FORMAT(BayerRG12);
    MAP_PIXEL_FORMAT(BayerRG14);
    MAP_PIXEL_FORMAT(BayerRG16);
    MAP_PIXEL_FORMAT(BayerGB8);
    MAP_PIXEL_FORMAT(BayerGB10);
    MAP_PIXEL_FORMAT(BayerGB12);
    MAP_PIXEL_FORMAT(BayerGB14);
    MAP_PIXEL_FORMAT(BayerGB16);
    MAP_PIXEL_FORMAT(BayerBG8);
    MAP_PIXEL_FORMAT(BayerBG10);
    MAP_PIXEL_FORMAT(BayerBG12);
    MAP_PIXEL_FORMAT(BayerBG14);
    MAP_PIXEL_FORMAT(BayerBG16);
    MAP_PIXEL_FORMAT(BiColorRGBG8);
    MAP_PIXEL_FORMAT(BiColorRGBG10);
    MAP_PIXEL_FORMAT(BiColorRGBG12);
    MAP_PIXEL_FORMAT(BiColorGRGB8);
    MAP_PIXEL_FORMAT(BiColorGRGB10);
    MAP_PIXEL_FORMAT(BiColorGRGB12);
    MAP_PIXEL_FORMAT(BiColorBGRG8);
    MAP_PIXEL_FORMAT(BiColorBGRG10);
    MAP_PIXEL_FORMAT(BiColorBGRG12);
    MAP_PIXEL_FORMAT(BiColorGBGR8);
    MAP_PIXEL_FORMAT(BiColorGBGR10);
    MAP_PIXEL_FORMAT(BiColorGBGR12);
    MAP_PIXEL_FORMAT(Raw);
    MAP_PIXEL_FORMAT(Jpeg);
    }
#undef MAP_PIXEL_FORMAT
    return GraphicsImagePixelFormat::Unknown;
}

[[nodiscard]] GraphicsImageBitAlignment graphicsBitAlignment(
    const Framegrabber::BitAlignment alignment) noexcept
{
    switch (alignment)
    {
    case Framegrabber::BitAlignment::LeastSignificant:
        return GraphicsImageBitAlignment::LeastSignificant;
    case Framegrabber::BitAlignment::MostSignificant:
        return GraphicsImageBitAlignment::MostSignificant;
    case Framegrabber::BitAlignment::Packed:
        return GraphicsImageBitAlignment::Packed;
    }
    return GraphicsImageBitAlignment::Packed;
}
}

namespace FramegrabberGraphicsFrameAdapter
{
GraphicsImagePixelFormat graphicsPixelFormat(const Framegrabber::PixelFormat format) noexcept
{
    return ::graphicsPixelFormat(format);
}

GraphicsImageBitAlignment graphicsBitAlignment(const Framegrabber::BitAlignment alignment) noexcept
{
    return ::graphicsBitAlignment(alignment);
}

GraphicsResult<GraphicsImageWriteLayout> planImage(
    const FramegrabberDetail::CaptureFrameDescriptor& descriptor,
    GraphicsImageFailure* const failure) noexcept
{
    const auto reject = [failure](const GraphicsImageFailure reason)
        -> GraphicsResult<GraphicsImageWriteLayout> {
        if (failure) *failure = reason;
        return reason == GraphicsImageFailure::ExtentOverflow
            ? GraphicsFrameError::Capacity
            : GraphicsFrameError::InvalidInput;
    };
    if (descriptor.size == 0U)
    {
        return reject(GraphicsImageFailure::MissingBytes);
    }
    if (descriptor.width <= 0 || descriptor.height <= 0)
    {
        return reject(GraphicsImageFailure::InvalidDimensions);
    }
    if (descriptor.stride <= 0)
    {
        return reject(GraphicsImageFailure::InvalidStride);
    }

    // planGraphicsImageWrite checks data != nullptr without dereferencing it, so a static
    // non-null byte plans the destination without exposing any raw DMA pointer.
    alignas(16) static constexpr std::uint8_t kPlannedBytePlaceholder = 0U;
    GraphicsImageWriterSource source;
    source.data = &kPlannedBytePlaceholder;
    source.size = descriptor.size;
    source.width = descriptor.width;
    source.height = descriptor.height;
    source.stride = descriptor.stride;
    source.pixelFormat = ::graphicsPixelFormat(descriptor.pixelFormat);
    source.bitsPerPixel = descriptor.bitsPerPixel;
    source.sourceFormat = descriptor.fgFormat;
    source.bitAlignment = ::graphicsBitAlignment(descriptor.bitAlignment);
    source.frameSequence = descriptor.frameSeq;
    source.operation = GraphicsImageWriterOperation::Copy;
    return planGraphicsImageWrite(source, failure);
}

GraphicsResult<GraphicsFrame> captureFrame(
    GraphicsFrameContextSource& contextSource,
    const FramegrabberDetail::CaptureFrameDescriptor& descriptor,
    FramegrabberDetail::CaptureCopyGate& copyGate,
    GraphicsImageFailure* const outImageFailure,
    GraphicsImage* const rejectedImage,
    const std::atomic<bool>* const cancelled)
{
    if (outImageFailure) *outImageFailure = GraphicsImageFailure::None;
    const auto isCancelled = [cancelled]() noexcept {
        return cancelled && cancelled->load(std::memory_order_acquire);
    };
    const auto recordRejected = [rejectedImage, &descriptor](
        const GraphicsImagePixelFormat pixelFormat,
        const int width,
        const int height,
        const int stride) noexcept {
        if (!rejectedImage) return;
        GraphicsImage rejected;
        rejected.width = width;
        rejected.height = height;
        rejected.stride = stride;
        rejected.bitsPerPixel = descriptor.bitsPerPixel;
        rejected.sourceFormat = descriptor.fgFormat;
        rejected.pixelFormat = pixelFormat;
        rejected.bitAlignment = ::graphicsBitAlignment(descriptor.bitAlignment);
        rejected.frameSequence = descriptor.frameSeq;
        *rejectedImage = std::move(rejected);
    };

    if (isCancelled())
    {
        return GraphicsFrameError::Disconnected;
    }

    GraphicsImageFailure planFailure = GraphicsImageFailure::None;
    const auto plan = planImage(descriptor, &planFailure);
    if (!plan)
    {
        if (outImageFailure) *outImageFailure = planFailure;
        recordRejected(
            ::graphicsPixelFormat(descriptor.pixelFormat),
            descriptor.width,
            descriptor.height,
            descriptor.stride);
        return plan.error();
    }

    const GraphicsImageWriteLayout& layout = plan.value();
    auto contextResult = contextSource.beginFrame(layout.bytes);
    if (!contextResult)
    {
        return contextResult.error();
    }
    if (isCancelled())
    {
        return GraphicsFrameError::Disconnected;
    }

    GraphicsFrameBuilder builder(contextResult.value());
    auto reserved = builder.reserve<std::uint8_t>(layout.bytes, layout.alignment);
    if (!reserved)
    {
        if (outImageFailure) *outImageFailure = GraphicsImageFailure::StorageRefused;
        recordRejected(layout.format, layout.width, layout.height, layout.strideBytes);
        return reserved.error();
    }

    GraphicsWriteRegion<std::uint8_t> region = std::move(reserved).value();
    if (!copyGate.copyAndVerify(region.data(), layout.bytes))
    {
        // RAII destroys region and builder without sealing or publishing.
        return GraphicsFrameError::InvalidInput;
    }
    if (isCancelled())
    {
        // RAII releases the unsealed write region on stop/cancellation before seal.
        return GraphicsFrameError::Disconnected;
    }

    GraphicsImage image;
    image.pixels = builder.seal(std::move(region));
    image.width = layout.width;
    image.height = layout.height;
    image.stride = layout.strideBytes;
    image.bitsPerPixel = descriptor.bitsPerPixel;
    image.sourceFormat = descriptor.fgFormat;
    image.pixelFormat = layout.format;
    image.bitAlignment = ::graphicsBitAlignment(descriptor.bitAlignment);
    image.frameSequence = descriptor.frameSeq;

    const GraphicsImageFailure failure = imageFailure(image);
    if (failure != GraphicsImageFailure::None)
    {
        if (outImageFailure) *outImageFailure = failure;
        if (rejectedImage) *rejectedImage = std::move(image);
        return GraphicsFrameError::InvalidInput;
    }

    builder.addImage("image", std::move(image), "Image");
    GraphicsFrameMetadata metadata;
    metadata.sourceName = "Framegrabber";
    metadata.frameIndex = descriptor.frameSeq;
    builder.setMetadata(std::move(metadata));
    auto published = std::move(builder).publish();
    if (!published)
    {
        if (outImageFailure)
        {
            *outImageFailure = (published.error() == GraphicsFrameError::InvalidInput
                                || published.error() == GraphicsFrameError::UnsealedWrite
                                || published.error() == GraphicsFrameError::None)
                ? GraphicsImageFailure::InvalidDimensions
                : GraphicsImageFailure::StorageRefused;
        }
        recordRejected(layout.format, layout.width, layout.height, layout.strideBytes);
        return published.error();
    }
    if (isCancelled())
    {
        return GraphicsFrameError::Disconnected;
    }
    return published;
}

GraphicsImageFailure imageFailure(const GraphicsImage& image) noexcept
{
    if (!image.data()) return GraphicsImageFailure::MissingBytes;
    return GraphicsImageValidation::layoutFailure(
        image.pixelFormat, image.width, image.height, image.stride, image.pixels.size());
}
}
