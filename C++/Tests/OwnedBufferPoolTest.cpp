#include "FramegrabberOwnedBufferPool.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>

namespace {

using FramegrabberDetail::OwnedBufferPool;

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
    leaseMayOutlivePool();
    if (failures != 0)
    {
        std::cerr << failures << " owned buffer pool test(s) failed.\n";
        return 1;
    }
    std::cout << "OwnedBufferPool exhaustion and lifetime contracts passed.\n";
    return 0;
}
