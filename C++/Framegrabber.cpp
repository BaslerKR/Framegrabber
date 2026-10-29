#include "Framegrabber.h"

#include "FramegrabberSystem.h"

#include <GenApi/GenApi.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <unordered_set>
#include <utility>

namespace
{
// The SDK interprets this timeout in seconds. Keep it short so a disconnected
// camera or an idle trigger source cannot delay stop()/close() for a long time.
constexpr int kFrameWaitTimeoutSeconds = 1;
// Time Sgc_scanPortsEx waits for at least one camera, as in the SDK discovery examples.
constexpr int kCameraDiscoveryTimeoutMilliseconds = 10000;

template<typename Callback>
Framegrabber::CallbackId registerCallback(
    std::mutex& mutex,
    std::unordered_map<Framegrabber::CallbackId, Callback>& callbacks,
    std::atomic<Framegrabber::CallbackId>& nextId,
    Callback callback)
{
    if (!callback)
    {
        return 0;
    }

    const Framegrabber::CallbackId id = nextId.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mutex);
    callbacks.emplace(id, std::move(callback));
    return id;
}

template<typename Callback>
bool deregisterCallback(
    std::mutex& mutex,
    std::unordered_map<Framegrabber::CallbackId, Callback>& callbacks,
    const Framegrabber::CallbackId id)
{
    std::lock_guard<std::mutex> lock(mutex);
    return callbacks.erase(id) != 0;
}

template<typename Callback>
void clearCallbacks(
    std::mutex& mutex,
    std::unordered_map<Framegrabber::CallbackId, Callback>& callbacks)
{
    std::lock_guard<std::mutex> lock(mutex);
    callbacks.clear();
}

std::string lowerExtension(const std::string& path)
{
    std::string extension = std::filesystem::path(path).extension().string();
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](const unsigned char value)
        {
            return static_cast<char>(std::tolower(value));
        });
    return extension;
}

std::string trimCopy(std::string value)
{
    const auto first = std::find_if_not(value.begin(), value.end(), [](const unsigned char character)
    {
        return std::isspace(character) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](const unsigned char character)
    {
        return std::isspace(character) != 0;
    }).base();
    return first < last ? std::string(first, last) : std::string{};
}

std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool appletReferenceMatchesPath(const std::string& appletReference,
                                const std::string& appletPath)
{
    const std::filesystem::path referencePath(appletReference);
    const std::filesystem::path candidatePath(appletPath);
    const std::string referenceFileName = lowercase(referencePath.filename().string());
    if (referenceFileName.empty())
    {
        return false;
    }
    if (lowercase(candidatePath.filename().string()) == referenceFileName)
    {
        return true;
    }
    return referencePath.extension().empty()
        && lowercase(candidatePath.stem().string()) == referenceFileName;
}

std::string safeString(const char* value)
{
    return value ? value : "";
}

std::string readCameraXmlProperty(SgcCameraHandle* handle)
{
    if (!handle)
    {
        return {};
    }

    size_t xmlSize = 0;
    unsigned int propertyType = SGC_PROPERTY_TYPE_STRING;
    int result = Sgc_getCameraPropertyWithType(
        handle,
        CAM_PROP_XML_DATA,
        nullptr,
        &propertyType,
        &xmlSize);
    if (result != SGC_OK || xmlSize == 0)
    {
        return {};
    }

    std::vector<char> buffer(xmlSize + 1, '\0');
    propertyType = SGC_PROPERTY_TYPE_STRING;
    result = Sgc_getCameraPropertyWithType(
        handle,
        CAM_PROP_XML_DATA,
        buffer.data(),
        &propertyType,
        &xmlSize);
    if (result != SGC_OK || xmlSize == 0)
    {
        return {};
    }

    return std::string(buffer.data());
}

Framegrabber::AppletFeatureKind appletFeatureKind(
    const GenApi::EInterfaceType interfaceType)
{
    switch (interfaceType)
    {
    case GenApi::intfICategory:
        return Framegrabber::AppletFeatureKind::Category;
    case GenApi::intfIInteger:
        return Framegrabber::AppletFeatureKind::Integer;
    case GenApi::intfIFloat:
        return Framegrabber::AppletFeatureKind::Float;
    case GenApi::intfIBoolean:
        return Framegrabber::AppletFeatureKind::Boolean;
    case GenApi::intfIString:
        return Framegrabber::AppletFeatureKind::String;
    case GenApi::intfIEnumeration:
        return Framegrabber::AppletFeatureKind::Enumeration;
    case GenApi::intfICommand:
        return Framegrabber::AppletFeatureKind::Command;
    default:
        return Framegrabber::AppletFeatureKind::Unknown;
    }
}

std::string genApiText(const GenICam::gcstring& text)
{
    return text.c_str();
}

bool parameterValuesMatch(const Framegrabber::ParameterValue& expected,
                          const Framegrabber::ParameterValue& actual)
{
    if (expected.index() != actual.index())
    {
        return false;
    }
    return std::visit(
        [](const auto& left, const auto& right)
        {
            using Left = std::decay_t<decltype(left)>;
            using Right = std::decay_t<decltype(right)>;
            if constexpr (!std::is_same_v<Left, Right>)
            {
                return false;
            }
            else if constexpr (std::is_same_v<Left, double>)
            {
                const double scale = (std::max)(
                    {1.0, std::abs(left), std::abs(right)});
                return std::abs(left - right)
                    <= std::numeric_limits<double>::epsilon() * scale * 8.0;
            }
            else
            {
                return left == right;
            }
        },
        expected,
        actual);
}

bool readAppletAccessFlags(Fg_Struct* handle,
                           const int parameterId,
                           const unsigned int dmaIndex,
                           std::int32_t& access)
{
    std::uint64_t accessParameterId = 0;
    if (Fg_getParameterPropertyWithTypeEx(
            handle,
            parameterId,
            PROP_ID_ACCESS_ID,
            &accessParameterId,
            dmaIndex) == FG_OK
        && accessParameterId > 0
        && accessParameterId <= static_cast<std::uint64_t>(
            (std::numeric_limits<int>::max)()))
    {
        int dynamicAccess = 0;
        if (Fg_getParameter(
                handle,
                static_cast<int>(accessParameterId),
                &dynamicAccess,
                dmaIndex) == FG_OK)
        {
            access = static_cast<std::int32_t>(dynamicAccess);
            return true;
        }
    }

    return Fg_getParameterPropertyWithTypeEx(
               handle,
               parameterId,
               PROP_ID_ACCESS,
               &access,
               dmaIndex) == FG_OK
        && access != 0;
}

bool appletParameterWritable(const std::int32_t access, const bool grabbing)
{
    return (access & FP_PARAMETER_PROPERTY_ACCESS_WRITE) != 0
        && (access & FP_PARAMETER_PROPERTY_ACCESS_LOCKED) == 0
        && (!grabbing
            || (access & FP_PARAMETER_PROPERTY_ACCESS_MODIFY) != 0);
}

bool appletStaticAccess(GenApi::INode* node,
                        bool& readable,
                        bool& writable,
                        std::unordered_set<std::string>& visited)
{
    if (!node)
    {
        return false;
    }
    const std::string name = genApiText(node->GetName());
    if (!visited.insert(name).second)
    {
        return false;
    }

    GenICam::gcstring accessMode;
    GenICam::gcstring attributes;
    if (node->GetProperty("AccessMode", accessMode, attributes))
    {
        const std::string mode = genApiText(accessMode);
        if (mode == "RW" || mode == "RO" || mode == "WO")
        {
            readable = mode == "RW" || mode == "RO";
            writable = mode == "RW" || mode == "WO";
            return true;
        }
    }
    if (dynamic_cast<GenApi::IRegister*>(node))
    {
        try
        {
            const GenApi::EAccessMode mode = node->GetAccessMode();
            if (mode == GenApi::RW || mode == GenApi::RO || mode == GenApi::WO)
            {
                readable = mode == GenApi::RW || mode == GenApi::RO;
                writable = mode == GenApi::RW || mode == GenApi::WO;
                return true;
            }
        }
        catch (...)
        {
        }
    }

    for (const char* propertyName : {"pValue", "pCommandValue"})
    {
        GenICam::gcstring referenceValue;
        if (!node->GetProperty(propertyName, referenceValue, attributes))
        {
            continue;
        }
        const std::string reference = genApiText(referenceValue);
        const std::size_t separator = reference.find('\t');
        if (appletStaticAccess(
                node->GetNodeMap()->GetNode(
                    reference.substr(0, separator).c_str()),
                readable,
                writable,
                visited))
        {
            return true;
        }
    }

    for (const GenApi::ELinkType linkType :
         {GenApi::ctWritingChildren, GenApi::ctReadingChildren})
    {
        GenApi::NodeList_t children;
        node->GetChildren(children, linkType);
        for (GenApi::INode* child : children)
        {
            if (appletStaticAccess(
                    child,
                    readable,
                    writable,
                    visited))
            {
                return true;
            }
        }
    }
    return false;
}

std::int64_t appletParameterId(
    GenApi::INode* node,
    std::unordered_set<std::string>& visited)
{
    if (!node)
    {
        return -1;
    }

    const std::string name = genApiText(node->GetName());
    if (!visited.insert(name).second)
    {
        return -1;
    }

    if (auto* parameter = dynamic_cast<GenApi::IRegister*>(node))
    {
        try
        {
            return parameter->GetAddress(false);
        }
        catch (...)
        {
            return -1;
        }
    }

    GenICam::gcstring_vector propertyNames;
    node->GetPropertyNames(propertyNames);
    for (const GenICam::gcstring& propertyName : propertyNames)
    {
        const std::string property = genApiText(propertyName);
        if (property != "pValue" && property != "pCommandValue")
        {
            continue;
        }

        GenICam::gcstring value;
        GenICam::gcstring attributes;
        if (!node->GetProperty(propertyName, value, attributes))
        {
            continue;
        }
        const std::string reference = genApiText(value);
        const std::size_t separator = reference.find('\t');
        GenApi::INode* child = node->GetNodeMap()->GetNode(
            reference.substr(0, separator).c_str());
        const std::int64_t parameterId = appletParameterId(child, visited);
        if (parameterId >= 0)
        {
            return parameterId;
        }
    }

    for (const GenApi::ELinkType linkType :
         {GenApi::ctWritingChildren, GenApi::ctReadingChildren})
    {
        GenApi::NodeList_t children;
        node->GetChildren(children, linkType);
        for (GenApi::INode* child : children)
        {
            const std::int64_t parameterId = appletParameterId(child, visited);
            if (parameterId >= 0)
            {
                return parameterId;
            }
        }
    }
    return -1;
}

bool buildAppletFeatureNode(
    GenApi::INode* node,
    Framegrabber::AppletFeatureNode& output,
    std::unordered_set<std::string>& visiting)
{
    if (!node || node->GetVisibility() == GenApi::Invisible)
    {
        return false;
    }

    output.name = genApiText(node->GetName());
    if (output.name.empty() || !visiting.insert(output.name).second)
    {
        return false;
    }

    output.displayName = genApiText(node->GetDisplayName());
    if (output.displayName.empty())
    {
        output.displayName = output.name;
    }
    output.toolTip = genApiText(node->GetToolTip());
    output.description = genApiText(node->GetDescription());
    output.kind = appletFeatureKind(node->GetPrincipalInterfaceType());
    if (output.kind != Framegrabber::AppletFeatureKind::Category)
    {
        std::unordered_set<std::string> visited;
        output.parameterId = appletParameterId(node, visited);
        visited.clear();
        appletStaticAccess(
            node,
            output.readable,
            output.writable,
            visited);
    }

    if (output.kind == Framegrabber::AppletFeatureKind::Category)
    {
        if (auto* category = dynamic_cast<GenApi::ICategory*>(node))
        {
            GenApi::FeatureList_t features;
            category->GetFeatures(features);
            output.children.reserve(features.size());
            for (GenApi::IValue* value : features)
            {
                GenApi::INode* feature = value != nullptr ? value->GetNode() : nullptr;
                Framegrabber::AppletFeatureNode child;
                if (buildAppletFeatureNode(feature, child, visiting))
                {
                    output.children.push_back(std::move(child));
                }
            }
        }
    }
    else if (output.kind == Framegrabber::AppletFeatureKind::Enumeration)
    {
        if (auto* enumeration = dynamic_cast<GenApi::IEnumeration*>(node))
        {
            GenApi::NodeList_t entries;
            enumeration->GetEntries(entries);
            output.enumEntries.reserve(entries.size());
            for (GenApi::INode* entryNode : entries)
            {
                if (!entryNode || entryNode->GetVisibility() == GenApi::Invisible)
                {
                    continue;
                }
                auto* entry = dynamic_cast<GenApi::IEnumEntry*>(entryNode);
                if (!entry)
                {
                    continue;
                }
                Framegrabber::AppletEnumEntry item;
                item.name = genApiText(entry->GetSymbolic());
                GenICam::gcstring displayName;
                GenICam::gcstring attributes;
                if (entryNode->GetProperty(
                        "DisplayName",
                        displayName,
                        attributes))
                {
                    item.displayName = genApiText(displayName);
                }
                if (item.displayName.empty())
                {
                    item.displayName = item.name;
                }
                item.value = entry->GetValue();
                output.enumEntries.push_back(std::move(item));
            }
        }
    }

    visiting.erase(output.name);
    return true;
}

