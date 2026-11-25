#pragma once

#include <atomic>
#include <thread>

namespace FramegrabberDetail
{

inline bool acquisitionRunStopped(
    const unsigned int previousActiveChannels,
    const std::atomic<unsigned int>& activeChannels,
    const std::atomic<bool>& startingChannels) noexcept
{
    if (previousActiveChannels != 1U) return false;
    while (startingChannels.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }
    return activeChannels.load(std::memory_order_acquire) == 0U;
}

}
