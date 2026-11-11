#include "Framegrabber.h"
#include "FramegrabberSystem.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

namespace {

bool expect(const bool condition, const char* message)
{
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

struct ClaimState
{
    std::string holder;
    int claims = 0;
    int releases = 0;
};

std::shared_ptr<void> claimFrom(ClaimState& state, const std::string& owner, std::string& error)
{
    if (!state.holder.empty())
    {
        error = "Resource is held by " + state.holder + ".";
        return {};
    }
    state.holder = owner;
    ++state.claims;
    return std::shared_ptr<void>(new int(1), [&state, owner](void* value) {
        delete static_cast<int*>(value);
        if (state.holder == owner)
        {
            state.holder.clear();
            ++state.releases;
        }
    });
}

struct TemporaryApplet
{
    TemporaryApplet()
        : path(std::filesystem::temp_directory_path() / "playground-framegrabber-claim-test.hap")
    {
        std::ofstream stream(path);
        stream << "not a frame grabber applet";
    }
    ~TemporaryApplet()
    {
        std::error_code error;
        std::filesystem::remove(path, error);
    }

    std::filesystem::path path;
};

FramegrabberSystem::BoardInfo testBoard()
{
    FramegrabberSystem::BoardInfo board;
    board.index = 0;
    board.name = "Test Board";
    board.serial = "SN_TEST";
    return board;
}

} // namespace

int main()
{
    TemporaryApplet applet;
    const auto board = testBoard();

    ClaimState state;
    FramegrabberSystem system(false);
    if (!expect(system.isInitialized(), "The frame grabber SDK runtime did not initialize.")) return 1;
    system.setCachedBoardInfo({board});
    system.setResourceClaimCallback([&state](const std::string& serial, std::string& error) {
        if (serial != "SN_TEST")
        {
            error = "Unexpected board serial.";
            return std::shared_ptr<void>{};
        }
        return claimFrom(state, "framegrabber", error);
    });
    Framegrabber* framegrabber = system.addFramegrabber();
    if (!expect(framegrabber, "The test frame grabber was not created.")) return 1;

    const bool opened = framegrabber->loadApplet(applet.path.string());
    if (opened)
    {
        framegrabber->close();
        std::cerr << "A synthetic applet unexpectedly opened a board.\n";
        return 1;
    }
    if (!expect(state.claims == 1 && state.releases == 1 && state.holder.empty(),
            "A failed open retained the hardware resource claim.")) return 1;

    std::string error;
    auto other = claimFrom(state, "other", error);
    if (!expect(other != nullptr, "Another owner could not claim after the failed open.")) return 1;
    other.reset();
    if (!expect(state.holder.empty() && state.releases == 2,
            "The verification claim was not released.")) return 1;

    FramegrabberSystem standalone(false);
    if (!expect(standalone.isInitialized(), "The standalone SDK runtime did not initialize.")) return 1;
    standalone.setCachedBoardInfo({board});
    Framegrabber* standaloneBoard = standalone.addFramegrabber();
    if (!expect(standaloneBoard, "The standalone frame grabber was not created.")) return 1;
    static_cast<void>(standaloneBoard->loadApplet(applet.path.string()));
    standaloneBoard->close();
    if (!expect(standaloneBoard->lastError().message.find("hardware resource claim") == std::string::npos,
            "The standalone core required a hardware resource claim callback.")) return 1;

    return 0;
}