class OwnedBufferPool final
    : public std::enable_shared_from_this<OwnedBufferPool>
{
public:
    using Lease = std::shared_ptr<std::uint8_t>;

    static std::shared_ptr<OwnedBufferPool> create(
        const std::size_t bufferSize,
        const std::size_t bufferCount)
    {
        auto pool = std::shared_ptr<OwnedBufferPool>(
            new OwnedBufferPool);
        pool->_available.reserve(bufferCount);
        for (std::size_t index = 0; index < bufferCount; ++index)
        {
            pool->_available.push_back(
                std::unique_ptr<std::uint8_t[]>(
                    new std::uint8_t[bufferSize]));
        }
        return pool;
    }

    Lease acquire(const std::atomic<bool>& stopRequested)
    {
        std::unique_lock<std::mutex> lock(_mutex);
        _condition.wait(lock, [this, &stopRequested]
        {
            return !_available.empty()
                || _stopped
                || stopRequested.load(std::memory_order_acquire);
        });
        if (_stopped
            || stopRequested.load(std::memory_order_acquire)
            || _available.empty())
        {
            return {};
        }

        std::uint8_t* data = _available.back().release();
        _available.pop_back();
        const std::weak_ptr<OwnedBufferPool> weakPool = shared_from_this();
        return Lease(data, [weakPool](std::uint8_t* released)
        {
            if (const std::shared_ptr<OwnedBufferPool> pool = weakPool.lock())
            {
                pool->release(released);
            }
            else
            {
                delete[] released;
            }
        });
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _stopped = true;
        }
        _condition.notify_all();
    }

private:
    OwnedBufferPool() = default;

    void release(std::uint8_t* data)
    {
        if (!data)
        {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _available.push_back(
                std::unique_ptr<std::uint8_t[]>(data));
        }
        _condition.notify_one();
    }

    std::mutex _mutex;
    std::condition_variable _condition;
    std::vector<std::unique_ptr<std::uint8_t[]>> _available;
    bool _stopped = false;
};
}

struct Framegrabber::DmaChannel
{
    unsigned int index = 0;
    dma_mem* memory = nullptr;
    std::thread thread;
    std::mutex resourceMutex;
    std::mutex permitMutex;
    std::condition_variable permitCondition;
    std::shared_ptr<OwnedBufferPool> ownedBufferPool;
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> acquisitionStarted{false};
    std::atomic<bool> cameraStarted{false};
    std::atomic<std::size_t> permits{0};
    std::atomic<std::uint64_t> delivered{0};
    std::atomic<std::uint64_t> skipped{0};
    std::atomic<std::uint64_t> overwritten{0};
    std::atomic<std::uint64_t> rejected{0};
};

struct Framegrabber::PreviousApplet
{
    bool opened = false;
    std::string appletPath;
    std::string configurationPath;
    std::string boardName;
};

struct Framegrabber::CameraEntry
{
    CameraInfo info;
    SgcCameraHandle* handle = nullptr;
    std::string featureXml;
};

std::string Framegrabber::CameraInfo::displayName() const
{
    std::string result;
    if (!vendor.empty())
    {
        result += vendor;
    }
    if (!model.empty())
    {
        if (!result.empty())
        {
            result += " ";
        }
        result += model;
    }
    if (!serial.empty())
    {
        if (!result.empty())
        {
            result += " ";
        }
        result += "(" + serial + ")";
    }
    return result.empty() ? "Camera" : result;
}

Framegrabber::Framegrabber(FramegrabberSystem* parent, const int allottedNumber)
    : _system(parent),
      _allottedNumber(allottedNumber)
{
}

Framegrabber::~Framegrabber()
{
    close();
    clearNodeUpdatedCallbacks();
    clearGrabCallbacks();
    clearStatusCallbacks();
}

Framegrabber::CallbackId Framegrabber::registerStatusCallback(StatusCallback callback)
{
    return registerCallback(
        _statusCallbackMutex,
        _statusCallbacks,
        _nextStatusCallbackId,
        std::move(callback));
}

bool Framegrabber::deregisterStatusCallback(const CallbackId id)
{
    return deregisterCallback(_statusCallbackMutex, _statusCallbacks, id);
}

void Framegrabber::clearStatusCallbacks()
{
    clearCallbacks(_statusCallbackMutex, _statusCallbacks);
}

Framegrabber::CallbackId Framegrabber::registerGrabCallback(GrabCallback callback)
{
    return registerCallback(
        _grabCallbackMutex,
        _grabCallbacks,
        _nextGrabCallbackId,
        std::move(callback));
}

bool Framegrabber::deregisterGrabCallback(const CallbackId id)
{
    return deregisterCallback(_grabCallbackMutex, _grabCallbacks, id);
}

void Framegrabber::clearGrabCallbacks()
{
    clearCallbacks(_grabCallbackMutex, _grabCallbacks);
}

Framegrabber::CallbackId Framegrabber::registerNodeUpdatedCallback(NodeCallback callback)
{
    return registerCallback(
        _nodeCallbackMutex,
        _nodeCallbacks,
        _nextNodeCallbackId,
        std::move(callback));
}

bool Framegrabber::deregisterNodeUpdatedCallback(const CallbackId id)
{
    return deregisterCallback(_nodeCallbackMutex, _nodeCallbacks, id);
}

void Framegrabber::clearNodeUpdatedCallbacks()
{
    clearCallbacks(_nodeCallbackMutex, _nodeCallbacks);
}

void Framegrabber::setAppletPath(std::string path)
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    _appletPath = std::move(path);
}

std::string Framegrabber::appletPath() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _appletPath;
}

void Framegrabber::setConfigurationPath(std::string path)
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    _configurationPath = std::move(path);
}

std::string Framegrabber::configurationPath() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _configurationPath;
}

bool Framegrabber::loadApplet(const std::string& path, const std::string& boardName)
{
    std::lock_guard<std::mutex> lifecycleLock(_lifecycleMutex);
    return loadAppletUnlocked(path, boardName);
}

std::string Framegrabber::appletPathFromConfiguration(
    const std::string& configurationPath,
    const std::string& boardName) const
{
    if (configurationPath.empty()
        || lowerExtension(configurationPath) != ".mcf"
        || !std::filesystem::is_regular_file(configurationPath))
    {
        return {};
    }

    const auto resolveReference = [this, &configurationPath, &boardName](
                                      const std::string& appletReference)
    {
        std::filesystem::path resolvedPath(appletReference);
        if (resolvedPath.is_relative())
        {
            resolvedPath = std::filesystem::path(configurationPath).parent_path() / resolvedPath;
        }
        resolvedPath = resolvedPath.lexically_normal();
        if (std::filesystem::is_regular_file(resolvedPath))
        {
            return resolvedPath.string();
        }

        std::string loadedAppletPath;
        bool isOpened = false;
        {
            std::lock_guard<std::mutex> lock(_stateMutex);
            loadedAppletPath = _appletPath;
            isOpened = _handle != nullptr;
        }
        if (std::filesystem::is_regular_file(loadedAppletPath)
            && appletReferenceMatchesPath(appletReference, loadedAppletPath))
        {
            return loadedAppletPath;
        }
        // SDK-saved MCF files name the applet ([fglib5] FileName='Acq_...'); the cached
        // catalog resolves it without an iterator query.
        const std::string cataloged = findCatalogApplet(
            std::filesystem::path(appletReference).filename().string());
        if (!cataloged.empty())
        {
            return cataloged;
        }

        // Siso GenICam keeps state associated with an initialized applet. Avoid a
        // filesystem iterator query while that applet is live; the user can select
        // a replacement when the current applet does not identify the MCF target.
        if (_system && !isOpened)
        {
            const std::string sdkPath = _system->resolveLoadableAppletPath(
                boardName,
                appletReference);
            if (!sdkPath.empty())
            {
                return sdkPath;
            }
        }
        return resolvedPath.string();
    };

    std::ifstream configuration(configurationPath);
    std::string line;
    std::string section;
    std::string fileNameReference;
    while (std::getline(configuration, line))
    {
        if (line.size() >= 3
            && static_cast<unsigned char>(line[0]) == 0xEF
            && static_cast<unsigned char>(line[1]) == 0xBB
            && static_cast<unsigned char>(line[2]) == 0xBF)
        {
            line.erase(0, 3);
        }
        const std::string trimmedLine = trimCopy(line);
        if (trimmedLine.size() >= 2
            && trimmedLine.front() == '['
            && trimmedLine.back() == ']')
        {
            section = lowercase(trimCopy(
                trimmedLine.substr(1, trimmedLine.size() - 2)));
            continue;
        }

        const std::size_t separator = line.find('=');
        if (separator == std::string::npos)
        {
            continue;
        }

        const std::string key = lowercase(trimCopy(line.substr(0, separator)));
        std::string appletReference = trimCopy(line.substr(separator + 1));
        if (!appletReference.empty() && appletReference.back() == ';')
        {
            appletReference.pop_back();
            appletReference = trimCopy(std::move(appletReference));
        }
        if (appletReference.size() >= 2
            && ((appletReference.front() == '\'' && appletReference.back() == '\'')
                || (appletReference.front() == '"' && appletReference.back() == '"')))
        {
            appletReference = appletReference.substr(1, appletReference.size() - 2);
        }
        if (appletReference.empty())
        {
            continue;
        }
        if (key == "hapname")
        {
            return resolveReference(appletReference);
        }
        if (section == "fglib5" && key == "filename")
        {
            fileNameReference = std::move(appletReference);
        }
    }
    return fileNameReference.empty() ? std::string{} : resolveReference(fileNameReference);
}

bool Framegrabber::loadAppletConfiguration(const std::string& appletPath,
                                            const std::string& configurationPath,
                                            const std::string& boardName)
{
    std::lock_guard<std::mutex> lifecycleLock(_lifecycleMutex);
    clearLastError();
    if (configurationPath.empty()
        || lowerExtension(configurationPath) != ".mcf"
        || !std::filesystem::is_regular_file(configurationPath))
    {
        setLastError(FG_INVALID_FILENAME, "Select an existing .mcf configuration file.");
        return false;
    }
    if (_isRunning.load(std::memory_order_acquire))
    {
        setLastError(FG_ALREADY_STARTED, "Stop acquisition before loading a configuration.");
        return false;
    }
    FramegrabberSystem::BoardInfo board;
    if (!_system || !_system->isInitialized() || !_system->resolveBoard(boardName, board))
    {
        setLastError(FG_INVALID_BOARD_NUMBER, "No matching frame grabber board was found.");
        return false;
    }

    std::string overridePath;
    if (!appletPath.empty())
    {
        overridePath = resolveAppletReference(appletPath);
        if (overridePath.empty())
        {
            setLastError(FG_FILE_NOT_FOUND, "Applet '" + appletPath + "' is not available.");
            return false;
        }
    }

    std::string currentAppletPath;
    bool isOpened = false;
    bool sameBoard = false;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        currentAppletPath = _appletPath;
        isOpened = _handle != nullptr;
        sameBoard = isOpened && _boardIndex == board.index;
    }
    // The MCF applet reference is resolved without an iterator query while an applet
    // is open; see appletPathFromConfiguration().
    const std::string configuredPath = overridePath.empty()
        ? appletPathFromConfiguration(configurationPath, board.displayName())
        : overridePath;
    std::error_code error;
    const bool isCurrentApplet = sameBoard
        && !configuredPath.empty()
        && std::filesystem::equivalent(
            std::filesystem::path(currentAppletPath),
            std::filesystem::path(configuredPath),
            error)
        && !error;

    if (isCurrentApplet)
    {
        // Siso GenICam is not released and re-created for the same applet: Fg_loadConfig
        // replaces the parameters and the existing CXP board handle remains valid.
        if (!loadConfigurationUnlocked(configurationPath))
        {
            return false;
        }
        notifyStatus(ConfigurationStatus, true);
        bool hasCameraControl = false;
        {
            std::lock_guard<std::mutex> lock(_stateMutex);
            hasCameraControl = _cxpBoard != nullptr;
        }
        if (hasCameraControl)
        {
            refreshCamerasUnlocked(true);
        }
        return true;
    }

    const PreviousApplet previous = capturePreviousApplet();
    if (!closeUnlocked(true))
    {
        setLastError(FG_ERROR, "The previous applet could not be released.");
        return false;
    }
    refreshAppletCatalogUnlocked(board);

    Fg_Struct* handle = nullptr;
    std::string loadedAppletPath = overridePath;
    if (overridePath.empty())
    {
        handle = Fg_InitConfig(configurationPath.c_str(), board.index);
        if (handle)
        {
            Fg_getStringSystemInformationForFgHandle(
                handle,
                INFO_APPLET_FULL_PATH,
                PROP_ID_VALUE,
                loadedAppletPath);
            if (loadedAppletPath.empty())
            {
                loadedAppletPath = configuredPath;
            }
            std::error_code sameError;
            if (!configuredPath.empty()
                && std::filesystem::is_regular_file(configuredPath)
                && !(std::filesystem::equivalent(loadedAppletPath, configuredPath, sameError)
                     && !sameError))
            {
                // SDK 5.11.8 and earlier can load the first applet of a board subfolder
                // instead of the MCF applet (fixed in 5.11.9, ticket 347258).
                log("Fg_InitConfig loaded " + loadedAppletPath + " instead of "
                        + configuredPath + "; applying the configuration to the configured applet.",
                    true);
                Fg_FreeGrabber(handle);
                handle = nullptr;
                loadedAppletPath = configuredPath;
            }
        }
        else
        {
            const int code = Fg_getLastErrorNumber(nullptr);
            if (configuredPath.empty() || !std::filesystem::is_regular_file(configuredPath))
            {
                setLastSdkError("Failed to initialize from '" + configurationPath + "'", code);
                restorePreviousApplet(previous);
                return false;
            }
            // The MCF names an applet the SDK cannot locate itself (for example an
            // absolute path from another machine); apply it to the locally resolved applet.
            log("Fg_InitConfig failed (" + safeString(Fg_getErrorDescription(nullptr, code))
                    + "); applying the configuration to " + configuredPath + ".",
                true);
            loadedAppletPath = configuredPath;
        }
    }
    if (!handle)
    {
        handle = initializeBoard(board, loadedAppletPath);
        if (!handle)
        {
            restorePreviousApplet(previous);
            return false;
        }
        const int result = Fg_loadConfig(handle, configurationPath.c_str());
        if (result != FG_OK)
        {
            setLastSdkError("Failed to load configuration '" + configurationPath + "'", result);
            Fg_FreeGrabber(handle);
            restorePreviousApplet(previous);
            return false;
        }
    }
    return adoptBoard(handle, board, loadedAppletPath, configurationPath);
}

