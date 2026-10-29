#ifndef FRAMEGRABBERSYSTEM_H
#define FRAMEGRABBERSYSTEM_H

/**
 * @file FramegrabberSystem.h
 * @brief Owns the Basler frame grabber SDK runtime, board discovery, and Framegrabber instances.
 */

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class Framegrabber;

class FramegrabberSystem
{
public:
    struct BoardInfo
    {
        unsigned int index = 0;
        std::string name;
        std::string serial;
        std::string firmware;
        std::string driver;
        std::string hardware;
        std::string pcieLinkWidth;
        std::string pcieLinkSpeed;
        std::string pciePayloadSize;

        [[nodiscard]] std::string displayName() const;
    };

    struct AppletMetadata
    {
        std::string version;
        bool supportsCameraControl = false;
    };

    /** @brief Copies one loadable applet reported by the SDK applet iterator. */
    struct AppletInfo
    {
        std::string name;
        std::string file;
        std::string path;
        std::string uid;
        std::string version;
        std::string description;
        std::string category;
        std::string tags;
        int dmaCount = 0;
        int cameraCount = 0;
        bool active = false;
        bool powerUp = false;
        bool supportsCameraControl = false;
    };

    explicit FramegrabberSystem(bool discoverBoardsOnCreate = true);
    ~FramegrabberSystem();

    FramegrabberSystem(const FramegrabberSystem&) = delete;
    FramegrabberSystem& operator=(const FramegrabberSystem&) = delete;

    bool updateFramegrabberList();
    [[nodiscard]] std::vector<std::string> getFramegrabberList();
    [[nodiscard]] std::vector<std::string> getCachedFramegrabberList() const;
    [[nodiscard]] std::vector<BoardInfo> getCachedBoardInfo() const;
    void setCachedBoardInfo(std::vector<BoardInfo> boards);
    [[nodiscard]] bool isAccessible(const std::string& boardName) const;
    [[nodiscard]] std::string getBoardAppletPath(const std::string& boardName) const;
    [[nodiscard]] std::string resolveLoadableAppletPath(
        const std::string& boardName,
        const std::string& appletReference) const;
    [[nodiscard]] AppletMetadata getAppletMetadata(
        const std::string& boardName,
        const std::string& appletPath) const;
    /**
     * @brief Enumerates applets that Fg_Init can load on the board.
     * @note Uses Fg_getAppletIterator(FG_AIS_FILESYSTEM, FG_AF_IS_LOADABLE) as documented
     *       by the Framegrabber API. Avoid calling it while an applet of the board is open.
     */
    [[nodiscard]] std::vector<AppletInfo> getLoadableApplets(
        const std::string& boardName) const;
    [[nodiscard]] bool isInitialized() const noexcept;

    Framegrabber* addFramegrabber();
    void removeFramegrabber(Framegrabber* framegrabber);
    [[nodiscard]] Framegrabber* getFramegrabber(int allottedNumber) const;

    bool resolveBoard(const std::string& boardName, BoardInfo& board) const;

    static void syslog(const std::string& message, bool warning = false);

private:
    // fglib5 library state is process-wide; every system shares one reference.
    static bool acquireLibraries();
    static void releaseLibraries();

    bool _initialized = false;
    mutable std::mutex _mutex;
    mutable std::mutex _appletDiscoveryMutex;
    std::vector<BoardInfo> _boards;
    std::vector<std::unique_ptr<Framegrabber>> _framegrabbers;
    std::size_t _nextFramegrabberNumber = 0;
};

#endif // FRAMEGRABBERSYSTEM_H
