#include "FramegrabberSourceController.h"

#include <QDebug>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>

#include <charconv>
#include <cmath>
#include <functional>
#include <limits>
#include <tuple>
#include <type_traits>

// Applet parameters are exposed per DMA channel under the scope "Applet:<dma>". Their
// names are the SDK access names (Fg_getParameterNameById). Applet parameters have no
// GenICam selectors, so targets carry an empty selector list.

namespace
{
using Error = DeviceParameterError;
using Kind = DeviceParameterKind;
using FeatureKind = Framegrabber::AppletFeatureKind;

constexpr const char* kScopePrefix = "Applet:";

std::string appletScope(const unsigned int dmaIndex)
{
    return kScopePrefix + std::to_string(dmaIndex);
}

std::optional<unsigned int> parseScope(const std::string& scope)
{
    const std::string prefix(kScopePrefix);
    if (scope.compare(0, prefix.size(), prefix) != 0 || scope.size() == prefix.size())
    {
        return std::nullopt;
    }
    unsigned int dmaIndex = 0;
    const char* first = scope.data() + prefix.size();
    const char* last = scope.data() + scope.size();
    const auto parsed = std::from_chars(first, last, dmaIndex);
    if (parsed.ec != std::errc{} || parsed.ptr != last || scope != appletScope(dmaIndex))
    {
        return std::nullopt;
    }
    return dmaIndex;
}

DeviceParameterResult failure(const Error error, std::string message)
{
    DeviceParameterResult result;
    result.error = error;
    result.message = std::move(message);
    return result;
}

std::optional<Kind> parameterKind(const FeatureKind kind)
{
    switch (kind)
    {
    case FeatureKind::Integer: return Kind::Integer;
    case FeatureKind::Float: return Kind::Float;
    case FeatureKind::Boolean: return Kind::Boolean;
    case FeatureKind::String: return Kind::String;
    case FeatureKind::Enumeration: return Kind::Enumeration;
    case FeatureKind::Command: return Kind::Command;
    default: return std::nullopt;
    }
}

std::string accessName(const Framegrabber::AppletFeatureNode& node)
{
    return node.accessName.empty() ? node.name : node.accessName;
}

/** @brief Converts an SDK integer to the exact signed protocol integer, if representable. */
std::optional<std::int64_t> signedInteger(const Framegrabber::ParameterValue& value)
{
    return std::visit([](const auto& typed) -> std::optional<std::int64_t> {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, std::uint64_t>)
        {
            if (typed > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))
                return std::nullopt;
            return static_cast<std::int64_t>(typed);
        }
        else if constexpr (std::is_integral_v<T>)
        {
            return static_cast<std::int64_t>(typed);
        }
        else
        {
            return std::nullopt;
        }
    }, value);
}

std::optional<double> floatingValue(const Framegrabber::ParameterValue& value)
{
    if (const auto* number = std::get_if<double>(&value))
    {
        return *number;
    }
    return std::nullopt;
}

/** @brief Stores an exact integer in the SDK type of the prototype, refusing overflow. */
bool assignInteger(Framegrabber::ParameterValue& prototype, const std::int64_t value)
{
    return std::visit([value](auto& typed) -> bool {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_integral_v<T>)
        {
            if constexpr (std::is_signed_v<T>)
            {
                if (value < static_cast<std::int64_t>((std::numeric_limits<T>::min)())
                    || value > static_cast<std::int64_t>((std::numeric_limits<T>::max)()))
                    return false;
            }
            else
            {
                if (value < 0
                    || static_cast<std::uint64_t>(value)
                        > static_cast<std::uint64_t>((std::numeric_limits<T>::max)()))
                    return false;
            }
            typed = static_cast<T>(value);
            return true;
        }
        else
        {
            return false;
        }
    }, prototype);
}

const Framegrabber::AppletFeatureNode* findNode(
    const std::vector<Framegrabber::AppletFeatureNode>& nodes,
    const std::string& name)
{
    for (const auto& node : nodes)
    {
        if (node.kind == FeatureKind::Category)
        {
            if (const auto* found = findNode(node.children, name)) return found;
        }
        else if (accessName(node) == name)
        {
            return &node;
        }
    }
    return nullptr;
}

