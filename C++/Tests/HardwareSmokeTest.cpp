#include "Framegrabber.h"
#include "FramegrabberSystem.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace std::chrono_literals;

namespace {

struct Capture
{
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::size_t count = 0;
    std::size_t invalid = 0;
    std::vector<std::uint64_t> sequences;

    void reset()
    {
        std::lock_guard<std::mutex> lock(mutex);
        count = 0;
        invalid = 0;
        sequences.clear();
    }

    void add(const Framegrabber::Image& image, const std::size_t sequence)
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++count;
        if (!image.isValid() || image.dmaIndex != 0) ++invalid;
        sequences.push_back(sequence);
        changed.notify_all();
    }

    bool valid(const std::size_t expected) const
    {
        return count == expected && invalid == 0 && sequences.size() == expected;
    }
};

struct BoardCleanup
{
    explicit BoardCleanup(Framegrabber& board)
        : board(board)
    {
    }

    ~BoardCleanup()
    {
        board.clearGrabCallbacks();
        board.clearStatusCallbacks();
        board.clearNodeUpdatedCallbacks();
        board.setFrameStorage({});
        board.stop();
        board.close();
    }

    BoardCleanup(const BoardCleanup&) = delete;
    BoardCleanup& operator=(const BoardCleanup&) = delete;

private:
    Framegrabber& board;
};

