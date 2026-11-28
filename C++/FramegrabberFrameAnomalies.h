#pragma once

/**
 * @file FramegrabberFrameAnomalies.h
 * @brief Bounded, allocation-free record of the data anomalies fitted into one DMA frame.
 *
 * Playground never rejects device data for a validation reason: a short, padded, oversized or
 * unknown-format transfer is fitted into a valid image layout and published. Each fit that
 * changed what the device delivered is recorded here instead, with fixed numeric evidence only.
 * The record is a plain value with fixed capacity, so the per-frame path allocates nothing.
 */

#include <array>
#include <cstddef>
#include <cstdint>

namespace FramegrabberDetail
{

/** @brief What a fit changed relative to what the device delivered. */
enum class FrameAnomalyReason : std::uint8_t
{
    /** FG_TRANSFER_LEN could not be read; the declared frame size was assumed readable. */
    TransferLengthUnavailable,
    /** The transfer is larger than the DMA buffer; only the buffer's bytes were read. */
    TransferExceedsBuffer,
    /** Fewer bytes arrived than the planned image holds; the rest is zero-filled. */
    ShortTransfer,
    /** The transfer length is not a whole number of rows; the declared geometry was used. */
    PartialRowLength,
    /** A delivered row is smaller than one pixel; the declared geometry was used. */
    RowSmallerThanPixel,
    /** Rows are padded or narrower than configured; the delivered width was published. */
    GeometryNarrowed,
    /** The pixel format has no GraphicsEngine layout; the nearest format by bit depth was used. */
    UnknownFormatMapped,
    /** No GraphicsEngine format fits; the raw bytes were published as Mono8. */
    UnknownFormatRaw,
    /** Width, height or stride was unusable; the raw bytes were published as Mono8 rows. */
    InvalidGeometryRaw,
    /** The source stride is below the format's row size; rows were copied into a wider stride. */
    StridePadded,
    /** A YCbCr 4:2:2 image has an odd width; the last column was trimmed. */
    OddWidthTrimmed
};

inline constexpr std::size_t kFrameAnomalyReasonCount =
    static_cast<std::size_t>(FrameAnomalyReason::OddWidthTrimmed) + 1U;

/** @brief Returns a stable lowercase name for diagnostics. */
[[nodiscard]] constexpr const char* frameAnomalyName(const FrameAnomalyReason reason) noexcept
{
    switch (reason)
    {
    case FrameAnomalyReason::TransferLengthUnavailable: return "transfer_length_unavailable";
    case FrameAnomalyReason::TransferExceedsBuffer: return "transfer_exceeds_buffer";
    case FrameAnomalyReason::ShortTransfer: return "short_transfer";
    case FrameAnomalyReason::PartialRowLength: return "partial_row_length";
    case FrameAnomalyReason::RowSmallerThanPixel: return "row_smaller_than_pixel";
    case FrameAnomalyReason::GeometryNarrowed: return "geometry_narrowed";
    case FrameAnomalyReason::UnknownFormatMapped: return "unknown_format_mapped";
    case FrameAnomalyReason::UnknownFormatRaw: return "unknown_format_raw";
    case FrameAnomalyReason::InvalidGeometryRaw: return "invalid_geometry_raw";
    case FrameAnomalyReason::StridePadded: return "stride_padded";
    case FrameAnomalyReason::OddWidthTrimmed: return "odd_width_trimmed";
    }
    return "unknown";
}

/** @brief Fixed numeric evidence for one fitted anomaly; no strings and no pixel data. */
struct FrameAnomalyEvidence
{
    FrameAnomalyReason reason = FrameAnomalyReason::ShortTransfer;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t sourceStride = 0;
    std::uint64_t plannedBytes = 0;
    std::uint64_t readableBytes = 0;
    std::int64_t sourceFormat = 0;
};

/** @brief Per-frame anomaly record with fixed capacity; overflow is counted per reason. */
struct FrameAnomalies
{
    static constexpr std::size_t capacity = 8;

    std::array<FrameAnomalyEvidence, capacity> entries{};
    std::size_t count = 0;
    std::array<std::uint32_t, kFrameAnomalyReasonCount> overflowByReason{};
    std::size_t overflow = 0;

    /** @brief Records one anomaly, or counts it as overflow when the record is full. */
    void add(const FrameAnomalyEvidence& evidence) noexcept
    {
        if (count < entries.size())
        {
            entries[count++] = evidence;
            return;
        }
        ++overflow;
        ++overflowByReason[static_cast<std::size_t>(evidence.reason)];
    }

    /** @brief Number of anomalies of @p reason, including overflowed ones. */
    [[nodiscard]] std::size_t countOf(const FrameAnomalyReason reason) const noexcept
    {
        std::size_t total = overflowByReason[static_cast<std::size_t>(reason)];
        for (std::size_t index = 0; index < count; ++index)
        {
            if (entries[index].reason == reason) ++total;
        }
        return total;
    }

    /** @brief Number of anomalies recorded or overflowed. */
    [[nodiscard]] std::size_t total() const noexcept { return count + overflow; }
};

/** @brief Per-reason running totals for one DMA; plain values so they can be copied out. */
using FrameAnomalyTotals = std::array<std::uint64_t, kFrameAnomalyReasonCount>;

}