bool Framegrabber::loadAppletUnlocked(const std::string& reference, const std::string& boardName)
{
    clearLastError();
    if (!_system || !_system->isInitialized())
    {
        setLastError(FG_NOT_INIT, "Frame grabber system is not initialized.");
        return false;
    }
    FramegrabberSystem::BoardInfo board;
    if (!_system->resolveBoard(boardName, board))
    {
        setLastError(FG_INVALID_BOARD_NUMBER, "No matching frame grabber board was found.");
        return false;
    }
    if (reference.empty())
    {
        setLastError(FG_INVALID_FILENAME, "Select an applet before opening the board.");
        return false;
    }
    if (_isRunning.load(std::memory_order_acquire))
    {
        setLastError(FG_ALREADY_STARTED, "Stop acquisition before loading an applet.");
        return false;
    }

    // While an applet is open the catalog from its last closed state is used.
    const bool catalogRefreshed = refreshAppletCatalogUnlocked(board);
    const std::string path = resolveAppletReference(reference);
    if (path.empty())
    {
        setLastError(FG_FILE_NOT_FOUND,
            "Applet '" + reference + "' is not a loadable applet for " + board.displayName() + ".");
        return false;
    }

    const PreviousApplet previous = capturePreviousApplet();
    if (!closeUnlocked(true))
    {
        setLastError(FG_ERROR, "The previous applet could not be released.");
        return false;
    }
    if (!catalogRefreshed)
    {
        refreshAppletCatalogUnlocked(board);
    }

    Fg_Struct* handle = initializeBoard(board, path);
    if (handle && adoptBoard(handle, board, path, {}))
    {
        return true;
    }
    restorePreviousApplet(previous);
    return false;
}

bool Framegrabber::open(const std::string& boardName)
{
    std::lock_guard<std::mutex> lifecycleLock(_lifecycleMutex);
    return openUnlocked(boardName);
}

bool Framegrabber::openUnlocked(const std::string& boardName)
{
    return loadAppletUnlocked(appletPath(), boardName);
}

Framegrabber::PreviousApplet Framegrabber::capturePreviousApplet() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    PreviousApplet previous;
    previous.opened = _handle != nullptr;
    previous.appletPath = _appletPath;
    previous.configurationPath = _configurationPath;
    previous.boardName = _connectedBoardName;
    return previous;
}

void Framegrabber::restorePreviousApplet(const PreviousApplet& previous)
{
    if (!previous.opened || previous.appletPath.empty() || !_system)
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        _appletPath = previous.appletPath;
        _configurationPath = previous.configurationPath;
        return;
    }

    FramegrabberSystem::BoardInfo board;
    if (!_system->resolveBoard(previous.boardName, board))
    {
        return;
    }
    log("Restoring the previous frame grabber applet.", true);
    // Keep the error of the failed request; restoration reports only through the log.
    const OperationError requestError = lastError();
    Fg_Struct* handle = initializeBoard(board, previous.appletPath);
    if (handle && !previous.configurationPath.empty())
    {
        const int result = Fg_loadConfig(handle, previous.configurationPath.c_str());
        if (result != FG_OK)
        {
            log("Failed to restore configuration '" + previous.configurationPath + "': "
                    + safeString(Fg_getErrorDescription(nullptr, result)),
                true);
        }
    }
    if (handle)
    {
        adoptBoard(handle, board, previous.appletPath, previous.configurationPath);
    }
    std::lock_guard<std::mutex> lock(_stateMutex);
    _lastError = requestError;
}

Fg_Struct* Framegrabber::initializeBoard(const FramegrabberSystem::BoardInfo& board,
                                         const std::string& appletPath)
{
    // Fg_Init is Fg_InitEx(..., FG_INIT_FLAG_DEFAULT): this process configures the FPGA.
    Fg_Struct* handle = Fg_Init(appletPath.c_str(), board.index);
    if (!handle)
    {
        setLastSdkError("Failed to load applet '" + appletPath + "'",
            Fg_getLastErrorNumber(nullptr));
    }
    return handle;
}

bool Framegrabber::adoptBoard(Fg_Struct* handle,
                              const FramegrabberSystem::BoardInfo& board,
                              const std::string& appletPath,
                              const std::string& configurationPath)
{
    const AppletInfo applet = catalogEntry(appletPath);
    SgcBoardHandle* cxpBoard = nullptr;
    if (applet.supportsCameraControl)
    {
        const int result = Sgc_initBoard(handle, 0, &cxpBoard);
        if (result != SGC_OK)
        {
            cxpBoard = nullptr;
            log("CXP control module is unavailable for this applet: "
                    + safeString(Sgc_getErrorDescription(result)),
                true);
        }
    }

    std::string appletFileName;
    Fg_getStringSystemInformationForFgHandle(
        handle,
        INFO_APPLET_FILE_NAME,
        PROP_ID_VALUE,
        appletFileName);
    if (appletFileName.empty())
    {
        appletFileName = std::filesystem::path(appletPath).filename().string();
    }
    const std::string appletName = !applet.name.empty()
        ? applet.name
        : std::filesystem::path(appletFileName).stem().string();
    const int appletId = Fg_getAppletId(handle, nullptr);
    std::string appletVersion =
        appletId >= 0 ? safeString(Fg_getAppletVersion(handle, appletId)) : "";
    if (appletVersion.empty())
    {
        appletVersion = applet.version;
    }

    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        _handle = handle;
        _cxpBoard = cxpBoard;
        _boardIndex = board.index;
        _connectedBoardName = board.displayName();
        _appletPath = appletPath;
        _loadedAppletName = appletName;
        _loadedAppletVersion = appletVersion;
        _configurationPath = configurationPath;
        _channels.clear();
        _cxpCameras.clear();
        _lastStatistics.clear();
        _lastCameraDiscovery = {};
        ++_connectionGeneration;
    }
    clearAppletFeatureModels();

    if (cxpBoard)
    {
        // The first scan starts the SDK discovery thread; no reset is needed.
        refreshCamerasUnlocked(false);
    }

    notifyStatus(ConnectionStatus, true);
    log("Opened " + board.displayName() + " with "
        + std::filesystem::path(appletPath).filename().string()
        + (configurationPath.empty() ? std::string{} : " and " + configurationPath) + ".");
    return true;
}

bool Framegrabber::refreshAppletCatalog(const std::string& boardName)
{
    std::lock_guard<std::mutex> lifecycleLock(_lifecycleMutex);
    if (!_system)
    {
        return false;
    }
    FramegrabberSystem::BoardInfo board;
    if (!_system->resolveBoard(boardName, board))
    {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        if (_handle)
        {
            return false;
        }
    }
    return refreshAppletCatalogUnlocked(board);
}

bool Framegrabber::refreshAppletCatalogUnlocked(const FramegrabberSystem::BoardInfo& board)
{
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        if (_handle || !_system)
        {
            return false;
        }
    }
    std::vector<AppletInfo> catalog = _system->getLoadableApplets(board.displayName());
    std::lock_guard<std::mutex> lock(_stateMutex);
    _appletCatalog = std::move(catalog);
    return true;
}

std::vector<Framegrabber::AppletInfo> Framegrabber::appletCatalog() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _appletCatalog;
}

Framegrabber::AppletInfo Framegrabber::catalogEntry(const std::string& appletPath) const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    for (const AppletInfo& applet : _appletCatalog)
    {
        std::error_code error;
        if (applet.path == appletPath
            || (std::filesystem::equivalent(applet.path, appletPath, error) && !error))
        {
            return applet;
        }
    }
    // A HAP outside the standard locations is loadable but not enumerated.
    AppletInfo applet;
    applet.path = appletPath;
    return applet;
}

std::string Framegrabber::resolveAppletReference(const std::string& reference) const
{
    if (reference.empty())
    {
        return {};
    }
    std::error_code error;
    if (std::filesystem::is_regular_file(reference, error))
    {
        return std::filesystem::absolute(reference, error).lexically_normal().string();
    }
    return findCatalogApplet(reference);
}

std::string Framegrabber::findCatalogApplet(const std::string& reference) const
{
    if (reference.empty())
    {
        return {};
    }
    const std::string normalized = lowercase(reference);
    std::vector<std::string> matches;
    std::lock_guard<std::mutex> lock(_stateMutex);
    for (const AppletInfo& applet : _appletCatalog)
    {
        const std::filesystem::path file(applet.file);
        if (lowercase(applet.path) == normalized
            || lowercase(applet.name) == normalized
            || lowercase(file.string()) == normalized
            || lowercase(file.stem().string()) == normalized)
        {
            if (std::find(matches.begin(), matches.end(), applet.path) == matches.end())
            {
                matches.push_back(applet.path);
            }
        }
    }
    // Duplicate names must be loaded by full path, as documented for Fg_Init.
    return matches.size() == 1 ? matches.front() : std::string{};
}

std::uint64_t Framegrabber::connectionGeneration() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _handle ? _connectionGeneration : 0;
}

Framegrabber::OperationError Framegrabber::lastError() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _lastError;
}

void Framegrabber::setLastError(const int code, std::string message)
{
    log(message, true);
    std::lock_guard<std::mutex> lock(_stateMutex);
    _lastError.code = code;
    _lastError.message = std::move(message);
}

void Framegrabber::setLastSdkError(const std::string& context, const int code)
{
    const std::string description = safeString(Fg_getErrorDescription(nullptr, code));
    setLastError(code, context + ": " + (description.empty() ? "SDK error" : description)
        + " (" + std::to_string(code) + ").");
}

void Framegrabber::clearLastError()
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    _lastError = {};
}

bool Framegrabber::tryBeginControl()
{
    bool expected = false;
    if (!_controlBusy.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        return false;
    }
    notifyStatus(ControlStatus, true);
    return true;
}

void Framegrabber::endControl()
{
    if (_controlBusy.exchange(false, std::memory_order_acq_rel))
    {
        notifyStatus(ControlStatus, false);
    }
}

bool Framegrabber::controlBusy() const noexcept
{
    return _controlBusy.load(std::memory_order_acquire);
}

bool Framegrabber::isOpened() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _handle != nullptr;
}

void Framegrabber::close()
{
    std::lock_guard<std::mutex> lifecycleLock(_lifecycleMutex);
    closeUnlocked(true);
}

bool Framegrabber::closeUnlocked(const bool notifyConnectionStatus)
{
    requestStopChannels();
    if (!joinStoppedChannels())
    {
        return false;
    }

    bool wasOpened = false;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        wasOpened = _handle != nullptr;
        releaseHandles();
        _connectedBoardName.clear();
        _loadedAppletName.clear();
        _loadedAppletVersion.clear();
        _channels.clear();
        _cxpCameras.clear();
    }
    clearAppletFeatureModels();

    if (wasOpened && notifyConnectionStatus)
    {
        notifyStatus(ConnectionStatus, false);
    }
    return true;
}

void Framegrabber::releaseHandles()
{
    for (CameraEntry& camera : _cxpCameras)
    {
        if (camera.handle && camera.info.connected)
        {
            Sgc_disconnectCamera(camera.handle);
            camera.info.connected = false;
        }
    }

    if (_cxpBoard)
    {
        Sgc_freeBoard(_cxpBoard);
        _cxpBoard = nullptr;
    }

    if (_handle)
    {
        Fg_FreeGrabber(_handle);
        _handle = nullptr;
    }
}

std::string Framegrabber::getConnectedFramegrabberName() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _connectedBoardName;
}

std::string Framegrabber::getLoadedAppletName() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _loadedAppletName;
}

std::string Framegrabber::getLoadedAppletVersion() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _loadedAppletVersion;
}

bool Framegrabber::loadConfiguration(const std::string& path)
{
    std::lock_guard<std::mutex> lifecycleLock(_lifecycleMutex);
    clearLastError();
    if (!loadConfigurationUnlocked(path))
    {
        return false;
    }
    notifyStatus(ConfigurationStatus, true);
    return true;
}

bool Framegrabber::loadConfigurationUnlocked(const std::string& path)
{
    if (path.empty()
        || lowerExtension(path) != ".mcf"
        || !std::filesystem::is_regular_file(path))
    {
        setLastError(FG_INVALID_FILENAME, "Select an existing .mcf configuration file.");
        return false;
    }

    int result = FG_NOT_INIT;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        if (!_handle)
        {
            result = FG_NOT_INIT;
        }
        else if (_isRunning.load(std::memory_order_acquire))
        {
            result = FG_ALREADY_STARTED;
        }
        else
        {
            result = Fg_loadConfig(_handle, path.c_str());
            if (result == FG_OK)
            {
                _configurationPath = path;
            }
        }
    }
    clearAppletFeatureModels();
    if (result != FG_OK)
    {
        setLastSdkError("Failed to load configuration '" + path + "'", result);
        return false;
    }
    return true;
}

