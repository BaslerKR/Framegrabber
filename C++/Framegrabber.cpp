#include "Framegrabber.h"

#include "FramegrabberSystem.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <limits>
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
}

struct Framegrabber::DmaChannel
{
    unsigned int index = 0;
    dma_mem* memory = nullptr;
    std::thread thread;
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
    return result.empty() ? "CXP Camera" : result;
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

    const std::string path = configurationPath();
    if (path.empty())
    {
        log("Select an applet or MCF configuration before opening the board.", true);
        return false;
    }

    close();
    return openResolvedBoard(board.displayName(), board.index, path);
}

bool Framegrabber::openResolvedBoard(const std::string& boardName,
                                     const unsigned int boardIndex,
                                     const std::string& configurationPath)
{
    Fg_Struct* handle = nullptr;
    if (lowerExtension(configurationPath) == ".mcf")
    {
        handle = Fg_InitConfig(configurationPath.c_str(), boardIndex);
    }
    else
    {
        handle = Fg_Init(configurationPath.c_str(), boardIndex);
    }

    if (!handle)
    {
        log("Failed to load configuration '" + configurationPath + "': "
                + safeString(Fg_getLastErrorDescription(nullptr)),
            true);
        return false;
    }

    SgcBoardHandle* cxpBoard = nullptr;
    if (Sgc_initBoard(handle, 0, &cxpBoard) != SGC_OK)
    {
        cxpBoard = nullptr;
        log("CXP control module is unavailable for this configuration.", true);
    }

    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        _handle = handle;
        _cxpBoard = cxpBoard;
        _boardIndex = boardIndex;
        _connectedBoardName = boardName;
        _configurationPath = configurationPath;
        _channels.clear();
        _cameras.clear();
    }

    if (cxpBoard)
    {
        refreshCameras();
    }

    notifyStatus(ConnectionStatus, true);
    log("Opened " + boardName + " with " + std::filesystem::path(configurationPath).filename().string() + ".");
    return true;
}

bool Framegrabber::isOpened() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _handle != nullptr;
}

void Framegrabber::close()
{
    stop();

    bool wasOpened = false;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        wasOpened = _handle != nullptr;
        releaseHandles();
        _connectedBoardName.clear();
        _channels.clear();
        _cameras.clear();
    }

    if (wasOpened)
    {
        notifyStatus(ConnectionStatus, false);
    }
}

