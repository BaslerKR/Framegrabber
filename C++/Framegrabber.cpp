#include "Framegrabber.h"

#include "FramegrabberSystem.h"

#include <GenApi/GenApi.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <unordered_set>
#include <utility>

namespace
{
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

std::string safeString(const char* value)
{
    return value ? value : "";
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
}

struct Framegrabber::DmaChannel
{
    unsigned int index = 0;
    dma_mem* memory = nullptr;
    std::thread thread;
    std::mutex resourceMutex;
    std::mutex permitMutex;
    std::condition_variable permitCondition;
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> acquisitionStarted{false};
    std::atomic<bool> cameraStarted{false};
    std::atomic<std::size_t> permits{0};
};

struct Framegrabber::CameraEntry
{
    CameraInfo info;
    SgcCameraHandle* handle = nullptr;
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
    if (path.empty() || !std::filesystem::is_regular_file(path))
    {
        return false;
    }

    std::string previousPath;
    std::string previousConfigurationPath;
    std::string previousBoardName;
    bool wasOpened = false;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        previousPath = _appletPath;
        previousConfigurationPath = _configurationPath;
        previousBoardName = _connectedBoardName;
        wasOpened = _handle != nullptr;
        _appletPath = path;
    }

    if (open(boardName))
    {
        return true;
    }

    log("Restoring the previous frame grabber applet.", true);
    setAppletPath(previousPath);
    if (wasOpened && !previousPath.empty())
    {
        if (open(previousBoardName) && !previousConfigurationPath.empty())
        {
            loadConfiguration(previousConfigurationPath);
        }
    }
    else
    {
        setConfigurationPath(previousConfigurationPath);
    }
    return false;
}

bool Framegrabber::open(const std::string& boardName)
{
    if (!_system || !_system->isInitialized())
    {
        log("Frame grabber system is not initialized.", true);
        return false;
    }

    FramegrabberSystem::BoardInfo board;
    if (!_system->resolveBoard(boardName, board))
    {
        log("No matching frame grabber board was found.", true);
        return false;
    }

    const std::string path = appletPath();
    if (path.empty())
    {
        log("Select an applet before opening the board.", true);
        return false;
    }

    const FramegrabberSystem::AppletMetadata appletMetadata =
        _system->getAppletMetadata(board.displayName(), path);
    close();
    return openResolvedBoard(
        board.displayName(),
        board.index,
        path,
        appletMetadata.version,
        appletMetadata.supportsCameraControl);
}

bool Framegrabber::openResolvedBoard(const std::string& boardName,
                                     const unsigned int boardIndex,
                                     const std::string& appletPath,
                                     const std::string& discoveredAppletVersion,
                                     const bool initializeCameraControl)
{
    Fg_Struct* handle = Fg_Init(appletPath.c_str(), boardIndex);
    if (!handle)
    {
        log("Failed to load applet '" + appletPath + "': "
                + safeString(Fg_getLastErrorDescription(nullptr)),
            true);
        return false;
    }

    SgcBoardHandle* cxpBoard = nullptr;
    if (initializeCameraControl
        && Sgc_initBoard(handle, 0, &cxpBoard) != SGC_OK)
    {
        cxpBoard = nullptr;
        log("CXP control module is unavailable for this configuration.", true);
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
    const std::string appletName =
        std::filesystem::path(appletFileName).stem().string();
    const int appletId = Fg_getAppletId(handle, nullptr);
    std::string appletVersion =
        appletId >= 0 ? safeString(Fg_getAppletVersion(handle, appletId)) : "";
    if (appletVersion.empty())
    {
        appletVersion = discoveredAppletVersion;
    }

    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        _handle = handle;
        _cxpBoard = cxpBoard;
        _boardIndex = boardIndex;
        _connectedBoardName = boardName;
        _appletPath = appletPath;
        _loadedAppletName = appletName;
        _loadedAppletVersion = appletVersion;
        _configurationPath.clear();
        _channels.clear();
        _cxpCameras.clear();
    }
    clearAppletFeatureModels();

    if (cxpBoard)
    {
        refreshCameras(CameraTransport::CoaXPress);
    }

    notifyStatus(ConnectionStatus, true);
    log("Opened " + boardName + " with "
        + std::filesystem::path(appletPath).filename().string() + ".");
    return true;
}

bool Framegrabber::isOpened() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _handle != nullptr;
}

