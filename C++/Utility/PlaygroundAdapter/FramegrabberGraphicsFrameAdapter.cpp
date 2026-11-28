#include "FramegrabberGraphicsFrameAdapter.h"

#include <algorithm>
#include <limits>
#include <optional>

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

namespace
{
constexpr std::size_t kRawRowBytes = 4096U;

/** Layout fitted to one descriptor before the GraphicsEngine plan check. */
struct FittedImage
{
    GraphicsImagePixelFormat format = GraphicsImagePixelFormat::Mono8;
    int width = 0;
    int height = 0;
    std::size_t destinationStride = 0;
    std::size_t sourceStride = 0;
};

[[nodiscard]] bool hasLayout(const GraphicsImagePixelFormat format) noexcept
{
    return GraphicsImageValidation::packedBitsPerPixel(format) != 0;
}

/** Nearest GraphicsEngine layout for a bit depth whose SDK format has no layout of its own. */
[[nodiscard]] GraphicsImagePixelFormat nearestFormatForBits(const int bits) noexcept
{
    switch (bits)
    {
    case 1: return GraphicsImagePixelFormat::Binary;
    case 8: return GraphicsImagePixelFormat::Mono8;
    case 10: return GraphicsImagePixelFormat::Mono10Packed;
    case 12: return GraphicsImagePixelFormat::Mono12Packed;
    case 14: return GraphicsImagePixelFormat::Mono14Packed;
    case 16: return GraphicsImagePixelFormat::Mono16;
    case 24: return GraphicsImagePixelFormat::RGB24;
    case 30: return GraphicsImagePixelFormat::RGB30;
    case 32: return GraphicsImagePixelFormat::Mono32;
    case 36: return GraphicsImagePixelFormat::RGB12Packed;
    case 40: return GraphicsImagePixelFormat::RGBA10Packed;
    case 42: return GraphicsImagePixelFormat::RGB14Packed;
    case 48: return GraphicsImagePixelFormat::RGB48;
    case 56: return GraphicsImagePixelFormat::RGBA14Packed;
    case 64: return GraphicsImagePixelFormat::RGBA64;
    default: return GraphicsImagePixelFormat::Unknown;
    }
}

/** Raw bytes as Mono8 rows; rows follow the delivered stride when it is usable. */
[[nodiscard]] std::optional<FittedImage> rawFit(
    const FramegrabberDetail::CaptureFrameDescriptor& descriptor) noexcept
{
    const std::size_t size = descriptor.size;
    if (size == 0U) return std::nullopt;
    const auto intMax = static_cast<std::size_t>((std::numeric_limits<int>::max)());
    std::size_t width = descriptor.stride > 0 && static_cast<std::size_t>(descriptor.stride) <= size
        ? static_cast<std::size_t>(descriptor.stride)
        : (std::min)(size, kRawRowBytes);
    if (size / width + (size % width != 0U ? 1U : 0U) > intMax)
    {
        width = size / intMax + 1U;
    }
    if (width > intMax) return std::nullopt;
    const std::size_t height = size / width + (size % width != 0U ? 1U : 0U);
    FittedImage fit;
    fit.format = GraphicsImagePixelFormat::Mono8;
    fit.width = static_cast<int>(width);
    fit.height = static_cast<int>(height);
    fit.destinationStride = width;
    fit.sourceStride = width;
    return fit;
}

/** Plans @p fit through GraphicsEngine; the planned destination holds `stride * height` bytes. */
[[nodiscard]] GraphicsResult<GraphicsImageWriteLayout> planFit(
    const FittedImage& fit,
    const FramegrabberDetail::CaptureFrameDescriptor& descriptor,
    GraphicsImageFailure* const failure) noexcept
{
    const auto rows = static_cast<std::size_t>(fit.height);
    if (fit.destinationStride == 0U
        || fit.destinationStride > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        if (failure) *failure = GraphicsImageFailure::InvalidStride;
        return GraphicsFrameError::InvalidInput;
    }
    if (rows != 0U && fit.destinationStride > (std::numeric_limits<std::size_t>::max)() / rows)
    {
        if (failure) *failure = GraphicsImageFailure::ExtentOverflow;
        return GraphicsFrameError::InvalidInput;
    }
    alignas(16) static constexpr std::uint8_t kPlannedBytePlaceholder = 0U;
    GraphicsImageWriterSource source;
    source.data = &kPlannedBytePlaceholder;
    source.size = fit.destinationStride * rows;
    source.width = fit.width;
    source.height = fit.height;
    source.stride = static_cast<int>(fit.destinationStride);
    source.pixelFormat = fit.format;
    source.bitsPerPixel = descriptor.bitsPerPixel;
    source.sourceFormat = descriptor.fgFormat;
    source.bitAlignment = ::graphicsBitAlignment(descriptor.bitAlignment);
    source.frameSequence = descriptor.frameSeq;
    source.operation = GraphicsImageWriterOperation::Copy;
    auto plan = planGraphicsImageWrite(source, failure);
    if (!plan) return plan;
    GraphicsImageWriteLayout layout = plan.value();
    layout.sourceStride = fit.sourceStride;
    return layout;
}

void noteAnomaly(
    FramegrabberDetail::FrameAnomalies* const anomalies,
    const FramegrabberDetail::FrameAnomalyReason reason,
    const FramegrabberDetail::CaptureFrameDescriptor& descriptor,
    const GraphicsImageWriteLayout& layout) noexcept
{
    if (!anomalies) return;
    FramegrabberDetail::FrameAnomalyEvidence evidence;
    evidence.reason = reason;
    evidence.width = static_cast<std::uint32_t>(layout.width);
    evidence.height = static_cast<std::uint32_t>(layout.height);
    evidence.sourceStride = descriptor.stride > 0 ? static_cast<std::uint64_t>(descriptor.stride) : 0U;
    evidence.plannedBytes = layout.bytes;
    evidence.readableBytes = descriptor.size;
    evidence.sourceFormat = descriptor.fgFormat;
    anomalies->add(evidence);
}
}

