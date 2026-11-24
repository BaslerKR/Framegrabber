#include "FramegrabberSourceController.h"

#include <QDebug>
#include <QDomDocument>
#include <QHash>
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

/** @brief One feature and category index built once per XML document. */
struct CameraDocumentIndex
{
    QHash<QString, QDomElement> features;
    QHash<QString, QDomElement> categories;
};

void indexCameraElement(const QDomElement& element, CameraDocumentIndex* index)
{
    const QString name = element.attribute(QStringLiteral("Name"));
    if (!name.isEmpty())
    {
        if (element.tagName() == QStringLiteral("Category")) index->categories.insert(name, element);
        else if (cameraKind(element.tagName())) index->features.insert(name, element);
    }
    for (QDomElement child = element.firstChildElement(); !child.isNull();
         child = child.nextSiblingElement())
        indexCameraElement(child, index);
}

CameraDocumentIndex indexCameraDocument(const QDomElement& root)
{
    CameraDocumentIndex index;
    indexCameraElement(root, &index);
    return index;
}

/** @brief Resolves the access mode declared on the feature or the register it references. */
QString featureAccessMode(const QDomElement& feature, const CameraDocumentIndex& index)
{
    const QString imposed = elementText(feature, QStringLiteral("ImposedAccessMode")).trimmed();
    if (!imposed.isEmpty()) return imposed;
    const QString direct = elementText(feature, QStringLiteral("AccessMode")).trimmed();
    if (!direct.isEmpty()) return direct;
    const QString value = elementText(feature, QStringLiteral("pValue")).trimmed();
    const QString command = elementText(feature, QStringLiteral("pCommand")).trimmed();
    const QDomElement referenced = index.features.value(
        value.isEmpty() ? command : value);
    return referenced.isNull() ? QString()
                               : elementText(referenced, QStringLiteral("AccessMode")).trimmed();
}

void appendCategoryPath(std::vector<std::string>* path, const QString& displayName,
                        const QString& fallback)
{
    const std::string segment = (displayName.isEmpty() ? fallback : displayName).toStdString();
    if (segment.empty() || segment.size() > deviceParameterCategorySegmentLimit) return;
    if (path->size() >= deviceParameterCategoryDepthLimit) return;
    path->push_back(segment);
}

void collectCategory(const CameraDocumentIndex& index, const QDomElement& category,
                     std::vector<std::string> path,
                     std::vector<FramegrabberCameraFeatureDeclaration>* features,
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
        const QDomElement childCategory = index.categories.value(featureName);
        if (!childCategory.isNull())
        {
            collectCategory(index, childCategory, path, features, visiting);
            continue;
        }
        const QDomElement feature = index.features.value(featureName);
        if (feature.isNull()) continue;
        if (!cameraKind(feature.tagName())) continue;
        if (path.size() > deviceParameterCategoryDepthLimit) continue;
        FramegrabberCameraFeatureDeclaration declaration;
        declaration.name = featureName.toStdString();
        declaration.tag = feature.tagName().toStdString();
        declaration.displayName = featureDisplayName(feature, featureName).toStdString();
        declaration.description = elementText(feature, QStringLiteral("Description")).toStdString();
        if (declaration.description.empty())
            declaration.description = elementText(feature, QStringLiteral("ToolTip")).toStdString();
        declaration.unit = elementText(feature, QStringLiteral("Unit")).toStdString();
        declaration.accessMode = featureAccessMode(feature, index).toStdString();
        declaration.visibility = visibilityFrom(elementText(feature, QStringLiteral("Visibility")));
        declaration.lockedFeature = elementText(feature, QStringLiteral("pIsLocked")).toStdString();
        declaration.availableFeature = elementText(feature, QStringLiteral("pIsAvailable")).toStdString();
        declaration.categoryPath = path;
        const QDomNodeList entries = feature.elementsByTagName(QStringLiteral("EnumEntry"));
        for (int position = 0; position < entries.count(); ++position)
        {
            const QString entry = entries.at(position).toElement().attribute(QStringLiteral("Name"));
            if (!entry.isEmpty()) declaration.enumEntries.push_back(entry.toStdString());
        }
        features->push_back(std::move(declaration));
    }
    visiting.remove(categoryName);
}

