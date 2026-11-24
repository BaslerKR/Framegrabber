#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>

namespace FramegrabberDetail
{

enum class PayloadLayoutError
{
    None,
    InvalidTransferLength,
    StrideTooLarge,
    RowTooSmall
};

struct PayloadLayout
{
    std::size_t bytes = 0;
    std::size_t stride = 0;
    int width = 0;
};

inline PayloadLayoutError resolvePayloadLayout(
    const std::size_t transferLength,
    const int height,
    const int pixelBits,
    const int configuredWidth,
    const std::size_t frameBytes,
    PayloadLayout& layout) noexcept
{
    layout = {};
    if (transferLength == 0 || height <= 0 || pixelBits <= 0 || configuredWidth <= 0
        || transferLength > frameBytes || transferLength % static_cast<std::size_t>(height) != 0)
    {
        return PayloadLayoutError::InvalidTransferLength;
    }

    const auto stride = transferLength / static_cast<std::size_t>(height);
    if (stride > static_cast<std::size_t>((std::numeric_limits<int>::max)())
        || stride > (std::numeric_limits<std::size_t>::max)() / 8U)
    {
        return PayloadLayoutError::StrideTooLarge;
    }

    const auto effectiveWidth = stride * 8U / static_cast<std::size_t>(pixelBits);
    if (effectiveWidth == 0)
    {
        return PayloadLayoutError::RowTooSmall;
    }

    layout.bytes = transferLength;
    layout.stride = stride;
    layout.width = static_cast<int>((std::min)(
        static_cast<std::size_t>(configuredWidth), effectiveWidth));
    return PayloadLayoutError::None;
}

}
