#include "FramegrabberSourceController.h"

#include <QDebug>
#include <QDomDocument>
#include <QSet>
#include <QStringList>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <tuple>
#include <type_traits>
#include <utility>

// Applet parameters are exposed per DMA channel under the scope "Applet:<dma>". Their
// names are the SDK access names (Fg_getParameterNameById). CoaXPress camera features are
// exposed per DMA channel under "Camera:<transport>:<dma>"; their names are GenICam feature
// names parsed from the SDK's feature XML. Neither scope has GenICam selectors, so targets
// carry an empty selector list. Both scopes provide a category path, visibility, preferred
// representation and numeric constraints so a consumer can render the tree.

namespace
{
using Error = DeviceParameterError;
using Kind = DeviceParameterKind;
using FeatureKind = Framegrabber::AppletFeatureKind;
using Transport = Framegrabber::CameraTransport;

constexpr const char* kAppletPrefix = "Applet:";
constexpr const char* kCameraPrefix = "Camera:";

std::string appletScope(const unsigned int dmaIndex)
{
    return kAppletPrefix + std::to_string(dmaIndex);
}

std::optional<unsigned int> parseAppletScope(const std::string& scope)
{
    const std::string prefix(kAppletPrefix);
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

std::string transportLabel(const Transport transport)
{
    switch (transport)
    {
    case Transport::CoaXPress: return "cxp";
    case Transport::CameraLink: return "cl";
    case Transport::None: break;
    }
    return "none";
}

std::string cameraScope(const Transport transport, const unsigned int dmaIndex)
{
    return std::string(kCameraPrefix) + transportLabel(transport) + ":" + std::to_string(dmaIndex);
}

struct CameraScope
{
    Transport transport = Transport::None;
    unsigned int dmaIndex = 0;
};

std::optional<CameraScope> parseCameraScope(const std::string& scope)
{
    const std::string prefix(kCameraPrefix);
    if (scope.compare(0, prefix.size(), prefix) != 0 || scope.size() == prefix.size())
    {
        return std::nullopt;
    }
    const std::string rest = scope.substr(prefix.size());
    const auto separator = rest.rfind(':');
    if (separator == std::string::npos || separator == 0 || separator + 1 == rest.size())
    {
        return std::nullopt;
    }
    const std::string label = rest.substr(0, separator);
    Transport transport = Transport::None;
    if (label == "cxp") transport = Transport::CoaXPress;
    else if (label == "cl") transport = Transport::CameraLink;
    else return std::nullopt;
    unsigned int dmaIndex = 0;
    const char* first = rest.data() + separator + 1;
    const char* last = rest.data() + rest.size();
    const auto parsed = std::from_chars(first, last, dmaIndex);
    if (parsed.ec != std::errc{} || parsed.ptr != last
        || scope != cameraScope(transport, dmaIndex))
    {
        return std::nullopt;
    }
    return CameraScope{transport, dmaIndex};
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

std::optional<Kind> cameraKind(const QString& tag)
{
    if (tag == QStringLiteral("Integer") || tag == QStringLiteral("IntReg")
        || tag == QStringLiteral("MaskedIntReg"))
        return Kind::Integer;
    if (tag == QStringLiteral("Float") || tag == QStringLiteral("FloatReg")) return Kind::Float;
    if (tag == QStringLiteral("Boolean")) return Kind::Boolean;
    if (tag == QStringLiteral("String") || tag == QStringLiteral("StringReg")) return Kind::String;
    if (tag == QStringLiteral("Enumeration")) return Kind::Enumeration;
    if (tag == QStringLiteral("Command")) return Kind::Command;
    return std::nullopt;
}

DeviceParameterRepresentation representationFor(const Kind kind)
{
    switch (kind)
    {
    case Kind::Integer:
    case Kind::Float: return DeviceParameterRepresentation::Number;
    case Kind::Boolean: return DeviceParameterRepresentation::Toggle;
    case Kind::Enumeration: return DeviceParameterRepresentation::Choice;
    case Kind::String: return DeviceParameterRepresentation::Text;
    case Kind::Command: return DeviceParameterRepresentation::Automatic;
    }
    return DeviceParameterRepresentation::Automatic;
}

DeviceParameterVisibility visibilityFrom(const QString& visibility)
{
    if (visibility == QStringLiteral("Expert")) return DeviceParameterVisibility::Advanced;
    if (visibility == QStringLiteral("Guru")) return DeviceParameterVisibility::Expert;
    if (visibility == QStringLiteral("Invisible")) return DeviceParameterVisibility::Hidden;
    return DeviceParameterVisibility::Primary;
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
    const std::string& name,
    std::vector<std::string>* categoryPath = nullptr)
{
    for (const auto& node : nodes)
    {
        if (node.kind == FeatureKind::Category)
        {
            if (categoryPath)
            {
                const std::string segment = node.displayName.empty() ? node.name : node.displayName;
                if (categoryPath->size() < deviceParameterCategoryDepthLimit
                    && !segment.empty() && segment.size() <= deviceParameterCategorySegmentLimit)
                {
                    categoryPath->push_back(segment);
                    if (const auto* found = findNode(node.children, name, categoryPath)) return found;
                    categoryPath->pop_back();
                }
                continue;
            }
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
            d.floatIncrement, d.enumEntries, d.target.selectors, d.categoryPath, d.visibility,
            d.representation, d.producerShared);
    };
    return fields(a) == fields(b);
}

template<class Callback, class Value>
void complete(const Callback& callback, Value value) noexcept
{
    try { callback(std::move(value)); }
    catch (...) { qWarning() << "[Framegrabber] parameter consumer callback threw."; }
}

QString elementText(const QDomElement& element, const QString& childName)
{
    return element.firstChildElement(childName).text().trimmed();
}

QString featureDisplayName(const QDomElement& element, const QString& fallback)
{
    const QString displayName = elementText(element, QStringLiteral("DisplayName"));
    return displayName.isEmpty() ? fallback : displayName;
}

QDomElement findFeatureElement(const QDomElement& root, const QString& name)
{
    static const QStringList featureTags{
        QStringLiteral("Integer"), QStringLiteral("IntReg"), QStringLiteral("MaskedIntReg"),
        QStringLiteral("Float"), QStringLiteral("FloatReg"), QStringLiteral("Enumeration"),
        QStringLiteral("Boolean"), QStringLiteral("String"), QStringLiteral("StringReg"),
        QStringLiteral("Command"), QStringLiteral("Register")};
    for (const QString& tag : featureTags)
    {
        const QDomNodeList nodes = root.elementsByTagName(tag);
        for (int index = 0; index < nodes.count(); ++index)
        {
            const QDomElement element = nodes.at(index).toElement();
            if (element.attribute(QStringLiteral("Name")) == name) return element;
        }
    }
    return {};
}

QDomElement findCategoryElement(const QDomElement& root, const QString& name)
{
    const QDomNodeList categories = root.elementsByTagName(QStringLiteral("Category"));
    for (int index = 0; index < categories.count(); ++index)
    {
        const QDomElement category = categories.at(index).toElement();
        if (category.attribute(QStringLiteral("Name")) == name) return category;
    }
    return {};
}

/** @brief One flattened camera feature declaration copied from the SDK XML. */
struct CameraFeatureDecl
{
    std::string name;
    std::string tag;
    std::string displayName;
    std::string description;
    std::string unit;
    std::string accessMode;
    DeviceParameterVisibility visibility = DeviceParameterVisibility::Primary;
    std::vector<std::string> categoryPath;
    std::vector<std::string> enumEntries;
};

void appendCategoryPath(std::vector<std::string>* path, const QString& displayName,
                        const QString& fallback)
{
    const std::string segment = (displayName.isEmpty() ? fallback : displayName).toStdString();
    if (segment.empty() || segment.size() > deviceParameterCategorySegmentLimit) return;
    if (path->size() >= deviceParameterCategoryDepthLimit) return;
    path->push_back(segment);
}

void collectCategory(const QDomElement& root, const QDomElement& category,
                     std::vector<std::string> path, std::vector<CameraFeatureDecl>* features,
                     QSet<QString>& visiting)
{
    const QString categoryName = category.attribute(QStringLiteral("Name"));
    if (categoryName.isEmpty() || visiting.contains(categoryName)) return;
    visiting.insert(categoryName);
    if (categoryName != QStringLiteral("Root"))
        appendCategoryPath(&path, featureDisplayName(category, categoryName), categoryName);

    for (QDomElement child = category.firstChildElement(QStringLiteral("pFeature"));
         !child.isNull();
         child = child.nextSiblingElement(QStringLiteral("pFeature")))
    {
        const QString featureName = child.text().trimmed();
        const QDomElement childCategory = findCategoryElement(root, featureName);
        if (!childCategory.isNull())
        {
            collectCategory(root, childCategory, path, features, visiting);
            continue;
        }
        const QDomElement feature = findFeatureElement(root, featureName);
        if (feature.isNull()) continue;
        const auto kind = cameraKind(feature.tagName());
        if (!kind) continue;
        if (path.size() > deviceParameterCategoryDepthLimit) continue;
        CameraFeatureDecl decl;
        decl.name = featureName.toStdString();
        decl.tag = feature.tagName().toStdString();
        decl.displayName = featureDisplayName(feature, featureName).toStdString();
        decl.description = elementText(feature, QStringLiteral("Description")).toStdString();
        if (decl.description.empty()) decl.description = elementText(feature, QStringLiteral("ToolTip")).toStdString();
        decl.unit = elementText(feature, QStringLiteral("Unit")).toStdString();
        decl.accessMode = elementText(feature, QStringLiteral("AccessMode")).toStdString();
        decl.visibility = visibilityFrom(elementText(feature, QStringLiteral("Visibility")));
        decl.categoryPath = path;
        const QDomNodeList entries = feature.elementsByTagName(QStringLiteral("EnumEntry"));
        for (int index = 0; index < entries.count(); ++index)
        {
            const QString entry = entries.at(index).toElement().attribute(QStringLiteral("Name"));
            if (!entry.isEmpty()) decl.enumEntries.push_back(entry.toStdString());
        }
        features->push_back(std::move(decl));
    }
    visiting.remove(categoryName);
}

std::vector<CameraFeatureDecl> cameraFeatureDeclarations(const std::string& xml)
{
    std::vector<CameraFeatureDecl> features;
    if (xml.empty()) return features;
    QDomDocument document;
    if (!document.setContent(QString::fromStdString(xml)))
    {
        qWarning() << "[Framegrabber] Camera feature XML could not be parsed.";
        return features;
    }
    const QDomElement root = document.documentElement();
    QSet<QString> visiting;
    const QDomElement rootCategory = findCategoryElement(root, QStringLiteral("Root"));
    if (!rootCategory.isNull())
    {
        collectCategory(root, rootCategory, {}, &features, visiting);
    }
    else
    {
        const QDomNodeList categories = root.elementsByTagName(QStringLiteral("Category"));
        for (int index = 0; index < categories.count(); ++index)
        {
            const QDomElement category = categories.at(index).toElement();
            const QDomElement parent = category.parentNode().toElement();
            if (parent.isNull() || parent.tagName() != QStringLiteral("Categories")) continue;
            collectCategory(root, category, {}, &features, visiting);
        }
    }
    return features;
}

const CameraFeatureDecl* findCameraFeature(const std::vector<CameraFeatureDecl>& features,
                                           const std::string& name)
{
    for (const auto& feature : features)
        if (feature.name == name) return &feature;
    return nullptr;
}
}

DeviceParameterDescriptor FramegrabberSourceController::observeParameter(
    const Framegrabber::AppletFeatureNode& node,
    const unsigned int dmaIndex,
    const std::string& connection,
    const std::vector<std::string>& categoryPath)
{
    DeviceParameterDescriptor result;
    result.target = {connection, appletScope(dmaIndex), accessName(node), {}};
    result.kind = parameterKind(node.kind).value_or(Kind::String);
    result.displayName = node.displayName;
    result.description = node.description.empty() ? node.toolTip : node.description;
    result.categoryPath = categoryPath;
    result.representation = representationFor(result.kind);
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
    if (result.available && (result.kind == Kind::Integer || result.kind == Kind::Float)
        && _backend.appletParameterLimits)
    {
        const auto limits = _backend.appletParameterLimits(node.parameterId, dmaIndex);
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

    if (result.readable && _backend.getAppletParameter)
    {
        Framegrabber::ParameterValue value = std::int64_t{0};
        if (!_backend.getAppletParameter(node.parameterId, dmaIndex, value, true))
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
    std::lock_guard<std::mutex> lock(_observationMutex);
    const auto previous = _observations.find(key);
    result.revision = previous != _observations.end() && sameObservation(previous->second, result)
        ? previous->second.revision : ++_revision;
    _observations[key] = result;
    return result;
}

DeviceParameterDescriptor FramegrabberSourceController::observeCameraParameter(
    const Transport transport, const unsigned int dmaIndex, const std::string& connection,
    const std::string& featureName, const std::string& tag, const std::string& displayName,
    const std::string& description, const std::string& unit, const std::string& accessMode,
    const DeviceParameterVisibility visibility,
    const std::vector<std::string>& categoryPath, const std::vector<std::string>& enumEntries)
{
    DeviceParameterDescriptor result;
    result.target = {connection, cameraScope(transport, dmaIndex), featureName, {}};
    result.kind = cameraKind(QString::fromStdString(tag)).value_or(Kind::String);
    result.displayName = displayName;
    result.description = description;
    result.unit = unit;
    result.categoryPath = categoryPath;
    result.visibility = visibility;
    result.representation = representationFor(result.kind);
    const QString mode = QString::fromStdString(accessMode).trimmed();
    const bool readable = mode.isEmpty() || mode == QStringLiteral("RW") || mode == QStringLiteral("RO");
    // Writability is the access mode observed now, gated by the current acquisition state;
    // it is never cached from a locked state.
    const bool writable = (mode.isEmpty() || mode == QStringLiteral("RW") || mode == QStringLiteral("WO"))
        && !(_backend.isGrabbing && _backend.isGrabbing());
    result.readable = readable && result.kind != Kind::Command;
    result.writable = writable;
    result.available = result.readable || result.writable;
    if (!result.available)
        result.unavailableReason = "The camera does not allow this feature in its current access mode.";
    if (result.kind == Kind::Enumeration) result.enumEntries = enumEntries;

    if (result.kind == Kind::Command)
    {
        result.value.reset();
    }
    else if (result.readable && _backend.getCameraFeature)
    {
        Framegrabber::ParameterValue value = std::int64_t{0};
        if (result.kind == Kind::Float) value = 0.0;
        else if (result.kind == Kind::String || result.kind == Kind::Enumeration) value = std::string{};
        else if (result.kind == Kind::Boolean) value = std::uint32_t{0};
        if (!_backend.getCameraFeature(transport, dmaIndex, featureName, value))
        {
            result.observationError = "The SDK could not read the camera feature.";
        }
        else
        {
            switch (result.kind)
            {
            case Kind::Integer:
                if (const auto number = signedInteger(value)) result.value = *number;
                else result.observationError = "The value is not representable as a signed 64-bit integer.";
                break;
            case Kind::Boolean:
                if (const auto number = signedInteger(value)) result.value = *number != 0;
                else result.observationError = "The boolean feature has a non-integer SDK type.";
                break;
            case Kind::Float:
                if (const auto number = floatingValue(value); number && std::isfinite(*number))
                    result.value = *number;
                else if (const auto integral = signedInteger(value))
                    result.value = static_cast<double>(*integral);
                else result.observationError = "Non-finite numeric observation.";
                break;
            case Kind::String:
                if (const auto* textValue = std::get_if<std::string>(&value)) result.value = *textValue;
                else result.observationError = "The string feature has a non-string SDK type.";
                break;
            case Kind::Enumeration:
                if (const auto* entry = std::get_if<std::string>(&value)) result.value = *entry;
                else result.observationError = "The enumeration feature has a non-string SDK type.";
                break;
            case Kind::Command:
                break;
            }
        }
    }

    if (result.available && (result.kind == Kind::Integer || result.kind == Kind::Float)
        && _backend.cameraFeatureLimits)
    {
        const auto limits = _backend.cameraFeatureLimits(transport, dmaIndex, featureName,
            result.kind == Kind::Float);
        if (result.kind == Kind::Integer)
        {
            const auto minimum = limits.minimum ? signedInteger(*limits.minimum) : std::nullopt;
            const auto maximum = limits.maximum ? signedInteger(*limits.maximum) : std::nullopt;
            const auto increment = limits.increment ? signedInteger(*limits.increment) : std::nullopt;
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

    const std::string key = connection + "/" + result.target.scope + "/" + result.target.name;
    std::lock_guard<std::mutex> lock(_observationMutex);
    const auto previous = _observations.find(key);
    result.revision = previous != _observations.end() && sameObservation(previous->second, result)
        ? previous->second.revision : ++_revision;
    _observations[key] = result;
    return result;
}

FramegrabberSourceController::Observation FramegrabberSourceController::observeAll(
    const std::string& connection, const bool purge)
{
    Observation result;
    std::map<std::string, DeviceParameterDescriptor> previous;
    {
        std::lock_guard<std::mutex> lock(_observationMutex);
        previous = _observations;
    }
    std::set<std::string> seen;
    const auto visit = [&](DeviceParameterDescriptor descriptor) {
        const std::string key = descriptor.target.connectionId + "/"
            + descriptor.target.scope + "/" + descriptor.target.name;
        seen.insert(key);
        const auto earlier = previous.find(key);
        if (earlier == previous.end() || !sameObservation(earlier->second, descriptor))
            result.changed.push_back(descriptor);
        if (result.descriptors.size() < deviceParameterSnapshotDescriptorLimit)
            result.descriptors.push_back(std::move(descriptor));
        else if (result.error.empty())
            result.error = "The parameter inventory exceeds the snapshot bound.";
    };

    const int dmaCount = _backend.dmaCount ? _backend.dmaCount() : 0;
    for (int dma = 0; dma < dmaCount && result.error.empty(); ++dma)
    {
        const auto dmaIndex = static_cast<unsigned int>(dma);
        const auto model = _backend.appletFeatureModel ? _backend.appletFeatureModel(dmaIndex)
                                                       : std::vector<Framegrabber::AppletFeatureNode>{};
        std::vector<std::string> path;
        std::function<void(const std::vector<Framegrabber::AppletFeatureNode>&)> flatten =
            [&](const std::vector<Framegrabber::AppletFeatureNode>& nodes) {
                for (const auto& node : nodes)
                {
                    if (node.kind == FeatureKind::Category)
                    {
                        const std::string segment = node.displayName.empty() ? node.name : node.displayName;
                        if (path.size() < deviceParameterCategoryDepthLimit
                            && !segment.empty()
                            && segment.size() <= deviceParameterCategorySegmentLimit)
                        {
                            path.push_back(segment);
                            flatten(node.children);
                            path.pop_back();
                        }
                        continue;
                    }
                    if (!parameterKind(node.kind)) continue;
                    visit(observeParameter(node, dmaIndex, connection, path));
                }
            };
        flatten(model);
    }

    if (result.error.empty() && _backend.cameraCapabilities && _backend.cachedCameras
        && _backend.cameraFeatureXml)
    {
        for (const auto& capability : _backend.cameraCapabilities())
        {
            if (!capability.canReadFeatures || capability.transport == Transport::None) continue;
            for (const auto& camera : _backend.cachedCameras(capability.transport))
            {
                if (!camera.connected) continue;
                const auto xml = _backend.cameraFeatureXml(capability.transport, camera.dmaIndex);
                if (xml.empty()) continue;
                for (const auto& feature : cameraFeatureDeclarations(xml))
                {
                    visit(observeCameraParameter(capability.transport, camera.dmaIndex, connection,
                        feature.name, feature.tag, feature.displayName,
                        feature.description, feature.unit,
                        feature.accessMode, feature.visibility, feature.categoryPath,
                        feature.enumEntries));
                    if (!result.error.empty()) break;
                }
                if (!result.error.empty()) break;
            }
            if (!result.error.empty()) break;
        }
    }

    if (purge)
    {
        std::lock_guard<std::mutex> lock(_observationMutex);
        for (auto it = _observations.begin(); it != _observations.end();)
        {
            if (it->second.target.connectionId == connection && !seen.count(it->first))
            {
                result.invalidated.push_back(it->second.target);
                it = _observations.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
    return result;
}

DeviceParameterChange FramegrabberSourceController::snapshotChange(
    const std::string& connection, const std::string& subscriptionId) const
{
    DeviceParameterChange change;
    change.kind = DeviceParameterChangeKind::Snapshot;
    change.connectionId = connection;
    change.subscriptionId = subscriptionId;
    std::lock_guard<std::mutex> lock(_observationMutex);
    for (const auto& entry : _observations)
    {
        const auto& descriptor = entry.second;
        if (descriptor.target.connectionId != connection) continue;
        if (change.descriptors.size() >= deviceParameterSnapshotDescriptorLimit) break;
        change.descriptors.push_back(descriptor);
        change.revision = std::max(change.revision, descriptor.revision);
    }
    return change;
}

bool FramegrabberSourceController::hasObservations(const std::string& connection) const
{
    std::lock_guard<std::mutex> lock(_observationMutex);
    for (const auto& entry : _observations)
        if (entry.second.target.connectionId == connection) return true;
    return false;
}

DeviceParameterResult FramegrabberSourceController::requestParameters(InventoryCompletion completion)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!completion) return failure(Error::InvalidArgument, "A completion is required.");
    if (!_sessionAvailable.load(std::memory_order_acquire)
        || (_backend.isOpened && !_backend.isOpened()))
        return failure(Error::Disconnected, "No applet is loaded.");
    if (workerBusy() || (_backend.tryBeginControl && !_backend.tryBeginControl()))
        return failure(Error::Busy, "Another frame grabber control operation is active.");
    const std::string connection = connectionId().toStdString();
    _work = QtConcurrent::run([this, connection, completion = std::move(completion)] {
        DeviceParameterInventory inventory;
        try {
            const Observation observed = observeAll(connection, true);
            if (!observed.error.empty()) {
                inventory.result = failure(Error::BackendFailure, observed.error);
            } else if (connectionId().toStdString() != connection) {
                inventory.result = failure(Error::Conflict, "Connection generation changed during enumeration.");
            } else {
                inventory.parameters = observed.descriptors;
            }
        } catch (const std::exception& error) {
            inventory.parameters.clear();
            inventory.result = failure(Error::BackendFailure, error.what());
        } catch (...) {
            inventory.parameters.clear();
            inventory.result = failure(Error::BackendFailure, "Unexpected parameter inventory failure.");
        }
        if (_backend.endControl) _backend.endControl();
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
    const auto appletDma = parseAppletScope(target.scope);
    const auto camera = parseCameraScope(target.scope);
    if (!appletDma && !camera)
        return failure(Error::Unsupported, "An exact Applet:<dma> or Camera:<transport>:<dma> scope is required.");
    if (!target.selectors.empty())
        return failure(Error::InvalidArgument, "Frame grabber parameters have no selectors.");
    if (!_sessionAvailable.load(std::memory_order_acquire)
        || (_backend.isOpened && !_backend.isOpened()))
        return failure(Error::Disconnected, "No applet is loaded.");
    if (target.connectionId != connectionId().toStdString())
        return failure(Error::Conflict, "Connection generation is not active.");
    if (camera)
    {
        bool supported = false;
        if (_backend.cameraCapabilities)
            for (const auto& capability : _backend.cameraCapabilities())
                if (capability.transport == camera->transport && capability.canReadFeatures)
                    supported = true;
        if (!supported) return failure(Error::NotFound, "The camera transport has no feature tree.");
    }
    if (workerBusy() || (_backend.tryBeginControl && !_backend.tryBeginControl()))
        return failure(Error::Busy, "Another frame grabber control operation is active.");

    _work = QtConcurrent::run([this, target = std::move(target), value = std::move(value), revision,
                               mutate, appletDma, camera, completion = std::move(completion)] {
        DeviceParameterResult result;
        bool dispatched = false;
        const Transport transport = camera ? camera->transport : Transport::None;
        const unsigned int dma = camera ? camera->dmaIndex : *appletDma;
        try {
            if (camera)
            {
                const auto declarations = _backend.cameraFeatureXml
                    ? cameraFeatureDeclarations(_backend.cameraFeatureXml(transport, dma))
                    : std::vector<CameraFeatureDecl>{};
                const auto* feature = findCameraFeature(declarations, target.name);
                if (!feature) {
                    result = failure(Error::NotFound, "Camera feature was not found.");
                } else {
                    auto current = observeCameraParameter(transport, dma, target.connectionId,
                        feature->name, feature->tag, feature->displayName,
                        feature->description, feature->unit,
                        feature->accessMode, feature->visibility, feature->categoryPath,
                        feature->enumEntries);
                    result.descriptor = current;
                    if (!mutate) {
                        if (!current.available) result.error = Error::Unavailable;
                        else if (!current.readable) result.error = Error::Unreadable;
                        else if (!current.observationError.empty()) result.error = Error::BackendFailure;
                    } else {
                        result.error = validateDeviceParameterWrite(current, target, value, revision);
                        Framegrabber::ParameterValue sdkValue = std::int64_t{0};
                        if (result.error == Error::None && current.kind != Kind::Command) {
                            switch (current.kind) {
                            case Kind::Integer: sdkValue = std::get<std::int64_t>(*value); break;
                            case Kind::Boolean: sdkValue = std::int64_t(std::get<bool>(*value) ? 1 : 0); break;
                            case Kind::Enumeration: sdkValue = std::get<std::string>(*value); break;
                            case Kind::Float: sdkValue = std::get<double>(*value); break;
                            case Kind::String: sdkValue = std::get<std::string>(*value); break;
                            case Kind::Command: break;
                            }
                        }
                        if (result.error == Error::None) {
                            dispatched = true;
                            const bool applied = current.kind == Kind::Command
                                ? (_backend.executeCameraCommand
                                    && _backend.executeCameraCommand(transport, dma, target.name))
                                : (_backend.setCameraFeature
                                    && _backend.setCameraFeature(transport, dma, target.name, sdkValue, true));
                            result.effect = applied ? DeviceParameterEffect::Applied : DeviceParameterEffect::None;
                            if (!applied) {
                                result.error = Error::BackendFailure;
                                result.message = "The SDK rejected the camera write.";
                            }
                            result.descriptor.reset();
                            const auto refreshed = _backend.cameraFeatureXml
                                ? cameraFeatureDeclarations(_backend.cameraFeatureXml(transport, dma))
                                : std::vector<CameraFeatureDecl>{};
                            if (const auto* after = findCameraFeature(refreshed, target.name))
                                result.descriptor = observeCameraParameter(transport, dma,
                                    target.connectionId, after->name, after->tag,
                                    after->displayName, after->description,
                                    after->unit, after->accessMode, after->visibility,
                                    after->categoryPath, after->enumEntries);
                        }
                    }
                    if (result.error != Error::None && result.message.empty())
                        result.message = "Parameter access, value, connection or revision validation failed.";
                }
            }
            else
            {
                const auto model = _backend.appletFeatureModel
                    ? _backend.appletFeatureModel(dma) : std::vector<Framegrabber::AppletFeatureNode>{};
                std::vector<std::string> categoryPath;
                const auto* node = findNode(model, target.name, &categoryPath);
                if (!node) result = failure(Error::NotFound, "Parameter was not found.");
                else if (!parameterKind(node->kind)) result = failure(Error::Unsupported, "Feature is not a typed parameter.");
                else {
                    auto current = observeParameter(*node, dma, target.connectionId, categoryPath);
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
                            const auto prototype = _backend.appletParameterPrototype
                                ? _backend.appletParameterPrototype(node->parameterId, dma)
                                : std::optional<Framegrabber::ParameterValue>{};
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
                        }
                        if (result.error == Error::None) {
                            dispatched = true;
                            int sdkResult = FG_OK;
                            const bool applied = current.kind == Kind::Command
                                ? (_backend.executeAppletCommand
                                    && _backend.executeAppletCommand(dma, node->parameterId, &sdkResult))
                                : (_backend.setAppletParameter
                                    && _backend.setAppletParameter(node->parameterId, dma, sdkValue, true, false, &sdkResult));
                            result.effect = applied ? DeviceParameterEffect::Applied : DeviceParameterEffect::None;
                            if (!applied) {
                                result.error = Error::BackendFailure;
                                result.message = "The SDK rejected the write: "
                                    + std::string(Fg_getErrorDescription(nullptr, sdkResult))
                                    + " (" + std::to_string(sdkResult) + ").";
                            }
                            // Report actual readback after the effect, not the requested value.
                            result.descriptor.reset();
                            const auto refreshed = _backend.appletFeatureModel
                                ? _backend.appletFeatureModel(dma)
                                : std::vector<Framegrabber::AppletFeatureNode>{};
                            std::vector<std::string> refreshedPath;
                            if (const auto* after = findNode(refreshed, target.name, &refreshedPath))
                                result.descriptor = observeParameter(*after, dma, target.connectionId, refreshedPath);
                        }
                    }
                    if (result.error != Error::None && result.message.empty())
                        result.message = "Parameter access, value, connection or revision validation failed.";
                }
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
        if (_backend.endControl) _backend.endControl();
        if (dispatched) wakeObservation(false);
        complete(completion, std::move(result));
    });
    return {};
}

DeviceParameterResult FramegrabberSourceController::requestParameterChanges(
    std::string connectionId, std::string subscriptionId, ChangeListener delivery)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (validateDeviceParameterSubscription(connectionId, subscriptionId) != Error::None || !delivery)
        return failure(Error::InvalidArgument, "The parameter change subscription identity is invalid.");
    if (!_sessionAvailable.load(std::memory_order_acquire))
        return failure(Error::Unavailable, "The frame grabber session has ended.");
    if (_backend.isOpened && !_backend.isOpened())
        return failure(Error::Disconnected, "No applet is loaded.");
    if (connectionId != this->connectionId().toStdString())
        return failure(Error::Conflict, "Connection generation is not active.");
    const auto added = _notifications.add(std::move(connectionId), std::move(subscriptionId),
        std::move(delivery));
    if (added.error == Error::None) wakeObservation(true);
    return added;
}

DeviceParameterResult FramegrabberSourceController::releaseParameterChanges(
    std::string connectionId, std::string subscriptionId, IDeviceParameterControl::Completion completion)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!completion) return failure(Error::InvalidArgument, "A completion is required.");
    if (validateDeviceParameterSubscription(connectionId, subscriptionId) != Error::None)
        return failure(Error::InvalidArgument, "The parameter change subscription identity is invalid.");
    const auto removed = _notifications.remove(connectionId, subscriptionId);
    if (removed.error != Error::None) return removed;
    complete(completion, DeviceParameterResult{});
    return {};
}
