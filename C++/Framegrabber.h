#ifndef FRAMEGRABBER_H
#define FRAMEGRABBER_H

/**
 * @file Framegrabber.h
 * @brief Wrapper for one Basler frame grabber board, acquisition channels, and CXP cameras.
 *
 * The module owns every SDK handle and DMA buffer. Consumers receive an owning
 * Image value and never need to include or retain a raw SDK image pointer.
 */

#include "basler_fg.h"
#include "siso_genicam.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

class FramegrabberSystem;

class Framegrabber
{
    friend class FramegrabberSystem;

public:
    using CallbackId = std::size_t;

    enum Status
    {
        GrabbingStatus,
        ConnectionStatus
    };

    enum class PixelFormat
    {
        Unknown,
        Mono8,
        Mono16,
        RGB24,
        RGBA32,
        RGB30,
        RGB48
    };

    struct Image
    {
        std::shared_ptr<const std::vector<std::uint8_t>> storage;
        std::size_t size = 0;
        int width = 0;
        int height = 0;
        int stride = 0;
        int bytesPerPixel = 0;
        int sdkPixelFormat = 0;
        PixelFormat pixelFormat = PixelFormat::Unknown;
        unsigned int dmaIndex = 0;
        std::uint64_t frameSeq = 0;

        [[nodiscard]] const std::uint8_t* data() const noexcept
        {
            return storage && !storage->empty() ? storage->data() : nullptr;
        }

        [[nodiscard]] bool isValid() const noexcept
        {
            return data() != nullptr && width > 0 && height > 0 && stride > 0;
        }
    };

    enum class FeatureSource
    {
        Applet,
        Camera
    };

    using ParameterValue = std::variant<std::int32_t,
                                        std::uint32_t,
                                        std::int64_t,
                                        std::uint64_t,
                                        double,
                                        std::string>;

    struct CameraInfo
    {
        unsigned int dmaIndex = 0;
        std::string vendor;
        std::string model;
        std::string serial;
        bool connected = false;

        [[nodiscard]] std::string displayName() const;
    };

    explicit Framegrabber(FramegrabberSystem* parent, int allottedNumber = 0);
    ~Framegrabber();

    Framegrabber(const Framegrabber&) = delete;
    Framegrabber& operator=(const Framegrabber&) = delete;

    using StatusCallback = std::function<void(Status status, bool on)>;
    CallbackId registerStatusCallback(StatusCallback callback);
    bool deregisterStatusCallback(CallbackId id);
    void clearStatusCallbacks();

    using GrabCallback = std::function<void(const Image& image, std::size_t frame)>;
    CallbackId registerGrabCallback(GrabCallback callback);
    bool deregisterGrabCallback(CallbackId id);
    void clearGrabCallbacks();

    using NodeCallback = std::function<void(FeatureSource source,
                                            unsigned int dmaIndex,
                                            const std::string& nodeName)>;
    CallbackId registerNodeUpdatedCallback(NodeCallback callback);
    bool deregisterNodeUpdatedCallback(CallbackId id);
    void clearNodeUpdatedCallbacks();

    void setConfigurationPath(std::string path);
    [[nodiscard]] std::string configurationPath() const;

    bool open(const std::string& boardName = "");
    [[nodiscard]] bool isOpened() const;
    void close();
    [[nodiscard]] std::string getConnectedFramegrabberName() const;

    bool loadConfiguration(const std::string& path);
    bool saveConfiguration(const std::string& path) const;

    void grab(std::size_t frames = 0);
    void stop();
    void requestStop();
    void ready(unsigned int dmaIndex);

    [[nodiscard]] bool isGrabbing() const noexcept;
    [[nodiscard]] int getDMACount() const;
    [[nodiscard]] std::vector<std::string> getUpdatedFramegrabberList() const;
    [[nodiscard]] std::vector<std::string> getCachedFramegrabberList() const;

    void setDMABufferCount(std::size_t count);
    [[nodiscard]] std::size_t dmaBufferCount() const;

    [[nodiscard]] std::string getAppletFeatureXml(unsigned int dmaIndex) const;
    bool getAppletParameter(const std::string& name,
                            unsigned int dmaIndex,
                            ParameterValue& value,
                            bool silent = false) const;
    bool setAppletParameter(const std::string& name,
                            unsigned int dmaIndex,
                            const ParameterValue& value,
                            bool silent = false);
    bool executeAppletCommand(unsigned int dmaIndex, const std::string& name);

    bool refreshCameras();
    [[nodiscard]] std::vector<CameraInfo> getCachedCameraList() const;
    bool connectCamera(unsigned int dmaIndex);
    void disconnectCamera(unsigned int dmaIndex);
    [[nodiscard]] std::string getCameraFeatureXml(unsigned int dmaIndex) const;
    bool getCameraFeature(FeatureSource source,
                          unsigned int dmaIndex,
                          const std::string& name,
                          ParameterValue& value) const;
    bool setCameraFeature(FeatureSource source,
                          unsigned int dmaIndex,
                          const std::string& name,
                          const ParameterValue& value);
    bool executeCameraCommand(unsigned int dmaIndex, const std::string& name);

private:
    struct DmaChannel;
    struct CameraEntry;

    template<typename Callback>
    using CallbackMap = std::unordered_map<CallbackId, Callback>;

    FramegrabberSystem* _system = nullptr;
    int _allottedNumber = 0;

    mutable std::mutex _stateMutex;
    Fg_Struct* _handle = nullptr;
    SgcBoardHandle* _cxpBoard = nullptr;
    std::string _configurationPath;
    std::string _connectedBoardName;
    unsigned int _boardIndex = 0;
    std::size_t _dmaBufferCount = 4;
    std::vector<std::unique_ptr<DmaChannel>> _channels;
    std::vector<CameraEntry> _cameras;

    std::atomic<bool> _isRunning{false};
    std::atomic<std::uint64_t> _frameSeq{0};
    std::atomic<unsigned int> _activeChannels{0};

    mutable std::mutex _statusCallbackMutex;
    CallbackMap<StatusCallback> _statusCallbacks;
    std::atomic<CallbackId> _nextStatusCallbackId{1};

    mutable std::mutex _grabCallbackMutex;
    CallbackMap<GrabCallback> _grabCallbacks;
    std::atomic<CallbackId> _nextGrabCallbackId{1};

    mutable std::mutex _nodeCallbackMutex;
    CallbackMap<NodeCallback> _nodeCallbacks;
    std::atomic<CallbackId> _nextNodeCallbackId{1};

    static PixelFormat toPixelFormat(int sdkFormat);
    static int bytesPerPixel(int sdkFormat);

    bool openResolvedBoard(const std::string& boardName,
                           unsigned int boardIndex,
                           const std::string& configurationPath);
    void releaseHandles();
    void startChannel(unsigned int dmaIndex, std::size_t frames);
    void finishChannel(DmaChannel& channel);
    CameraEntry* findCamera(unsigned int dmaIndex);
    const CameraEntry* findCamera(unsigned int dmaIndex) const;

    void notifyStatus(Status status, bool on);
    void notifyNode(FeatureSource source, unsigned int dmaIndex, const std::string& nodeName);
    void log(const std::string& message, bool warning = false) const;
};

#endif // FRAMEGRABBER_H
