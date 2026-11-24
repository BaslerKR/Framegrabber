#include "FramegrabberParameterNotifications.h"

#include <algorithm>
#include <utility>

namespace framegrabber {
namespace {

constexpr const char* kOverflowReason =
    "The frame grabber parameter change queue overflowed; the snapshot was resynchronized.";

std::string truncated(std::string text)
{
    if (text.size() > deviceParameterChangeTextLimit) text.resize(deviceParameterChangeTextLimit);
    return text;
}

} // namespace

ParameterNotificationHub::ParameterNotificationHub(ParameterSnapshotFactory snapshot)
    : _snapshot(std::move(snapshot))
{
}

DeviceParameterResult ParameterNotificationHub::add(std::string connectionId,
    std::string subscriptionId, IDeviceParameterControl::ChangeListener delivery)
{
    if (validateDeviceParameterSubscription(connectionId, subscriptionId) != DeviceParameterError::None
        || !delivery) {
        return {DeviceParameterError::InvalidArgument,
            "The parameter change subscription identity is invalid.",
            DeviceParameterEffect::None, std::nullopt};
    }

    auto watch = std::make_shared<Watch>();
    watch->connectionId = std::move(connectionId);
    watch->subscriptionId = subscriptionId;
    watch->delivery = std::move(delivery);
    std::lock_guard<std::mutex> lock(_mutex);
    if (_watches.count(subscriptionId)) {
        return {DeviceParameterError::Conflict, "The parameter change subscription is already active.",
            DeviceParameterEffect::None, std::nullopt};
    }
    if (_watches.size() >= deviceParameterSubscriptionLimit) {
        return {DeviceParameterError::Busy,
            "The parameter change subscription bound is reached.",
            DeviceParameterEffect::None, std::nullopt};
    }
    _watches.emplace(std::move(subscriptionId), std::move(watch));
    return {};
}

DeviceParameterResult ParameterNotificationHub::remove(const std::string& connectionId,
    const std::string& subscriptionId)
{
    std::shared_ptr<Watch> watch;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto found = _watches.find(subscriptionId);
        if (found == _watches.end() || found->second->connectionId != connectionId) {
            return {DeviceParameterError::NotFound,
                "The parameter change subscription is not active.",
                DeviceParameterEffect::None, std::nullopt};
        }
        watch = found->second;
        watch->active = false;
        _watches.erase(found);
    }
    // Wait for an in-flight drain callback before the admission completes. A listener must not
    // call release synchronously; the host releases subscriptions from its own thread.
    std::lock_guard<std::mutex> delivering(watch->deliveryMutex);
    watch->delivery = {};
    return {};
}

void ParameterNotificationHub::publishSnapshot(const std::string& connectionId)
{
    if (!_snapshot) return;
    const auto watches = watchesFor(connectionId);
    for (const auto& watch : watches) {
        auto change = _snapshot(watch->connectionId, watch->subscriptionId);
        change.kind = DeviceParameterChangeKind::Snapshot;
        change.resync = false;
        change.invalidated.clear();
        change.overflowReason.clear();
        push(watch, std::move(change));
    }
}

void ParameterNotificationHub::publishDelta(const std::string& connectionId,
    std::vector<DeviceParameterDescriptor> descriptors,
    std::vector<DeviceParameterTarget> invalidated)
{
    if (descriptors.empty() && invalidated.empty()) return;
    const auto watches = watchesFor(connectionId);
    for (const auto& watch : watches) {
        {
            std::lock_guard<std::mutex> state(watch->stateMutex);
            // The first delivered message is always the complete snapshot; a delta that arrives
            // before it is superseded by that snapshot.
            if (!watch->snapshotSent) continue;
        }
        DeviceParameterChange change;
        change.kind = DeviceParameterChangeKind::Delta;
        change.connectionId = watch->connectionId;
        change.subscriptionId = watch->subscriptionId;
        change.descriptors = descriptors;
        change.invalidated = invalidated;
        for (const auto& descriptor : descriptors)
            change.revision = std::max(change.revision, descriptor.revision);
        if (push(watch, std::move(change))) continue;
        if (!_snapshot) continue;
        auto snapshot = _snapshot(watch->connectionId, watch->subscriptionId);
        snapshot.kind = DeviceParameterChangeKind::Snapshot;
        snapshot.resync = true;
        snapshot.invalidated.clear();
        snapshot.overflowReason = kOverflowReason;
        replace(watch, std::move(snapshot), kOverflowReason);
    }
}

