#include "FramegrabberSystem.h"

#include "Framegrabber.h"
#include "basler_fg.h"
#include "sisoboards.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <utility>

namespace
{
struct AppletRecord
{
    std::string uid;
    std::string name;
    std::string file;
    std::string path;
    std::string tags;
    std::string version;
};

std::string boardProperty(const unsigned int boardIndex, const Fg_Info_Selector selector)
{
    std::string value;
    Fg_getStringSystemInformationForBoardIndex(boardIndex, selector, PROP_ID_VALUE, value);
    return value;
}

std::string appletString(Fg_AppletIteratorItem item,
                         const FgAppletStringProperty property)
{
    const char* value = Fg_getAppletStringProperty(item, property);
    return value ? value : "";
}

bool readAppletRecord(Fg_AppletIteratorItem item, AppletRecord& record)
{
    if (!item
        || (Fg_getAppletIntProperty(item, FG_AP_INT_INFO) & FG_AI_IS_VALID) == 0)
    {
        return false;
    }

    record.uid = appletString(item, FG_AP_STRING_APPLET_UID);
    record.name = appletString(item, FG_AP_STRING_APPLET_NAME);
    record.file = appletString(item, FG_AP_STRING_APPLET_FILE);
    record.path = appletString(item, FG_AP_STRING_APPLET_PATH);
    record.tags = appletString(item, FG_AP_STRING_TAGS);
    record.version = appletString(item, FG_AP_STRING_VERSION);
    return true;
}

bool findBoardApplet(const unsigned int boardIndex,
                     const int requiredFlag,
                     AppletRecord& record)
{
    Fg_AppletIteratorType iterator = nullptr;
    const int count = Fg_getAppletIterator(
        static_cast<int>(boardIndex),
        FG_AIS_BOARD,
        &iterator,
        requiredFlag);
    if (count <= 0)
    {
        if (iterator)
        {
            Fg_freeAppletIterator(iterator);
        }
        return false;
    }

    bool found = false;
    for (int index = 0; index < count && !found; ++index)
    {
        AppletRecord candidate;
        Fg_AppletIteratorItem item = Fg_getAppletIteratorItem(iterator, index);
        if (!readAppletRecord(item, candidate))
        {
            continue;
        }

        const std::int64_t flags = Fg_getAppletIntProperty(item, FG_AP_INT_FLAGS);
        if ((flags & requiredFlag) != 0)
        {
            record = std::move(candidate);
            found = true;
        }
    }

    Fg_freeAppletIterator(iterator);
    return found;
}

std::string findLoadableAppletPath(const unsigned int boardIndex,
                                   const AppletRecord& boardApplet)
{
    Fg_AppletIteratorType iterator = nullptr;
    const int count = Fg_getAppletIterator(
        static_cast<int>(boardIndex),
        FG_AIS_FILESYSTEM,
        &iterator,
        FG_AF_IS_LOADABLE);
    if (count <= 0)
    {
        if (iterator)
        {
            Fg_freeAppletIterator(iterator);
        }
        return {};
    }

    std::string matchedPath;
    for (int index = 0; index < count; ++index)
    {
        AppletRecord candidate;
        if (!readAppletRecord(
                Fg_getAppletIteratorItem(iterator, index),
                candidate))
        {
            continue;
        }

        const bool uidMatches = !boardApplet.uid.empty()
                                && candidate.uid == boardApplet.uid;
        const bool identityMatches =
            boardApplet.uid.empty()
            && ((!boardApplet.name.empty() && candidate.name == boardApplet.name)
                || (!boardApplet.file.empty() && candidate.file == boardApplet.file)
                || (!boardApplet.path.empty() && candidate.path == boardApplet.path));
        if (uidMatches || identityMatches)
        {
            matchedPath = std::move(candidate.path);
            break;
        }
    }

    Fg_freeAppletIterator(iterator);
    return matchedPath;
}

bool findAppletRecord(const unsigned int boardIndex,
                      const std::string& appletPath,
                      AppletRecord& record)
{
    Fg_AppletIteratorType iterator = nullptr;
    const int count = Fg_getAppletIterator(
        static_cast<int>(boardIndex),
        FG_AIS_FILESYSTEM,
        &iterator,
        FG_AF_IS_LOADABLE);
    if (count <= 0)
    {
        if (iterator)
        {
            Fg_freeAppletIterator(iterator);
        }
        return false;
    }

    Fg_AppletIteratorItem item =
        Fg_findAppletIteratorItem(iterator, appletPath.c_str());
    bool found = readAppletRecord(item, record);
    for (int index = 0; index < count && !found; ++index)
    {
        AppletRecord candidate;
        if (!readAppletRecord(
                Fg_getAppletIteratorItem(iterator, index),
                candidate))
        {
            continue;
        }

        std::error_code error;
        if (std::filesystem::equivalent(
                std::filesystem::path(candidate.path),
                std::filesystem::path(appletPath),
                error)
            && !error)
        {
            record = std::move(candidate);
            found = true;
        }
    }
    Fg_freeAppletIterator(iterator);
    return found;
}

bool hasAppletTag(const std::string& tags, const std::string& expected)
{
    std::size_t start = 0;
    while (start <= tags.size())
    {
        const std::size_t end = tags.find(',', start);
        const std::size_t length =
            end == std::string::npos ? std::string::npos : end - start;
        if (tags.substr(start, length) == expected)
        {
            return true;
        }
        if (end == std::string::npos)
        {
            break;
        }
        start = end + 1;
    }
    return false;
}
}

