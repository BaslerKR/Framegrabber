#include "FramegrabberCaptureCounter.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

namespace {

using FramegrabberDetail::CaptureCounterRegistry;

// assert() is compiled out in Release test builds; this check is not.
void check(const bool condition, const char* expression, const int line)
{
    if (!condition)
    {
        std::cerr << "CaptureCounterTest:" << line << ": check failed: " << expression << '\n';
        std::exit(1);
    }
}
#define CHECK(expression) check((expression), #expression, __LINE__)

void captureSequenceIsSdkNumber()
{
    CaptureCounterRegistry registry;
    const std::uint64_t generation = registry.beginRun();
    std::atomic<std::int64_t> sdkGrabbed{41};
    registry.attach(0, generation, [&sdkGrabbed] { return sdkGrabbed.load(); });

    auto mark = registry.read(0);
    CHECK(mark.has_value());
    CHECK(mark->generation == generation);
    CHECK(mark->value == 41);

    sdkGrabbed.store(97);
    mark = registry.read(0);
    CHECK(mark.has_value());
    CHECK(mark->value == 97);

    // A failed SDK status query is reported as unavailable, not as zero.
    sdkGrabbed.store(-1);
    CHECK(!registry.read(0).has_value());
    CHECK(!registry.read(1).has_value());
}

void generationBumpsOnRestart()
{
    CaptureCounterRegistry registry;
    const std::uint64_t first = registry.beginRun();
    registry.attach(0, first, [] { return std::int64_t{500}; });

    const std::uint64_t second = registry.beginRun();
    CHECK(second > first);
    CHECK(registry.generation() == second);
    registry.attach(0, second, [] { return std::int64_t{1}; });

    // The late teardown of the previous run must not detach the new run.
    registry.detach(0, first);
    auto mark = registry.read(0);
    CHECK(mark.has_value());
    CHECK(mark->generation == second);
    CHECK(mark->value == 1);

    registry.detach(0, second);
    CHECK(!registry.read(0).has_value());
}

void captureCounterNonBlocking()
{
    CaptureCounterRegistry registry;
    const std::uint64_t generation = registry.beginRun();
    std::atomic<std::int64_t> sdkGrabbed{0};
    registry.attach(0, generation, [&sdkGrabbed] { return sdkGrabbed.load(); });

    // The acquisition worker waits for and copies frames without the counter
    // lock, so the counter read stays fast while it copies large payloads.
    std::atomic<bool> stop{false};
    std::thread acquisition([&] {
        std::vector<std::uint8_t> source(32U << 20U, 0x5a);
        std::vector<std::uint8_t> owned(source.size());
        while (!stop.load(std::memory_order_acquire))
        {
            std::memcpy(owned.data(), source.data(), source.size());
            sdkGrabbed.fetch_add(1, std::memory_order_acq_rel);
        }
    });

    std::vector<std::chrono::steady_clock::duration> samples;
    samples.reserve(1000);
    for (int index = 0; index < 1000; ++index)
    {
        const auto started = std::chrono::steady_clock::now();
        const auto mark = registry.read(0);
        samples.push_back(std::chrono::steady_clock::now() - started);
        CHECK(mark.has_value());
    }
    stop.store(true, std::memory_order_release);
    acquisition.join();
    // p99 rather than the maximum: a shared CI runner can preempt any single
    // call, which is not a lock wait.
    std::sort(samples.begin(), samples.end());
    CHECK(samples[samples.size() * 99 / 100] < std::chrono::milliseconds(1));
}

} // namespace

int main()
{
    captureSequenceIsSdkNumber();
    generationBumpsOnRestart();
    captureCounterNonBlocking();
    std::cout << "CaptureCounterTest passed\n";
    return 0;
}