bool sameObservation(const DeviceParameterDescriptor& a, const DeviceParameterDescriptor& b)
{
    const auto fields = [](const DeviceParameterDescriptor& d) {
        return std::tie(d.kind, d.displayName, d.description, d.unit, d.available, d.readable,
            d.writable, d.unavailableReason, d.observationError, d.value, d.integerMinimum,
            d.integerMaximum, d.integerIncrement, d.floatMinimum, d.floatMaximum,
            d.floatIncrement, d.enumEntries, d.target.selectors);
    };
    return fields(a) == fields(b);
}

template<class Callback, class Value>
void complete(const Callback& callback, Value value) noexcept
{
    try { callback(std::move(value)); }
    catch (...) { qWarning() << "[Framegrabber] parameter consumer callback threw."; }
}
}

DeviceParameterDescriptor FramegrabberSourceController::observeParameter(
    const Framegrabber::AppletFeatureNode& node,
    const unsigned int dmaIndex,
    const std::string& connection)
{
    DeviceParameterDescriptor result;
    result.target = {connection, appletScope(dmaIndex), accessName(node), {}};
    result.kind = parameterKind(node.kind).value_or(Kind::String);
    result.displayName = node.displayName;
    result.description = node.description.empty() ? node.toolTip : node.description;
    result.available = node.parameterId >= 0 && (node.readable || node.writable);
    result.readable = result.available && node.readable && result.kind != Kind::Command;
    result.writable = result.available && node.writable;
    if (!result.available)
    {
        result.unavailableReason = node.parameterId < 0
            ? "The applet description does not bind this feature to a parameter."
            : "The parameter is not accessible in the current applet state.";
    }
    if (result.kind == Kind::Enumeration)
    {
        for (const auto& entry : node.enumEntries) result.enumEntries.push_back(entry.name);
    }

    const auto addError = [&result](const std::string& message) {
        if (!result.observationError.empty()) result.observationError += "; ";
        result.observationError += message;
    };
    if (result.available && (result.kind == Kind::Integer || result.kind == Kind::Float))
    {
        const auto limits = _framegrabber->getAppletParameterLimits(node.parameterId, dmaIndex);
        if (result.kind == Kind::Integer)
        {
            const auto minimum = limits.minimum ? signedInteger(*limits.minimum) : std::nullopt;
            const auto maximum = limits.maximum ? signedInteger(*limits.maximum) : std::nullopt;
            const auto increment = limits.increment ? signedInteger(*limits.increment) : std::nullopt;
            // Reported limits are used only when they are mutually consistent.
            if (!(minimum && maximum && *minimum > *maximum))
            {
                result.integerMinimum = minimum;
                result.integerMaximum = maximum;
                if (minimum && increment && *increment > 1) result.integerIncrement = increment;
            }
        }
        else
        {
            const auto minimum = limits.minimum ? floatingValue(*limits.minimum) : std::nullopt;
            const auto maximum = limits.maximum ? floatingValue(*limits.maximum) : std::nullopt;
            const auto increment = limits.increment ? floatingValue(*limits.increment) : std::nullopt;
            if (minimum && std::isfinite(*minimum)) result.floatMinimum = minimum;
            if (maximum && std::isfinite(*maximum)) result.floatMaximum = maximum;
            if (result.floatMinimum && result.floatMaximum && *result.floatMinimum > *result.floatMaximum)
            {
                result.floatMinimum.reset();
                result.floatMaximum.reset();
            }
            if (increment && std::isfinite(*increment) && *increment > 0) result.floatIncrement = increment;
        }
    }

    if (result.readable)
    {
        Framegrabber::ParameterValue value = std::int64_t{0};
        if (!_framegrabber->getAppletParameterById(node.parameterId, dmaIndex, value, true))
        {
            addError("The SDK could not read the parameter.");
        }
        else
        {
            switch (result.kind)
            {
            case Kind::Integer:
                if (const auto number = signedInteger(value)) result.value = *number;
                else addError("The value is not representable as a signed 64-bit integer.");
                break;
            case Kind::Boolean:
                if (const auto number = signedInteger(value)) result.value = *number != 0;
                else addError("The boolean parameter has a non-integer SDK type.");
                break;
            case Kind::Float:
                if (const auto number = floatingValue(value); number && std::isfinite(*number))
                    result.value = *number;
                else if (const auto integral = signedInteger(value))
                    result.value = static_cast<double>(*integral);
                else addError("Non-finite numeric observation.");
                break;
            case Kind::String:
                if (const auto* textValue = std::get_if<std::string>(&value)) result.value = *textValue;
                else addError("The string parameter has a non-string SDK type.");
                break;
            case Kind::Enumeration:
            {
                const auto number = signedInteger(value);
                for (const auto& entry : node.enumEntries)
                    if (number && entry.value == *number) result.value = entry.name;
                if (!result.value) addError("The current value is not a declared enumeration entry.");
                break;
            }
            case Kind::Command:
                break;
            }
        }
    }

    const std::string key = connection + "/" + result.target.scope + "/" + result.target.name;
    const auto previous = _observations.find(key);
    result.revision = previous != _observations.end() && sameObservation(previous->second, result)
        ? previous->second.revision : ++_revision;
    _observations[key] = result;
    return result;
}

