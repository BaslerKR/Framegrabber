#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace FramegrabberDetail
{
/**
 * @brief Non-blocking owned-frame buffer pool.
 *
 * acquire() never waits. An empty pool returns an empty lease so the
 * acquisition worker can drop the frame and count the loss.
 */
class OwnedBufferPool final
    : public std::enable_shared_from_this<OwnedBufferPool>
{
public:
    using Lease = std::shared_ptr<std::uint8_t>;

    static std::shared_ptr<OwnedBufferPool> create(
        const std::size_t bufferSize,
        const std::size_t bufferCount)
    {
        auto pool = std::shared_ptr<OwnedBufferPool>(
            new OwnedBufferPool);
        pool->_available.reserve(bufferCount);
        for (std::size_t index = 0; index < bufferCount; ++index)
        {
            pool->_available.push_back(
                std::unique_ptr<std::uint8_t[]>(
                    new std::uint8_t[bufferSize]));
        }
        return pool;
    }

    Lease acquire()
    {
        std::unique_ptr<std::uint8_t[]> buffer;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_stopped || _available.empty())
            {
                return {};
            }
            buffer = std::move(_available.back());
            _available.pop_back();
        }

        std::uint8_t* data = buffer.release();
        const std::weak_ptr<OwnedBufferPool> weakPool = shared_from_this();
        return Lease(data, [weakPool](std::uint8_t* released)
        {
            if (const std::shared_ptr<OwnedBufferPool> pool = weakPool.lock())
            {
                pool->release(released);
            }
            else
            {
                delete[] released;
            }
        });
    }

    void stop()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _stopped = true;
    }

private:
    OwnedBufferPool() = default;

    void release(std::uint8_t* data)
    {
        if (!data)
        {
            return;
        }
        std::lock_guard<std::mutex> lock(_mutex);
        _available.push_back(std::unique_ptr<std::uint8_t[]>(data));
    }

    std::mutex _mutex;
    std::vector<std::unique_ptr<std::uint8_t[]>> _available;
    bool _stopped = false;
};
}
