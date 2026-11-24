#include "Framegrabber.h"

#include <atomic>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>

int main()
{
    Framegrabber board(nullptr);
    std::atomic<bool> entered{false};
    std::atomic<bool> nestedBegin{true};
    std::atomic<bool> nestedEnd{true};
    std::atomic<bool> busyAfterCallback{false};
    std::atomic<bool> lifecycleRefused{true};
    std::atomic<bool> callbackReasonSeen{false};
    std::atomic<bool> closeReasonSeen{false};
    std::mutex errorMutex;
    std::string callbackError;
    std::string closeError;
    const auto callbackId = board.registerStatusCallback([&](const Framegrabber::Status status,
        const bool on) {
        if (status != Framegrabber::ControlStatus || !on) return;
        entered.store(true, std::memory_order_release);
        nestedBegin.store(board.tryBeginControl(), std::memory_order_release);
        nestedEnd.store(board.endControl(), std::memory_order_release);
        busyAfterCallback.store(board.controlBusy(), std::memory_order_release);
        const bool refused = !board.loadApplet("unused")
            && !board.open()
            && !board.loadConfiguration("unused.mcf")
            && !board.refreshCameras(Framegrabber::CameraTransport::CoaXPress)
            && !board.connectCamera(Framegrabber::CameraTransport::CoaXPress, 0);
        board.disconnectCamera(Framegrabber::CameraTransport::CoaXPress, 0);
        lifecycleRefused.store(refused && board.lastError().message.find("callback") != std::string::npos,
            std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(errorMutex);
            callbackError = board.lastError().message;
        }
        board.grab();
        callbackReasonSeen.store(board.lastError().message.find("callback") != std::string::npos,
            std::memory_order_release);
        board.close();
        {
            std::lock_guard<std::mutex> lock(errorMutex);
            closeError = board.lastError().message;
        }
        closeReasonSeen.store(closeError.find("only requests acquisition stop") != std::string::npos,
            std::memory_order_release);
        board.stop();
        board.requestStop();
    });
    if (callbackId == 0) return 1;
    if (!board.tryBeginControl()) return 2;
    if (!entered.load(std::memory_order_acquire)) return 3;
    if (nestedBegin.load(std::memory_order_acquire)) return 4;
    if (nestedEnd.load(std::memory_order_acquire)) return 5;
    if (!busyAfterCallback.load(std::memory_order_acquire) || !board.controlBusy()) return 6;
    if (!lifecycleRefused.load(std::memory_order_acquire)) return 7;
    if (!callbackReasonSeen.load(std::memory_order_acquire)) return 8;
    if (!closeReasonSeen.load(std::memory_order_acquire)) return 9;
    if (!board.endControl()) return 10;
    if (board.controlBusy()) return 11;
    if (board.endControl()) return 12;
    if (board.lastError().message.find("No frame grabber control operation") == std::string::npos) return 13;
    {
        std::lock_guard<std::mutex> lock(errorMutex);
        if (callbackError.find("callback") == std::string::npos) return 14;
        if (closeError.find("only requests acquisition stop") == std::string::npos) return 15;
    }
    board.clearStatusCallbacks();
    const auto throwingId = board.registerStatusCallback([](Framegrabber::Status, bool) {
        throw std::runtime_error("intentional status callback failure");
    });
    if (throwingId == 0) return 16;
    if (!board.tryBeginControl()) return 17;
    if (!board.controlBusy()) return 18;
    if (!board.endControl()) return 19;
    if (board.controlBusy()) return 20;
    board.clearStatusCallbacks();
    std::cout << "Callback control-gate and lifecycle re-entry contracts passed.\n";
    return 0;
}
