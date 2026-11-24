#include "Framegrabber.h"

#include <atomic>
#include <iostream>
#include <stdexcept>

int main()
{
    Framegrabber board(nullptr);
    std::atomic<bool> entered{false};
    std::atomic<bool> nestedBegin{true};
    std::atomic<bool> busyAfterCallback{false};
    const auto callbackId = board.registerStatusCallback([&](const Framegrabber::Status status,
        const bool on) {
        if (status != Framegrabber::ControlStatus || !on) return;
        entered.store(true, std::memory_order_release);
        nestedBegin.store(board.tryBeginControl(), std::memory_order_release);
        board.endControl();
        busyAfterCallback.store(board.controlBusy(), std::memory_order_release);
    });
    if (callbackId == 0) return 1;
    if (!board.tryBeginControl()) return 2;
    if (!entered.load(std::memory_order_acquire)) return 3;
    if (nestedBegin.load(std::memory_order_acquire)) return 4;
    if (!busyAfterCallback.load(std::memory_order_acquire) || !board.controlBusy()) return 5;
    board.endControl();
    if (board.controlBusy()) return 6;
    board.clearStatusCallbacks();
    const auto throwingId = board.registerStatusCallback([](Framegrabber::Status, bool) {
        throw std::runtime_error("intentional status callback failure");
    });
    if (throwingId == 0) return 7;
    if (!board.tryBeginControl()) return 8;
    if (!board.controlBusy()) return 9;
    board.endControl();
    if (board.controlBusy()) return 10;
    board.clearStatusCallbacks();
    std::cout << "Callback control-gate re-entry and exception containment passed.\n";
    return 0;
}
