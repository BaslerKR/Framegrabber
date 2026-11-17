#include "DevicePluginSession.h"

#include "FramegrabberSourceController.h"
#include "Framegrabber.h"
#include "FramegrabberSystem.h"

#include <QDebug>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

class FramegrabberPluginTitleState final {
public:
    QString title() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _title;
    }

    void setCallback(std::function<void(const QString&)> callback)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _callback = std::move(callback);
    }

    bool readConnectedName(Framegrabber* framegrabber, QString* title) const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_active || !framegrabber || !title) return false;
        *title = QString::fromStdString(framegrabber->getConnectedFramegrabberName());
        return true;
    }

    void publish(const QString& title)
    {
        std::function<void(const QString&)> callback;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (!_active) return;
            _title = title;
            callback = _callback;
        }
        if (callback) callback(title);
    }

    void deactivate()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _active = false;
        _callback = {};
    }

private:
    mutable std::mutex _mutex;
    bool _active = true;
    QString _title = QStringLiteral("Basler Frame Grabber Session");
    std::function<void(const QString&)> _callback;
};

} // namespace

class FramegrabberPluginSession final : public IDevicePluginSession {
public:
    FramegrabberPluginSession(std::unique_ptr<FramegrabberSystem> system, Framegrabber* framegrabber)
        : _system(std::move(system)), _framegrabber(framegrabber),
          _controller(std::make_unique<FramegrabberSourceController>(_framegrabber))
    {
        std::weak_ptr<FramegrabberPluginTitleState> titleState = _titleState;
        Framegrabber* const framegrabberForCallback = _framegrabber;
        _statusCallback = _framegrabber->registerStatusCallback([titleState, framegrabberForCallback](Framegrabber::Status status, bool connected) {
            if (status != Framegrabber::ConnectionStatus || !connected || !framegrabberForCallback) return;
            const auto state = titleState.lock();
            if (!state) return;

            QString title;
            if (state->readConnectedName(framegrabberForCallback, &title) && !title.isEmpty()) {
                state->publish(title);
            }
        });
    }

    ~FramegrabberPluginSession() override
    {
        _titleState->deactivate();
        if (_framegrabber && _statusCallback != 0) _framegrabber->deregisterStatusCallback(_statusCallback);
        _controller.reset();
        if (_system && _framegrabber) _system->removeFramegrabber(_framegrabber);
    }

    QString title() const override { return _titleState->title(); }
    AbstractSourceController* sourceController() const override { return _controller.get(); }
    unsigned int capabilities() const noexcept override
    {
        return DevicePluginSessionCapability::GraphicsEngine
            | DevicePluginSessionCapability::ScriptEditor;
    }
    void setTitleChangedCallback(std::function<void(const QString&)> callback) override { _titleState->setCallback(std::move(callback)); }

private:
    std::unique_ptr<FramegrabberSystem> _system;
    Framegrabber* _framegrabber = nullptr;
    std::unique_ptr<FramegrabberSourceController> _controller;
    Framegrabber::CallbackId _statusCallback = 0;
    std::shared_ptr<FramegrabberPluginTitleState> _titleState = std::make_shared<FramegrabberPluginTitleState>();
};

class FramegrabberPlugin final : public QObject,
                                 public IDeviceRuntime,
                                 public IDevicePluginResourceClaimsReceiver {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID PlaygroundDeviceRuntime_iid)
    Q_INTERFACES(IDeviceRuntime IDevicePluginResourceClaimsReceiver)

public:
    DevicePluginDescriptor descriptor() const override
    {
        return {QStringLiteral("framegrabber"), playgroundDevicePluginDisplayName(), playgroundDevicePluginVersion(), false};
    }
    void setResourceClaims(IDevicePluginResourceClaims* claims) override
    {
        _claims.store(claims, std::memory_order_release);
    }
    bool discoverDevices(QString* errorMessage) override
    {
        try {
            auto system = std::make_unique<FramegrabberSystem>();
            if (!system->isInitialized() || system->getCachedFramegrabberList().empty()) {
                if (errorMessage) *errorMessage = QStringLiteral("No frame grabber board was detected.");
                return false;
            }
            std::lock_guard<std::mutex> lock(_discoveredBoardsMutex);
            _discoveredBoards = system->getCachedBoardInfo();
            return true;
        } catch (const std::exception& error) {
            if (errorMessage) *errorMessage = QString::fromLocal8Bit(error.what());
            return false;
        }
    }

    std::unique_ptr<IDevicePluginSession> createSession(
        const QVariantMap& sessionOptions, QString* errorMessage) override
    {
        Q_UNUSED(sessionOptions)
        try {
            std::vector<FramegrabberSystem::BoardInfo> boards;
            {
                std::lock_guard<std::mutex> lock(_discoveredBoardsMutex);
                boards = _discoveredBoards;
            }
            if (boards.empty()) {
                if (errorMessage) *errorMessage = QStringLiteral("No frame grabber board was detected.");
                return {};
            }
            auto system = std::make_unique<FramegrabberSystem>(false);
            if (!system->isInitialized()) {
                if (errorMessage) *errorMessage = QStringLiteral("Frame grabber runtime initialization failed.");
                return {};
            }
            system->setResourceClaimCallback([this](const std::string& serial, std::string& error) {
                return claimBoard(serial, error);
            });
            system->setCachedBoardInfo(boards);
            Framegrabber* framegrabber = system->addFramegrabber();
            if (!framegrabber) {
                if (errorMessage) *errorMessage = QStringLiteral("Failed to create a frame grabber session.");
                return {};
            }
            // Populate the installed-applet catalog once so declared actions and the panel can
            // present the installed choices before any load. Enumeration has no device effect.
            try { framegrabber->refreshAppletCatalog(); }
            catch (const std::exception& error) {
                qWarning() << "[Framegrabber] Applet enumeration failed:" << error.what();
            } catch (...) {
                qWarning() << "[Framegrabber] Applet enumeration failed.";
            }
            return std::make_unique<FramegrabberPluginSession>(std::move(system), framegrabber);
        } catch (const std::exception& error) {
            if (errorMessage) *errorMessage = QString::fromLocal8Bit(error.what());
            return {};
        }
    }

private:
    std::shared_ptr<void> claimBoard(const std::string& serial, std::string& error)
    {
        auto* claims = _claims.load(std::memory_order_acquire);
        if (!claims) {
            error = "The host did not provide hardware resource claims.";
            return {};
        }
        const QString key = playgroundSisoBoardResourceKey(QString::fromStdString(serial));
        if (key.isEmpty()) {
            error = "The frame grabber board serial is unavailable for an exclusive resource claim.";
            return {};
        }
        QString claimError;
        auto claim = claims->claimResource(key, &claimError);
        if (!claim) {
            error = claimError.toStdString();
            return {};
        }
        return claim;
    }

    std::atomic<IDevicePluginResourceClaims*> _claims{nullptr};
    std::mutex _discoveredBoardsMutex;
    std::vector<FramegrabberSystem::BoardInfo> _discoveredBoards;
};

#include "FramegrabberPlugin.moc"