bool Framegrabber::saveConfiguration(const std::string& path)
{
    std::lock_guard<std::mutex> lifecycleLock(_lifecycleMutex);
    clearLastError();
    // Fg_saveConfig writes the .mcf and, for field parameters, an .mfs with the same stem.
    if (path.empty() || lowerExtension(path) != ".mcf")
    {
        setLastError(FG_INVALID_FILENAME, "Configuration files must use the .mcf extension.");
        return false;
    }

    int result = FG_NOT_INIT;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        if (!_handle)
        {
            result = FG_NOT_INIT;
        }
        else if (_isRunning.load(std::memory_order_acquire))
        {
            result = FG_ALREADY_STARTED;
        }
        else
        {
            result = Fg_saveConfig(_handle, path.c_str());
            if (result == FG_OK)
            {
                _configurationPath = path;
            }
        }
    }
    if (result != FG_OK)
    {
        setLastSdkError("Failed to save configuration '" + path + "'", result);
        return false;
    }
    return true;
}

void Framegrabber::grab(const std::size_t frames)
{
    if (_isRunning.load(std::memory_order_acquire))
    {
        return;
    }

    std::lock_guard<std::mutex> lifecycleLock(_lifecycleMutex);
    if (_isRunning.load(std::memory_order_acquire))
    {
        return;
    }

    requestStopChannels();
    if (!joinStoppedChannels())
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        if (!_handle)
        {
            return;
        }
    }
    if (_isRunning.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }

    const int dmaCount = getDMACount();
    if (dmaCount <= 0)
    {
        _isRunning.store(false, std::memory_order_release);
        log("The active applet exposes no DMA channels.", true);
        return;
    }

    _frameSeq.store(0, std::memory_order_release);
    _activeChannels.store(0, std::memory_order_release);
    _startingChannels.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        _channels.clear();
        _channels.resize(static_cast<std::size_t>(dmaCount));
        _lastStatistics.clear();
    }

    notifyStatus(GrabbingStatus, true);
    for (int dmaIndex = 0; dmaIndex < dmaCount; ++dmaIndex)
    {
        startChannel(static_cast<unsigned int>(dmaIndex), frames);
    }

    _startingChannels.store(false, std::memory_order_release);
    if (_activeChannels.load(std::memory_order_acquire) == 0)
    {
        _isRunning.store(false, std::memory_order_release);
        notifyStatus(GrabbingStatus, false);
    }
}

void Framegrabber::startChannel(const unsigned int dmaIndex, const std::size_t frames)
{
    Fg_Struct* handle = nullptr;
    std::size_t bufferCount = 0;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        handle = _handle;
        bufferCount = _dmaBufferCount;
    }
    if (!handle)
    {
        return;
    }

    const auto readIntegerParameter = [handle, dmaIndex](const int parameterId,
                                                         std::int64_t& output) -> bool
    {
        const FgParamTypes type = Fg_getParameterTypeById(
            handle,
            parameterId,
            static_cast<int>(dmaIndex));
        switch (type)
        {
        case FG_PARAM_TYPE_INT32_T:
        {
            std::int32_t value = 0;
            const int result = Fg_getParameterWithType(
                handle,
                parameterId,
                &value,
                dmaIndex,
                type);
            output = value;
            return result == FG_OK;
        }
        case FG_PARAM_TYPE_UINT32_T:
        {
            std::uint32_t value = 0;
            const int result = Fg_getParameterWithType(
                handle,
                parameterId,
                &value,
                dmaIndex,
                type);
            output = value;
            return result == FG_OK;
        }
        case FG_PARAM_TYPE_INT64_T:
        {
            std::int64_t value = 0;
            const int result = Fg_getParameterWithType(
                handle,
                parameterId,
                &value,
                dmaIndex,
                type);
            output = value;
            return result == FG_OK;
        }
        case FG_PARAM_TYPE_UINT64_T:
        case FG_PARAM_TYPE_SIZE_T:
        {
            std::uint64_t value = 0;
            const int result = Fg_getParameterWithType(
                handle,
                parameterId,
                &value,
                dmaIndex,
                type);
            if (value > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))
            {
                return false;
            }
            output = static_cast<std::int64_t>(value);
            return result == FG_OK;
        }
        default:
            return false;
        }
    };

    std::int64_t widthValue = 0;
    std::int64_t heightValue = 0;
    std::int64_t formatValue = 0;
    if (!readIntegerParameter(FG_WIDTH, widthValue)
        || !readIntegerParameter(FG_HEIGHT, heightValue)
        || !readIntegerParameter(FG_FORMAT, formatValue)
        || widthValue > (std::numeric_limits<int>::max)()
        || heightValue > (std::numeric_limits<int>::max)()
        || formatValue > (std::numeric_limits<int>::max)())
    {
        log("DMA(" + std::to_string(dmaIndex) + ") image geometry is unavailable.", true);
        return;
    }
    const int width = static_cast<int>(widthValue);
    const int height = static_cast<int>(heightValue);
    const int sdkFormat = static_cast<int>(formatValue);

    std::int64_t areaTriggerMode = -1;
    std::int64_t triggerState = -1;
    const bool hasAreaTriggerMode =
        readIntegerParameter(FG_AREATRIGGERMODE, areaTriggerMode);
    const bool hasTriggerState = readIntegerParameter(FG_TRIGGERSTATE, triggerState);
    if (hasAreaTriggerMode || hasTriggerState)
    {
        log(
            "DMA(" + std::to_string(dmaIndex) + ") starting with "
                + "FG_AREATRIGGERMODE="
                + (hasAreaTriggerMode ? std::to_string(areaTriggerMode) : "unavailable")
                + ", FG_TRIGGERSTATE="
                + (hasTriggerState ? std::to_string(triggerState) : "unavailable")
                + ".");
    }

    std::int64_t bitAlignmentValue = FG_RIGHT_ALIGNED;
    const bool hasBitAlignment = readIntegerParameter(FG_BITALIGNMENT, bitAlignmentValue);
    const BitAlignment bitAlignment = !hasBitAlignment
        ? BitAlignment::Packed
        : bitAlignmentValue == FG_LEFT_ALIGNED
            ? BitAlignment::MostSignificant
            : BitAlignment::LeastSignificant;

    const PixelFormat pixelFormat = toPixelFormat(sdkFormat);
    const int sdkPixelBits = Fg_getBitsPerPixel(sdkFormat);
    const int pixelBits = sdkPixelBits > 0
        ? sdkPixelBits
        : bitsPerPixel(pixelFormat);
    const int pixelBytes = pixelBits > 0 ? (pixelBits + 7) / 8 : 0;
    if (width <= 0 || height <= 0 || pixelBits <= 0 || pixelBytes <= 0)
    {
        log("DMA(" + std::to_string(dmaIndex) + ") has an unsupported image format.", true);
        return;
    }

    const std::size_t rowBytes = static_cast<std::size_t>(width)
                                 * static_cast<std::size_t>(pixelBytes);
    if (static_cast<std::size_t>(height)
        > (std::numeric_limits<std::size_t>::max)() / rowBytes)
    {
        log("DMA(" + std::to_string(dmaIndex) + ") image size overflows address space.", true);
        return;
    }
    const std::size_t frameBytes = rowBytes * static_cast<std::size_t>(height);
    if (bufferCount > (std::numeric_limits<std::size_t>::max)() / frameBytes)
    {
        log("DMA(" + std::to_string(dmaIndex) + ") buffer allocation is too large.", true);
        return;
    }

    std::shared_ptr<OwnedBufferPool> ownedBufferPool;
    try
    {
        ownedBufferPool = OwnedBufferPool::create(
            frameBytes,
            bufferCount);
    }
    catch (const std::exception& exception)
    {
        log(
            "DMA(" + std::to_string(dmaIndex)
                + ") owned buffer pool allocation failed: "
                + exception.what(),
            true);
        return;
    }

    dma_mem* memory = Fg_AllocMemEx(
        handle,
        frameBytes * bufferCount,
        static_cast<frameindex_t>(bufferCount));
    if (!memory)
    {
        log("DMA(" + std::to_string(dmaIndex) + ") buffer allocation failed.", true);
        return;
    }

    auto channel = std::make_unique<DmaChannel>();
    channel->index = dmaIndex;
    channel->memory = memory;
    channel->ownedBufferPool = std::move(ownedBufferPool);
    channel->permits.store(1, std::memory_order_release);
    DmaChannel* channelPtr = channel.get();
    std::unique_lock<std::mutex> registrationLock(_stateMutex);
    _channels[dmaIndex] = std::move(channel);
    _activeChannels.fetch_add(1, std::memory_order_acq_rel);
    try
    {
        channelPtr->thread = std::thread(
            [this, channelPtr, handle, frames, width, height, sdkFormat, pixelFormat, pixelBits,
             pixelBytes, bitAlignment, frameBytes, bufferCount]
            {
                const unsigned int dmaIndex = channelPtr->index;
                const int acquireResult =
                    Fg_AcquireEx(handle, static_cast<int>(dmaIndex), GRAB_INFINITE, ACQ_STANDARD,
                                 channelPtr->memory);
                if (acquireResult != FG_OK)
                {
                    log("DMA(" + std::to_string(dmaIndex) + ") acquisition start failed: " +
                            safeString(Fg_getErrorDescription(nullptr, acquireResult)),
                        true);
                    finishChannel(*channelPtr);
                    return;
                }
                channelPtr->acquisitionStarted.store(true, std::memory_order_release);

                if (startCameraAcquisitionForChannel(dmaIndex))
                {
                    channelPtr->cameraStarted.store(true, std::memory_order_release);
                }

                frameindex_t lastPicture = 0;
                std::size_t delivered = 0;
                bool payloadGeometryWarningLogged = false;
                while (!channelPtr->stopRequested.load(std::memory_order_acquire))
                {
                    {
                        std::unique_lock<std::mutex> lock(channelPtr->permitMutex);
                        channelPtr->permitCondition.wait(
                            lock,
                            [channelPtr]
                            {
                                return channelPtr->stopRequested.load(std::memory_order_acquire) ||
                                       channelPtr->permits.load(std::memory_order_acquire) > 0;
                            });
                    }
                    if (channelPtr->stopRequested.load(std::memory_order_acquire))
                    {
                        break;
                    }
                    channelPtr->permits.fetch_sub(1, std::memory_order_acq_rel);

                    const frameindex_t nextPicture =
                        lastPicture < FRAMEINDEX_MAX ? lastPicture + 1 : 1;
                    const frameindex_t picture = Fg_getLastPicNumberBlockingEx(
                        handle,
                        nextPicture,
                        static_cast<int>(dmaIndex),
                        kFrameWaitTimeoutSeconds,
                        channelPtr->memory);
                    if (picture <= 0)
                    {
                        // The short wait timeout keeps stop responsive; it is not an error.
                        if (picture == FG_NO_PICTURE_AVAILABLE || picture == FG_TIMEOUT_ERR)
                        {
                            channelPtr->permits.fetch_add(1, std::memory_order_acq_rel);
                            continue;
                        }
                        if (!channelPtr->stopRequested.load(std::memory_order_acquire))
                        {
                            // FG_TRANSFER_NOT_ACTIVE: the driver ended the acquisition, for
                            // example after its FG_TIMEOUT. Waiting again cannot succeed.
                            log("DMA(" + std::to_string(dmaIndex) + ") acquisition ended: "
                                    + safeString(Fg_getErrorDescription(
                                        nullptr,
                                        static_cast<int>(picture)))
                                    + " (" + std::to_string(picture) + ").",
                                true);
                        }
                        break;
                    }

                    if (picture > nextPicture)
                    {
                        // ACQ_STANDARD returns the newest frame; older frames are skipped.
                        channelPtr->skipped.fetch_add(
                            static_cast<std::uint64_t>(picture - nextPicture),
                            std::memory_order_relaxed);
                    }
                    lastPicture = picture;
                    const auto* source = static_cast<const std::uint8_t*>(Fg_getImagePtrEx(
                        handle, picture, static_cast<int>(dmaIndex), channelPtr->memory));
                    if (!source)
                    {
                        channelPtr->rejected.fetch_add(1, std::memory_order_relaxed);
                        ready(dmaIndex);
                        continue;
                    }

                    std::size_t transferLength = 0;
                    const int transferResult = Fg_getParameterEx(
                        handle,
                        FG_TRANSFER_LEN,
                        &transferLength,
                        static_cast<int>(dmaIndex),
                        channelPtr->memory,
                        picture);
                    if (transferResult != FG_OK
                        || transferLength == 0U
                        || transferLength > frameBytes
                        || transferLength % static_cast<std::size_t>(height) != 0U)
                    {
                        log("DMA(" + std::to_string(dmaIndex)
                                + ") reported an invalid transfer length.",
                            true);
                        channelPtr->rejected.fetch_add(1, std::memory_order_relaxed);
                        ready(dmaIndex);
                        continue;
                    }
                    const std::size_t payloadBytes = transferLength;
                    const std::size_t payloadStride = payloadBytes / static_cast<std::size_t>(height);
                    if (payloadStride > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
                    {
                        log("DMA(" + std::to_string(dmaIndex)
                                + ") image stride exceeds the supported range.",
                            true);
                        ready(dmaIndex);
                        continue;
                    }

                    // The applet may keep an ROI that is narrower than the camera's
                    // current output. In that case FG_WIDTH still describes the camera
                    // geometry while FG_TRANSFER_LEN describes the delivered DMA rows.
                    // Preserve the image by publishing the width the DMA payload can
                    // actually represent instead of forwarding an invalid stride to the
                    // display backend.
                    const std::size_t payloadBits = payloadStride * 8U;
                    const std::size_t effectiveWidth = payloadBits
                        / static_cast<std::size_t>(pixelBits);
                    if (effectiveWidth == 0U)
                    {
                        log("DMA(" + std::to_string(dmaIndex)
                                + ") transfer row is smaller than one pixel.",
                            true);
                        ready(dmaIndex);
                        continue;
                    }
                    const int imageWidth = static_cast<int>(std::min<std::size_t>(
                        static_cast<std::size_t>(width), effectiveWidth));
                    if (imageWidth < width && !payloadGeometryWarningLogged)
                    {
                        log("DMA(" + std::to_string(dmaIndex)
                                + ") payload geometry differs from the configured geometry: "
                                + std::to_string(width) + "x" + std::to_string(height)
                                + " configured, " + std::to_string(imageWidth) + "x"
                                + std::to_string(height) + " delivered (transfer="
                                + std::to_string(payloadBytes) + " bytes, stride="
                                + std::to_string(payloadStride) + "). Displaying the delivered ROI.",
                            true);
                        payloadGeometryWarningLogged = true;
                    }

                    OwnedBufferPool::Lease ownedBytes =
                        channelPtr->ownedBufferPool->acquire(
                            channelPtr->stopRequested);
                    if (!ownedBytes)
                    {
                        break;
                    }
                    std::memcpy(ownedBytes.get(), source, payloadBytes);

                    // ACQ_STANDARD does not protect frame buffers. The buffer of `picture`
                    // is reused by frame picture + bufferCount, whose transfer can start
                    // once picture + bufferCount - 1 is complete. Drop a copy that may
                    // contain data from that later transfer.
                    const frameindex_t grabbed = Fg_getStatusEx(
                        handle,
                        NUMBER_OF_GRABBED_IMAGES,
                        0,
                        dmaIndex,
                        channelPtr->memory);
                    if (grabbed > 0
                        && grabbed - picture
                            >= static_cast<frameindex_t>(bufferCount) - 1)
                    {
                        channelPtr->overwritten.fetch_add(1, std::memory_order_relaxed);
                        ready(dmaIndex);
                        continue;
                    }

                    Image image;
                    image.storage = std::move(ownedBytes);
                    image.size = payloadBytes;
                    image.width = imageWidth;
                    image.height = height;
                    image.stride = static_cast<int>(payloadStride);
                    image.bitsPerPixel = pixelBits;
                    image.bytesPerPixel = pixelBytes;
                    image.fgFormat = sdkFormat;
                    image.pixelFormat = pixelFormat;
                    image.bitAlignment = bitAlignment;
                    image.dmaIndex = dmaIndex;
                    image.frameSeq = _frameSeq.fetch_add(1, std::memory_order_acq_rel) + 1;

                    std::vector<GrabCallback> callbacks;
                    {
                        std::lock_guard<std::mutex> lock(_grabCallbackMutex);
                        callbacks.reserve(_grabCallbacks.size());
                        for (const auto& item : _grabCallbacks)
                        {
                            callbacks.push_back(item.second);
                        }
                    }
                    for (const GrabCallback& callback : callbacks)
                    {
                        try
                        {
                            callback(image, image.frameSeq);
                        }
                        catch (const std::exception& exception)
                        {
                            log(std::string("Grab callback failed: ") + exception.what(), true);
                        }
                        catch (...)
                        {
                            log("Grab callback failed with an unknown exception.", true);
                        }
                    }

                    ++delivered;
                    channelPtr->delivered.fetch_add(1, std::memory_order_relaxed);
                    if (frames != 0 && delivered >= frames)
                    {
                        break;
                    }
                }

                finishChannel(*channelPtr);
            });
    }
    catch (const std::exception& exception)
    {
        channelPtr->memory = nullptr;
        _channels[dmaIndex].reset();
        _activeChannels.fetch_sub(1, std::memory_order_acq_rel);
        registrationLock.unlock();
        Fg_FreeMemEx(handle, memory);
        log("DMA(" + std::to_string(dmaIndex) + ") worker creation failed: " + exception.what(),
            true);
        return;
    }
    registrationLock.unlock();
}

