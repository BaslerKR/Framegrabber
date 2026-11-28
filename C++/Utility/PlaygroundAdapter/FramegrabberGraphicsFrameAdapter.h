#pragma once

#include "engine/GraphicsFrameBuilder.h"
#include "engine/GraphicsFrameContext.h"
#include "engine/GraphicsImageWriter.h"
#include "Framegrabber.h"
#include "FramegrabberCaptureTransaction.h"

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

/**
 * @brief Fits @p descriptor into a GraphicsEngine image layout and plans it.
 *
 * Device data is never refused for a validation reason: an unknown format maps to the nearest
 * GraphicsEngine format by bit depth (otherwise the raw bytes become Mono8), a stride below the
 * format's row size is widened, an odd YCbCr 4:2:2 width loses its last column, and an unusable
 * geometry falls back to Mono8 rows. Each fit is appended to @p anomalies. Only a descriptor
 * with zero bytes, or one whose bytes cannot be addressed by an int-sized layout, fails.
 * `sourceStride` of the returned layout is the row spacing to read the delivered bytes with.
 */
[[nodiscard]] GraphicsResult<GraphicsImageWriteLayout> planImage(
    const FramegrabberDetail::CaptureFrameDescriptor& descriptor,
    GraphicsImageFailure* failure = nullptr,
    FramegrabberDetail::FrameAnomalies* anomalies = nullptr) noexcept;

/**
 * @brief Executes the synchronous capture transaction for one DMA frame.
 *
 * Fits the layout via planImage(), acquires one context from @p contextSource,
 * reserves one region, performs the single SDK-to-region copy plus overwrite check
 * before seal(), and publishes one immutable GraphicsFrame. The fits are appended to
 * `copyGate.anomalies`. Aborts under RAII on overwrite, storage refusal, zero bytes,
 * or cancellation without publishing.
 */
[[nodiscard]] GraphicsResult<GraphicsFrame> captureFrame(
    GraphicsFrameContextSource& contextSource,
    const FramegrabberDetail::CaptureFrameDescriptor& descriptor,
    FramegrabberDetail::CaptureCopyGate& copyGate,
    GraphicsImageFailure* outImageFailure = nullptr,
    GraphicsImage* rejectedImage = nullptr,
    const std::atomic<bool>* cancelled = nullptr,
    const std::atomic<bool>* externalCancelled = nullptr);

/** Reports the first rule that prevents the captured image from being readable. */
[[nodiscard]] GraphicsImageFailure imageFailure(const GraphicsImage& image) noexcept;
}
