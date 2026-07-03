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

    FramegrabberSystem();
    ~FramegrabberSystem();

    FramegrabberSystem(const FramegrabberSystem&) = delete;
    FramegrabberSystem& operator=(const FramegrabberSystem&) = delete;

    bool updateFramegrabberList();
    [[nodiscard]] std::vector<std::string> getFramegrabberList();
    [[nodiscard]] std::vector<std::string> getCachedFramegrabberList() const;
    [[nodiscard]] std::vector<BoardInfo> getCachedBoardInfo() const;
    [[nodiscard]] bool isAccessible(const std::string& boardName) const;
    [[nodiscard]] std::string getBoardAppletPath(const std::string& boardName) const;
    [[nodiscard]] AppletMetadata getAppletMetadata(
        const std::string& boardName,
        const std::string& appletPath) const;
    [[nodiscard]] bool isInitialized() const noexcept;

    Framegrabber* addFramegrabber();
    void removeFramegrabber(Framegrabber* framegrabber);
    [[nodiscard]] Framegrabber* getFramegrabber(int allottedNumber) const;

    bool resolveBoard(const std::string& boardName, BoardInfo& board) const;

    static void syslog(const std::string& message, bool warning = false);

private:
    bool _initialized = false;
    mutable std::mutex _mutex;
    mutable std::mutex _appletDiscoveryMutex;
    std::vector<BoardInfo> _boards;
    std::vector<std::unique_ptr<Framegrabber>> _framegrabbers;
    std::size_t _nextFramegrabberNumber = 0;
};

#endif // FRAMEGRABBERSYSTEM_H
