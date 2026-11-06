#include "FramegrabberCameraGeometry.h"

#include <cstdio>
#include <string>

namespace
{
using namespace FramegrabberCameraGeometry;

int failures = 0;

void expect(const bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

Callbacks makeCallbacks(Size applet, Size& camera, bool& failHeightOnce, int& writes)
{
    Callbacks callbacks;
    callbacks.readApplet = [applet](Size& size, std::string&)
    {
        size = applet;
        return ReadStatus::Available;
    };
    callbacks.readCamera = [&camera](Size& size, std::string&)
    {
        size = camera;
        return true;
    };
    callbacks.writeCamera = [&camera, &failHeightOnce, &writes](const Size& target, std::string& error)
    {
        ++writes;
        camera.width = target.width;
        if (failHeightOnce && target.height != camera.height)
        {
            failHeightOnce = false;
            error = "Height write failed for the test.";
            return false;
        }
        camera.height = target.height;
        return true;
    };
    return callbacks;
}
} // namespace

int main()
{
    {
        Callbacks callbacks;
        callbacks.readApplet = [](Size&, std::string&)
        {
            return ReadStatus::NotApplicable;
        };
        callbacks.readCamera = [](Size&, std::string&) { return true; };
        callbacks.writeCamera = [](const Size&, std::string&) { return true; };
        std::string error;
        expect(synchronize(callbacks, &error) == Result::NotApplicable,
            "an applet without geometry is not applicable");
        expect(error.empty(), "not-applicable does not report an error");
    }

    {
        Size camera{13376, 9528};
        bool failHeightOnce = false;
        int writes = 0;
        const auto callbacks = makeCallbacks({13376, 9528}, camera, failHeightOnce, writes);
        expect(synchronize(callbacks) == Result::Applied, "matching geometry needs no write");
        expect(writes == 0, "matching geometry performs no write");
    }

    {
        Size camera{1024, 1024};
        bool failHeightOnce = false;
        int writes = 0;
        const auto callbacks = makeCallbacks({13376, 9528}, camera, failHeightOnce, writes);
        expect(synchronize(callbacks) == Result::Applied, "valid geometry is applied");
        expect(camera == Size{13376, 9528}, "camera holds the applet geometry");
        expect(writes == 1, "one transaction writes both axes");
    }

    {
        Size camera{1024, 1024};
        bool failHeightOnce = true;
        int writes = 0;
        const auto callbacks = makeCallbacks({13376, 9528}, camera, failHeightOnce, writes);
        std::string error;
        expect(synchronize(callbacks, &error) == Result::Failed,
            "a partial Width/Height write fails the transaction");
        expect(camera == Size{1024, 1024}, "partial write restores the previous geometry");
        expect(writes == 2, "partial write performs one rollback transaction");
        expect(error.find("restored") != std::string::npos,
            "partial write reports that the previous geometry was restored");
    }

    {
        Callbacks callbacks;
        callbacks.readApplet = [](Size&, std::string& error)
        {
            error = "The applet geometry exists but could not be read.";
            return ReadStatus::Failed;
        };
        callbacks.readCamera = [](Size&, std::string&) { return true; };
        callbacks.writeCamera = [](const Size&, std::string&) { return true; };
        std::string error;
        expect(synchronize(callbacks, &error) == Result::Failed,
            "an applet read failure is not treated as not-applicable");
        expect(error.find("could not be read") != std::string::npos,
            "applet read failure keeps its diagnostic");
    }

    {
        Size camera{1024, 1024};
        int writes = 0;
        Callbacks callbacks;
        callbacks.readApplet = [](Size& size, std::string&)
        {
            size = {13376, 9528};
            return ReadStatus::Available;
        };
        callbacks.readCamera = [&camera](Size& size, std::string&)
        {
            size = camera;
            return true;
        };
        callbacks.writeCamera = [&camera, &writes](const Size& target, std::string& error)
        {
            ++writes;
            if (writes == 1)
            {
                camera.width = target.width;
                error = "Height write failed for the test.";
                return false;
            }
            // The rollback write reports success but does not restore the device.
            return true;
        };
        std::string error;
        expect(synchronize(callbacks, &error) == Result::Failed,
            "rollback write success with mismatched read-back fails the transaction");
        expect(camera != Size{1024, 1024},
            "the test fixture keeps the partially written geometry");
        expect(error.find("could not be restored") != std::string::npos,
            "rollback read-back mismatch reports an unrestored geometry");
    }

    if (failures != 0)
    {
        std::fprintf(stderr, "%d camera geometry synchronization test(s) failed.\n", failures);
        return 1;
    }
    return 0;
}