void Framegrabber::close()
{
    std::lock_guard<std::mutex> lifecycleLock(_lifecycleMutex);
    requestStopChannels();
    if (!joinStoppedChannels())
    {
        return;
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

    if (wasOpened)
    {
        notifyStatus(ConnectionStatus, false);
    }
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
    if (path.empty()
        || lowerExtension(path) != ".mcf"
        || !std::filesystem::is_regular_file(path))
    {
        return false;
    }

    int result = FG_NOT_INIT;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        if (!_handle || _isRunning.load(std::memory_order_acquire))
        {
            return false;
        }
        result = Fg_loadConfig(_handle, path.c_str());
        if (result == FG_OK)
        {
            _configurationPath = path;
        }
    }
    if (result != FG_OK)
    {
        log("Failed to load configuration '" + path + "': "
                + safeString(Fg_getErrorDescription(nullptr, result)),
            true);
        return false;
    }
    return true;
}

bool Framegrabber::saveConfiguration(const std::string& path) const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    if (!_handle || path.empty())
    {
        return false;
    }
    return Fg_saveConfig(_handle, path.c_str()) == FG_OK;
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

    const int pixelBytes = bytesPerPixel(sdkFormat);
    if (width <= 0 || height <= 0 || pixelBytes <= 0)
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
    channel->permits.store(1, std::memory_order_release);
    DmaChannel* channelPtr = channel.get();
    std::unique_lock<std::mutex> registrationLock(_stateMutex);
    _channels[dmaIndex] = std::move(channel);
    _activeChannels.fetch_add(1, std::memory_order_acq_rel);
    try
    {
        channelPtr->thread = std::thread(
            [this, channelPtr, handle, frames, width, height, sdkFormat, pixelBytes, frameBytes,
             rowBytes]
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

                SgcCameraHandle* cameraHandle = nullptr;
                if (connectCamera(CameraTransport::CoaXPress, dmaIndex))
                {
                    std::lock_guard<std::mutex> lock(_stateMutex);
                    if (CameraEntry* camera = findCamera(CameraTransport::CoaXPress, dmaIndex))
                    {
                        cameraHandle = camera->handle;
                    }
                }
                if (cameraHandle && Sgc_startAcquisition(cameraHandle, 1) == SGC_OK)
                {
                    channelPtr->cameraStarted.store(true, std::memory_order_release);
                }

                frameindex_t lastPicture = 0;
                std::size_t delivered = 0;
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

                    const frameindex_t picture = Fg_getLastPicNumberBlockingEx(
                        handle, lastPicture + 1, static_cast<int>(dmaIndex), 100,
                        channelPtr->memory);
                    if (picture <= 0)
                    {
                        if (picture == FG_NO_PICTURE_AVAILABLE)
                        {
                            channelPtr->permits.fetch_add(1, std::memory_order_acq_rel);
                            continue;
                        }
                        if (!channelPtr->stopRequested.load(std::memory_order_acquire))
                        {
                            log("DMA(" + std::to_string(dmaIndex) + ") frame wait failed.", true);
                            channelPtr->permits.fetch_add(1, std::memory_order_acq_rel);
                        }
                        continue;
                    }

                    lastPicture = picture;
                    const auto* source = static_cast<const std::uint8_t*>(Fg_getImagePtrEx(
                        handle, picture, static_cast<int>(dmaIndex), channelPtr->memory));
                    if (!source)
                    {
                        ready(dmaIndex);
                        continue;
                    }

                    auto ownedBytes = std::make_shared<std::vector<std::uint8_t>>(frameBytes);
                    std::memcpy(ownedBytes->data(), source, frameBytes);

                    Image image;
                    image.storage = std::move(ownedBytes);
                    image.size = frameBytes;
                    image.width = width;
                    image.height = height;
                    image.stride = static_cast<int>(rowBytes);
                    image.bytesPerPixel = pixelBytes;
                    image.sdkPixelFormat = sdkFormat;
                    image.pixelFormat = toPixelFormat(sdkFormat);
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
    SgcCameraHandle* cameraHandle = nullptr;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        handle = _handle;
        if (CameraEntry* camera = findCamera(CameraTransport::CoaXPress, channel.index))
        {
            cameraHandle = camera->handle;
        }
    }

    if (cameraHandle && channel.cameraStarted.exchange(false, std::memory_order_acq_rel))
    {
        Sgc_stopAcquisition(cameraHandle, 1);
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
            channels.push_back(channel.get());
        }
    }

    if (!handle)
    {
        return;
    }
    for (DmaChannel* channel : channels)
    {
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
    int count = 0;
    if (Fg_getParameterPropertyWithType(_handle, FG_NR_OF_DMAS, PROP_ID_VALUE, &count) != FG_OK)
    {
        return 0;
    }
    return count;
}

std::vector<std::string> Framegrabber::getUpdatedFramegrabberList() const
{
    return _system ? _system->getFramegrabberList() : std::vector<std::string>{};
}

