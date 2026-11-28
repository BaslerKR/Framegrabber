#pragma once

#include "FramegrabberFrameAnomalies.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace FramegrabberDetail
{

/** @brief Geometry and transfer facts of one DMA frame, as the SDK reported them. */
struct PayloadInput
{
    /** False when FG_TRANSFER_LEN could not be read for this frame. */
    bool transferKnown = false;
    std::size_t transferLength = 0;
    int height = 0;
    /** Bits per pixel used to size the DMA buffer; always positive. */
    int pixelBits = 0;
    int configuredWidth = 0;
    /** Bytes of the DMA buffer behind this frame: configured width * pixel bytes * height. */
    std::size_t frameBytes = 0;
    std::int64_t sourceFormat = 0;
};

/**
 * @brief Image layout fitted to one DMA frame; it never rejects delivered bytes.
 *
 * `bytes` is the size of the planned image (`stride * height`), `readable` the number of those
 * bytes the SDK buffer actually holds. The rest is zero-filled by copyPayloadRows().
 */
struct PayloadLayout
{
    std::size_t bytes = 0;
    std::size_t readable = 0;
    std::size_t stride = 0;
    int width = 0;
    /** False only when the device delivered zero bytes or the geometry is unusable. */
    bool hasData = false;
};

/**
 * @brief Fits the delivered transfer into a valid layout and records every fit in @p anomalies.
 *
 * A transfer that is a whole number of rows is published at its delivered stride, narrowing the
 * width when the rows are padded or the ROI is narrower than configured. Any other transfer
 * (partial row, row below one pixel, unreadable length) is planned at the declared geometry: the
 * bytes that exist are kept and the rest is zero-filled. A transfer larger than the DMA buffer is
 * read only up to the buffer end. `hasData` is false only for a transfer that genuinely has zero
 * bytes or for a geometry that violates the caller's preconditions.
 */
inline PayloadLayout resolvePayloadLayout(const PayloadInput& input, FrameAnomalies& anomalies) noexcept
{
    PayloadLayout layout;
    if (input.height <= 0 || input.pixelBits <= 0 || input.configuredWidth <= 0
        || input.frameBytes == 0 || (input.transferKnown && input.transferLength == 0))
    {
        return layout;
    }
    const auto height = static_cast<std::size_t>(input.height);
    const std::size_t declaredStride = input.frameBytes / height;
    if (declaredStride == 0 || declaredStride > static_cast<std::size_t>((std::numeric_limits<int>::max)())
        || declaredStride > (std::numeric_limits<std::size_t>::max)() / 8U)
    {
        return layout;
    }

    const auto note = [&](const FrameAnomalyReason reason, const std::size_t planned,
                          const std::size_t readable, const std::size_t stride, const int width) {
        FrameAnomalyEvidence evidence;
        evidence.reason = reason;
        evidence.width = static_cast<std::uint32_t>(width > 0 ? width : 0);
        evidence.height = static_cast<std::uint32_t>(input.height);
        evidence.sourceStride = stride;
        evidence.plannedBytes = planned;
        evidence.readableBytes = readable;
        evidence.sourceFormat = input.sourceFormat;
        anomalies.add(evidence);
    };

    std::size_t readable = input.frameBytes;
    if (!input.transferKnown)
    {
        note(FrameAnomalyReason::TransferLengthUnavailable, input.frameBytes, readable,
             declaredStride, input.configuredWidth);
    }
    else if (input.transferLength > input.frameBytes)
    {
        readable = input.frameBytes;
        note(FrameAnomalyReason::TransferExceedsBuffer, input.frameBytes, readable,
             declaredStride, input.configuredWidth);
    }
    else
    {
        readable = input.transferLength;
    }

    const std::size_t wholeRowStride = readable % height == 0 ? readable / height : 0;
    const std::size_t effectiveWidth =
        wholeRowStride * 8U / static_cast<std::size_t>(input.pixelBits);
    if (wholeRowStride != 0 && effectiveWidth != 0)
    {
        layout.stride = wholeRowStride;
        layout.bytes = readable;
        layout.width = static_cast<int>((std::min)(
            static_cast<std::size_t>(input.configuredWidth), effectiveWidth));
        if (layout.width < input.configuredWidth)
        {
            note(FrameAnomalyReason::GeometryNarrowed, layout.bytes, readable, layout.stride,
                 layout.width);
        }
    }
    else
    {
        layout.stride = declaredStride;
        layout.bytes = input.frameBytes;
        layout.width = input.configuredWidth;
        note(readable % height != 0 ? FrameAnomalyReason::PartialRowLength
                                    : FrameAnomalyReason::RowSmallerThanPixel,
             layout.bytes, readable, layout.stride, layout.width);
        note(FrameAnomalyReason::ShortTransfer, layout.bytes, readable, layout.stride, layout.width);
    }
    layout.readable = readable;
    layout.hasData = true;
    return layout;
}

/**
 * @brief Copies source rows into destination rows of another stride and zero-fills the rest.
 *
 * Reads at most @p readable bytes from @p source and never past `readable`; every destination
 * byte is written exactly once. Equal strides take a single flat copy.
 */
inline void copyPayloadRows(std::uint8_t* destination, const std::size_t destinationStride,
                            const std::size_t rows, const std::uint8_t* source,
                            const std::size_t sourceStride, const std::size_t readable) noexcept
{
    if (destinationStride == sourceStride)
    {
        const std::size_t total = destinationStride * rows;
        const std::size_t copied = (std::min)(readable, total);
        if (copied != 0) std::memcpy(destination, source, copied);
        if (copied < total) std::memset(destination + copied, 0, total - copied);
        return;
    }
    for (std::size_t row = 0; row < rows; ++row)
    {
        std::uint8_t* const out = destination + row * destinationStride;
        const std::size_t offset = row * sourceStride;
        const std::size_t available = offset < readable ? readable - offset : 0;
        const std::size_t copied = (std::min)({available, sourceStride, destinationStride});
        if (copied != 0) std::memcpy(out, source + offset, copied);
        if (copied < destinationStride) std::memset(out + copied, 0, destinationStride - copied);
    }
}

}
