#include "FramegrabberSystem.h"

#include "Framegrabber.h"
#include "basler_fg.h"
#include "sisoboards.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace
{
std::string boardProperty(const unsigned int boardIndex, const Fg_Info_Selector selector)
{
    std::string value;
    Fg_getStringSystemInformationForBoardIndex(boardIndex, selector, PROP_ID_VALUE, value);
    return value;
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
