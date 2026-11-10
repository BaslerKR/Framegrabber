#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>

namespace FramegrabberDetail
{
/**
 * @brief Capture-counter registry for the running DMA channels.
 *
 * Each acquisition run receives a new generation. A running channel attaches a
 * reader that queries the SDK grabbed-image counter; the channel detaches it
 * before its DMA memory is released. The registry mutex guards only reader
 * registration and the query itself. The acquisition worker never takes it
 * while it waits for or copies a frame, so a counter read cannot wait for the
 * acquisition path.
 */
class CaptureCounterRegistry final
{
public:
    /** @brief Reads the SDK counter; a negative value means unavailable. */
    using Reader = std::function<std::int64_t()>;

    struct Mark
    {
        std::uint64_t generation = 0;
        std::uint64_t value = 0;
    };

    /** @brief Starts a new acquisition run and returns its generation. */
    std::uint64_t beginRun() noexcept
    {
        return _generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    }

    /** @brief Returns the generation of the newest acquisition run. */
    [[nodiscard]] std::uint64_t generation() const noexcept
    {
        return _generation.load(std::memory_order_acquire);
    }

    /** @brief Attaches the counter reader of one running channel. */
    void attach(const unsigned int dmaIndex, const std::uint64_t generation, Reader reader)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _readers[dmaIndex] = Entry{generation, std::move(reader)};
    }

    /** @brief Detaches a channel reader; call before the SDK memory is freed. */
    void detach(const unsigned int dmaIndex, const std::uint64_t generation)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto it = _readers.find(dmaIndex);
        if (it != _readers.end() && it->second.generation == generation)
        {
            _readers.erase(it);
        }
    }

    /** @brief Reads the counter of a running channel, or `nullopt` when it is not running. */
    [[nodiscard]] std::optional<Mark> read(const unsigned int dmaIndex) const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto it = _readers.find(dmaIndex);
        if (it == _readers.end() || !it->second.reader)
        {
            return std::nullopt;
        }
        const std::int64_t value = it->second.reader();
        if (value < 0)
        {
            return std::nullopt;
        }
        return Mark{it->second.generation, static_cast<std::uint64_t>(value)};
    }

private:
    struct Entry
    {
        std::uint64_t generation = 0;
        Reader reader;
    };

    mutable std::mutex _mutex;
    std::map<unsigned int, Entry> _readers;
    std::atomic<std::uint64_t> _generation{0};
};
} // namespace FramegrabberDetail