std::string FramegrabberSystem::BoardInfo::displayName() const
{
    if (serial.empty())
    {
        return name;
    }
    return name + " (" + serial + ")";
}

FramegrabberSystem::FramegrabberSystem()
{
    _initialized = Fg_InitLibraries(nullptr) == FG_OK;
    if (!_initialized)
    {
        syslog("Library initialization failed.", true);
        return;
    }

    syslog("Library initialization succeeded.");
    updateFramegrabberList();
}

FramegrabberSystem::~FramegrabberSystem()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _framegrabbers.clear();
        _boards.clear();
    }

    if (_initialized)
    {
        Fg_FreeLibraries();
        _initialized = false;
    }
}

bool FramegrabberSystem::updateFramegrabberList()
{
    if (!_initialized)
    {
        return false;
    }

    char buffer[256] = {};
    unsigned int bufferLength = sizeof(buffer);
    if (Fg_getSystemInformation(nullptr,
                                INFO_NR_OF_BOARDS,
                                PROP_ID_VALUE,
                                0,
                                buffer,
                                &bufferLength) != FG_OK)
    {
        syslog("Failed to query the number of frame grabber boards.", true);
        return false;
    }

    const int boardCount = (std::max)(0, std::atoi(buffer));
    std::vector<BoardInfo> discovered;
    discovered.reserve(static_cast<std::size_t>(boardCount));

    for (int index = 0; index < boardCount; ++index)
    {
        BoardInfo info;
        info.index = static_cast<unsigned int>(index);
        const char* boardName = Fg_getBoardNameByType(Fg_getBoardType(index), 0);
        info.name = boardName ? boardName : "Basler Frame Grabber";
        info.serial = boardProperty(info.index, INFO_BOARDSERIALNO);
        info.firmware = boardProperty(info.index, INFO_FIRMWAREVERSION);
        info.driver = boardProperty(info.index, INFO_DRIVERVERSION);
        info.hardware = boardProperty(info.index, INFO_HARDWAREVERSION);
        info.pcieLinkWidth = boardProperty(info.index, INFO_STATUS_PCI_LINK_WIDTH);
        info.pcieLinkSpeed = boardProperty(info.index, INFO_STATUS_PCI_LINK_SPEED);
        info.pciePayloadSize = boardProperty(info.index, INFO_STATUS_PCI_PAYLOAD_SIZE);
        discovered.push_back(std::move(info));
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        _boards = std::move(discovered);
    }

    return true;
}

std::vector<std::string> FramegrabberSystem::getFramegrabberList()
{
    updateFramegrabberList();
    return getCachedFramegrabberList();
}