void Framegrabber::finishChannel(DmaChannel& channel)
{
    Fg_Struct* handle = nullptr;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        handle = _handle;
        ChannelStatistics statistics;
        statistics.dmaIndex = channel.index;
        statistics.delivered = channel.delivered.load(std::memory_order_relaxed);
        statistics.skipped = channel.skipped.load(std::memory_order_relaxed);
        statistics.overwritten = channel.overwritten.load(std::memory_order_relaxed);
        statistics.rejected = channel.rejected.load(std::memory_order_relaxed);
        _lastStatistics.push_back(statistics);
        if (statistics.skipped != 0 || statistics.overwritten != 0 || statistics.rejected != 0)
        {
            log("DMA(" + std::to_string(channel.index) + ") run finished: delivered="
                + std::to_string(statistics.delivered) + ", skipped="
                + std::to_string(statistics.skipped) + ", overwritten="
                + std::to_string(statistics.overwritten) + ", rejected="
                + std::to_string(statistics.rejected) + ".",
                statistics.overwritten != 0);
        }
    }

    if (channel.cameraStarted.exchange(false, std::memory_order_acq_rel))
    {
        stopCameraAcquisitionForChannel(channel.index);
    }
    {
        std::lock_guard<std::mutex> resourceLock(channel.resourceMutex);
        if (handle && channel.acquisitionStarted.exchange(false, std::memory_order_acq_rel))
        {
            Fg_stopAcquireEx(handle, static_cast<int>(channel.index), channel.memory, 0);
        }
        if (handle && channel.memory)
        {
            Fg_FreeMemEx(handle, channel.memory);
            channel.memory = nullptr;
        }
    }

    const unsigned int previous = _activeChannels.fetch_sub(1, std::memory_order_acq_rel);
    if (previous == 1
        && !_startingChannels.load(std::memory_order_acquire))
    {
        _isRunning.store(false, std::memory_order_release);
        notifyStatus(GrabbingStatus, false);
    }
}

bool Framegrabber::startCameraAcquisitionForChannel(const unsigned int dmaIndex)
{
    SgcCameraHandle* cameraHandle = nullptr;
    if (connectCamera(CameraTransport::CoaXPress, dmaIndex))
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        if (CameraEntry* camera = findCamera(CameraTransport::CoaXPress, dmaIndex))
        {
            cameraHandle = camera->handle;
        }
    }
    if (!cameraHandle)
    {
        log("DMA(" + std::to_string(dmaIndex)
                + ") camera acquisition cannot start because no connected CXP camera is mapped.",
            true);
        return false;
    }

    const int result = Sgc_startAcquisition(cameraHandle, 1);
    if (result != SGC_OK)
    {
        log("DMA(" + std::to_string(dmaIndex) + ") camera acquisition start failed: "
                + safeString(Sgc_getErrorDescription(result)),
            true);
        return false;
    }
    log("DMA(" + std::to_string(dmaIndex) + ") camera acquisition started.");
    return true;
}

void Framegrabber::stopCameraAcquisitionForChannel(const unsigned int dmaIndex)
{
    SgcCameraHandle* cameraHandle = nullptr;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        if (CameraEntry* camera = findCamera(CameraTransport::CoaXPress, dmaIndex))
        {
            cameraHandle = camera->handle;
        }
    }
    if (cameraHandle)
    {
        Sgc_stopAcquisition(cameraHandle, 1);
    }
}

void Framegrabber::requestStop()
{
    std::lock_guard<std::mutex> lifecycleLock(_lifecycleMutex);
    requestStopChannels();
}

void Framegrabber::requestStopChannels()
{
    _isRunning.store(false, std::memory_order_release);

    std::vector<DmaChannel*> channels;
    Fg_Struct* handle = nullptr;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        handle = _handle;
        channels.reserve(_channels.size());
        for (const auto& channel : _channels)
        {
            if (!channel)
            {
                continue;
            }
            channel->stopRequested.store(true, std::memory_order_release);
            channel->permitCondition.notify_all();
            if (channel->ownedBufferPool)
            {
                channel->ownedBufferPool->stop();
            }
            channels.push_back(channel.get());
        }
    }

    if (!handle)
    {
        return;
    }
    for (DmaChannel* channel : channels)
    {
        // Documented order: stop the camera stream, then the frame grabber DMA.
        if (channel->cameraStarted.exchange(false, std::memory_order_acq_rel))
        {
            stopCameraAcquisitionForChannel(channel->index);
        }
        std::lock_guard<std::mutex> resourceLock(channel->resourceMutex);
        if (channel->memory
            && channel->acquisitionStarted.exchange(false, std::memory_order_acq_rel))
        {
            Fg_stopAcquireEx(
                handle,
                static_cast<int>(channel->index),
                channel->memory,
                0);
        }
    }
}

void Framegrabber::stop()
{
    std::lock_guard<std::mutex> lifecycleLock(_lifecycleMutex);
    requestStopChannels();
    joinStoppedChannels();
}

bool Framegrabber::joinStoppedChannels()
{
    std::vector<std::thread*> threads;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        for (const auto& channel : _channels)
        {
            if (channel && channel->thread.joinable())
            {
                threads.push_back(&channel->thread);
            }
        }
    }

    bool allThreadsJoined = true;
    for (std::thread* thread : threads)
    {
        if (thread->get_id() == std::this_thread::get_id())
        {
            allThreadsJoined = false;
            continue;
        }
        thread->join();
    }

    if (!allThreadsJoined)
    {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        _channels.clear();
    }
    return true;
}

void Framegrabber::ready(const unsigned int dmaIndex)
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    if (dmaIndex >= _channels.size() || !_channels[dmaIndex])
    {
        return;
    }
    DmaChannel& channel = *_channels[dmaIndex];
    if (channel.permits.exchange(1, std::memory_order_acq_rel) == 0)
    {
        channel.permitCondition.notify_one();
    }
}

bool Framegrabber::isGrabbing() const noexcept
{
    return _isRunning.load(std::memory_order_acquire);
}

int Framegrabber::getDMACount() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    if (!_handle)
    {
        return 0;
    }
    // FG_NR_OF_DMAS exists once per applet process; its index argument is the process.
    std::int32_t processes = 0;
    if (Fg_getParameterWithType(_handle, FG_NR_OF_PROCESSES, &processes, 0u) != FG_OK
        || processes <= 0)
    {
        processes = 1;
    }
    int count = 0;
    for (std::int32_t process = 0; process < processes; ++process)
    {
        std::int32_t dmas = 0;
        if (Fg_getParameterWithType(
                _handle,
                FG_NR_OF_DMAS,
                &dmas,
                static_cast<unsigned int>(process)) != FG_OK)
        {
            return process == 0 ? 0 : count;
        }
        count += (std::max)(0, static_cast<int>(dmas));
    }
    return count;
}

std::vector<Framegrabber::ChannelStatistics> Framegrabber::acquisitionStatistics() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    std::vector<ChannelStatistics> result = _lastStatistics;
    for (const auto& channel : _channels)
    {
        if (!channel)
        {
            continue;
        }
        ChannelStatistics statistics;
        statistics.dmaIndex = channel->index;
        statistics.delivered = channel->delivered.load(std::memory_order_relaxed);
        statistics.skipped = channel->skipped.load(std::memory_order_relaxed);
        statistics.overwritten = channel->overwritten.load(std::memory_order_relaxed);
        statistics.rejected = channel->rejected.load(std::memory_order_relaxed);
        const auto existing = std::find_if(result.begin(), result.end(),
            [&statistics](const ChannelStatistics& item)
            {
                return item.dmaIndex == statistics.dmaIndex;
            });
        if (existing == result.end())
        {
            result.push_back(statistics);
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right)
    {
        return left.dmaIndex < right.dmaIndex;
    });
    return result;
}

std::vector<std::string> Framegrabber::getUpdatedFramegrabberList() const
{
    return _system ? _system->getFramegrabberList() : std::vector<std::string>{};
}

std::vector<std::string> Framegrabber::getCachedFramegrabberList() const
{
    return _system ? _system->getCachedFramegrabberList() : std::vector<std::string>{};
}

std::vector<FramegrabberSystem::BoardInfo> Framegrabber::getCachedBoardInfo() const
{
    return _system ? _system->getCachedBoardInfo() : std::vector<FramegrabberSystem::BoardInfo>{};
}

std::string Framegrabber::getBoardAppletPath(const std::string& boardName) const
{
    return _system ? _system->getBoardAppletPath(boardName) : std::string{};
}

void Framegrabber::setDMABufferCount(const std::size_t count)
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    if (!_isRunning.load(std::memory_order_acquire))
    {
        // A frame copy is valid only until the next transfer completes; one buffer would drop every frame.
        _dmaBufferCount = (std::max<std::size_t>)(2, count);
    }
}

std::size_t Framegrabber::dmaBufferCount() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _dmaBufferCount;
}

std::string Framegrabber::getAppletFeatureXml(const unsigned int dmaIndex) const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    if (!_handle)
    {
        return {};
    }

    std::size_t size = 0;
    Fg_getParameterInfoXML(_handle, static_cast<int>(dmaIndex), nullptr, &size);
    if (size == 0)
    {
        return {};
    }

    std::vector<char> buffer(size + 1, '\0');
    if (Fg_getParameterInfoXML(
            _handle,
            static_cast<int>(dmaIndex),
            buffer.data(),
            &size) != FG_OK)
    {
        return {};
    }
    return std::string(buffer.data());
}