void Framegrabber::releaseHandles()
{
    for (CameraEntry& camera : _cameras)
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

bool Framegrabber::loadConfiguration(const std::string& path)
{
    if (path.empty() || !std::filesystem::is_regular_file(path))
    {
        return false;
    }

    std::string previousPath;
    std::string boardName;
    bool reopen = false;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        previousPath = _configurationPath;
        boardName = _connectedBoardName;
        reopen = _handle != nullptr;
        _configurationPath = path;
    }

    if (!reopen)
    {
        return true;
    }

    close();
    if (open(boardName))
    {
        return true;
    }

    log("Restoring the previous frame grabber configuration.", true);
    setConfigurationPath(previousPath);
    if (!previousPath.empty())
    {
        open(boardName);
    }
    return false;
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
    if (!isOpened() || _isRunning.exchange(true, std::memory_order_acq_rel))
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
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        _channels[dmaIndex] = std::move(channel);
    }

    _activeChannels.fetch_add(1, std::memory_order_acq_rel);
    channelPtr->thread = std::thread(
        [this,
         channelPtr,
         handle,
         frames,
         width,
         height,
         sdkFormat,
         pixelBytes,
         frameBytes,
         rowBytes]
        {
            const unsigned int dmaIndex = channelPtr->index;
            const int acquireResult = Fg_AcquireEx(
                handle,
                static_cast<int>(dmaIndex),
                GRAB_INFINITE,
                ACQ_STANDARD,
                channelPtr->memory);
            if (acquireResult != FG_OK)
            {
                log("DMA(" + std::to_string(dmaIndex) + ") acquisition start failed: "
                        + safeString(Fg_getErrorDescription(nullptr, acquireResult)),
                    true);
                finishChannel(*channelPtr);
                return;
            }
            channelPtr->acquisitionStarted.store(true, std::memory_order_release);

            SgcCameraHandle* cameraHandle = nullptr;
            if (connectCamera(dmaIndex))
            {
                std::lock_guard<std::mutex> lock(_stateMutex);
                if (CameraEntry* camera = findCamera(dmaIndex))
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
                            return channelPtr->stopRequested.load(std::memory_order_acquire)
                                   || channelPtr->permits.load(std::memory_order_acquire) > 0;
                        });
                }
                if (channelPtr->stopRequested.load(std::memory_order_acquire))
                {
                    break;
                }
                channelPtr->permits.fetch_sub(1, std::memory_order_acq_rel);

                const frameindex_t picture = Fg_getLastPicNumberBlockingEx(
                    handle,
                    lastPicture + 1,
                    static_cast<int>(dmaIndex),
                    100,
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
                const auto* source = static_cast<const std::uint8_t*>(
                    Fg_getImagePtrEx(
                        handle,
                        picture,
                        static_cast<int>(dmaIndex),
                        channelPtr->memory));
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

void Framegrabber::finishChannel(DmaChannel& channel)
{
    Fg_Struct* handle = nullptr;
    SgcCameraHandle* cameraHandle = nullptr;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        handle = _handle;
        if (CameraEntry* camera = findCamera(channel.index))
        {
            cameraHandle = camera->handle;
        }
    }

    if (cameraHandle && channel.cameraStarted.exchange(false, std::memory_order_acq_rel))
    {
        Sgc_stopAcquisition(cameraHandle, 1);
    }
    if (handle && channel.acquisitionStarted.exchange(false, std::memory_order_acq_rel))
    {
        Fg_stopAcquireEx(handle, static_cast<int>(channel.index), channel.memory, 0);
    }
    if (handle && channel.memory)
    {
        Fg_FreeMemEx(handle, channel.memory);
        channel.memory = nullptr;
    }

    const unsigned int previous = _activeChannels.fetch_sub(1, std::memory_order_acq_rel);
    if (previous == 1)
    {
        _isRunning.store(false, std::memory_order_release);
        notifyStatus(GrabbingStatus, false);
    }
}

void Framegrabber::requestStop()
{
    _isRunning.store(false, std::memory_order_release);

    struct StopTarget
    {
        unsigned int index = 0;
        dma_mem* memory = nullptr;
        bool acquisitionStarted = false;
    };
    std::vector<StopTarget> targets;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        targets.reserve(_channels.size());
        for (const auto& channel : _channels)
        {
            if (!channel)
            {
                continue;
            }
            channel->stopRequested.store(true, std::memory_order_release);
            channel->permitCondition.notify_all();
            targets.push_back({
                channel->index,
                channel->memory,
                channel->acquisitionStarted.load(std::memory_order_acquire)});
        }
    }

    Fg_Struct* handle = nullptr;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        handle = _handle;
    }
    if (!handle)
    {
        return;
    }
    for (const StopTarget& target : targets)
    {
        if (target.memory && target.acquisitionStarted)
        {
            Fg_stopAcquireEx(
                handle,
                static_cast<int>(target.index),
                target.memory,
                0);
        }
    }
}

void Framegrabber::stop()
{
    requestStop();

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

    for (std::thread* thread : threads)
    {
        if (thread->get_id() != std::this_thread::get_id())
        {
            thread->join();
        }
    }
}

void Framegrabber::ready(const unsigned int dmaIndex)
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    if (dmaIndex >= _channels.size() || !_channels[dmaIndex])
    {
        return;
    }
    DmaChannel& channel = *_channels[dmaIndex];
    channel.permits.fetch_add(1, std::memory_order_acq_rel);
    channel.permitCondition.notify_one();
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

