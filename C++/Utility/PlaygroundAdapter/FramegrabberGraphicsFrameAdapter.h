#pragma once

#include "Framegrabber.h"
#include "engine/GraphicsFrameAdapter.h"

#include <cstddef>

namespace FramegrabberGraphicsFrameAdapter
{
/** Maps an owned Framegrabber image into a GraphicsEngine image value. */
[[nodiscard]] GraphicsImage wrapImage(
    const Framegrabber::Image& image,
    std::size_t frameSeq) noexcept;

/** Reports the first rule that prevents the wrapped image from being readable. */
[[nodiscard]] GraphicsImageFailure imageFailure(const GraphicsImage& image) noexcept;
}
