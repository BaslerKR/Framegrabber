#include "FramegrabberOwnedBufferPool.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>

namespace {

using FramegrabberDetail::OwnedBufferPool;

void ownedPoolExhaustionDropsAndCounts()
{
    const auto pool = OwnedBufferPool::create(64, 2);
    auto first = pool->acquire();
    auto second = pool->acquire();
    assert(first);
    assert(second);
    assert(first.get() != second.get());

    const auto started = std::chrono::steady_clock::now();
    const auto exhausted = pool->acquire();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    assert(!exhausted);
    assert(elapsed < std::chrono::milliseconds(50));

    const std::uint8_t* firstData = first.get();
    first.reset();
    const auto reused = pool->acquire();
    assert(reused);
    assert(reused.get() == firstData);

    pool->stop();
    const auto stopped = pool->acquire();
    assert(!stopped);
}

}

int main()
{
    ownedPoolExhaustionDropsAndCounts();
    std::cout << "OwnedBufferPool non-blocking exhaustion passed.\n";
    return 0;
}