bool Framegrabber::getAppletParameter(const std::string& name,
                                      const unsigned int dmaIndex,
                                      ParameterValue& value,
                                      const bool silent) const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    if (!_handle || name.empty())
    {
        return false;
    }

    int parameterId = Fg_getParameterIdByName(_handle, name.c_str());
    if (parameterId < 0)
    {
        if (!silent)
        {
            log("Unknown applet parameter '" + name + "'.", true);
        }
        return false;
    }

    const FgParamTypes type = Fg_getParameterTypeById(
        _handle,
        parameterId,
        static_cast<int>(dmaIndex));
    int result = FG_ERROR;
    switch (type)
    {
    case FG_PARAM_TYPE_INT32_T:
    {
        std::int32_t output = 0;
        result = Fg_getParameterWithType(_handle, parameterId, &output, dmaIndex, type);
        value = output;
        break;
    }
    case FG_PARAM_TYPE_UINT32_T:
    {
        std::uint32_t output = 0;
        result = Fg_getParameterWithType(_handle, parameterId, &output, dmaIndex, type);
        value = output;
        break;
    }
    case FG_PARAM_TYPE_INT64_T:
    {
        std::int64_t output = 0;
        result = Fg_getParameterWithType(_handle, parameterId, &output, dmaIndex, type);
        value = output;
        break;
    }
    case FG_PARAM_TYPE_UINT64_T:
    {
        std::uint64_t output = 0;
        result = Fg_getParameterWithType(_handle, parameterId, &output, dmaIndex, type);
        value = output;
        break;
    }
    case FG_PARAM_TYPE_SIZE_T:
    {
        std::size_t output = 0;
        result = Fg_getParameterWithType(_handle, parameterId, &output, dmaIndex, type);
        value = static_cast<std::uint64_t>(output);
        break;
    }
    case FG_PARAM_TYPE_DOUBLE:
    {
        double output = 0.0;
        result = Fg_getParameterWithType(_handle, parameterId, &output, dmaIndex, type);
        value = output;
        break;
    }
    case FG_PARAM_TYPE_CHAR_PTR:
    {
        std::vector<char> output(4096, '\0');
        result = Fg_getParameterWithType(_handle, parameterId, output.data(), dmaIndex, type);
        value = std::string(output.data());
        break;
    }
    default:
        return false;
    }

    if (result != FG_OK && !silent)
    {
        log("Failed to read applet parameter '" + name + "'.", true);
    }
    return result == FG_OK;
}

bool Framegrabber::setAppletParameter(const std::string& name,
                                      const unsigned int dmaIndex,
                                      const ParameterValue& value,
                                      const bool silent)
{
    int result = FG_ERROR;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        if (!_handle || name.empty())
        {
            return false;
        }

        const int parameterId = Fg_getParameterIdByName(_handle, name.c_str());
        if (parameterId < 0)
        {
            return false;
        }
        const FgParamTypes type = Fg_getParameterTypeById(
            _handle,
            parameterId,
            static_cast<int>(dmaIndex));

        try
        {
            switch (type)
            {
            case FG_PARAM_TYPE_INT32_T:
            {
                const auto typed = std::get<std::int32_t>(value);
                result = Fg_setParameterWithType(_handle, parameterId, &typed, dmaIndex, type);
                break;
            }
            case FG_PARAM_TYPE_UINT32_T:
            {
                const auto typed = std::get<std::uint32_t>(value);
                result = Fg_setParameterWithType(_handle, parameterId, &typed, dmaIndex, type);
                break;
            }
            case FG_PARAM_TYPE_INT64_T:
            {
                const auto typed = std::get<std::int64_t>(value);
                result = Fg_setParameterWithType(_handle, parameterId, &typed, dmaIndex, type);
                break;
            }
            case FG_PARAM_TYPE_UINT64_T:
            {
                const auto typed = std::get<std::uint64_t>(value);
                result = Fg_setParameterWithType(_handle, parameterId, &typed, dmaIndex, type);
                break;
            }
            case FG_PARAM_TYPE_SIZE_T:
            {
                const auto typed = static_cast<std::size_t>(std::get<std::uint64_t>(value));
                result = Fg_setParameterWithType(_handle, parameterId, &typed, dmaIndex, type);
                break;
            }
            case FG_PARAM_TYPE_DOUBLE:
            {
                const auto typed = std::get<double>(value);
                result = Fg_setParameterWithType(_handle, parameterId, &typed, dmaIndex, type);
                break;
            }
            case FG_PARAM_TYPE_CHAR_PTR:
            {
                const auto& typed = std::get<std::string>(value);
                result = Fg_setParameterWithType(
                    _handle,
                    parameterId,
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
                log("Type mismatch for applet parameter '" + name + "'.", true);
            }
            return false;
        }
    }

    if (result == FG_OK)
    {
        notifyNode(FeatureSource::Applet, dmaIndex, name);
        return true;
    }
    if (!silent)
    {
        log("Failed to set applet parameter '" + name + "'.", true);
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
    return setAppletParameter(name, dmaIndex, current);
}

bool Framegrabber::refreshCameras()
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    if (!_cxpBoard || _isRunning.load(std::memory_order_acquire))
    {
        return false;
    }

    for (CameraEntry& camera : _cameras)
    {
        if (camera.handle && camera.info.connected)
        {
            Sgc_disconnectCamera(camera.handle);
        }
    }
    _cameras.clear();

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

    _cameras = std::move(discovered);
    return true;
}

std::vector<Framegrabber::CameraInfo> Framegrabber::getCachedCameraList() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    std::vector<CameraInfo> cameras;
    cameras.reserve(_cameras.size());
    for (const CameraEntry& entry : _cameras)
    {
        cameras.push_back(entry.info);
    }
    return cameras;
}