std::vector<Framegrabber::AppletFeatureNode>
Framegrabber::getAppletFeatureModel(const unsigned int dmaIndex) const
{
    std::vector<AppletFeatureNode> cachedModel;
    {
        std::lock_guard<std::mutex> lock(_appletFeatureModelMutex);
        const auto cached = _appletFeatureModels.find(dmaIndex);
        if (cached != _appletFeatureModels.end())
        {
            cachedModel = cached->second;
        }
    }
    if (!cachedModel.empty())
    {
        refreshAppletFeatureAccess(cachedModel, dmaIndex);
        return cachedModel;
    }

    const std::string xml = getAppletFeatureXml(dmaIndex);
    if (xml.empty())
    {
        return {};
    }

    std::vector<AppletFeatureNode> model;
    try
    {
        GenApi::CNodeMapRef nodeMap("FramegrabberApplet");
        nodeMap._LoadXMLFromString(xml.c_str());
        auto* root = dynamic_cast<GenApi::ICategory*>(nodeMap._GetNode("Root"));
        if (root)
        {
            GenApi::FeatureList_t features;
            root->GetFeatures(features);
            model.reserve(features.size());
            std::unordered_set<std::string> visiting;
            for (GenApi::IValue* value : features)
            {
                GenApi::INode* feature = value != nullptr ? value->GetNode() : nullptr;
                AppletFeatureNode node;
                if (buildAppletFeatureNode(feature, node, visiting))
                {
                    model.push_back(std::move(node));
                }
            }
        }
    }
    catch (const std::exception& exception)
    {
        log(std::string("Failed to build applet GenApi node map: ") + exception.what(), true);
        return {};
    }
    catch (...)
    {
        log("Failed to build applet GenApi node map.", true);
        return {};
    }

    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        if (!_handle)
        {
            return {};
        }
        std::function<void(std::vector<AppletFeatureNode>&)> enrich =
            [&](std::vector<AppletFeatureNode>& nodes)
            {
                for (AppletFeatureNode& node : nodes)
                {
                    if (node.kind == AppletFeatureKind::Category)
                    {
                        enrich(node.children);
                        continue;
                    }

                    int parameterId = node.parameterId >= 0
                        && node.parameterId <= (std::numeric_limits<int>::max)()
                        ? static_cast<int>(node.parameterId)
                        : Fg_getParameterIdByName(_handle, node.name.c_str());
                    if (parameterId < 0)
                    {
                        continue;
                    }
                    node.parameterId = parameterId;

                    if (const char* accessName = Fg_getParameterNameById(
                            _handle,
                            static_cast<unsigned int>(parameterId),
                            dmaIndex))
                    {
                        node.accessName = accessName;
                    }
                    if (node.accessName.empty())
                    {
                        node.accessName = node.name;
                    }

                    std::string symbolicName;
                    if (node.displayName == node.name
                        && Fg_getParameterPropertyWithTypeEx(
                               _handle,
                               parameterId,
                               PROP_ID_NAME,
                               symbolicName,
                               dmaIndex) == FG_OK
                        && !symbolicName.empty())
                    {
                        node.displayName = std::move(symbolicName);
                    }

                }
            };
        enrich(model);
    }

    refreshAppletFeatureAccess(model, dmaIndex);
    {
        std::lock_guard<std::mutex> lock(_appletFeatureModelMutex);
        _appletFeatureModels[dmaIndex] = model;
    }
    return model;
}

void Framegrabber::clearAppletFeatureModels()
{
    std::lock_guard<std::mutex> lock(_appletFeatureModelMutex);
    _appletFeatureModels.clear();
}

void Framegrabber::clearAppletFeatureModel(const unsigned int dmaIndex)
{
    std::lock_guard<std::mutex> lock(_appletFeatureModelMutex);
    _appletFeatureModels.erase(dmaIndex);
}

void Framegrabber::refreshAppletFeatureAccess(
    std::vector<AppletFeatureNode>& model,
    const unsigned int dmaIndex) const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    if (!_handle)
    {
        return;
    }

    std::function<void(std::vector<AppletFeatureNode>&)> refresh =
        [&](std::vector<AppletFeatureNode>& nodes)
        {
            for (AppletFeatureNode& node : nodes)
            {
                if (node.kind == AppletFeatureKind::Category)
                {
                    refresh(node.children);
                    continue;
                }
                if (node.parameterId < 0
                    || node.parameterId > (std::numeric_limits<int>::max)())
                {
                    node.readable = false;
                    node.writable = false;
                    continue;
                }

                std::int32_t access = 0;
                if (readAppletAccessFlags(
                        _handle,
                        static_cast<int>(node.parameterId),
                        dmaIndex,
                        access))
                {
                    node.readable =
                        (access & FP_PARAMETER_PROPERTY_ACCESS_READ) != 0;
                    node.writable = appletParameterWritable(
                        access,
                        _isRunning.load(std::memory_order_acquire));
                }
                else
                {
                    node.writable = node.writable
                        && !_isRunning.load(std::memory_order_acquire);
                }
            }
        };
    refresh(model);
}

bool Framegrabber::getAppletParameter(const std::string& name,
                                      const unsigned int dmaIndex,
                                      ParameterValue& value,
                                      const bool silent) const
{
    int parameterId = -1;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        if (!_handle || name.empty())
        {
            return false;
        }
        parameterId = Fg_getParameterIdByName(_handle, name.c_str());
    }
    if (parameterId >= 0)
    {
        return getAppletParameterById(parameterId, dmaIndex, value, silent);
    }
    if (!silent)
    {
        log("Unknown applet parameter '" + name + "'.", true);
    }
    return false;
}

bool Framegrabber::getAppletParameterById(const std::int64_t parameterId,
                                          const unsigned int dmaIndex,
                                          ParameterValue& value,
                                          const bool silent) const
{
    if (parameterId < 0 || parameterId > (std::numeric_limits<int>::max)())
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(_stateMutex);
    if (!_handle)
    {
        return false;
    }
    const int sdkParameterId = static_cast<int>(parameterId);
    const FgParamTypes type = Fg_getParameterTypeById(
        _handle,
        sdkParameterId,
        static_cast<int>(dmaIndex));
    int result = FG_ERROR;
    switch (type)
    {
    case FG_PARAM_TYPE_INT32_T:
    {
        std::int32_t output = 0;
        result = Fg_getParameterWithType(_handle, sdkParameterId, &output, dmaIndex, type);
        value = output;
        break;
    }
    case FG_PARAM_TYPE_UINT32_T:
    {
        std::uint32_t output = 0;
        result = Fg_getParameterWithType(_handle, sdkParameterId, &output, dmaIndex, type);
        value = output;
        break;
    }
    case FG_PARAM_TYPE_INT64_T:
    {
        std::int64_t output = 0;
        result = Fg_getParameterWithType(_handle, sdkParameterId, &output, dmaIndex, type);
        value = output;
        break;
    }
    case FG_PARAM_TYPE_UINT64_T:
    {
        std::uint64_t output = 0;
        result = Fg_getParameterWithType(_handle, sdkParameterId, &output, dmaIndex, type);
        value = output;
        break;
    }
    case FG_PARAM_TYPE_SIZE_T:
    {
        std::size_t output = 0;
        result = Fg_getParameterWithType(_handle, sdkParameterId, &output, dmaIndex, type);
        value = static_cast<std::uint64_t>(output);
        break;
    }
    case FG_PARAM_TYPE_DOUBLE:
    {
        double output = 0.0;
        result = Fg_getParameterWithType(_handle, sdkParameterId, &output, dmaIndex, type);
        value = output;
        break;
    }
    case FG_PARAM_TYPE_CHAR_PTR:
    {
        // The std::string overload reads an SDK-allocated string of any length.
        std::string output;
        result = Fg_getParameterWithType(_handle, sdkParameterId, output, dmaIndex);
        value = std::move(output);
        break;
    }
    default:
        return false;
    }

    if (result != FG_OK && !silent)
    {
        log(
            "Failed to read applet parameter ID "
                + std::to_string(sdkParameterId) + " (SDK result "
                + std::to_string(result) + ").",
            true);
    }
    return result == FG_OK;
}

bool Framegrabber::setAppletParameter(const std::string& name,
                                      const unsigned int dmaIndex,
                                      const ParameterValue& value,
                                      const bool silent,
                                      const bool verifyReadBack)
{
    int parameterId = -1;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        if (!_handle || name.empty())
        {
            return false;
        }

        parameterId = Fg_getParameterIdByName(_handle, name.c_str());
    }
    return parameterId >= 0
        && setAppletParameterById(
            parameterId,
            dmaIndex,
            value,
            silent,
            verifyReadBack);
}

bool Framegrabber::setAppletParameterById(const std::int64_t parameterId,
                                          const unsigned int dmaIndex,
                                          const ParameterValue& value,
                                          const bool silent,
                                          const bool verifyReadBack,
                                          int* sdkResult)
{
    int result = FG_ERROR;
    struct ResultCopy
    {
        int* output;
        const int& result;
        ~ResultCopy()
        {
            if (output) *output = result;
        }
    } resultCopy{sdkResult, result};
    if (parameterId < 0 || parameterId > (std::numeric_limits<int>::max)())
    {
        result = FG_INVALID_PARAMETER;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        if (!_handle)
        {
            result = FG_NOT_INIT;
            return false;
        }
        const int sdkParameterId = static_cast<int>(parameterId);
        std::int32_t access = 0;
        if (readAppletAccessFlags(
                _handle,
                sdkParameterId,
                dmaIndex,
                access)
            && !appletParameterWritable(
                access,
                _isRunning.load(std::memory_order_acquire)))
        {
            if (!silent)
            {
                log(
                    "Applet parameter ID " + std::to_string(parameterId)
                        + " is not writable in the current state.",
                    true);
            }
            result = FG_WRITE_ACCESS_DENIED;
            return false;
        }
        const FgParamTypes type = Fg_getParameterTypeById(
            _handle,
            sdkParameterId,
            static_cast<int>(dmaIndex));

        try
        {
            switch (type)
            {
            case FG_PARAM_TYPE_INT32_T:
            {
                const auto typed = std::get<std::int32_t>(value);
                result = Fg_setParameterWithType(_handle, sdkParameterId, &typed, dmaIndex, type);
                break;
            }
            case FG_PARAM_TYPE_UINT32_T:
            {
                const auto typed = std::get<std::uint32_t>(value);
                result = Fg_setParameterWithType(_handle, sdkParameterId, &typed, dmaIndex, type);
                break;
            }
            case FG_PARAM_TYPE_INT64_T:
            {
                const auto typed = std::get<std::int64_t>(value);
                result = Fg_setParameterWithType(_handle, sdkParameterId, &typed, dmaIndex, type);
                break;
            }
            case FG_PARAM_TYPE_UINT64_T:
            {
                const auto typed = std::get<std::uint64_t>(value);
                result = Fg_setParameterWithType(_handle, sdkParameterId, &typed, dmaIndex, type);
                break;
            }
            case FG_PARAM_TYPE_SIZE_T:
            {
                const auto typed = static_cast<std::size_t>(std::get<std::uint64_t>(value));
                result = Fg_setParameterWithType(_handle, sdkParameterId, &typed, dmaIndex, type);
                break;
            }
            case FG_PARAM_TYPE_DOUBLE:
            {
                const auto typed = std::get<double>(value);
                result = Fg_setParameterWithType(_handle, sdkParameterId, &typed, dmaIndex, type);
                break;
            }
            case FG_PARAM_TYPE_CHAR_PTR:
            {
                const auto& typed = std::get<std::string>(value);
                result = Fg_setParameterWithType(
                    _handle,
                    sdkParameterId,
                    typed.c_str(),
                    dmaIndex,
                    type);
                break;
            }
            default:
                result = FG_INVALID_PARAMETER;
                return false;
            }
        }
        catch (const std::bad_variant_access&)
        {
            result = FG_INVALID_PARAMETER;
            if (!silent)
            {
                log(
                    "Type mismatch for applet parameter ID "
                        + std::to_string(sdkParameterId) + ".",
                    true);
            }
            return false;
        }
    }

    if (result == FG_OK)
    {
        if (verifyReadBack)
        {
            ParameterValue actual = value;
            if (!getAppletParameterById(
                    parameterId,
                    dmaIndex,
                    actual,
                    true))
            {
                clearAppletFeatureModel(dmaIndex);
                if (!silent)
                {
                    log(
                        "Applet parameter ID " + std::to_string(parameterId)
                            + " was written but read-back verification failed.",
                        true);
                }
                return false;
            }
            if (!parameterValuesMatch(value, actual))
            {
                clearAppletFeatureModel(dmaIndex);
                if (!silent)
                {
                    log(
                        "Applet parameter ID " + std::to_string(parameterId)
                            + " read-back value does not match the requested value.",
                        true);
                }
                return false;
            }
        }
        clearAppletFeatureModel(dmaIndex);
        notifyNode(
            FeatureSource::Applet,
            CameraTransport::None,
            dmaIndex,
            std::to_string(parameterId));
        return true;
    }
    if (!silent)
    {
        log(
            "Failed to set applet parameter ID " + std::to_string(parameterId)
                + " (SDK result " + std::to_string(result) + ").",
            true);
    }
    return false;
}

bool Framegrabber::executeAppletCommand(const unsigned int dmaIndex, const std::string& name)
{
    int parameterId = -1;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        if (!_handle || name.empty())
        {
            return false;
        }
        parameterId = Fg_getParameterIdByName(_handle, name.c_str());
    }
    return parameterId >= 0 && executeAppletCommandById(dmaIndex, parameterId);
}

