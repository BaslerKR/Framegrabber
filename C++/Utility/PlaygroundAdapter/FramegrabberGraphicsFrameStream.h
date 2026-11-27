#pragma once

/**
 * @file FramegrabberGraphicsFrameStream.h
 * @brief Owns Framegrabber grab callbacks and publishes owned GraphicsFrame values.
 */

#include "engine/GraphicsFrameAdapter.h"
#include "engine/GraphicsFrameContext.h"

#include <atomic>
#include <functional>
#include <memory>

class Framegrabber;

/** Owns frame-grabber callback registration and emits only owned GraphicsFrame values. */
class FramegrabberGraphicsFrameStream final
{
public:
    using RejectedImage = std::function<void(const GraphicsImage&, unsigned int, GraphicsImageFailure, std::size_t)>;
    using StorageOutcome = std::function<void(GraphicsFrameError)>;

    FramegrabberGraphicsFrameStream(
        Framegrabber* framegrabber,
        std::shared_ptr<GraphicsFrameContextSource> contextSource,
        GraphicsFrameCallback callback,
        RejectedImage rejectedImage = {},
        StorageOutcome storageOutcome = {},
        std::shared_ptr<const std::atomic<bool>> cancelled = {});
    ~FramegrabberGraphicsFrameStream();

    FramegrabberGraphicsFrameStream(const FramegrabberGraphicsFrameStream&) = delete;
    FramegrabberGraphicsFrameStream& operator=(const FramegrabberGraphicsFrameStream&) = delete;

    /** @brief Cancels in-flight capture/conversion before seal and publication. */
    void cancel() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};
