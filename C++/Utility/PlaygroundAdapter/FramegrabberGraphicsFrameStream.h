#pragma once

/**
 * @file FramegrabberGraphicsFrameStream.h
 * @brief Owns Framegrabber grab callbacks and publishes owned GraphicsFrame values.
 */

#include "engine/GraphicsFrameAdapter.h"

#include <cstdint>
#include <functional>
#include <memory>

class Framegrabber;

/** @brief SDK capture identity of one delivered frame. */
struct FramegrabberFrameCapture
{
    /** @brief Acquisition run; increases on every grab start. */
    std::uint64_t generation = 0;
    /** @brief SDK picture number within the run, never a module delivery counter. */
    std::uint64_t picture = 0;
};

/** @brief Receives one owned frame, its DMA index and its SDK capture identity. */
using FramegrabberFrameCallback = std::function<void(
    GraphicsFrame&&, unsigned int dmaIndex, const FramegrabberFrameCapture& capture)>;

/** Owns frame-grabber callback registration and emits only owned GraphicsFrame values. */
class FramegrabberGraphicsFrameStream final
{
public:
    FramegrabberGraphicsFrameStream(Framegrabber* framegrabber, FramegrabberFrameCallback callback);
    ~FramegrabberGraphicsFrameStream();

    FramegrabberGraphicsFrameStream(const FramegrabberGraphicsFrameStream&) = delete;
    FramegrabberGraphicsFrameStream& operator=(const FramegrabberGraphicsFrameStream&) = delete;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};