void ParameterNotificationHub::publishTerminal(const std::string& connectionId,
    DeviceParameterError error, std::string message)
{
    const auto watches = watchesFor(connectionId);
    for (const auto& watch : watches) {
        bool needsSnapshot = false;
        {
            std::lock_guard<std::mutex> state(watch->stateMutex);
            needsSnapshot = !watch->snapshotSent;
        }
        if (needsSnapshot) {
            DeviceParameterChange snapshot;
            if (_snapshot) snapshot = _snapshot(watch->connectionId, watch->subscriptionId);
            snapshot.kind = DeviceParameterChangeKind::Snapshot;
            snapshot.connectionId = watch->connectionId;
            snapshot.subscriptionId = watch->subscriptionId;
            snapshot.resync = false;
            snapshot.invalidated.clear();
            snapshot.overflowReason.clear();
            snapshot.error = DeviceParameterError::None;
            snapshot.message.clear();
            push(watch, std::move(snapshot));
        }
        DeviceParameterChange change;
        change.kind = DeviceParameterChangeKind::Delta;
        change.connectionId = watch->connectionId;
        change.subscriptionId = watch->subscriptionId;
        change.error = error;
        change.message = truncated(std::move(message));
        // A terminal change carries no replacement state. A full queue is cleared by the first
        // attempt, so the saved copy always admits the terminal message on retry.
        auto retry = change;
        if (!push(watch, std::move(change))) push(watch, std::move(retry));
    }
}

void ParameterNotificationHub::drain()
{
    for (;;) {
        std::shared_ptr<Watch> watch;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            for (const auto& entry : _watches) {
                if (!entry.second->active || entry.second->queue.size() == 0) continue;
                watch = entry.second;
                break;
            }
        }
        if (!watch) return;

        // One watch delivers one message at a time: a slower thread can never deliver a later
        // sequence before an earlier one that another thread already popped.
        std::unique_lock<std::mutex> delivering(watch->deliveryMutex);
        if (!watch->active) continue;
        DeviceParameterChange change;
        bool terminal = false;
        {
            std::lock_guard<std::mutex> state(watch->stateMutex);
            auto queued = watch->queue.pop();
            if (!queued) continue;
            change = std::move(*queued);
            terminal = change.error != DeviceParameterError::None;
        }
        if (_afterPopForTest) _afterPopForTest();
        if (watch->delivery) {
            try { watch->delivery(std::move(change)); } catch (...) {}
        }
        if (terminal) {
            watch->active = false;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                const auto found = _watches.find(watch->subscriptionId);
                if (found != _watches.end() && found->second == watch) _watches.erase(found);
            }
            watch->delivery = {};
        }
    }
}

void ParameterNotificationHub::close()
{
    std::unordered_map<std::string, std::shared_ptr<Watch>> watches;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        watches = std::move(_watches);
        _watches.clear();
    }
    for (auto& entry : watches) {
        entry.second->active = false;
        std::lock_guard<std::mutex> delivering(entry.second->deliveryMutex);
        entry.second->delivery = {};
    }
}

void ParameterNotificationHub::setAfterPopHookForTest(std::function<void()> hook)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _afterPopForTest = std::move(hook);
}

bool ParameterNotificationHub::active() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return !_watches.empty();
}

std::size_t ParameterNotificationHub::size() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _watches.size();
}

std::size_t ParameterNotificationHub::pending(const std::string& subscriptionId) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    const auto found = _watches.find(subscriptionId);
    return found == _watches.end() ? 0 : found->second->queue.size();
}

bool ParameterNotificationHub::push(const std::shared_ptr<Watch>& watch, DeviceParameterChange change)
{
    if (!watch) return true;
    std::lock_guard<std::mutex> state(watch->stateMutex);
    if (!watch->active) return true;
    change.connectionId = watch->connectionId;
    change.subscriptionId = watch->subscriptionId;
    change.sequence = ++watch->sequence;
    if (change.kind == DeviceParameterChangeKind::Snapshot) watch->snapshotSent = true;
    return watch->queue.push(std::move(change));
}

void ParameterNotificationHub::replace(const std::shared_ptr<Watch>& watch,
    DeviceParameterChange snapshot, const char* reason)
{
    if (!watch) return;
    std::lock_guard<std::mutex> state(watch->stateMutex);
    if (!watch->active) return;
    snapshot.kind = DeviceParameterChangeKind::Snapshot;
    snapshot.resync = true;
    snapshot.connectionId = watch->connectionId;
    snapshot.subscriptionId = watch->subscriptionId;
    snapshot.invalidated.clear();
    snapshot.sequence = ++watch->sequence;
    watch->snapshotSent = true;
    watch->queue.replaceWithSnapshot(std::move(snapshot), reason);
}

std::vector<std::shared_ptr<ParameterNotificationHub::Watch>>
ParameterNotificationHub::watchesFor(const std::string& connectionId) const
{
    std::vector<std::shared_ptr<Watch>> watches;
    std::lock_guard<std::mutex> lock(_mutex);
    for (const auto& entry : _watches) {
        if (entry.second->active && entry.second->connectionId == connectionId)
            watches.push_back(entry.second);
    }
    return watches;
}

} // namespace framegrabber
