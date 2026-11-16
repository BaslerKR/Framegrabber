#pragma once

#include "DeviceParameter.h"
#include "DeviceParameterControl.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace framegrabber {

/**
 * @brief Builds the current complete snapshot for one subscription.
 * @note Runs on a module-owned notification thread, never on the GUI thread, and must not
 *       call back into the hub.
 */
using ParameterSnapshotFactory = std::function<DeviceParameterChange(
    const std::string& connectionId, const std::string& subscriptionId)>;

/**
 * @brief Owns bounded parameter change subscriptions and their delivery queues.
 *
 * The controller publishes revision-stamped deltas as it observes the device; the hub applies
 * the contract bound directly: when a subscription's queue is full, the pending deltas are
 * discarded and one resync snapshot replaces them. Delivery runs on a module thread that calls
 * drain() (never the GUI thread), and one subscription's messages are delivered in sequence
 * order, one at a time, even when several module threads publish.
 */
class ParameterNotificationHub
{
public:
    explicit ParameterNotificationHub(ParameterSnapshotFactory snapshot = {});

    /** @brief Admits one bounded subscription; a duplicate id or the limit is refused without effects. */
    DeviceParameterResult add(std::string connectionId, std::string subscriptionId,
        IDeviceParameterControl::ChangeListener delivery);
    /** @brief Stops delivery for one subscription and waits for an in-flight callback to end. */
    DeviceParameterResult remove(const std::string& connectionId, const std::string& subscriptionId);
    /** @brief Queues one complete snapshot for every subscription of @p connectionId. */
    void publishSnapshot(const std::string& connectionId);
    /** @brief Queues replaced descriptors and invalidated targets for one connection. */
    void publishDelta(const std::string& connectionId,
        std::vector<DeviceParameterDescriptor> descriptors,
        std::vector<DeviceParameterTarget> invalidated);
    /** @brief Queues a terminal change that ends every subscription of one connection. */
    void publishTerminal(const std::string& connectionId, DeviceParameterError error,
        std::string message);
    /** @brief Delivers queued changes; safe to call when no subscription is active. */
    void drain();
    /** @brief Stops every subscription without delivering further changes. */
    void close();

    /** @brief Test seam invoked after a pop and before delivery; set before subscriptions. */
    void setAfterPopHookForTest(std::function<void()> hook);

    [[nodiscard]] bool active() const;
    [[nodiscard]] std::size_t size() const;
    /** @brief Undelivered messages retained for one subscription; zero when it is not active. */
    [[nodiscard]] std::size_t pending(const std::string& subscriptionId) const;

private:
    struct Watch {
        std::string connectionId;
        std::string subscriptionId;
        IDeviceParameterControl::ChangeListener delivery;
        DeviceParameterChangeQueue queue;
        std::atomic<bool> active{true};
        std::mutex stateMutex;
        std::uint64_t sequence = 0;
        bool snapshotSent = false;
        std::mutex deliveryMutex;
    };

    /** @brief Assigns the next sequence and queues one change; false means the queue overflowed. */
    bool push(const std::shared_ptr<Watch>& watch, DeviceParameterChange change);
    /** @brief Clears pending deltas and queues one resync snapshot under the watch lock. */
    void replace(const std::shared_ptr<Watch>& watch, DeviceParameterChange snapshot,
        const char* reason);
    std::vector<std::shared_ptr<Watch>> watchesFor(const std::string& connectionId) const;

    ParameterSnapshotFactory _snapshot;
    std::function<void()> _afterPopForTest;
    mutable std::mutex _mutex;
    std::unordered_map<std::string, std::shared_ptr<Watch>> _watches;
};

} // namespace framegrabber
