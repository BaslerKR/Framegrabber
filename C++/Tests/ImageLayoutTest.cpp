#include "FramegrabberImageLayout.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>

namespace {

using FramegrabberDetail::PayloadLayout;
using FramegrabberDetail::PayloadLayoutError;
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

}

int main()
{
    PayloadLayout layout;
    expect(resolvePayloadLayout(8, 2, 8, 4, 8, layout) == PayloadLayoutError::None,
        "packed Mono8 layout is valid");
    expect(layout.bytes == 8 && layout.stride == 4 && layout.width == 4,
        "packed Mono8 layout preserves geometry");

    expect(resolvePayloadLayout(15, 3, 10, 4, 16, layout) == PayloadLayoutError::None,
        "packed 10-bit layout is valid");
    expect(layout.bytes == 15 && layout.stride == 5 && layout.width == 4,
        "packed 10-bit layout derives four pixels");

    expect(resolvePayloadLayout(12, 2, 8, 8, 16, layout) == PayloadLayoutError::None,
        "padded rows are valid");
    expect(layout.stride == 6 && layout.width == 6,
        "padded rows publish the delivered ROI width");

    expect(resolvePayloadLayout(3, 3, 16, 4, 16, layout) == PayloadLayoutError::RowTooSmall,
        "a row smaller than one pixel is refused");

    expect(resolvePayloadLayout(7, 2, 8, 4, 8, layout) == PayloadLayoutError::InvalidTransferLength,
        "a partial row is refused");

    expect(resolvePayloadLayout(9, 3, 8, 4, 8, layout) == PayloadLayoutError::InvalidTransferLength,
        "a transfer larger than the allocated frame is refused");

    expect(resolvePayloadLayout(0, 2, 8, 4, 8, layout) == PayloadLayoutError::InvalidTransferLength,
        "an empty transfer is refused");

    expect(resolvePayloadLayout(8, 0, 8, 4, 8, layout) == PayloadLayoutError::InvalidTransferLength,
        "a zero-height transfer is refused");

    expect(resolvePayloadLayout(8, 2, 0, 4, 8, layout) == PayloadLayoutError::InvalidTransferLength,
        "a zero-bit pixel layout is refused");

    expect(resolvePayloadLayout(8, 2, 8, 0, 8, layout) == PayloadLayoutError::InvalidTransferLength,
        "a zero-width configuration is refused");

    const auto huge = (std::numeric_limits<std::size_t>::max)();
    expect(resolvePayloadLayout(huge, 1, 8, 4, huge, layout) == PayloadLayoutError::StrideTooLarge,
        "an overflowing stride is refused");

    if (failures != 0)
    {
        std::cerr << failures << " image layout test(s) failed.\n";
        return 1;
    }
    std::cout << "DMA payload layout and overflow contracts passed.\n";
    return 0;
}
