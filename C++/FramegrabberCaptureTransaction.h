#pragma once

/**
 * @file FramegrabberCaptureTransaction.h
 * @brief Module-private synchronous capture/destination transaction for Framegrabber.
 *
 * Keeps the core Framegrabber library independent of GraphicsEngine and Playground
 * while allowing the Playground adapter to supply a checked destination buffer,
 * copy once from DMA, and run the ACQ_STANDARD overwrite check before sealing.
 */

#include "Framegrabber.h"
#include "FramegrabberFrameAnomalies.h"

#include <cstddef>
#include <cstdint>
#include <functional>

namespace FramegrabberDetail
{

/**
 * @brief Fitted SDK DMA frame descriptor passed synchronously before sample copy.
 *
 * `size`, `width`, `height` and `stride` describe the layout fitted to the bytes the device
 * delivered; `bitsPerPixel` is the SDK-reported depth and is zero when the SDK reported none.
 * `pixelFormat` may be Unknown or a format without a GraphicsEngine layout; the consumer fits it.
 */
struct CaptureFrameDescriptor
{
    std::size_t size = 0;
    int width = 0;
    int height = 0;
    int stride = 0;
    int bitsPerPixel = 0;
    int bytesPerPixel = 0;
    int fgFormat = 0;
    Framegrabber::PixelFormat pixelFormat = Framegrabber::PixelFormat::Unknown;
    Framegrabber::BitAlignment bitAlignment = Framegrabber::BitAlignment::Packed;
    unsigned int dmaIndex = 0;
    std::uint64_t frameSeq = 0;
};

/** @brief Destination layout the consumer fitted for one frame copy. */
struct CaptureCopyPlan
{
    /** Writable bytes behind the destination; equals `destinationStride * rows`. */
    std::size_t capacity = 0;
    std::size_t destinationStride = 0;
    std::size_t rows = 0;
    /** Source row spacing to read with; equal to `destinationStride` for a linear byte copy. */
    std::size_t sourceStride = 0;
};

/**
 * @brief Synchronous copy and ACQ_STANDARD overwrite-check gate valid only for one frame call.
 *
 * No raw DMA pointer leaves Framegrabber; the callee passes its writable destination buffer
 * and the destination layout it fitted, and Framegrabber copies once row by row, reading only
 * bytes the SDK buffer holds and zero-filling the rest, then verifies Fg_getStatusEx before
 * returning. The gate also carries the frame's anomaly record: Framegrabber seeds it with the
 * transfer fits and the consumer appends its own before the call returns.
 */
class CaptureCopyGate
{
public:
    virtual ~CaptureCopyGate() = default;
    [[nodiscard]] virtual bool copyAndVerify(
        std::uint8_t* destination, const CaptureCopyPlan& plan) noexcept = 0;

    /** @brief Fixed-capacity anomalies fitted into this frame; counted after the call. */
    FrameAnomalies anomalies;
};

enum class CaptureTransactionOutcome : std::uint8_t
{
    Delivered,
    DroppedNoCredit,
    DroppedStorageRefused,
    DroppedInvalidOrFailed,
    AbortedStop
};

using CaptureTransactionFn = std::function<CaptureTransactionOutcome(
    const CaptureFrameDescriptor& descriptor,
    CaptureCopyGate& copyGate)>;

struct CaptureTransactionSlot
{
    CaptureTransactionFn fn;
};

struct CaptureTransactionAccess
{
    static void enableManagedCaptureMode(Framegrabber& grabber) noexcept;
    static void setCaptureTransaction(
        Framegrabber& grabber,
        CaptureTransactionFn transaction);
    static void clearCaptureTransaction(Framegrabber& grabber) noexcept;
    static std::shared_ptr<const CaptureTransactionSlot> snapshot(Framegrabber& grabber) noexcept;
};

} // namespace FramegrabberDetail
