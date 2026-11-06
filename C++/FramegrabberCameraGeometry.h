#ifndef FRAMEGRABBERCAMERAGEOMETRY_H
#define FRAMEGRABBERCAMERAGEOMETRY_H

#include <cstdint>
#include <functional>
#include <string>

/**
 * @file FramegrabberCameraGeometry.h
 * @brief Pure camera/applet geometry synchronization with rollback.
 *
 * The callbacks isolate SDK access so the decision and rollback logic can be
 * tested without a frame grabber or camera.
 */

namespace FramegrabberCameraGeometry
{
struct Size
{
    std::int64_t width = 0;
    std::int64_t height = 0;
};

inline bool operator==(const Size& left, const Size& right) noexcept
{
    return left.width == right.width && left.height == right.height;
}

inline bool operator!=(const Size& left, const Size& right) noexcept
{
    return !(left == right);
}

enum class ReadStatus
{
    NotApplicable,
    Available,
    Failed
};

enum class Result
{
    NotApplicable,
    Applied,
    Failed
};

struct Callbacks
{
    /** Returns NotApplicable when the applet has no geometry contract. */
    std::function<ReadStatus(Size&, std::string&)> readApplet;
    std::function<bool(Size&, std::string&)> readCamera;
    /** Applies Width and Height as one transaction; false reports any partial failure. */
    std::function<bool(const Size&, std::string&)> writeCamera;
};

/**
 * @brief Aligns camera geometry with the applet and restores the previous pair on failure.
 * @param error On Failed, receives the reason and whether the previous pair was restored.
 */
inline Result synchronize(const Callbacks& callbacks, std::string* error = nullptr)
{
    if (error)
    {
        error->clear();
    }
    if (!callbacks.readApplet || !callbacks.readCamera || !callbacks.writeCamera)
    {
        if (error)
        {
            *error = "Camera geometry callbacks are incomplete.";
        }
        return Result::Failed;
    }

    Size target;
    std::string appletError;
    const ReadStatus appletStatus = callbacks.readApplet(target, appletError);
    if (appletStatus == ReadStatus::NotApplicable)
    {
        return Result::NotApplicable;
    }
    if (appletStatus == ReadStatus::Failed)
    {
        if (error)
        {
            *error = appletError.empty() ? "Applet geometry is unavailable." : appletError;
        }
        return Result::Failed;
    }
    if (target.width <= 0 || target.height <= 0)
    {
        if (error)
        {
            *error = "Applet geometry is not positive.";
        }
        return Result::Failed;
    }

    Size previous;
    std::string cameraError;
    if (!callbacks.readCamera(previous, cameraError))
    {
        if (error)
        {
            *error = cameraError.empty() ? "Camera geometry is unavailable." : cameraError;
        }
        return Result::Failed;
    }
    if (previous == target)
    {
        return Result::Applied;
    }

    std::string writeError;
    if (!callbacks.writeCamera(target, writeError))
    {
        std::string rollbackError;
        const bool restored = callbacks.writeCamera(previous, rollbackError);
        if (error)
        {
            *error = (writeError.empty() ? "Camera geometry write failed." : writeError)
                + (restored
                    ? std::string(" The previous geometry was restored.")
                    : std::string(" The previous geometry could not be restored.")
                        + (rollbackError.empty() ? std::string{} : " " + rollbackError));
        }
        return Result::Failed;
    }

    Size actual;
    std::string readBackError;
    if (!callbacks.readCamera(actual, readBackError) || actual != target)
    {
        std::string rollbackError;
        const bool restored = callbacks.writeCamera(previous, rollbackError);
        if (error)
        {
            *error = (readBackError.empty()
                    ? std::string("Camera geometry read-back did not match the applet.")
                    : readBackError)
                + (restored
                    ? std::string(" The previous geometry was restored.")
                    : std::string(" The previous geometry could not be restored.")
                        + (rollbackError.empty() ? std::string{} : " " + rollbackError));
        }
        return Result::Failed;
    }
    return Result::Applied;
}
} // namespace FramegrabberCameraGeometry

#endif // FRAMEGRABBERCAMERAGEOMETRY_H
