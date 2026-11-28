#include "FramegrabberImageLayout.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

namespace {

using FramegrabberDetail::FrameAnomalies;
using FramegrabberDetail::FrameAnomalyReason;
using FramegrabberDetail::PayloadInput;
using FramegrabberDetail::PayloadLayout;
using FramegrabberDetail::copyPayloadRows;
using FramegrabberDetail::resolvePayloadLayout;

int failures = 0;

void expect(const bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

PayloadInput input(const std::size_t transfer, const int height, const int bits, const int width,
                   const std::size_t frameBytes, const bool known = true)
{
    PayloadInput value;
    value.transferKnown = known;
    value.transferLength = transfer;
    value.height = height;
    value.pixelBits = bits;
    value.configuredWidth = width;
    value.frameBytes = frameBytes;
    value.sourceFormat = 7;
    return value;
}

}

int main()
{
    {
        FrameAnomalies anomalies;
        const auto layout = resolvePayloadLayout(input(8, 2, 8, 4, 8), anomalies);
        expect(layout.hasData && layout.bytes == 8 && layout.stride == 4 && layout.width == 4
                   && layout.readable == 8,
            "packed Mono8 layout preserves geometry");
        expect(anomalies.total() == 0, "a complete transfer records no anomaly");
    }
    {
        FrameAnomalies anomalies;
        const auto layout = resolvePayloadLayout(input(15, 3, 10, 4, 16), anomalies);
        expect(layout.hasData && layout.bytes == 15 && layout.stride == 5 && layout.width == 4,
            "packed 10-bit layout derives four pixels");
    }
    {
        FrameAnomalies anomalies;
        const auto layout = resolvePayloadLayout(input(12, 2, 8, 8, 16), anomalies);
        expect(layout.hasData && layout.stride == 6 && layout.width == 6,
            "padded rows publish the delivered ROI width");
        expect(anomalies.countOf(FrameAnomalyReason::GeometryNarrowed) == 1,
            "a narrowed ROI is counted");
    }
    {
        // A row smaller than one pixel used to be refused; it is planned at the declared size.
        FrameAnomalies anomalies;
        const auto layout = resolvePayloadLayout(input(3, 3, 16, 4, 24), anomalies);
        expect(layout.hasData && layout.bytes == 24 && layout.stride == 8 && layout.width == 4
                   && layout.readable == 3,
            "a row below one pixel is planned at the declared geometry");
        expect(anomalies.countOf(FrameAnomalyReason::RowSmallerThanPixel) == 1
                   && anomalies.countOf(FrameAnomalyReason::ShortTransfer) == 1,
            "a row below one pixel is counted as a short transfer");
    }
    {
        // A partial row used to be refused; the bytes that exist are kept and the rest is zero.
        FrameAnomalies anomalies;
        const auto layout = resolvePayloadLayout(input(7, 2, 8, 4, 8), anomalies);
        expect(layout.hasData && layout.bytes == 8 && layout.stride == 4 && layout.readable == 7,
            "a partial row is planned at the declared size");
        expect(anomalies.countOf(FrameAnomalyReason::PartialRowLength) == 1
                   && anomalies.countOf(FrameAnomalyReason::ShortTransfer) == 1,
            "a partial row is counted");
    }
    {
        // A transfer larger than the DMA buffer is read only up to the buffer end.
        FrameAnomalies anomalies;
        const auto layout = resolvePayloadLayout(input(9, 3, 8, 4, 8), anomalies);
        expect(layout.hasData && layout.bytes == 8 && layout.readable == 8 && layout.width == 4,
            "an oversized transfer is clamped to the buffer");
        expect(anomalies.countOf(FrameAnomalyReason::TransferExceedsBuffer) == 1,
            "an oversized transfer is counted");
    }
    {
        // An unreadable length assumes the declared frame is present.
        FrameAnomalies anomalies;
        const auto layout = resolvePayloadLayout(input(0, 2, 8, 4, 8, false), anomalies);
        expect(layout.hasData && layout.bytes == 8 && layout.readable == 8,
            "an unreadable transfer length uses the declared frame");
        expect(anomalies.countOf(FrameAnomalyReason::TransferLengthUnavailable) == 1,
            "an unreadable transfer length is counted");
    }
    {
        // A transfer that genuinely has zero bytes is the only data-less frame.
        FrameAnomalies anomalies;
        const auto layout = resolvePayloadLayout(input(0, 2, 8, 4, 8), anomalies);
        expect(!layout.hasData, "a zero-byte transfer has no data");
    }
    {
        FrameAnomalies anomalies;
        expect(!resolvePayloadLayout(input(8, 0, 8, 4, 8), anomalies).hasData,
            "a zero-height geometry has no layout");
        expect(!resolvePayloadLayout(input(8, 2, 0, 4, 8), anomalies).hasData,
            "a zero-bit geometry has no layout");
        expect(!resolvePayloadLayout(input(8, 2, 8, 0, 8), anomalies).hasData,
            "a zero-width geometry has no layout");
    }
    {
        FrameAnomalies anomalies;
        FramegrabberDetail::FrameAnomalyEvidence evidence;
        for (std::size_t index = 0; index < FrameAnomalies::capacity + 3; ++index)
        {
            evidence.reason = FrameAnomalyReason::ShortTransfer;
            anomalies.add(evidence);
        }
        evidence.reason = FrameAnomalyReason::StridePadded;
        anomalies.add(evidence);
        expect(anomalies.count == FrameAnomalies::capacity && anomalies.overflow == 4,
            "the record keeps its fixed capacity and counts overflow");
        expect(anomalies.countOf(FrameAnomalyReason::ShortTransfer) == FrameAnomalies::capacity + 3
                   && anomalies.countOf(FrameAnomalyReason::StridePadded) == 1
                   && anomalies.total() == FrameAnomalies::capacity + 4,
            "overflow is counted per reason");
    }
    {
        // Equal strides: bytes that exist are copied, the rest is zero.
        std::vector<std::uint8_t> destination(8, 0xAA);
        const std::vector<std::uint8_t> source{1, 2, 3, 4, 5, 6, 7, 99};
        copyPayloadRows(destination.data(), 4, 2, source.data(), 4, 7);
        expect(destination == std::vector<std::uint8_t>({1, 2, 3, 4, 5, 6, 7, 0}),
            "a short flat copy zero-fills the missing tail and never reads past readable");
    }
    {
        // Wider destination stride: each row is padded with zeros.
        std::vector<std::uint8_t> destination(12, 0xAA);
        const std::vector<std::uint8_t> source{1, 2, 3, 4, 5, 6};
        copyPayloadRows(destination.data(), 6, 2, source.data(), 3, 6);
        expect(destination == std::vector<std::uint8_t>({1, 2, 3, 0, 0, 0, 4, 5, 6, 0, 0, 0}),
            "rows are copied into a wider stride with zero padding");
    }
    {
        // Rows past the readable bytes are entirely zero.
        std::vector<std::uint8_t> destination(12, 0xAA);
        const std::vector<std::uint8_t> source{1, 2, 3, 4};
        copyPayloadRows(destination.data(), 4, 3, source.data(), 2, 4);
        expect(destination == std::vector<std::uint8_t>({1, 2, 0, 0, 3, 4, 0, 0, 0, 0, 0, 0}),
            "a narrower source stride keeps rows apart and zero-fills the rest");
    }

    if (failures != 0)
    {
        std::cerr << failures << " image layout test(s) failed.\n";
        return 1;
    }
    std::cout << "DMA payload fitting and zero-fill contracts passed.\n";
    return 0;
}