bool Framegrabber::connectCamera(const unsigned int dmaIndex)
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    CameraEntry* camera = findCamera(dmaIndex);
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

void Framegrabber::disconnectCamera(const unsigned int dmaIndex)
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    CameraEntry* camera = findCamera(dmaIndex);
    if (camera && camera->handle && camera->info.connected)
    {
        Sgc_disconnectCamera(camera->handle);
        camera->info.connected = false;
    }
}

std::string Framegrabber::getCameraFeatureXml(const unsigned int dmaIndex) const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    const CameraEntry* camera = findCamera(dmaIndex);
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

bool Framegrabber::getCameraFeature(const FeatureSource source,
                                    const unsigned int dmaIndex,
                                    const std::string& name,
                                    ParameterValue& value) const
{
    if (source != FeatureSource::Camera)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(_stateMutex);
    const CameraEntry* camera = findCamera(dmaIndex);
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

bool Framegrabber::setCameraFeature(const FeatureSource source,
                                    const unsigned int dmaIndex,
                                    const std::string& name,
                                    const ParameterValue& value)
{
    if (source != FeatureSource::Camera)
    {
        return false;
    }

    bool success = false;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        CameraEntry* camera = findCamera(dmaIndex);
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
        notifyNode(FeatureSource::Camera, dmaIndex, name);
    }
    return success;
}

bool Framegrabber::executeCameraCommand(const unsigned int dmaIndex, const std::string& name)
{
    bool success = false;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        CameraEntry* camera = findCamera(dmaIndex);
        success = camera
                  && camera->handle
                  && camera->info.connected
                  && Sgc_executeCommand(camera->handle, name.c_str()) == SGC_OK;
    }
    if (success)
    {
        notifyNode(FeatureSource::Camera, dmaIndex, name);
    }
    return success;
}

Framegrabber::CameraEntry* Framegrabber::findCamera(const unsigned int dmaIndex)
{
    const auto it = std::find_if(
        _cameras.begin(),
        _cameras.end(),
        [dmaIndex](const CameraEntry& camera)
        {
            return camera.info.dmaIndex == dmaIndex;
        });
    return it == _cameras.end() ? nullptr : &*it;
}

const Framegrabber::CameraEntry* Framegrabber::findCamera(const unsigned int dmaIndex) const
{
    const auto it = std::find_if(
        _cameras.begin(),
        _cameras.end(),
        [dmaIndex](const CameraEntry& camera)
        {
            return camera.info.dmaIndex == dmaIndex;
        });
    return it == _cameras.end() ? nullptr : &*it;
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
        callback(status, on);
    }
}

void Framegrabber::notifyNode(const FeatureSource source,
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
        callback(source, dmaIndex, nodeName);
    }
}

void Framegrabber::log(const std::string& message, const bool warning) const
{
    FramegrabberSystem::syslog(message, warning);
}
