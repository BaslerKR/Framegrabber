#pragma once

#include "Framegrabber.h"
#include "engine/GraphicsImage.h"

#include <cstddef>

namespace FramegrabberGraphicsImageAdapter
{
[[nodiscard]] GraphicsImage wrapImage(
    const Framegrabber::Image& image,
    std::size_t frameSeq) noexcept;
}