GraphicsResult<GraphicsImageWriteLayout> planImage(
    const FramegrabberDetail::CaptureFrameDescriptor& descriptor,
    GraphicsImageFailure* const failure,
    FramegrabberDetail::FrameAnomalies* const anomalies) noexcept
{
    using Reason = FramegrabberDetail::FrameAnomalyReason;
    if (failure) *failure = GraphicsImageFailure::None;
    // Zero bytes is the only input without data to publish; everything else is fitted.
    if (descriptor.size == 0U)
    {
        if (failure) *failure = GraphicsImageFailure::MissingBytes;
        return GraphicsFrameError::InvalidInput;
    }

    const GraphicsImagePixelFormat declared = ::graphicsPixelFormat(descriptor.pixelFormat);
    GraphicsImagePixelFormat format = declared;
    bool formatMapped = false;
    if (!hasLayout(format))
    {
        format = nearestFormatForBits(descriptor.bitsPerPixel);
        formatMapped = hasLayout(format);
    }

    // First attempt: keep the delivered geometry in the (mapped) format, widening the stride or
    // trimming a column only where the format demands it.
    if (hasLayout(format) && descriptor.width > 0 && descriptor.height > 0 && descriptor.stride > 0)
    {
        FittedImage fit;
        fit.format = format;
        fit.width = descriptor.width;
        fit.height = descriptor.height;
        bool trimmed = false;
        if (GraphicsImageValidation::requiresEvenWidth(format) && (fit.width & 1) != 0)
        {
            --fit.width;
            trimmed = true;
        }
        const std::size_t minimumRow = fit.width > 0
            ? GraphicsImageValidation::minimumRowBytes(format, fit.width) : 0U;
        const auto sourceStride = static_cast<std::size_t>(descriptor.stride);
        const bool padded = minimumRow != 0U && sourceStride < minimumRow;
        fit.sourceStride = sourceStride;
        fit.destinationStride = padded ? minimumRow : sourceStride;
        if (minimumRow != 0U)
        {
            GraphicsImageFailure planFailure = GraphicsImageFailure::None;
            const auto plan = planFit(fit, descriptor, &planFailure);
            if (plan)
            {
                if (formatMapped) noteAnomaly(anomalies, Reason::UnknownFormatMapped, descriptor, plan.value());
                if (trimmed) noteAnomaly(anomalies, Reason::OddWidthTrimmed, descriptor, plan.value());
                if (padded) noteAnomaly(anomalies, Reason::StridePadded, descriptor, plan.value());
                return plan;
            }
        }
    }

    // Last fit: no layout matches the delivered bytes, so publish them unchanged as Mono8 rows.
    GraphicsImageFailure rawFailure = GraphicsImageFailure::ExtentOverflow;
    const auto raw = rawFit(descriptor);
    if (raw)
    {
        const auto plan = planFit(*raw, descriptor, &rawFailure);
        if (plan)
        {
            noteAnomaly(anomalies, hasLayout(format) ? Reason::InvalidGeometryRaw : Reason::UnknownFormatRaw,
                descriptor, plan.value());
            return plan;
        }
    }
    if (failure) *failure = rawFailure;
    return GraphicsFrameError::InvalidInput;
}

GraphicsResult<GraphicsFrame> captureFrame(
    GraphicsFrameContextSource& contextSource,
    const FramegrabberDetail::CaptureFrameDescriptor& descriptor,
    FramegrabberDetail::CaptureCopyGate& copyGate,
    GraphicsImageFailure* const outImageFailure,
    GraphicsImage* const rejectedImage,
    const std::atomic<bool>* const cancelled,
    const std::atomic<bool>* const externalCancelled)
{
    if (outImageFailure) *outImageFailure = GraphicsImageFailure::None;
    const auto isCancelled = [cancelled, externalCancelled]() noexcept {
        return (cancelled && cancelled->load(std::memory_order_acquire))
            || (externalCancelled && externalCancelled->load(std::memory_order_acquire));
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
    const auto plan = planImage(descriptor, &planFailure, &copyGate.anomalies);
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
    FramegrabberDetail::CaptureCopyPlan copyPlan;
    copyPlan.capacity = layout.bytes;
    copyPlan.destinationStride = static_cast<std::size_t>(layout.strideBytes);
    copyPlan.rows = static_cast<std::size_t>(layout.height);
    copyPlan.sourceStride = layout.sourceStride;
    if (!copyGate.copyAndVerify(region.data(), copyPlan))
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