bool waitForStop(Framegrabber& board, const std::chrono::seconds timeout = 10s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (board.isGrabbing() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    return !board.isGrabbing();
}

template<class Predicate>
bool waitFor(Predicate predicate, const std::chrono::seconds timeout = 5s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate())
    {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

bool sequencesIncrease(const Capture& capture)
{
    std::lock_guard<std::mutex> lock(capture.mutex);
    return std::adjacent_find(capture.sequences.begin(), capture.sequences.end(),
        [](const std::uint64_t left, const std::uint64_t right) { return right <= left; })
        == capture.sequences.end();
}

std::size_t poolExhausted(const Framegrabber& board)
{
    std::size_t total = 0;
    for (const auto& channel : board.acquisitionStatistics()) total += channel.poolExhausted;
    return total;
}

bool sdkVersionAtLeast(const char* version, const int major, const int minor, const int patch)
{
    if (!version) return false;
    const std::string text(version);
    std::size_t position = 0;
    int parts[3]{};
    for (int part = 0; part < 3; ++part)
    {
        const auto separator = text.find('.', position);
        const auto token = text.substr(position, separator == std::string::npos
            ? std::string::npos : separator - position);
        if (token.empty() || !std::all_of(token.begin(), token.end(), [](const char value) {
            return value >= '0' && value <= '9';
        })) return false;
        parts[part] = std::stoi(token);
        if (part < 2)
        {
            if (separator == std::string::npos) return false;
            position = separator + 1;
        }
    }
    return std::tie(parts[0], parts[1], parts[2]) >= std::tie(major, minor, patch);
}

}

int main()
{
    Capture capture;
    Capture exceptionCapture;
    std::atomic<bool> exceptionThrown{false};
    std::atomic<bool> storageStopped{false};
    std::atomic<bool> storageGateProbed{false};
    std::atomic<bool> storageGateFinished{false};
    std::atomic<bool> storageNestedBegin{true};
    std::atomic<bool> grabGateProbed{false};
    std::atomic<bool> grabGateFinished{false};
    std::atomic<bool> grabNestedBegin{true};
    std::mutex retainedMutex;
    std::vector<std::shared_ptr<const std::uint8_t>> retained;
    std::atomic<bool> callbackStopped{false};
    std::mutex gateMutex;
    std::condition_variable gate;
    bool callbackEntered = false;
    bool releaseCallback = false;
    std::atomic<bool> reentryCalled{false};
    std::atomic<bool> reentryResult{true};
    std::mutex errorMutex;
    std::string reentryError;
    std::atomic<bool> nodeReentryCalled{false};
    std::atomic<bool> nodeReentryResult{true};
    std::mutex nodeErrorMutex;
    std::string nodeReentryError;
    std::atomic<int> recursiveNodeCalls{0};
    std::atomic<int> grabStatusStarts{0};
    std::atomic<int> grabStatusStops{0};
    std::atomic<bool> stopModelReads{false};
    std::atomic<std::size_t> modelReads{0};

    FramegrabberSystem system;
    if (!system.isInitialized() || !system.updateFramegrabberList()) return 1;
    const auto boards = system.getCachedBoardInfo();
    if (boards.empty())
    {
        std::cout << "No frame grabber board detected; hardware smoke test skipped.\n";
        return 77;
    }
    const auto applets = system.getLoadableApplets(boards.front().displayName());
    const auto applet = std::find_if(applets.begin(), applets.end(), [](const auto& item) {
        return item.name == "FrameGrabberTest";
    });
    if (applet == applets.end())
    {
        std::cout << "FrameGrabberTest applet is unavailable; hardware smoke test skipped.\n";
        return 77;
    }

    Framegrabber* board = system.addFramegrabber();
    if (!board || !board->loadApplet(applet->path)) return 2;
    BoardCleanup cleanup(*board);
    if (board->getDMACount() <= 0) return 3;
    board->setDMABufferCount(0);
    if (board->dmaBufferCount() != 2) return 4;

    const auto model = board->getAppletFeatureModel(0);
    if (model.empty()) return 5;
    Framegrabber::ParameterValue width = std::int64_t{0};
    Framegrabber::ParameterValue height = std::int64_t{0};
    Framegrabber::ParameterValue format = std::int64_t{0};
    if (!board->getAppletParameter("FG_WIDTH", 0, width)
        || !board->getAppletParameter("FG_HEIGHT", 0, height)
        || !board->getAppletParameter("FG_FORMAT", 0, format)) return 6;
    if (!board->setAppletParameter("FG_WIDTH", 0, width, false, true)) return 7;
    if (board->setAppletParameter("FG_WIDTH", 0, 1.5, false, true)) return 40;

    const auto captureId = board->registerGrabCallback([&](const Framegrabber::Image& image,
        const std::size_t sequence) { capture.add(image, sequence); });
    if (captureId == 0) return 8;

    capture.reset();
    board->grab(5);
    if (!waitForStop(*board) || !capture.valid(5) || !sequencesIncrease(capture)) return 9;
    const auto single = board->acquisitionStatistics();
    if (single.size() != 1 || single.front().delivered != 5
        || single.front().consumerDelivered != 5 || single.front().failed != 0
        || single.front().rejected != 0) return 10;

    const auto throwingId = board->registerGrabCallback([&](const Framegrabber::Image&, std::size_t) {
        if (!exceptionThrown.exchange(true)) throw std::runtime_error("intentional callback failure");
    });
    const auto siblingId = board->registerGrabCallback([&](const Framegrabber::Image& image,
        const std::size_t sequence) { exceptionCapture.add(image, sequence); });
    if (throwingId == 0 || siblingId == 0) return 11;
    exceptionCapture.reset();
    board->grab(5);
    if (!waitForStop(*board) || !exceptionCapture.valid(5)) return 12;
    const auto callbackFailure = board->acquisitionStatistics();
    if (callbackFailure.size() != 1 || callbackFailure.front().failed < 1
        || callbackFailure.front().consumerDelivered != 5) return 13;
    board->deregisterGrabCallback(throwingId);
    board->deregisterGrabCallback(siblingId);

    board->setFrameStorage([](std::size_t) -> std::shared_ptr<std::uint8_t> {
        throw std::runtime_error("intentional storage failure");
    });
    board->grab(0);
    if (!waitFor([&] { return poolExhausted(*board) > 0; })) return 14;
    board->stop();
    const auto storageFailure = board->acquisitionStatistics();
    if (storageFailure.size() != 1 || storageFailure.front().delivered != 0
        || storageFailure.front().poolExhausted == 0) return 15;
    board->setFrameStorage({});

    board->setFrameStorage([&](const std::size_t bytes) -> std::shared_ptr<std::uint8_t> {
        if (!storageStopped.exchange(true)) board->stop();
        auto storage = std::make_shared<std::vector<std::uint8_t>>(bytes);
        return std::shared_ptr<std::uint8_t>(storage, storage->data());
    });
    capture.reset();
    board->grab(0);
    if (!waitForStop(*board) || !storageStopped.load()) return 37;
    {
        std::lock_guard<std::mutex> lock(capture.mutex);
        if (capture.count != 0) return 38;
    }
    board->setFrameStorage({});

    if (!board->tryBeginControl()) return 55;
    board->setFrameStorage([&](const std::size_t bytes) -> std::shared_ptr<std::uint8_t> {
        storageGateProbed.store(true, std::memory_order_release);
        storageNestedBegin.store(board->tryBeginControl(), std::memory_order_release);
        board->endControl();
        auto storage = std::make_shared<std::vector<std::uint8_t>>(bytes);
        storageGateFinished.store(true, std::memory_order_release);
        return std::shared_ptr<std::uint8_t>(storage, storage->data());
    });
    capture.reset();
    board->grab(0);
    const auto storageGateCompleted = waitFor([&] {
        return storageGateFinished.load(std::memory_order_acquire);
    });
    board->stop();
    const auto storageGateStopped = waitForStop(*board);
    if (!storageGateStopped || !storageGateCompleted
        || !storageGateProbed.load(std::memory_order_acquire)
        || storageNestedBegin.load(std::memory_order_acquire) || !board->controlBusy())
    {
        std::cerr << "Storage control gate probe failed: completed=" << storageGateCompleted
                  << ", stopped=" << storageGateStopped
                  << ", probed=" << storageGateProbed.load(std::memory_order_acquire)
                  << ", nestedBegin=" << storageNestedBegin.load(std::memory_order_acquire)
                  << ", busy=" << board->controlBusy() << '\n';
        return 56;
    }
    board->endControl();
    if (board->controlBusy()) return 57;
    board->setFrameStorage({});

    if (!board->tryBeginControl()) return 58;
    const auto gateProbeId = board->registerGrabCallback([&](const Framegrabber::Image&, std::size_t) {
        grabGateProbed.store(true, std::memory_order_release);
        grabNestedBegin.store(board->tryBeginControl(), std::memory_order_release);
        board->endControl();
        grabGateFinished.store(true, std::memory_order_release);
    });
    if (gateProbeId == 0) return 59;
    capture.reset();
    board->grab(0);
    const auto grabGateCompleted = waitFor([&] {
        return grabGateFinished.load(std::memory_order_acquire);
    });
    board->stop();
    const auto grabGateStopped = waitForStop(*board);
    if (!grabGateStopped || !grabGateCompleted
        || !grabGateProbed.load(std::memory_order_acquire)
        || grabNestedBegin.load(std::memory_order_acquire) || !board->controlBusy())
    {
        std::cerr << "Grab control gate probe failed: completed=" << grabGateCompleted
                  << ", stopped=" << grabGateStopped
                  << ", probed=" << grabGateProbed.load(std::memory_order_acquire)
                  << ", nestedBegin=" << grabNestedBegin.load(std::memory_order_acquire)
                  << ", busy=" << board->controlBusy() << '\n';
        return 60;
    }
    board->endControl();
    if (board->controlBusy()) return 61;
    board->deregisterGrabCallback(gateProbeId);

    const auto retentionId = board->registerGrabCallback([&](const Framegrabber::Image& image, std::size_t) {
        std::lock_guard<std::mutex> lock(retainedMutex);
        if (retained.size() < 2) retained.push_back(image.storage);
    });
    if (retentionId == 0) return 49;
    board->grab(0);
    if (!waitFor([&] { return poolExhausted(*board) > 0; })) return 50;
    board->stop();
    const auto poolPressure = board->acquisitionStatistics();
    if (poolPressure.size() != 1 || poolPressure.front().poolExhausted == 0
        || poolPressure.front().delivered > 2) return 51;
    {
        std::lock_guard<std::mutex> lock(retainedMutex);
        retained.clear();
    }
    board->deregisterGrabCallback(retentionId);

    capture.reset();
    board->grab(0);
    const auto sustainedObserved = waitFor([&] {
        std::lock_guard<std::mutex> lock(capture.mutex);
        return capture.count >= 10;
    }, 5s);
    bool sustainedValid = true;
    {
        std::lock_guard<std::mutex> lock(capture.mutex);
        sustainedValid = capture.count >= 10 && capture.invalid == 0;
    }
    if (!sustainedObserved || !sustainedValid) return 52;
    const auto sustainedStarted = std::chrono::steady_clock::now();
    board->stop();
    if (std::chrono::steady_clock::now() - sustainedStarted > 5s) return 53;
    const auto sustained = board->acquisitionStatistics();
    if (sustained.size() != 1 || sustained.front().failed != 0
        || sustained.front().rejected != 0) return 54;

    const auto stoppingId = board->registerGrabCallback([&](const Framegrabber::Image&, std::size_t) {
        if (!callbackStopped.exchange(true)) board->stop();
    });
    if (stoppingId == 0) return 16;
    capture.reset();
    board->grab(0);
    const auto callbackStopCompleted = waitForStop(*board);
    bool captureDelivered = false;
    {
        std::lock_guard<std::mutex> lock(capture.mutex);
        captureDelivered = capture.count != 0;
    }
    if (!callbackStopCompleted || !callbackStopped.load() || !captureDelivered) return 17;
    board->deregisterGrabCallback(stoppingId);

    const auto concurrentId = board->registerGrabCallback([&](const Framegrabber::Image&, std::size_t) {
        std::unique_lock<std::mutex> lock(gateMutex);
        callbackEntered = true;
        gate.notify_all();
        gate.wait(lock, [&] { return releaseCallback; });
        board->stop();
    });
    if (concurrentId == 0) return 18;
    capture.reset();
    board->grab(0);
    {
        std::unique_lock<std::mutex> lock(gateMutex);
        if (!gate.wait_for(lock, 5s, [&] { return callbackEntered; })) std::_Exit(19);
    }
    std::atomic<bool> externalStopped{false};
    std::thread external([&] {
        board->stop();
        externalStopped.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(200ms);
    {
        std::lock_guard<std::mutex> lock(gateMutex);
        releaseCallback = true;
    }
    gate.notify_all();
    if (!waitFor([&] { return externalStopped.load(std::memory_order_acquire); })) std::_Exit(20);
    external.join();
    if (board->isGrabbing()) return 21;
    board->deregisterGrabCallback(concurrentId);

    callbackEntered = false;
    releaseCallback = false;
    const auto concurrentCloseId = board->registerGrabCallback([&](const Framegrabber::Image&, std::size_t) {
        std::unique_lock<std::mutex> lock(gateMutex);
        callbackEntered = true;
        gate.notify_all();
        gate.wait(lock, [&] { return releaseCallback; });
        board->stop();
    });
    if (concurrentCloseId == 0) return 41;
    board->grab(0);
    {
        std::unique_lock<std::mutex> lock(gateMutex);
        if (!gate.wait_for(lock, 5s, [&] { return callbackEntered; })) std::_Exit(42);
    }
    std::atomic<bool> externalClosed{false};
    std::thread closer([&] {
        board->close();
        externalClosed.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(200ms);
    {
        std::lock_guard<std::mutex> lock(gateMutex);
        releaseCallback = true;
    }
    gate.notify_all();
    if (!waitFor([&] { return externalClosed.load(std::memory_order_acquire); })) std::_Exit(43);
    closer.join();
    if (board->isOpened() || board->isGrabbing()) return 44;
    board->deregisterGrabCallback(concurrentCloseId);
    if (!board->loadApplet(applet->path)) return 45;

    const auto statusId = board->registerStatusCallback([&](const Framegrabber::Status status,
        const bool on) {
        if (status == Framegrabber::GrabbingStatus && on) board->stop();
    });
    if (statusId == 0) return 22;
    board->grab(0);
    if (board->isGrabbing()) return 23;
    board->clearStatusCallbacks();

    const auto reentryId = board->registerStatusCallback([&](const Framegrabber::Status status,
        const bool on) {
        if (status == Framegrabber::GrabbingStatus && on)
        {
            reentryCalled.store(true, std::memory_order_release);
            reentryResult.store(board->loadApplet(applet->path), std::memory_order_release);
            std::lock_guard<std::mutex> lock(errorMutex);
            reentryError = board->lastError().message;
        }
    });
    if (reentryId == 0) return 24;
    board->grab(0);
    board->stop();
    board->clearStatusCallbacks();
    if (!reentryCalled.load(std::memory_order_acquire) || reentryResult.load(std::memory_order_acquire)
        || reentryError.find("callback") == std::string::npos) return 25;
    if (board->lastError().message.find("callback") == std::string::npos) return 35;

    const auto nodeReentryId = board->registerNodeUpdatedCallback([&](Framegrabber::FeatureSource,
        Framegrabber::CameraTransport, unsigned int, const std::string&) {
        nodeReentryCalled.store(true, std::memory_order_release);
        nodeReentryResult.store(board->loadApplet(applet->path), std::memory_order_release);
        std::lock_guard<std::mutex> lock(nodeErrorMutex);
        nodeReentryError = board->lastError().message;
    });
    if (nodeReentryId == 0) return 32;
    if (!board->setAppletParameter("FG_WIDTH", 0, width, false, true)) return 33;
    board->clearNodeUpdatedCallbacks();
    if (!nodeReentryCalled.load(std::memory_order_acquire)
        || nodeReentryResult.load(std::memory_order_acquire)
        || nodeReentryError.find("callback") == std::string::npos) return 34;
    if (board->lastError().message.find("callback") == std::string::npos) return 36;

    const auto recursiveNodeId = board->registerNodeUpdatedCallback([&](Framegrabber::FeatureSource,
        Framegrabber::CameraTransport, unsigned int, const std::string&) {
        if (recursiveNodeCalls.fetch_add(1, std::memory_order_relaxed) == 0)
            board->setAppletParameter("FG_WIDTH", 0, width, false, true);
    });
    if (recursiveNodeId == 0) return 62;
    if (!board->setAppletParameter("FG_WIDTH", 0, width, false, true)) return 63;
    board->clearNodeUpdatedCallbacks();
    if (recursiveNodeCalls.load(std::memory_order_relaxed) != 1) return 64;

    capture.reset();
    board->grab(0);
    {
        std::unique_lock<std::mutex> lock(capture.mutex);
        if (!capture.changed.wait_for(lock, 5s, [&] { return capture.count > 0; })) return 46;
    }
    const auto generationBeforeRefusal = board->connectionGeneration();
    if (board->loadApplet(applet->path)) return 47;
    if (!board->isGrabbing() || board->connectionGeneration() != generationBeforeRefusal) return 48;
    board->stop();

    const auto grabStatusId = board->registerStatusCallback([&](const Framegrabber::Status status,
        const bool on) {
        if (status != Framegrabber::GrabbingStatus) return;
        if (on) grabStatusStarts.fetch_add(1, std::memory_order_relaxed);
        else grabStatusStops.fetch_add(1, std::memory_order_relaxed);
    });
    if (grabStatusId == 0) return 65;
    for (int cycle = 0; cycle < 20; ++cycle)
    {
        capture.reset();
        board->grab(3);
        if (!waitForStop(*board) || !capture.valid(3) || !sequencesIncrease(capture)) return 26;
        const auto statistics = board->acquisitionStatistics();
        if (statistics.size() != 1 || statistics.front().delivered != 3
            || statistics.front().failed != 0 || statistics.front().rejected != 0) return 27;
    }
    board->clearStatusCallbacks();
    if (grabStatusStarts.load(std::memory_order_relaxed) != 20
        || grabStatusStops.load(std::memory_order_relaxed) != 20) return 66;

    for (int cycle = 0; cycle < 10; ++cycle)
    {
        capture.reset();
        board->grab(0);
        {
            std::unique_lock<std::mutex> lock(capture.mutex);
            if (!capture.changed.wait_for(lock, 5s, [&] { return capture.count >= 10; })) return 28;
        }
        board->stop();
        if (capture.invalid != 0) return 29;
    }

    std::thread modelReader([&] {
        while (!stopModelReads.load(std::memory_order_acquire))
        {
            const auto current = board->getAppletFeatureModel(0);
            if (!current.empty()) modelReads.fetch_add(1, std::memory_order_relaxed);
        }
    });
    bool reopened = true;
    for (int cycle = 0; cycle < 10; ++cycle)
    {
        board->close();
        if (board->isOpened() || !board->loadApplet(applet->path))
        {
            reopened = false;
            break;
        }
    }
    stopModelReads.store(true, std::memory_order_release);
    modelReader.join();
    if (!reopened) return 30;
    if (modelReads.load(std::memory_order_relaxed) == 0) return 39;

    if (sdkVersionAtLeast(Fg_getSWVersion(), 5, 11, 9))
    {
        const auto configuration = std::filesystem::temp_directory_path()
            / "playground-framegrabber-hardware-smoke.mcf";
        std::error_code error;
        std::filesystem::remove(configuration, error);
        if (!board->saveConfiguration(configuration.string())
            || !board->loadConfiguration(configuration.string())) return 31;
        std::filesystem::remove(configuration, error);
    }
    else
    {
        std::cout << "Configuration round-trip skipped for SDK " << Fg_getSWVersion()
                  << "; SDK 5.11.7 crashes in Fg_saveConfig for FrameGrabberTest.\n";
    }

    board->clearGrabCallbacks();
    board->close();
    std::cout << "FrameGrabberTest hardware smoke test passed on " << boards.front().displayName()
              << ".\n";
    return 0;
}