DeviceParameterResult FramegrabberSourceController::requestParameters(InventoryCompletion completion)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!completion) return failure(Error::InvalidArgument, "A completion is required.");
    if (!_framegrabber || !_framegrabber->isOpened()) return failure(Error::Disconnected, "No applet is loaded.");
    if (workerBusy() || !_framegrabber->tryBeginControl())
        return failure(Error::Busy, "Another frame grabber control operation is active.");
    const std::string connection = connectionId().toStdString();
    _work = QtConcurrent::run([this, connection, completion = std::move(completion)] {
        DeviceParameterInventory inventory;
        try {
            for (auto it = _observations.begin(); it != _observations.end();)
                it = it->second.target.connectionId != connection ? _observations.erase(it) : std::next(it);
            const int dmaCount = _framegrabber->getDMACount();
            for (int dma = 0; dma < dmaCount; ++dma) {
                const auto dmaIndex = static_cast<unsigned int>(dma);
                std::function<void(const std::vector<Framegrabber::AppletFeatureNode>&)> flatten =
                    [&](const std::vector<Framegrabber::AppletFeatureNode>& nodes) {
                        for (const auto& node : nodes) {
                            if (node.kind == FeatureKind::Category) flatten(node.children);
                            else if (parameterKind(node.kind))
                                inventory.parameters.push_back(observeParameter(node, dmaIndex, connection));
                        }
                    };
                flatten(_framegrabber->getAppletFeatureModel(dmaIndex));
            }
            if (connectionId().toStdString() != connection) {
                inventory.parameters.clear();
                inventory.result = failure(Error::Conflict, "Connection generation changed during enumeration.");
            }
        } catch (const std::exception& error) {
            inventory.parameters.clear();
            inventory.result = failure(Error::BackendFailure, error.what());
        } catch (...) {
            inventory.parameters.clear();
            inventory.result = failure(Error::BackendFailure, "Unexpected parameter inventory failure.");
        }
        _framegrabber->endControl();
        complete(completion, std::move(inventory));
    });
    return {};
}

DeviceParameterResult FramegrabberSourceController::requestReadParameter(
    DeviceParameterTarget target, IDeviceParameterControl::Completion completion)
{
    return requestParameter(std::move(target), {}, {}, false, std::move(completion));
}

DeviceParameterResult FramegrabberSourceController::requestWriteParameter(
    DeviceParameterTarget target, DeviceParameterValue value,
    std::optional<std::uint64_t> expectedRevision, IDeviceParameterControl::Completion completion)
{
    return requestParameter(std::move(target), std::move(value), expectedRevision, true,
        std::move(completion));
}

DeviceParameterResult FramegrabberSourceController::requestExecuteParameter(
    DeviceParameterTarget target, std::optional<std::uint64_t> expectedRevision,
    IDeviceParameterControl::Completion completion)
{
    return requestParameter(std::move(target), {}, expectedRevision, true, std::move(completion));
}

