#pragma once

#include "Framegrabber.h"
#include "engine/GraphicsFrameBuilder.h"

#include <cstddef>

namespace FramegrabberGraphicsFrameAdapter
{
/** Maps an owned Framegrabber image into a GraphicsEngine image by borrowing @p image.storage. */
[[nodiscard]] GraphicsImage wrapImage(
    GraphicsFrameBuilder& builder,
    const Framegrabber::Image& image,
    std::size_t frameSeq,
    GraphicsImageFailure* failure = nullptr);

/** Reports the first rule that prevents the wrapped image from being readable. */
[[nodiscard]] GraphicsImageFailure imageFailure(const GraphicsImage& image) noexcept;
}