std::vector<std::string> Framegrabber::getCachedFramegrabberList() const
{
    return _system ? _system->getCachedFramegrabberList() : std::vector<std::string>{};
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
        _dmaBufferCount = std::max<std::size_t>(1, count);
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
        std::vector<char> output(4096, '\0');
        result = Fg_getParameterWithType(
            _handle,
            sdkParameterId,
            output.data(),
            dmaIndex,
            type);
        value = std::string(output.data());
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
                                          const bool verifyReadBack)
{
    if (parameterId < 0 || parameterId > (std::numeric_limits<int>::max)())
    {
        return false;
    }
    int result = FG_ERROR;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        if (!_handle)
        {
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
                return false;
            }
        }
        catch (const std::bad_variant_access&)
        {
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
        clearAppletFeatureModel(dmaIndex);
        notifyNode(
            FeatureSource::Applet,
            CameraTransport::None,
            dmaIndex,
            std::to_string(parameterId));
        if (verifyReadBack)
        {
            ParameterValue actual = value;
            if (!getAppletParameterById(
                    parameterId,
                    dmaIndex,
                    actual,
                    true))
            {
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
    ParameterValue current = std::int32_t{0};
    if (!getAppletParameter(name, dmaIndex, current, true))
    {
        return false;
    }
    std::visit(
        [](auto& value)
        {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_integral_v<T>)
            {
                value = static_cast<T>(1);
            }
        },
        current);
    return setAppletParameter(name, dmaIndex, current, false, false);
}

bool Framegrabber::executeAppletCommandById(
    const unsigned int dmaIndex,
    const std::int64_t parameterId)
{
    ParameterValue current = std::int32_t{0};
    if (!getAppletParameterById(parameterId, dmaIndex, current, true))
    {
        return false;
    }
    std::visit(
        [](auto& value)
        {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_integral_v<T>)
            {
                value = static_cast<T>(1);
            }
        },
        current);
    return setAppletParameterById(parameterId, dmaIndex, current, false, false);
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
    std::lock_guard<std::mutex> lock(_stateMutex);
    if (transport != CameraTransport::CoaXPress
        || !_cxpBoard
        || _isRunning.load(std::memory_order_acquire))
    {
        return false;
    }

    for (CameraEntry& camera : _cxpCameras)
    {
        if (camera.handle && camera.info.connected)
        {
            Sgc_disconnectCamera(camera.handle);
        }
    }
    _cxpCameras.clear();

    if (Sgc_scanPorts(_cxpBoard, 0xFF, 10000, LINK_SPEED_NONE) != SGC_OK)
    {
        log("CXP camera discovery failed.", true);
        return false;
    }

    std::vector<CameraEntry> discovered;
    const int count = Sgc_getCameraCount(_cxpBoard);
    discovered.reserve(static_cast<std::size_t>((std::max)(0, count)));
    for (int index = 0; index < count; ++index)
    {
        SgcCameraHandle* handle = nullptr;
        if (Sgc_getCameraByIndex(_cxpBoard, static_cast<unsigned int>(index), &handle) != SGC_OK
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
        Sgc_getCameraPropertyWithType(
            handle,
            CAM_PROP_APPLETOPERATORINDEX,
            &dmaIndex,
            &propertyType,
            nullptr);
        entry.info.dmaIndex = dmaIndex;
        discovered.push_back(std::move(entry));
    }

    _cxpCameras = std::move(discovered);
    return true;
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

    std::size_t size = 0;
    Sgc_getGenICamXML(camera->handle, nullptr, &size);
    if (size == 0)
    {
        return {};
    }
    std::vector<char> buffer(size + 1, '\0');
    if (Sgc_getGenICamXML(camera->handle, buffer.data(), &size) != SGC_OK)
    {
        return {};
    }
    return std::string(buffer.data());
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
                                    const ParameterValue& value)
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
        return PixelFormat::Mono8;
    case FG_GRAY16:
        return PixelFormat::Mono16;
    case FG_COL24:
        return PixelFormat::RGB24;
    case FG_COL32:
        return PixelFormat::RGBA32;
    case FG_COL30:
        return PixelFormat::RGB30;
    case FG_COL48:
        return PixelFormat::RGB48;
    default:
        return PixelFormat::Unknown;
    }
}

int Framegrabber::bytesPerPixel(const int sdkFormat)
{
    switch (sdkFormat)
    {
    case FG_GRAY:
        return 1;
    case FG_GRAY16:
        return 2;
    case FG_COL24:
        return 3;
    case FG_COL32:
        return 4;
    case FG_COL30:
        return 5;
    case FG_COL48:
        return 6;
    default:
        return 0;
    }
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
