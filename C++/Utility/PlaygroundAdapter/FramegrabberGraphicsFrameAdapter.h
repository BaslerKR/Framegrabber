#pragma once

#include "Framegrabber.h"
#include "FramegrabberCaptureTransaction.h"
#include "engine/GraphicsFrameBuilder.h"
#include "engine/GraphicsFrameContext.h"
#include "engine/GraphicsImageWriter.h"

#include <atomic>
#include <cstddef>

namespace FramegrabberGraphicsFrameAdapter
{
/** Maps a Framegrabber pixel format to GraphicsEngine's pixel format enum. */
[[nodiscard]] GraphicsImagePixelFormat graphicsPixelFormat(
    Framegrabber::PixelFormat format) noexcept;

/** Maps a Framegrabber bit alignment to GraphicsEngine's bit alignment enum. */
[[nodiscard]] GraphicsImageBitAlignment graphicsBitAlignment(
    Framegrabber::BitAlignment alignment) noexcept;

/** Plans the destination layout for @p descriptor via planGraphicsImageWrite(). */
[[nodiscard]] GraphicsResult<GraphicsImageWriteLayout> planImage(
    const FramegrabberDetail::CaptureFrameDescriptor& descriptor,
    GraphicsImageFailure* failure = nullptr) noexcept;

/**
 * @brief Executes the synchronous capture transaction for one DMA frame.
 *
 * Plans via planGraphicsImageWrite(), acquires one context from @p contextSource,
 * reserves one region, performs the single SDK-to-region copy plus overwrite check
 * before seal(), and publishes one immutable GraphicsFrame. Aborts under RAII on
 * overwrite, refusal, invalid layout, or cancellation without publishing.
 */
[[nodiscard]] GraphicsResult<GraphicsFrame> captureFrame(
    GraphicsFrameContextSource& contextSource,
    const FramegrabberDetail::CaptureFrameDescriptor& descriptor,
    FramegrabberDetail::CaptureCopyGate& copyGate,
    GraphicsImageFailure* outImageFailure = nullptr,
    GraphicsImage* rejectedImage = nullptr,
    const std::atomic<bool>* cancelled = nullptr);

/** Maps an owned Framegrabber image into a GraphicsEngine image by borrowing @p image.storage. */
[[nodiscard]] GraphicsImage wrapImage(
    GraphicsFrameBuilder& builder,
    const Framegrabber::Image& image,
    std::size_t frameSeq,
    GraphicsImageFailure* failure = nullptr);

/** Reports the first rule that prevents the wrapped image from being readable. */
[[nodiscard]] GraphicsImageFailure imageFailure(const GraphicsImage& image) noexcept;
}
