#include "FramegrabberOwnedBufferPool.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using FramegrabberDetail::OwnedBufferPool;
using Storage = std::function<std::shared_ptr<std::uint8_t>(std::size_t)>;

int failures = 0;

void expect(const bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void ownedPoolExhaustionDropsAndCounts()
{
    const auto pool = OwnedBufferPool::create(64, 2);
    auto first = pool->acquire();
    auto second = pool->acquire();
    expect(first != nullptr, "first lease is available");
    expect(second != nullptr, "second lease is available");
    expect(first.get() != second.get(), "leases use distinct buffers");

    const auto started = std::chrono::steady_clock::now();
    const auto exhausted = pool->acquire();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    expect(!exhausted, "exhausted pool refuses a lease");
    expect(elapsed < std::chrono::milliseconds(50), "exhausted pool does not wait");

    const std::uint8_t* firstData = first.get();
    first.reset();
    const auto reused = pool->acquire();
    expect(reused != nullptr, "released lease is reused");
    expect(reused.get() == firstData, "reused lease keeps its storage");

    pool->stop();
    const auto stopped = pool->acquire();
    expect(!stopped, "stopped pool refuses a lease");
}

void frameStorageFailuresAreContained()
{
    const auto pool = OwnedBufferPool::create(16, 1);
    std::string error;
    auto provided = std::make_shared<std::vector<std::uint8_t>>(16);
    const auto acquired = FramegrabberDetail::acquireFrameStorage(
        [provided](std::size_t bytes) -> std::shared_ptr<std::uint8_t> {
            return bytes <= provided->size()
                ? std::shared_ptr<std::uint8_t>(provided, provided->data())
                : std::shared_ptr<std::uint8_t>{};
        }, pool, 16, &error);
    expect(acquired != nullptr, "provided storage is returned");
    expect(acquired.get() == provided->data(), "provided storage keeps its pointer");
    expect(error.empty(), "successful storage clears the error");

    const auto missing = FramegrabberDetail::acquireFrameStorage(
        [](std::size_t) -> std::shared_ptr<std::uint8_t> { return {}; }, pool, 16, &error);
    expect(!missing, "empty storage drops the frame");
    expect(error.empty(), "empty storage is not an exception");

    const auto thrown = FramegrabberDetail::acquireFrameStorage(
        [](std::size_t) -> std::shared_ptr<std::uint8_t> {
            throw std::runtime_error("storage failed");
        }, pool, 16, &error);
    expect(!thrown, "standard exception drops the frame");
    expect(error == "storage failed", "standard exception is retained");

    const auto unknown = FramegrabberDetail::acquireFrameStorage(
        [](std::size_t) -> std::shared_ptr<std::uint8_t> { throw 7; }, pool, 16, &error);
    expect(!unknown, "unknown exception drops the frame");
    expect(!error.empty(), "unknown exception has a reason");

    const auto fallback = FramegrabberDetail::acquireFrameStorage(
        Storage{}, pool, 16, &error);
    expect(fallback != nullptr, "empty storage callback uses the owned pool");
    expect(error.empty(), "pool fallback clears the error");

    const auto absent = FramegrabberDetail::acquireFrameStorage(
        Storage{}, std::shared_ptr<OwnedBufferPool>{}, 16, &error);
    expect(!absent, "missing pool drops the frame");
}

void leaseMayOutlivePool()
{
    std::shared_ptr<std::uint8_t> lease;
    {
        const auto pool = OwnedBufferPool::create(8, 1);
        lease = pool->acquire();
        pool->stop();
    }
    expect(lease != nullptr, "lease outlives its pool");
    if (lease)
    {
        lease.get()[0] = 42;
        expect(lease.get()[0] == 42, "lease remains usable after pool destruction");
    }
}

}

int main()
{
    ownedPoolExhaustionDropsAndCounts();
    frameStorageFailuresAreContained();
    leaseMayOutlivePool();
    if (failures != 0)
    {
        std::cerr << failures << " owned buffer pool test(s) failed.\n";
        return 1;
    }
    std::cout << "OwnedBufferPool exhaustion, storage failure and lifetime contracts passed.\n";
    return 0;
}
