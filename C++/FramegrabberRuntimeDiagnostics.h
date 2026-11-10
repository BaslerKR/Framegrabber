#pragma once

#include "Framegrabber.h"

namespace FramegrabberDetail {

/** @brief Per-DMA telemetry only; no acquisition completion or buffer policy state. */
class ProducerRuntimeCounters {
public:
    /** @brief Counts one frame only if at least one consumer returned successfully. */
    void noteFrameDelivered(bool consumerSucceeded) noexcept {
        if (consumerSucceeded) _delivered.fetch_add(1, std::memory_order_relaxed);
    }
    /** @brief Counts one callback failure independently of sibling consumer success. */
    void noteFailure() noexcept { _failed.fetch_add(1, std::memory_order_relaxed); }
    /** @brief Excludes idle polls and intentional shutdown from failed grab results. */
    void noteWaitResult(std::int64_t result, bool stopping) noexcept {
        if (result <= 0 && result != FG_NO_PICTURE_AVAILABLE && result != FG_TIMEOUT_ERR && !stopping)
            noteFailure();
    }
    /** @brief Copies successful consumer deliveries in the current run. */
    [[nodiscard]] std::uint64_t delivered() const noexcept { return _delivered.load(std::memory_order_relaxed); }
    /** @brief Copies failed grabs and callbacks in the current run. */
    [[nodiscard]] std::uint64_t failed() const noexcept { return _failed.load(std::memory_order_relaxed); }
private:
    std::atomic<std::uint64_t> _delivered{0};
    std::atomic<std::uint64_t> _failed{0};
};

/** @brief Sums producer outcomes for one DMA or all DMAs, excluding buffer-policy drops. */
inline Framegrabber::RuntimeDiagnostics sumRuntimeDiagnostics(bool grabbing,
    const std::vector<Framegrabber::ChannelStatistics>& channels, std::optional<unsigned> dmaIndex = {})
{
    Framegrabber::RuntimeDiagnostics value{grabbing, 0, 0};
    for (const auto& channel : channels) {
        if (dmaIndex && channel.dmaIndex != *dmaIndex) continue;
        value.delivered += channel.consumerDelivered;
        value.failed += channel.failed + channel.rejected;
    }
    return value;
}

} // namespace FramegrabberDetail