bool Framegrabber::executeAppletCommandById(
    const unsigned int dmaIndex,
    const std::int64_t parameterId,
    int* sdkResult)
{
    // Applet commands are often write-only; build the value from the declared SDK type
    // instead of reading the parameter first.
    std::optional<ParameterValue> command = appletParameterPrototype(parameterId, dmaIndex);
    if (!command)
    {
        if (sdkResult) *sdkResult = FG_INVALID_PARAMETER;
        return false;
    }
    bool integral = false;
    std::visit(
        [&integral](auto& value)
        {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_integral_v<T>)
            {
                value = static_cast<T>(1);
                integral = true;
            }
        },
        *command);
    if (!integral)
    {
        if (sdkResult) *sdkResult = FG_INVALID_PARAMETER;
        return false;
    }
    return setAppletParameterById(parameterId, dmaIndex, *command, false, false, sdkResult);
}

std::optional<Framegrabber::ParameterValue> Framegrabber::appletParameterPrototype(
    const std::int64_t parameterId,
    const unsigned int dmaIndex) const
{
    if (parameterId < 0 || parameterId > (std::numeric_limits<int>::max)())
    {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(_stateMutex);
    if (!_handle)
    {
        return std::nullopt;
    }
    switch (Fg_getParameterTypeById(_handle, static_cast<int>(parameterId), dmaIndex))
    {
    case FG_PARAM_TYPE_INT32_T:
        return ParameterValue{std::int32_t{0}};
    case FG_PARAM_TYPE_UINT32_T:
        return ParameterValue{std::uint32_t{0}};
    case FG_PARAM_TYPE_INT64_T:
        return ParameterValue{std::int64_t{0}};
    case FG_PARAM_TYPE_UINT64_T:
    case FG_PARAM_TYPE_SIZE_T:
        return ParameterValue{std::uint64_t{0}};
    case FG_PARAM_TYPE_DOUBLE:
        return ParameterValue{0.0};
    case FG_PARAM_TYPE_CHAR_PTR:
        return ParameterValue{std::string{}};
    default:
        return std::nullopt;
    }
}

Framegrabber::AppletParameterLimits Framegrabber::getAppletParameterLimits(
    const std::int64_t parameterId,
    const unsigned int dmaIndex) const
{
    AppletParameterLimits limits;
    if (parameterId < 0 || parameterId > (std::numeric_limits<int>::max)())
    {
        return limits;
    }
    std::lock_guard<std::mutex> lock(_stateMutex);
    if (!_handle)
    {
        return limits;
    }
    const int id = static_cast<int>(parameterId);
    const auto read = [this, id, dmaIndex](const FgProperty property, auto typed)
        -> std::optional<ParameterValue>
    {
        if (Fg_getParameterPropertyWithTypeEx(_handle, id, property, &typed, dmaIndex) != FG_OK)
        {
            return std::nullopt;
        }
        return ParameterValue{typed};
    };
    const auto readAll = [&](auto typed)
    {
        limits.minimum = read(PROP_ID_MIN, typed);
        limits.maximum = read(PROP_ID_MAX, typed);
        limits.increment = read(PROP_ID_STEP, typed);
    };
    switch (Fg_getParameterTypeById(_handle, id, dmaIndex))
    {
    case FG_PARAM_TYPE_INT32_T:
        readAll(std::int32_t{0});
        break;
    case FG_PARAM_TYPE_UINT32_T:
        readAll(std::uint32_t{0});
        break;
    case FG_PARAM_TYPE_INT64_T:
        readAll(std::int64_t{0});
        break;
    case FG_PARAM_TYPE_UINT64_T:
    case FG_PARAM_TYPE_SIZE_T:
        readAll(std::uint64_t{0});
        break;
    case FG_PARAM_TYPE_DOUBLE:
        readAll(0.0);
        break;
    default:
        break;
    }
    return limits;
}

std::vector<Framegrabber::CameraControlCapability>
Framegrabber::cameraControlCapabilities() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    std::vector<CameraControlCapability> capabilities;
    if (_cxpBoard)
    {
        capabilities.push_back(
            {CameraTransport::CoaXPress, true, true, true});
    }
    return capabilities;
}

bool Framegrabber::refreshCameras(const CameraTransport transport)
{
    if (transport != CameraTransport::CoaXPress)
    {
        return false;
    }
    std::lock_guard<std::mutex> lifecycleLock(_lifecycleMutex);
    return refreshCamerasUnlocked(true);
}

Framegrabber::CameraDiscovery Framegrabber::lastCameraDiscovery() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _lastCameraDiscovery;
}

bool Framegrabber::refreshCamerasUnlocked(const bool resetDiscovery)
{
    // The lifecycle lock keeps _cxpBoard alive; _stateMutex is not held during the
    // discovery wait so GUI and API state reads stay responsive.
    SgcBoardHandle* board = nullptr;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        if (!_cxpBoard || _isRunning.load(std::memory_order_acquire))
        {
            return false;
        }
        board = _cxpBoard;
        for (CameraEntry& camera : _cxpCameras)
        {
            if (camera.handle && camera.info.connected)
            {
                Sgc_disconnectCamera(camera.handle);
            }
        }
        _cxpCameras.clear();
    }

    // A single process scans every physical port (mask 0xf). A repeated Sgc_scanPorts
    // only synchronizes with the first discovery; a reset restarts it.
    const int scanResult = Sgc_scanPortsEx(
        board,
        0xf,
        kCameraDiscoveryTimeoutMilliseconds,
        LINK_SPEED_NONE,
        resetDiscovery ? SGC_SP_FLAG_RESET_DISCOVERY_INFOS : 0u);
    CameraDiscovery discovery;
    discovery.sdkResult = scanResult;
    discovery.topologyMismatch = scanResult == ERR_SGC_TOPOLOGY_MISMATCH;
    discovery.message = scanResult == SGC_OK
        ? std::string{}
        : safeString(Sgc_getErrorDescription(scanResult));
    if (discovery.topologyMismatch)
    {
        discovery.message += " Select an applet whose camera and link layout matches the "
                             "connected cameras; image data cannot be transferred correctly.";
    }
    if (scanResult != SGC_OK && !discovery.topologyMismatch)
    {
        log("CXP camera discovery: " + discovery.message + " (" + std::to_string(scanResult)
                + ").",
            true);
        std::lock_guard<std::mutex> lock(_stateMutex);
        _lastCameraDiscovery = std::move(discovery);
        return false;
    }
    if (discovery.topologyMismatch)
    {
        log("CXP camera discovery: " + discovery.message, true);
    }

    std::vector<CameraEntry> discovered;
    const int count = Sgc_getCameraCount(board);
    discovered.reserve(static_cast<std::size_t>((std::max)(0, count)));
    for (int index = 0; index < count; ++index)
    {
        SgcCameraHandle* handle = nullptr;
        if (Sgc_getCameraByIndex(board, static_cast<unsigned int>(index), &handle) != SGC_OK
            || !handle)
        {
            continue;
        }

        CameraEntry entry;
        entry.handle = handle;
        entry.info.transport = CameraTransport::CoaXPress;
        if (const SgcCameraInfo* info = Sgc_getCameraInfo(handle))
        {
            entry.info.vendor = safeString(info->deviceVendorName);
            entry.info.model = safeString(info->deviceModelName);
            entry.info.serial = safeString(info->deviceSerialNumber);
        }

        unsigned int propertyType = SGC_PROPERTY_TYPE_UINT;
        unsigned int dmaIndex = (std::numeric_limits<unsigned int>::max)();
        const int operatorResult = Sgc_getCameraPropertyWithType(
            handle,
            CAM_PROP_APPLETOPERATORINDEX,
            &dmaIndex,
            &propertyType,
            nullptr);
        if (operatorResult != SGC_OK)
        {
            // Without the applet operator index the camera cannot be bound to a DMA.
            log("CXP camera " + entry.info.displayName()
                    + " has no applet operator index: "
                    + safeString(Sgc_getErrorDescription(operatorResult)),
                true);
            continue;
        }
        entry.info.dmaIndex = dmaIndex;
        discovered.push_back(std::move(entry));
    }

    bool allConnected = true;
    for (CameraEntry& camera : discovered)
    {
        if (!camera.handle || Sgc_connectCamera(camera.handle) != SGC_OK)
        {
            allConnected = false;
            log(
                "Failed to connect the discovered CXP camera on DMA "
                    + std::to_string(camera.info.dmaIndex) + ".",
                true);
            continue;
        }
        camera.info.connected = true;
        camera.featureXml = readCameraXmlProperty(camera.handle);
        if (camera.featureXml.empty() && Sgc_loadCameraXml(camera.handle) == SGC_OK)
        {
            camera.featureXml = readCameraXmlProperty(camera.handle);
        }
    }

    std::lock_guard<std::mutex> lock(_stateMutex);
    _cxpCameras = std::move(discovered);
    _lastCameraDiscovery = std::move(discovery);
    return allConnected;
}

std::vector<Framegrabber::CameraInfo>
Framegrabber::getCachedCameraList(const CameraTransport transport) const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    std::vector<CameraInfo> cameras;
    if (transport != CameraTransport::CoaXPress)
    {
        return cameras;
    }
    cameras.reserve(_cxpCameras.size());
    for (const CameraEntry& entry : _cxpCameras)
    {
        cameras.push_back(entry.info);
    }
    return cameras;
}

bool Framegrabber::connectCamera(const CameraTransport transport,
                                 const unsigned int dmaIndex)
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    CameraEntry* camera = findCamera(transport, dmaIndex);
    if (!camera || !camera->handle)
    {
        return false;
    }
    if (camera->info.connected)
    {
        return true;
    }
    if (Sgc_connectCamera(camera->handle) != SGC_OK)
    {
        return false;
    }
    camera->info.connected = true;
    camera->featureXml = readCameraXmlProperty(camera->handle);
    if (camera->featureXml.empty() && Sgc_loadCameraXml(camera->handle) == SGC_OK)
    {
        camera->featureXml = readCameraXmlProperty(camera->handle);
    }
    return true;
}

void Framegrabber::disconnectCamera(const CameraTransport transport,
                                    const unsigned int dmaIndex)
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    CameraEntry* camera = findCamera(transport, dmaIndex);
    if (camera && camera->handle && camera->info.connected)
    {
        Sgc_disconnectCamera(camera->handle);
        camera->info.connected = false;
        camera->featureXml.clear();
    }
}

std::string Framegrabber::getCameraFeatureXml(const CameraTransport transport,
                                              const unsigned int dmaIndex) const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    const CameraEntry* camera = findCamera(transport, dmaIndex);
    if (!camera || !camera->handle || !camera->info.connected)
    {
        return {};
    }

    return camera->featureXml;
}

bool Framegrabber::getCameraFeature(const CameraTransport transport,
                                    const unsigned int dmaIndex,
                                    const std::string& name,
                                    ParameterValue& value) const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    const CameraEntry* camera = findCamera(transport, dmaIndex);
    if (!camera || !camera->handle || !camera->info.connected)
    {
        return false;
    }

    return std::visit(
        [&](auto& current) -> bool
        {
            using T = std::decay_t<decltype(current)>;
            if constexpr (std::is_same_v<T, std::int32_t>
                          || std::is_same_v<T, std::int64_t>)
            {
                std::int64_t output = 0;
                if (Sgc_getIntegerValue(camera->handle, name.c_str(), &output) != SGC_OK)
                {
                    unsigned int booleanValue = 0;
                    if (Sgc_getBooleanValue(
                            camera->handle,
                            name.c_str(),
                            &booleanValue) != SGC_OK)
                    {
                        return false;
                    }
                    output = booleanValue;
                }
                current = static_cast<T>(output);
                return true;
            }
            else if constexpr (std::is_same_v<T, std::uint32_t>
                               || std::is_same_v<T, std::uint64_t>)
            {
                std::int64_t output = 0;
                if (Sgc_getIntegerValue(camera->handle, name.c_str(), &output) != SGC_OK)
                {
                    unsigned int booleanValue = 0;
                    if (Sgc_getBooleanValue(
                            camera->handle,
                            name.c_str(),
                            &booleanValue) != SGC_OK)
                    {
                        return false;
                    }
                    output = booleanValue;
                }
                current = static_cast<T>(output);
                return true;
            }
            else if constexpr (std::is_same_v<T, double>)
            {
                return Sgc_getFloatValue(camera->handle, name.c_str(), &current) == SGC_OK;
            }
            else
            {
                const char* output = nullptr;
                if (Sgc_getStringValue(camera->handle, name.c_str(), &output) != SGC_OK)
                {
                    if (Sgc_getEnumerationValueAsString(
                            camera->handle,
                            name.c_str(),
                            &output) != SGC_OK)
                    {
                        return false;
                    }
                }
                current = safeString(output);
                return true;
            }
        },
        value);
}

