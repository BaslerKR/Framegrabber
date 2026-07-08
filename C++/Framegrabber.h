#ifndef FRAMEGRABBER_H
#define FRAMEGRABBER_H

/**
 * @file Framegrabber.h
 * @brief Wrapper for one Basler frame grabber board, acquisition channels, and cameras.
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
        Mono10Packed,
        Mono12Packed,
        Mono14Packed,
        Mono16,
        Mono32,
        Binary,
        RGB24,
        BGR24,
        YCbCr422_8,
        YCbCr422_8_CbYCrY,
        RGBA32,
        RGB30,
        RGB48,
        BGR48,
        RGB10Packed,
        RGB12Packed,
        RGB14Packed,
        BGR10Packed,
        BGR12Packed,
        BGR14Packed,
        RGBA10Packed,
        RGBA12Packed,
        RGBA14Packed,
        RGBA64,
        BGRA32,
        BGRA10Packed,
        BGRA12Packed,
        BGRA14Packed,
        BGRA64,
        RGBX32,
        RGBX10Packed,
        RGBX12Packed,
        RGBX14Packed,
        RGBX64,
        BayerGR8,
        BayerGR10,
        BayerGR12,
        BayerGR14,
        BayerGR16,
        BayerRG8,
        BayerRG10,
        BayerRG12,
        BayerRG14,
        BayerRG16,
        BayerGB8,
        BayerGB10,
        BayerGB12,
        BayerGB14,
        BayerGB16,
        BayerBG8,
        BayerBG10,
        BayerBG12,
        BayerBG14,
        BayerBG16,
        BiColorRGBG8,
        BiColorRGBG10,
        BiColorRGBG12,
        BiColorGRGB8,
        BiColorGRGB10,
        BiColorGRGB12,
        BiColorBGRG8,
        BiColorBGRG10,
        BiColorBGRG12,
        BiColorGBGR8,
        BiColorGBGR10,
        BiColorGBGR12,
        Raw,
        Jpeg
    };

    struct Image
    {
        std::shared_ptr<const std::uint8_t> storage;
        std::size_t size = 0;
        int width = 0;
        int height = 0;
        int stride = 0;
        int bitsPerPixel = 0;
        int bytesPerPixel = 0;
        int sdkPixelFormat = 0;
        PixelFormat pixelFormat = PixelFormat::Unknown;
        unsigned int dmaIndex = 0;
        std::uint64_t frameSeq = 0;

        [[nodiscard]] const std::uint8_t* data() const noexcept
        {
            return storage.get();
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

    enum class CameraTransport
    {
        None,
        CoaXPress,
        CameraLink
    };

    struct CameraControlCapability
    {
        CameraTransport transport = CameraTransport::None;
        bool canDiscover = false;
        bool canConnect = false;
        bool canReadFeatures = false;
    };

    using ParameterValue = std::variant<std::int32_t,
                                        std::uint32_t,
                                        std::int64_t,
                                        std::uint64_t,
                                        double,
                                        std::string>;

    struct CameraInfo
    {
        CameraTransport transport = CameraTransport::None;
        unsigned int dmaIndex = 0;
        std::string vendor;
        std::string model;
        std::string serial;
        bool connected = false;

        [[nodiscard]] std::string displayName() const;
    };

    enum class AppletFeatureKind
    {
        Category,
        Integer,
        Float,
        Boolean,
        String,
        Enumeration,
        Command,
        Unknown
    };

    struct AppletEnumEntry
    {
        std::string name;
        std::string displayName;
        std::int64_t value = 0;
    };

    struct AppletFeatureNode
    {
        std::string name;
        std::string accessName;
        std::string displayName;
        std::string toolTip;
        std::string description;
        std::int64_t parameterId = -1;
        AppletFeatureKind kind = AppletFeatureKind::Unknown;
        bool readable = true;
        bool writable = false;
        std::vector<AppletEnumEntry> enumEntries;
        std::vector<AppletFeatureNode> children;
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
                                            CameraTransport transport,
                                            unsigned int dmaIndex,
                                            const std::string& nodeName)>;
    CallbackId registerNodeUpdatedCallback(NodeCallback callback);
    bool deregisterNodeUpdatedCallback(CallbackId id);
    void clearNodeUpdatedCallbacks();

    void setAppletPath(std::string path);
    [[nodiscard]] std::string appletPath() const;
    void setConfigurationPath(std::string path);
    [[nodiscard]] std::string configurationPath() const;

    bool loadApplet(const std::string& path, const std::string& boardName = "");
    bool open(const std::string& boardName = "");
    [[nodiscard]] bool isOpened() const;
    void close();
    [[nodiscard]] std::string getConnectedFramegrabberName() const;
    [[nodiscard]] std::string getLoadedAppletName() const;
    [[nodiscard]] std::string getLoadedAppletVersion() const;

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
    [[nodiscard]] std::string getBoardAppletPath(
        const std::string& boardName = "") const;

    void setDMABufferCount(std::size_t count);
    [[nodiscard]] std::size_t dmaBufferCount() const;

    [[nodiscard]] std::string getAppletFeatureXml(unsigned int dmaIndex) const;
    [[nodiscard]] std::vector<AppletFeatureNode> getAppletFeatureModel(
        unsigned int dmaIndex) const;
    bool getAppletParameter(const std::string& name,
                            unsigned int dmaIndex,
                            ParameterValue& value,
                            bool silent = false) const;
    bool getAppletParameterById(std::int64_t parameterId,
                                unsigned int dmaIndex,
                                ParameterValue& value,
                                bool silent = false) const;
    bool setAppletParameter(const std::string& name,
                            unsigned int dmaIndex,
                            const ParameterValue& value,
                            bool silent = false,
                            bool verifyReadBack = true);
    bool setAppletParameterById(std::int64_t parameterId,
                                unsigned int dmaIndex,
                                const ParameterValue& value,
                                bool silent = false,
                                bool verifyReadBack = true);
    bool executeAppletCommand(unsigned int dmaIndex, const std::string& name);
    bool executeAppletCommandById(unsigned int dmaIndex, std::int64_t parameterId);

    [[nodiscard]] std::vector<CameraControlCapability> cameraControlCapabilities() const;
    bool refreshCameras(CameraTransport transport);
    [[nodiscard]] std::vector<CameraInfo> getCachedCameraList(CameraTransport transport) const;
    bool connectCamera(CameraTransport transport, unsigned int dmaIndex);
    void disconnectCamera(CameraTransport transport, unsigned int dmaIndex);
    [[nodiscard]] std::string getCameraFeatureXml(CameraTransport transport,
                                                  unsigned int dmaIndex) const;
    bool getCameraFeature(CameraTransport transport,
                          unsigned int dmaIndex,
                          const std::string& name,
                          ParameterValue& value) const;
    bool setCameraFeature(CameraTransport transport,
                          unsigned int dmaIndex,
                          const std::string& name,
                          const ParameterValue& value);
    bool executeCameraCommand(CameraTransport transport,
                              unsigned int dmaIndex,
                              const std::string& name);

private:
    struct DmaChannel;
    struct CameraEntry;

    template<typename Callback>
    using CallbackMap = std::unordered_map<CallbackId, Callback>;

    FramegrabberSystem* _system = nullptr;
    int _allottedNumber = 0;

    mutable std::mutex _lifecycleMutex;
    mutable std::mutex _stateMutex;
    Fg_Struct* _handle = nullptr;
    SgcBoardHandle* _cxpBoard = nullptr;
    std::string _appletPath;
    std::string _loadedAppletName;
    std::string _loadedAppletVersion;
    std::string _configurationPath;
    std::string _connectedBoardName;
    unsigned int _boardIndex = 0;
    std::size_t _dmaBufferCount = 4;
    std::vector<std::unique_ptr<DmaChannel>> _channels;
    std::vector<CameraEntry> _cxpCameras;

    std::atomic<bool> _isRunning{false};
    std::atomic<bool> _startingChannels{false};
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

    mutable std::mutex _appletFeatureModelMutex;
    mutable std::unordered_map<unsigned int, std::vector<AppletFeatureNode>>
        _appletFeatureModels;

    static PixelFormat toPixelFormat(int sdkFormat);
    static int bitsPerPixel(int sdkFormat);
    static int bytesPerPixel(int sdkFormat);

    bool openResolvedBoard(const std::string& boardName,
                           unsigned int boardIndex,
                           const std::string& appletPath,
                           const std::string& discoveredAppletVersion,
                           bool initializeCameraControl);
    bool closeUnlocked(bool notifyConnectionStatus);
    void requestStopChannels();
    bool joinStoppedChannels();
    void releaseHandles();
    void startChannel(unsigned int dmaIndex, std::size_t frames);
    void finishChannel(DmaChannel& channel);
    void clearAppletFeatureModels();
    void clearAppletFeatureModel(unsigned int dmaIndex);
    void refreshAppletFeatureAccess(std::vector<AppletFeatureNode>& model,
                                    unsigned int dmaIndex) const;
    CameraEntry* findCamera(CameraTransport transport, unsigned int dmaIndex);
    const CameraEntry* findCamera(CameraTransport transport, unsigned int dmaIndex) const;

    void notifyStatus(Status status, bool on);
    void notifyNode(FeatureSource source,
                    CameraTransport transport,
                    unsigned int dmaIndex,
                    const std::string& nodeName);
    void log(const std::string& message, bool warning = false) const;
};

#endif // FRAMEGRABBER_H
