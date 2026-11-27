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

#include <cstddef>
#include <cstdint>
#include <functional>

namespace FramegrabberDetail
{

/** @brief Validated SDK DMA frame descriptor passed synchronously before sample copy. */
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

/**
 * @brief Synchronous copy and ACQ_STANDARD overwrite-check gate valid only for one frame call.
 *
 * No raw DMA pointer leaves Framegrabber; the callee passes its writable destination buffer
 * and capacity, and Framegrabber copies once and verifies Fg_getStatusEx before returning.
 */
class CaptureCopyGate
{
public:
    virtual ~CaptureCopyGate() = default;
    [[nodiscard]] virtual bool copyAndVerify(std::uint8_t* destination, std::size_t capacity) noexcept = 0;
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
};

} // namespace FramegrabberDetail