bool Framegrabber::setCameraFeature(const CameraTransport transport,
                                    const unsigned int dmaIndex,
                                    const std::string& name,
                                    const ParameterValue& value,
                                    const bool verifyReadBack)
{
    bool success = false;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        CameraEntry* camera = findCamera(transport, dmaIndex);
        if (!camera || !camera->handle || !camera->info.connected)
        {
            return false;
        }

        success = std::visit(
            [&](const auto& current) -> bool
            {
                using T = std::decay_t<decltype(current)>;
                if constexpr (std::is_integral_v<T>)
                {
                    if (Sgc_setIntegerValue(
                            camera->handle,
                            name.c_str(),
                            static_cast<std::int64_t>(current)) == SGC_OK)
                    {
                        return true;
                    }
                    return Sgc_setBooleanValue(
                               camera->handle,
                               name.c_str(),
                               current != 0 ? 1u : 0u)
                           == SGC_OK;
                }
                else if constexpr (std::is_same_v<T, double>)
                {
                    return Sgc_setFloatValue(camera->handle, name.c_str(), current) == SGC_OK;
                }
                else
                {
                    if (Sgc_setStringValue(
                            camera->handle,
                            name.c_str(),
                            current.c_str()) == SGC_OK)
                    {
                        return true;
                    }
                    return Sgc_setEnumerationValue(
                               camera->handle,
                               name.c_str(),
                               current.c_str())
                           == SGC_OK;
                }
            },
            value);
    }

    if (success && verifyReadBack)
    {
        ParameterValue actual = value;
        if (!getCameraFeature(transport, dmaIndex, name, actual))
        {
            log(
                "Camera feature " + name
                    + " was written but read-back verification failed.",
                true);
            return false;
        }
        if (!parameterValuesMatch(value, actual))
        {
            log(
                "Camera feature " + name
                    + " read-back value does not match the requested value.",
                true);
            return false;
        }
    }
    if (success)
    {
        notifyNode(FeatureSource::Camera, transport, dmaIndex, name);
    }
    return success;
}

bool Framegrabber::executeCameraCommand(const CameraTransport transport,
                                        const unsigned int dmaIndex,
                                        const std::string& name)
{
    bool success = false;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        CameraEntry* camera = findCamera(transport, dmaIndex);
        success = camera
                  && camera->handle
                  && camera->info.connected
                  && Sgc_executeCommand(camera->handle, name.c_str()) == SGC_OK;
    }
    if (success)
    {
        notifyNode(FeatureSource::Camera, transport, dmaIndex, name);
    }
    return success;
}

Framegrabber::CameraEntry* Framegrabber::findCamera(const CameraTransport transport,
                                                    const unsigned int dmaIndex)
{
    if (transport != CameraTransport::CoaXPress)
    {
        return nullptr;
    }
    const auto it = std::find_if(
        _cxpCameras.begin(),
        _cxpCameras.end(),
        [dmaIndex](const CameraEntry& camera)
        {
            return camera.info.dmaIndex == dmaIndex;
        });
    return it == _cxpCameras.end() ? nullptr : &*it;
}

const Framegrabber::CameraEntry*
Framegrabber::findCamera(const CameraTransport transport,
                         const unsigned int dmaIndex) const
{
    if (transport != CameraTransport::CoaXPress)
    {
        return nullptr;
    }
    const auto it = std::find_if(
        _cxpCameras.begin(),
        _cxpCameras.end(),
        [dmaIndex](const CameraEntry& camera)
        {
            return camera.info.dmaIndex == dmaIndex;
        });
    return it == _cxpCameras.end() ? nullptr : &*it;
}

Framegrabber::PixelFormat Framegrabber::toPixelFormat(const int sdkFormat)
{
    switch (sdkFormat)
    {
    case FG_GRAY:
    case FG_GRAY_PLUS_PICNR:
        return PixelFormat::Mono8;
    case FG_GRAY10:
        return PixelFormat::Mono10Packed;
    case FG_GRAY12:
        return PixelFormat::Mono12Packed;
    case FG_GRAY14:
        return PixelFormat::Mono14Packed;
    case FG_GRAY16:
    case FG_GRAY16_PLUS_PICNR:
        return PixelFormat::Mono16;
    case FG_GRAY32:
        return PixelFormat::Mono32;
    case FG_BINARY:
        return PixelFormat::Binary;
    case FG_COL24:
        return PixelFormat::BGR24;
    case FG_COL32:
        return PixelFormat::RGBA32;
    case FG_COL48:
        return PixelFormat::BGR48;
    case FG_RGB8:
        return PixelFormat::RGB24;
    case FG_RGB10:
        return PixelFormat::RGB10Packed;
    case FG_RGB12:
        return PixelFormat::RGB12Packed;
    case FG_RGB14:
        return PixelFormat::RGB14Packed;
    case FG_RGB16:
        return PixelFormat::RGB48;
    case FG_RGBA8:
        return PixelFormat::RGBA32;
    case FG_RGBA10:
        return PixelFormat::RGBA10Packed;
    case FG_RGBA12:
        return PixelFormat::RGBA12Packed;
    case FG_RGBA14:
        return PixelFormat::RGBA14Packed;
    case FG_RGBA16:
        return PixelFormat::RGBA64;
    case FG_BGRA8:
        return PixelFormat::BGRA32;
    case FG_BGRA10:
        return PixelFormat::BGRA10Packed;
    case FG_BGRA12:
        return PixelFormat::BGRA12Packed;
    case FG_BGRA14:
        return PixelFormat::BGRA14Packed;
    case FG_BGRA16:
        return PixelFormat::BGRA64;
    case FG_COL30:
        return PixelFormat::BGR10Packed;
    case FG_COL36:
        return PixelFormat::BGR12Packed;
    case FG_COL42:
        return PixelFormat::BGR14Packed;
    case FG_RGBX32:
        return PixelFormat::RGBX32;
    case FG_RGBX40:
        return PixelFormat::RGBX10Packed;
    case FG_RGBX48:
        return PixelFormat::RGBX12Packed;
    case FG_RGBX56:
        return PixelFormat::RGBX14Packed;
    case FG_RGBX64:
        return PixelFormat::RGBX64;
    case YUV422_8:
    case FG_YUV422_8:
        return PixelFormat::YCbCr422_8;
    case FG_YCBCR422_8:
        return PixelFormat::YCbCr422_8_CbYCrY;
    case FG_BAYERGR8:
        return PixelFormat::BayerGR8;
    case FG_BAYERGR10:
        return PixelFormat::BayerGR10;
    case FG_BAYERGR12:
        return PixelFormat::BayerGR12;
    case FG_BAYERGR14:
        return PixelFormat::BayerGR14;
    case FG_BAYERGR16:
        return PixelFormat::BayerGR16;
    case FG_BAYERRG8:
        return PixelFormat::BayerRG8;
    case FG_BAYERRG10:
        return PixelFormat::BayerRG10;
    case FG_BAYERRG12:
        return PixelFormat::BayerRG12;
    case FG_BAYERRG14:
        return PixelFormat::BayerRG14;
    case FG_BAYERRG16:
        return PixelFormat::BayerRG16;
    case FG_BAYERGB8:
        return PixelFormat::BayerGB8;
    case FG_BAYERGB10:
        return PixelFormat::BayerGB10;
    case FG_BAYERGB12:
        return PixelFormat::BayerGB12;
    case FG_BAYERGB14:
        return PixelFormat::BayerGB14;
    case FG_BAYERGB16:
        return PixelFormat::BayerGB16;
    case FG_BAYERBG8:
        return PixelFormat::BayerBG8;
    case FG_BAYERBG10:
        return PixelFormat::BayerBG10;
    case FG_BAYERBG12:
        return PixelFormat::BayerBG12;
    case FG_BAYERBG14:
        return PixelFormat::BayerBG14;
    case FG_BAYERBG16:
        return PixelFormat::BayerBG16;
    case FG_BICOLOR_RGBG8:
        return PixelFormat::BiColorRGBG8;
    case FG_BICOLOR_RGBG10:
        return PixelFormat::BiColorRGBG10;
    case FG_BICOLOR_RGBG12:
        return PixelFormat::BiColorRGBG12;
    case FG_BICOLOR_GRGB8:
        return PixelFormat::BiColorGRGB8;
    case FG_BICOLOR_GRGB10:
        return PixelFormat::BiColorGRGB10;
    case FG_BICOLOR_GRGB12:
        return PixelFormat::BiColorGRGB12;
    case FG_BICOLOR_BGRG8:
        return PixelFormat::BiColorBGRG8;
    case FG_BICOLOR_BGRG10:
        return PixelFormat::BiColorBGRG10;
    case FG_BICOLOR_BGRG12:
        return PixelFormat::BiColorBGRG12;
    case FG_BICOLOR_GBGR8:
        return PixelFormat::BiColorGBGR8;
    case FG_BICOLOR_GBGR10:
        return PixelFormat::BiColorGBGR10;
    case FG_BICOLOR_GBGR12:
        return PixelFormat::BiColorGBGR12;
    case FG_RAW:
        return PixelFormat::Raw;
    case FG_JPEG:
        return PixelFormat::Jpeg;
    default:
        return PixelFormat::Unknown;
    }
}

int Framegrabber::bitsPerPixel(const PixelFormat pixelFormat)
{
    switch (pixelFormat)
    {
    case PixelFormat::Binary:
        return 1;
    case PixelFormat::Mono8:
    case PixelFormat::BayerGR8:
    case PixelFormat::BayerRG8:
    case PixelFormat::BayerGB8:
    case PixelFormat::BayerBG8:
    case PixelFormat::BiColorRGBG8:
    case PixelFormat::BiColorGRGB8:
    case PixelFormat::BiColorBGRG8:
    case PixelFormat::BiColorGBGR8:
        return 8;
    case PixelFormat::Mono10Packed:
    case PixelFormat::BayerGR10:
    case PixelFormat::BayerRG10:
    case PixelFormat::BayerGB10:
    case PixelFormat::BayerBG10:
    case PixelFormat::BiColorRGBG10:
    case PixelFormat::BiColorGRGB10:
    case PixelFormat::BiColorBGRG10:
    case PixelFormat::BiColorGBGR10:
        return 10;
    case PixelFormat::Mono12Packed:
    case PixelFormat::BayerGR12:
    case PixelFormat::BayerRG12:
    case PixelFormat::BayerGB12:
    case PixelFormat::BayerBG12:
    case PixelFormat::BiColorRGBG12:
    case PixelFormat::BiColorGRGB12:
    case PixelFormat::BiColorBGRG12:
    case PixelFormat::BiColorGBGR12:
        return 12;
    case PixelFormat::Mono14Packed:
    case PixelFormat::BayerGR14:
    case PixelFormat::BayerRG14:
    case PixelFormat::BayerGB14:
    case PixelFormat::BayerBG14:
        return 14;
    case PixelFormat::Mono16:
    case PixelFormat::BayerGR16:
    case PixelFormat::BayerRG16:
    case PixelFormat::BayerGB16:
    case PixelFormat::BayerBG16:
    case PixelFormat::YCbCr422_8:
    case PixelFormat::YCbCr422_8_CbYCrY:
        return 16;
    case PixelFormat::RGB24:
    case PixelFormat::BGR24:
        return 24;
    case PixelFormat::RGB30:
    case PixelFormat::RGB10Packed:
    case PixelFormat::BGR10Packed:
        return 30;
    case PixelFormat::Mono32:
    case PixelFormat::RGBA32:
    case PixelFormat::BGRA32:
    case PixelFormat::RGBX32:
        return 32;
    case PixelFormat::RGB12Packed:
    case PixelFormat::BGR12Packed:
        return 36;
    case PixelFormat::RGBA10Packed:
    case PixelFormat::BGRA10Packed:
    case PixelFormat::RGBX10Packed:
        return 40;
    case PixelFormat::RGB14Packed:
    case PixelFormat::BGR14Packed:
        return 42;
    case PixelFormat::RGB48:
    case PixelFormat::BGR48:
    case PixelFormat::RGBA12Packed:
    case PixelFormat::BGRA12Packed:
    case PixelFormat::RGBX12Packed:
        return 48;
    case PixelFormat::RGBA14Packed:
    case PixelFormat::BGRA14Packed:
    case PixelFormat::RGBX14Packed:
        return 56;
    case PixelFormat::RGBA64:
    case PixelFormat::BGRA64:
    case PixelFormat::RGBX64:
        return 64;
    case PixelFormat::Unknown:
    case PixelFormat::Raw:
    case PixelFormat::Jpeg:
        return 0;
    }
    return 0;
}

void Framegrabber::notifyStatus(const Status status, const bool on)
{
    if (status == GrabbingStatus)
    {
        clearAppletFeatureModels();
    }

    std::vector<StatusCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(_statusCallbackMutex);
        callbacks.reserve(_statusCallbacks.size());
        for (const auto& item : _statusCallbacks)
        {
            callbacks.push_back(item.second);
        }
    }
    for (const StatusCallback& callback : callbacks)
    {
        try
        {
            callback(status, on);
        }
        catch (const std::exception& exception)
        {
            log(std::string("Status callback failed: ") + exception.what(), true);
        }
        catch (...)
        {
            log("Status callback failed with an unknown exception.", true);
        }
    }
}

void Framegrabber::notifyNode(const FeatureSource source,
                              const CameraTransport transport,
                              const unsigned int dmaIndex,
                              const std::string& nodeName)
{
    std::vector<NodeCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(_nodeCallbackMutex);
        callbacks.reserve(_nodeCallbacks.size());
        for (const auto& item : _nodeCallbacks)
        {
            callbacks.push_back(item.second);
        }
    }
    for (const NodeCallback& callback : callbacks)
    {
        try
        {
            callback(source, transport, dmaIndex, nodeName);
        }
        catch (const std::exception& exception)
        {
            log(std::string("Node callback failed: ") + exception.what(), true);
        }
        catch (...)
        {
            log("Node callback failed with an unknown exception.", true);
        }
    }
}

void Framegrabber::log(const std::string& message, const bool warning) const
{
    FramegrabberSystem::syslog(message, warning);
}