DeviceParameterResult FramegrabberSourceController::requestParameter(
    DeviceParameterTarget target, std::optional<DeviceParameterValue> value,
    std::optional<std::uint64_t> revision, const bool mutate,
    IDeviceParameterControl::Completion completion)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!completion) return failure(Error::InvalidArgument, "A completion is required.");
    for (const auto* field : {&target.connectionId, &target.scope, &target.name})
        if (field->empty() || field->find('\0') != std::string::npos)
            return failure(Error::InvalidArgument, "Exact target fields are required.");
    const auto dmaIndex = parseScope(target.scope);
    if (!dmaIndex) return failure(Error::Unsupported, "An exact Applet:<dma> scope is required.");
    if (!target.selectors.empty())
        return failure(Error::InvalidArgument, "Applet parameters have no selectors.");
    if (!_framegrabber || !_framegrabber->isOpened()) return failure(Error::Disconnected, "No applet is loaded.");
    if (target.connectionId != connectionId().toStdString())
        return failure(Error::Conflict, "Connection generation is not active.");
    if (static_cast<int>(*dmaIndex) >= _framegrabber->getDMACount())
        return failure(Error::NotFound, "The applet has no such DMA channel.");
    if (workerBusy() || !_framegrabber->tryBeginControl())
        return failure(Error::Busy, "Another frame grabber control operation is active.");

    _work = QtConcurrent::run([this, target = std::move(target), value = std::move(value), revision,
                               mutate, dma = *dmaIndex, completion = std::move(completion)] {
        DeviceParameterResult result;
        bool dispatched = false;
        try {
            const auto model = _framegrabber->getAppletFeatureModel(dma);
            const auto* node = findNode(model, target.name);
            if (!node) result = failure(Error::NotFound, "Parameter was not found.");
            else if (!parameterKind(node->kind)) result = failure(Error::Unsupported, "Feature is not a typed parameter.");
            else {
                auto current = observeParameter(*node, dma, target.connectionId);
                result.descriptor = current;
                if (!mutate) {
                    if (!current.available) result.error = Error::Unavailable;
                    else if (!current.readable) result.error = Error::Unreadable;
                    else if (!current.observationError.empty()) result.error = Error::BackendFailure;
                } else {
                    // Fresh validation against the observed descriptor, then one SDK write.
                    result.error = validateDeviceParameterWrite(current, target, value, revision);
                    Framegrabber::ParameterValue sdkValue = std::int64_t{0};
                    if (result.error == Error::None && current.kind != Kind::Command) {
                        const auto prototype = _framegrabber->appletParameterPrototype(node->parameterId, dma);
                        bool converted = prototype.has_value();
                        if (converted) {
                            sdkValue = *prototype;
                            switch (current.kind) {
                            case Kind::Integer: converted = assignInteger(sdkValue, std::get<std::int64_t>(*value)); break;
                            case Kind::Boolean: converted = assignInteger(sdkValue, std::get<bool>(*value) ? 1 : 0); break;
                            case Kind::Enumeration: {
                                converted = false;
                                for (const auto& entry : node->enumEntries)
                                    if (entry.name == std::get<std::string>(*value))
                                        converted = assignInteger(sdkValue, entry.value);
                                break;
                            }
                            case Kind::Float:
                                if (auto* number = std::get_if<double>(&sdkValue)) *number = std::get<double>(*value);
                                else converted = false;
                                break;
                            case Kind::String:
                                if (auto* textValue = std::get_if<std::string>(&sdkValue)) *textValue = std::get<std::string>(*value);
                                else converted = false;
                                break;
                            case Kind::Command: break;
                            }
                        }
                        if (!converted) result.error = Error::InvalidArgument;
                    }
                    if (result.error == Error::None) {
                        dispatched = true;
                        int sdkResult = FG_OK;
                        const bool applied = current.kind == Kind::Command
                            ? _framegrabber->executeAppletCommandById(dma, node->parameterId, &sdkResult)
                            : _framegrabber->setAppletParameterById(
                                  node->parameterId, dma, sdkValue, true, false, &sdkResult);
                        result.effect = applied ? DeviceParameterEffect::Applied : DeviceParameterEffect::None;
                        if (!applied) {
                            result.error = Error::BackendFailure;
                            result.message = "The SDK rejected the write: "
                                + std::string(Fg_getErrorDescription(nullptr, sdkResult))
                                + " (" + std::to_string(sdkResult) + ").";
                        }
                        // Report actual readback after the effect, not the requested value.
                        result.descriptor.reset();
                        const auto refreshed = _framegrabber->getAppletFeatureModel(dma);
                        if (const auto* after = findNode(refreshed, target.name))
                            result.descriptor = observeParameter(*after, dma, target.connectionId);
                    }
                }
                if (result.error != Error::None && result.message.empty())
                    result.message = "Parameter access, value, connection or revision validation failed.";
            }
        } catch (const std::exception& error) {
            result.error = Error::BackendFailure;
            result.message = error.what();
            if (dispatched && result.effect != DeviceParameterEffect::Applied) {
                result.effect = DeviceParameterEffect::Unknown;
                result.descriptor.reset();
            }
        } catch (...) {
            result.error = Error::BackendFailure;
            result.message = "Unexpected parameter backend failure.";
            if (dispatched && result.effect != DeviceParameterEffect::Applied) {
                result.effect = DeviceParameterEffect::Unknown;
                result.descriptor.reset();
            }
        }
        _framegrabber->endControl();
        complete(completion, std::move(result));
    });
    return {};
}
