#include "DevicePluginTemplate.h"

#include "FramegrabberSourceController.h"
#include "Framegrabber.h"
#include "FramegrabberSystem.h"
#include "Utility/Qt/QFramegrabberWidget.h"
#include "Chrome/ThemedFileDialog.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QPointer>

#include <memory>
#include <mutex>

namespace {

QVariantList toDiscoveryData(const std::vector<FramegrabberSystem::BoardInfo>& boards)
{
    QVariantList result;
    result.reserve(static_cast<qsizetype>(boards.size()));
    for (const auto& board : boards) {
        result.append(QVariantMap{
            {QStringLiteral("index"), board.index},
            {QStringLiteral("name"), QString::fromStdString(board.name)},
            {QStringLiteral("serial"), QString::fromStdString(board.serial)},
            {QStringLiteral("firmware"), QString::fromStdString(board.firmware)},
            {QStringLiteral("driver"), QString::fromStdString(board.driver)},
            {QStringLiteral("hardware"), QString::fromStdString(board.hardware)},
            {QStringLiteral("pcieLinkWidth"), QString::fromStdString(board.pcieLinkWidth)},
            {QStringLiteral("pcieLinkSpeed"), QString::fromStdString(board.pcieLinkSpeed)},
            {QStringLiteral("pciePayloadSize"), QString::fromStdString(board.pciePayloadSize)},
        });
    }
    return result;
}

std::vector<FramegrabberSystem::BoardInfo> fromDiscoveryData(const QVariantList& values)
{
    std::vector<FramegrabberSystem::BoardInfo> boards;
    boards.reserve(static_cast<std::size_t>(values.size()));
    for (const QVariant& value : values) {
        const QVariantMap item = value.toMap();
        boards.push_back({
            item.value(QStringLiteral("index")).toUInt(),
            item.value(QStringLiteral("name")).toString().toStdString(),
            item.value(QStringLiteral("serial")).toString().toStdString(),
            item.value(QStringLiteral("firmware")).toString().toStdString(),
            item.value(QStringLiteral("driver")).toString().toStdString(),
            item.value(QStringLiteral("hardware")).toString().toStdString(),
            item.value(QStringLiteral("pcieLinkWidth")).toString().toStdString(),
            item.value(QStringLiteral("pcieLinkSpeed")).toString().toStdString(),
            item.value(QStringLiteral("pciePayloadSize")).toString().toStdString(),
        });
    }
    return boards;
}

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
        if (_widget) _widget->prepareForShutdown();
        _controller.reset();
        if (_system && _framegrabber) _system->removeFramegrabber(_framegrabber);
    }

    QString title() const override { return _titleState->title(); }
    std::vector<DevicePluginDock> createDockWidgets(QWidget* parent) override
    {
        if (!_widget) {
            _widget = new QFramegrabberWidget(parent, _framegrabber);
            _widget->setMissingAppletResolver([parent](const QString& configurationPath, const QString&) {
                return ThemedFileDialog::getOpenFileName(
                    parent,
                    QObject::tr("Locate Frame Grabber Applet"),
                    QFileInfo(configurationPath).absolutePath(),
                    QObject::tr("Frame Grabber Applet (*.hap *.dll *.so);;All Files (*)"));
            });
        }
        return {{QStringLiteral("device-controls"), QStringLiteral("Device Controls"),
                 Qt::LeftDockWidgetArea, _widget, true}};
    }
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
    QPointer<QFramegrabberWidget> _widget;
    Framegrabber::CallbackId _statusCallback = 0;
    std::shared_ptr<FramegrabberPluginTitleState> _titleState = std::make_shared<FramegrabberPluginTitleState>();
};

class FramegrabberPlugin final : public DevicePluginTemplate {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID PlaygroundDevicePlugin_iid)
    Q_INTERFACES(IDevicePlugin)

public:
    DevicePluginDescriptor descriptor() const override
    {
        return {QStringLiteral("framegrabber"), QStringLiteral("Basler Frame Grabber"), playgroundDevicePluginVersion(), false};
    }
    bool discoverDevices(QVariantMap* discoveryData, QString* errorMessage) override
    {
        try {
            auto system = std::make_unique<FramegrabberSystem>();
            if (!system->isInitialized() || system->getCachedFramegrabberList().empty()) {
                if (errorMessage) *errorMessage = QStringLiteral("No frame grabber board was detected.");
                return false;
            }
            if (discoveryData) discoveryData->insert(
                QStringLiteral("boards"), toDiscoveryData(system->getCachedBoardInfo()));
            return true;
        } catch (const std::exception& error) {
            if (errorMessage) *errorMessage = QString::fromLocal8Bit(error.what());
            return false;
        }
    }

    std::unique_ptr<IDevicePluginSession> createSession(
        const QVariantMap& discoveryData, QString* errorMessage) override
    {
        try {
            const auto boards = fromDiscoveryData(discoveryData.value(QStringLiteral("boards")).toList());
            if (boards.empty()) {
                if (errorMessage) *errorMessage = QStringLiteral("No frame grabber board was detected.");
                return {};
            }
            auto system = std::make_unique<FramegrabberSystem>(false);
            if (!system->isInitialized()) {
                if (errorMessage) *errorMessage = QStringLiteral("Frame grabber runtime initialization failed.");
                return {};
            }
            system->setCachedBoardInfo(boards);
            Framegrabber* framegrabber = system->addFramegrabber();
            if (!framegrabber) {
                if (errorMessage) *errorMessage = QStringLiteral("Failed to create a frame grabber session.");
                return {};
            }
            return std::make_unique<FramegrabberPluginSession>(std::move(system), framegrabber);
        } catch (const std::exception& error) {
            if (errorMessage) *errorMessage = QString::fromLocal8Bit(error.what());
            return {};
        }
    }
};

#include "FramegrabberPlugin.moc"