std::vector<FramegrabberCameraFeatureDeclaration> parseCameraFeatureDeclarations(
    const std::string& xml)
{
    std::vector<FramegrabberCameraFeatureDeclaration> features;
    if (xml.empty()) return features;
    QDomDocument document;
    if (!document.setContent(QString::fromStdString(xml)))
    {
        qWarning() << "[Framegrabber] Camera feature XML could not be parsed.";
        return features;
    }
    const QDomElement root = document.documentElement();
    const CameraDocumentIndex index = indexCameraDocument(root);
    QSet<QString> visiting;
    const QDomElement rootCategory = index.categories.value(QStringLiteral("Root"));
    if (!rootCategory.isNull())
    {
        collectCategory(index, rootCategory, {}, &features, visiting);
    }
    else
    {
        const QDomNodeList categories = root.elementsByTagName(QStringLiteral("Category"));
        for (int position = 0; position < categories.count(); ++position)
        {
            const QDomElement category = categories.at(position).toElement();
            const QDomElement parent = category.parentNode().toElement();
            if (parent.isNull() || parent.tagName() != QStringLiteral("Categories")) continue;
            collectCategory(index, category, {}, &features, visiting);
        }
    }
    return features;
}

const FramegrabberCameraFeatureDeclaration* findCameraFeature(
    const std::vector<FramegrabberCameraFeatureDeclaration>& features, const std::string& name)
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
    const FramegrabberCameraFeatureDeclaration& declaration,
    const Transport transport, const unsigned int dmaIndex, const std::string& connection)
{
    DeviceParameterDescriptor result;
    result.target = {connection, cameraScope(transport, dmaIndex), declaration.name, {}};
    result.kind = cameraKind(QString::fromStdString(declaration.tag)).value_or(Kind::String);
    result.displayName = declaration.displayName;
    result.description = declaration.description;
    result.unit = declaration.unit;
    result.categoryPath = declaration.categoryPath;
    result.visibility = declaration.visibility;
    result.representation = representationFor(result.kind);
    const QString mode = QString::fromStdString(declaration.accessMode).trimmed();
    const bool readable = mode.isEmpty() || mode == QStringLiteral("RW") || mode == QStringLiteral("RO");
    bool writable = (mode.isEmpty() || mode == QStringLiteral("RW") || mode == QStringLiteral("WO"))
        && !(_backend.isGrabbing && _backend.isGrabbing());
    // Dynamic GenICam gates are read from the device, so a locked or unavailable feature is
    // never reported writable from the static access mode alone.
    const auto readGate = [this, transport, dmaIndex](const std::string& feature, bool* value) {
        if (feature.empty() || !_backend.getCameraFeature) return false;
        Framegrabber::ParameterValue current = std::uint32_t{0};
        if (!_backend.getCameraFeature(transport, dmaIndex, feature, current)) return false;
        const auto boolean = signedInteger(current);
        if (!boolean) return false;
        *value = *boolean != 0;
        return true;
    };
    const auto addObservationError = [&result](const std::string& message) {
        if (!result.observationError.empty()) result.observationError += "; ";
        result.observationError += message;
    };
    bool locked = false;
    if (!declaration.lockedFeature.empty())
    {
        if (!readGate(declaration.lockedFeature, &locked))
        {
            writable = false;
            addObservationError("The SDK could not read the camera lock state.");
        }
        else if (locked)
        {
            writable = false;
        }
    }
    bool available = true;
    if (!declaration.availableFeature.empty())
    {
        if (!readGate(declaration.availableFeature, &available))
        {
            available = false;
            writable = false;
            addObservationError("The SDK could not read the camera availability state.");
        }
        else if (!available)
        {
            writable = false;
        }
    }
    result.readable = readable && result.kind != Kind::Command;
    result.writable = writable;
    result.available = available && (result.readable || result.writable);
    if (!result.available)
        result.unavailableReason = available
            ? "The camera does not allow this feature in its current access mode."
            : "The device reports this feature as unavailable.";
    if (result.kind == Kind::Enumeration) result.enumEntries = declaration.enumEntries;

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
        if (!_backend.getCameraFeature(transport, dmaIndex, declaration.name, value))
        {
            addObservationError("The SDK could not read the camera feature.");
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
        const auto limits = _backend.cameraFeatureLimits(transport, dmaIndex, declaration.name,
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
    std::set<std::string> seen;
    const auto visit = [&](DeviceParameterDescriptor descriptor) {
        const std::string key = descriptor.target.connectionId + "/"
            + descriptor.target.scope + "/" + descriptor.target.name;
        seen.insert(key);
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
                const auto declarations = cameraDeclarations(capability.transport,
                    camera.dmaIndex, connection);
                for (const auto& declaration : declarations)
                {
                    visit(observeCameraParameter(declaration, capability.transport,
                        camera.dmaIndex, connection));
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

std::vector<FramegrabberCameraFeatureDeclaration>
FramegrabberSourceController::cameraDeclarations(const Transport transport,
                                                 const unsigned int dmaIndex,
                                                 const std::string& connection)
{
    const std::string key = connection + "/" + cameraScope(transport, dmaIndex);
    const auto xml = _backend.cameraFeatureXml
        ? _backend.cameraFeatureXml(transport, dmaIndex) : std::string{};
    {
        std::lock_guard<std::mutex> lock(_observationMutex);
        if (xml.empty())
        {
            // A refresh or camera reconnect may temporarily report no XML; do not serve a
            // previous camera's feature list under the same connection and DMA.
            _cameraFeatureXml.erase(key);
            _cameraDeclarations.erase(key);
            return {};
        }
        const auto cachedXml = _cameraFeatureXml.find(key);
        if (cachedXml != _cameraFeatureXml.end() && cachedXml->second == xml)
        {
            const auto cached = _cameraDeclarations.find(key);
            if (cached != _cameraDeclarations.end()) return cached->second;
        }
    }
    auto parsed = parseCameraFeatureDeclarations(xml);
    std::lock_guard<std::mutex> lock(_observationMutex);
    _cameraFeatureXml[key] = xml;
    _cameraDeclarations[key] = parsed;
    return parsed;
}

void FramegrabberSourceController::publishObservation(
    const std::string& connection, const bool snapshot, const Observation& observed)
{
    if (!_notifications.active()) return;
    const auto keyOf = [](const DeviceParameterDescriptor& descriptor) {
        return descriptor.target.connectionId + "/" + descriptor.target.scope + "/"
            + descriptor.target.name;
    };
    std::vector<DeviceParameterDescriptor> changed;
    std::vector<DeviceParameterTarget> invalidated;
    {
        std::lock_guard<std::mutex> lock(_observationMutex);
        if (snapshot)
        {
            // A snapshot is the complete current inventory; it also resets the publication
            // baseline so a later delta is measured against what consumers actually received.
            _publishedObservations.clear();
            for (const auto& descriptor : observed.descriptors)
                _publishedObservations[keyOf(descriptor)] = descriptor;
        }
        else
        {
            std::set<std::string> currentKeys;
            for (const auto& descriptor : observed.descriptors)
            {
                const std::string key = keyOf(descriptor);
                currentKeys.insert(key);
                const auto published = _publishedObservations.find(key);
                if (published == _publishedObservations.end()
                    || !sameObservation(published->second, descriptor))
                    changed.push_back(descriptor);
            }
            for (auto it = _publishedObservations.begin(); it != _publishedObservations.end();)
            {
                if (it->second.target.connectionId != connection)
                {
                    it = _publishedObservations.erase(it);
                    continue;
                }
                if (!currentKeys.count(it->first))
                {
                    invalidated.push_back(it->second.target);
                    it = _publishedObservations.erase(it);
                    continue;
                }
                ++it;
            }
            for (const auto& descriptor : changed)
                _publishedObservations[keyOf(descriptor)] = descriptor;
        }
    }
    if (snapshot)
    {
        _notifications.publishSnapshot(connection);
        return;
    }
    if (!changed.empty() || !invalidated.empty())
        _notifications.publishDelta(connection, std::move(changed), std::move(invalidated));
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
                const auto declarations = cameraDeclarations(transport, dma, target.connectionId);
                const auto* feature = findCameraFeature(declarations, target.name);
                if (!feature) {
                    result = failure(Error::NotFound, "Camera feature was not found.");
                } else {
                    auto current = observeCameraParameter(*feature, transport, dma, target.connectionId);
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
                            const auto refreshed = cameraDeclarations(transport, dma,
                                target.connectionId);
                            if (const auto* after = findCameraFeature(refreshed, target.name))
                                result.descriptor = observeCameraParameter(*after, transport, dma,
                                    target.connectionId);
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
                            if (!converted) {
                                result.error = Error::InvalidArgument;
                                result.message = "The value is not representable by the SDK parameter type.";
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
