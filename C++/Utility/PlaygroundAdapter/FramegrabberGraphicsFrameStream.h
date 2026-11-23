#pragma once

/**
 * @file FramegrabberGraphicsFrameStream.h
 * @brief Owns Framegrabber grab callbacks and publishes owned GraphicsFrame values.
 */

#include "engine/GraphicsFrameAdapter.h"

#include <functional>
#include <memory>

class Framegrabber;

/** Owns frame-grabber callback registration and emits only owned GraphicsFrame values. */
class FramegrabberGraphicsFrameStream final
{
public:
    using RejectedImage = std::function<void(const GraphicsImage&, unsigned int, GraphicsImageFailure)>;

    FramegrabberGraphicsFrameStream(Framegrabber* framegrabber, GraphicsFrameCallback callback,
                                   RejectedImage rejectedImage = {});
    ~FramegrabberGraphicsFrameStream();

    FramegrabberGraphicsFrameStream(const FramegrabberGraphicsFrameStream&) = delete;
    FramegrabberGraphicsFrameStream& operator=(const FramegrabberGraphicsFrameStream&) = delete;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};