std::vector<std::string> FramegrabberSystem::getCachedFramegrabberList() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<std::string> names;
    names.reserve(_boards.size());
    for (const BoardInfo& board : _boards)
    {
        names.push_back(board.displayName());
    }
    return names;
}

std::vector<FramegrabberSystem::BoardInfo> FramegrabberSystem::getCachedBoardInfo() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _boards;
}

bool FramegrabberSystem::isAccessible(const std::string& boardName) const
{
    BoardInfo board;
    return resolveBoard(boardName, board);
}

std::string FramegrabberSystem::getBoardAppletPath(
    const std::string& boardName) const
{
    if (!_initialized)
    {
        return {};
    }

    BoardInfo board;
    if (!resolveBoard(boardName, board))
    {
        return {};
    }

    std::lock_guard<std::mutex> lock(_appletDiscoveryMutex);
    for (const int flag : {FG_AF_IS_ACTIVE, FG_AF_IS_POWERUP_APPLET})
    {
        AppletRecord boardApplet;
        if (!findBoardApplet(board.index, flag, boardApplet))
        {
            continue;
        }

        const std::string path = findLoadableAppletPath(board.index, boardApplet);
        if (!path.empty() && std::filesystem::is_regular_file(path))
        {
            return path;
        }
    }
    return {};
}

FramegrabberSystem::AppletMetadata FramegrabberSystem::getAppletMetadata(
    const std::string& boardName,
    const std::string& appletPath) const
{
    if (!_initialized || appletPath.empty())
    {
        return {};
    }

    BoardInfo board;
    if (!resolveBoard(boardName, board))
    {
        return {};
    }

    std::lock_guard<std::mutex> lock(_appletDiscoveryMutex);
    AppletRecord applet;
    if (!findAppletRecord(board.index, appletPath, applet))
    {
        return {};
    }

    AppletMetadata metadata;
    metadata.version = std::move(applet.version);
    metadata.supportsCameraControl =
        hasAppletTag(applet.tags, "interface=cxp")
        && !hasAppletTag(applet.tags, "family=test");
    return metadata;
}

bool FramegrabberSystem::isInitialized() const noexcept
{
    return _initialized;
}

Framegrabber* FramegrabberSystem::addFramegrabber()
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto framegrabber = std::make_unique<Framegrabber>(
        this,
        static_cast<int>(_nextFramegrabberNumber++));
    Framegrabber* result = framegrabber.get();
    _framegrabbers.push_back(std::move(framegrabber));
    return result;
}

void FramegrabberSystem::removeFramegrabber(Framegrabber* framegrabber)
{
    if (!framegrabber)
    {
        return;
    }

    std::unique_ptr<Framegrabber> removed;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto it = std::find_if(
            _framegrabbers.begin(),
            _framegrabbers.end(),
            [framegrabber](const std::unique_ptr<Framegrabber>& item)
            {
                return item.get() == framegrabber;
            });
        if (it == _framegrabbers.end())
        {
            return;
        }
        removed = std::move(*it);
        _framegrabbers.erase(it);
    }
}

Framegrabber* FramegrabberSystem::getFramegrabber(const int allottedNumber) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    const auto it = std::find_if(
        _framegrabbers.begin(),
        _framegrabbers.end(),
        [allottedNumber](const std::unique_ptr<Framegrabber>& framegrabber)
        {
            return framegrabber && framegrabber->_allottedNumber == allottedNumber;
        });
    return it == _framegrabbers.end() ? nullptr : it->get();
}

bool FramegrabberSystem::resolveBoard(const std::string& boardName, BoardInfo& board) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_boards.empty())
    {
        return false;
    }

    if (boardName.empty())
    {
        board = _boards.front();
        return true;
    }

    const auto it = std::find_if(
        _boards.begin(),
        _boards.end(),
        [&boardName](const BoardInfo& candidate)
        {
            return candidate.displayName() == boardName
                   || candidate.name == boardName
                   || candidate.serial == boardName;
        });
    if (it == _boards.end())
    {
        return false;
    }

    board = *it;
    return true;
}

void FramegrabberSystem::syslog(const std::string& message, const bool warning)
{
    std::ostream& output = warning ? std::cerr : std::cout;
    output << "[Framegrabber] " << message << std::endl;
}
