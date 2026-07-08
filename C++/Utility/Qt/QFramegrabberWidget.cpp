#include "QFramegrabberWidget.h"

#ifdef QT_GUI_LIB

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDomDocument>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QThread>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>
#include <memory>
#include <type_traits>

namespace
{
QString elementText(const QDomElement& element, const QString& childName)
{
    return element.firstChildElement(childName).text().trimmed();
}

QString featureDisplayName(const QDomElement& element, const QString& fallback)
{
    const QString displayName = elementText(element, QStringLiteral("DisplayName"));
    return displayName.isEmpty() ? fallback : displayName;
}

QDomElement findFeatureElement(const QDomElement& root, const QString& name)
{
    static const QStringList featureTags{
        QStringLiteral("Integer"),
        QStringLiteral("IntReg"),
        QStringLiteral("MaskedIntReg"),
        QStringLiteral("Float"),
        QStringLiteral("Enumeration"),
        QStringLiteral("Boolean"),
        QStringLiteral("String"),
        QStringLiteral("StringReg"),
        QStringLiteral("Command"),
        QStringLiteral("Register")};

    for (const QString& tag : featureTags)
    {
        const QDomNodeList nodes = root.elementsByTagName(tag);
        for (int index = 0; index < nodes.count(); ++index)
        {
            const QDomElement element = nodes.at(index).toElement();
            if (element.attribute(QStringLiteral("Name")) == name)
            {
                return element;
            }
        }
    }
    return {};
}

QDomElement findCategoryElement(const QDomElement& root, const QString& name)
{
    const QDomNodeList categories = root.elementsByTagName(QStringLiteral("Category"));
    for (int index = 0; index < categories.count(); ++index)
    {
        const QDomElement category = categories.at(index).toElement();
        if (category.attribute(QStringLiteral("Name")) == name)
        {
            return category;
        }
    }
    return {};
}

QString valueText(const Framegrabber::ParameterValue& value)
{
    return std::visit(
        [](const auto& current) -> QString
        {
            using T = std::decay_t<decltype(current)>;
            if constexpr (std::is_same_v<T, std::string>)
            {
                return QString::fromStdString(current);
            }
            else if constexpr (std::is_same_v<T, double>)
            {
                return QString::number(current, 'g', 12);
            }
            else if constexpr (std::is_signed_v<T>)
            {
                return QString::number(static_cast<qlonglong>(current));
            }
            else
            {
                return QString::number(static_cast<qulonglong>(current));
            }
        },
        value);
}

bool updateValueFromText(Framegrabber::ParameterValue& value, const QString& text)
{
    return std::visit(
        [&text](auto& current) -> bool
        {
            using T = std::decay_t<decltype(current)>;
            bool converted = false;
            if constexpr (std::is_same_v<T, std::string>)
            {
                current = text.toStdString();
                return true;
            }
            else if constexpr (std::is_same_v<T, double>)
            {
                const double parsed = text.toDouble(&converted);
                if (converted)
                {
                    current = parsed;
                }
            }
            else if constexpr (std::is_signed_v<T>)
            {
                const qlonglong parsed = text.toLongLong(&converted);
                if (converted)
                {
                    current = static_cast<T>(parsed);
                }
            }
            else
            {
                const qulonglong parsed = text.toULongLong(&converted);
                if (converted)
                {
                    current = static_cast<T>(parsed);
                }
            }
            return converted;
        },
        value);
}

QString cameraTransportName(const Framegrabber::CameraTransport transport)
{
    switch (transport)
    {
    case Framegrabber::CameraTransport::CoaXPress:
        return QCoreApplication::translate("QFramegrabberWidget", "CoaXPress");
    case Framegrabber::CameraTransport::CameraLink:
        return QCoreApplication::translate("QFramegrabberWidget", "Camera Link");
    case Framegrabber::CameraTransport::None:
        break;
    }
    return QCoreApplication::translate("QFramegrabberWidget", "Camera");
}

}

QFramegrabberWidget::QFramegrabberWidget(QWidget* parent, Framegrabber* framegrabber)
    : QWidget(parent),
      _framegrabber(framegrabber)
{
    setWindowTitle(tr("Basler Frame Grabber Configuration"));
    setMinimumSize(300, 350);
    buildUi();

    if (!_framegrabber)
    {
        setOperationActive(true);
        showStatusMessage(tr("Frame grabber instance is not configured."), true);
        return;
    }

    registerCallbacks();
    {
        QSignalBlocker blocker(_boardCombo);
        for (const std::string& name : _framegrabber->getCachedFramegrabberList())
        {
            _boardCombo->addItem(QString::fromStdString(name));
        }
    }
    _appletPathEdit->setText(
        QString::fromStdString(_framegrabber->appletPath()));
    applyConnectionState(_framegrabber->isOpened());
    if (!_framegrabber->isOpened() && _boardCombo->currentIndex() >= 0)
    {
        QMetaObject::invokeMethod(
            this,
            [this]
            {
                startAutomaticAppletLoad();
            },
            Qt::QueuedConnection);
    }
}

QFramegrabberWidget::~QFramegrabberWidget()
{
    prepareForShutdown();
}

void QFramegrabberWidget::prepareForShutdown()
{
    if (_shuttingDown)
    {
        return;
    }
    _shuttingDown = true;

    if (_framegrabber)
    {
        _framegrabber->requestStop();
    }
    if (_operationThread)
    {
        _operationThread->wait();
        _operationThread = nullptr;
    }
    if (_framegrabber)
    {
        if (_statusCallbackId != 0)
        {
            _framegrabber->deregisterStatusCallback(_statusCallbackId);
            _statusCallbackId = 0;
        }
        if (_nodeCallbackId != 0)
        {
            _framegrabber->deregisterNodeUpdatedCallback(_nodeCallbackId);
            _nodeCallbackId = 0;
        }
    }
}

void QFramegrabberWidget::buildUi()
{
    _boardCombo = new QComboBox(this);
    _boardCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    _grabOneButton = new QToolButton(this);
    _grabOneButton->setIcon(QIcon(QStringLiteral(":/Resources/Icons/icons8-camera-48.png")));
    _grabOneButton->setToolTip(tr("Grab one frame per DMA"));

    _grabLiveButton = new QToolButton(this);
    _grabLiveButton->setCheckable(true);
    QIcon grabIcon;
    grabIcon.addFile(
        QStringLiteral(":/Resources/Icons/icons8-cameras-48.png"),
        QSize(),
        QIcon::Normal,
        QIcon::Off);
    grabIcon.addFile(
        QStringLiteral(":/Resources/Icons/icons8-pause-48.png"),
        QSize(),
        QIcon::Normal,
        QIcon::On);
    _grabLiveButton->setIcon(grabIcon);
    _grabLiveButton->setToolTip(tr("Start or stop live acquisition"));

    auto* selectorLayout = new QHBoxLayout;
    selectorLayout->setObjectName(QStringLiteral("DeviceSelectorLayout"));
    selectorLayout->addWidget(_boardCombo);

    auto* toolLayout = new QHBoxLayout;
    toolLayout->setObjectName(QStringLiteral("DeviceToolLayout"));
    toolLayout->addWidget(_grabOneButton);
    toolLayout->addWidget(_grabLiveButton);

    auto* topLayout = new QHBoxLayout;
    topLayout->setObjectName(QStringLiteral("DeviceTopBarLayout"));
    topLayout->addLayout(selectorLayout);
    topLayout->addLayout(toolLayout);

    _tabs = new QTabWidget(this);
    _tabs->setObjectName(QStringLiteral("FramegrabberControlTabs"));
    _tabs->addTab(createSetupTab(), tr("Setup"));

    _statusBar = new QStatusBar(this);
    _statusBar->setObjectName(QStringLiteral("FramegrabberStatusBar"));
    _statusBar->setSizeGripEnabled(false);
    _statusLabel = new QLabel(this);
    _statusLabel->setObjectName(QStringLiteral("FramegrabberStatusLabel"));
    _statusLabel->setAlignment(Qt::AlignCenter);
    _messageLabel = new QLabel(this);
    _messageLabel->setObjectName(QStringLiteral("FramegrabberMessageLabel"));
    _messageLabel->setProperty("statusRole", "message");
    _messageLabel->setProperty("messageState", "normal");
    _messageLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    _messageLabel->hide();
    _statusBar->addWidget(_statusLabel);
    _statusBar->addWidget(_messageLabel, 1);

    auto* rootLayout = new QVBoxLayout;
    rootLayout->setObjectName(QStringLiteral("DeviceRootLayout"));
    rootLayout->addLayout(topLayout);
    rootLayout->addWidget(_tabs);
    rootLayout->addWidget(_statusBar);
    setLayout(rootLayout);

    connect(_grabOneButton, &QToolButton::clicked, this, [this]
    {
        if (_framegrabber)
        {
            _framegrabber->grab(1);
        }
    });
    connect(_grabLiveButton, &QToolButton::toggled, this, [this](const bool checked)
    {
        if (!_framegrabber)
        {
            return;
        }
        if (checked)
        {
            _framegrabber->grab();
        }
        else
        {
            _framegrabber->requestStop();
        }
    });
    connect(
        _boardCombo,
        QOverload<int>::of(&QComboBox::currentIndexChanged),
        this,
        [this](const int index)
        {
            if (index >= 0
                && _framegrabber
                && !_framegrabber->isOpened()
                && !_operationThread
                && !_shuttingDown)
            {
                startAutomaticAppletLoad();
            }
        });
}

QWidget* QFramegrabberWidget::createSetupTab()
{
    auto* page = new QWidget(this);
    _appletPathEdit = new QLineEdit(page);
    _appletPathEdit->setReadOnly(true);
    _loadAppletButton = new QPushButton(tr("Load Applet"), page);

    auto* pathLayout = new QHBoxLayout;
    pathLayout->setObjectName(QStringLiteral("FramegrabberAppletPathLayout"));
    pathLayout->addWidget(_loadAppletButton);
    pathLayout->addWidget(_appletPathEdit);

    _appletDmaCombo = new QComboBox(page);

    auto* selectorLayout = new QHBoxLayout;
    selectorLayout->setObjectName(QStringLiteral("FramegrabberAppletSelectorLayout"));
    selectorLayout->addWidget(_appletDmaCombo);

    _appletTree = new QTreeWidget(page);
    _appletTree->setObjectName(QStringLiteral("FramegrabberAppletFeaturesTree"));
    _appletTree->setProperty("treeRole", QStringLiteral("DeviceFeatureTree"));
    _appletTree->setHeaderLabels({tr("Feature"), tr("Value")});

    auto* layout = new QVBoxLayout;
    layout->setObjectName(QStringLiteral("FramegrabberAppletLayout"));
    layout->addLayout(pathLayout);
    layout->addLayout(selectorLayout);
    layout->addWidget(_appletTree);
    page->setLayout(layout);

    connect(_loadAppletButton, &QPushButton::clicked, this, [this]
    {
        if (_framegrabber && _framegrabber->isOpened())
        {
            startAppletUnload();
            return;
        }

        const QString path = QFileDialog::getOpenFileName(
            this,
            tr("Load frame grabber applet"),
            _appletPathEdit->text(),
            tr("Frame Grabber Applet (*.hap *.dll *.so);;All Files (*)"));
        if (!path.isEmpty())
        {
            startAppletLoad(path);
        }
    });
    connect(_appletDmaCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]
    {
        rebuildAppletTree();
    });
    return page;
}

std::unique_ptr<QFramegrabberWidget::CameraPage>
QFramegrabberWidget::createCameraPage(
    const Framegrabber::CameraControlCapability& capability)
{
    auto page = std::make_unique<CameraPage>();
    page->capability = capability;
    page->widget = new QWidget(this);
    page->cameraCombo = new QComboBox(page->widget);

    page->refreshButton = new QToolButton(page->widget);
    page->refreshButton->setIcon(
        QIcon(QStringLiteral(":/Resources/Icons/icons8-refresh-48.png")));
    page->refreshButton->setToolTip(
        tr("Rescan and reconnect %1 cameras")
            .arg(cameraTransportName(capability.transport)));

    auto* selectorLayout = new QHBoxLayout;
    selectorLayout->setObjectName(QStringLiteral("FramegrabberCameraSelectorLayout"));
    selectorLayout->addWidget(page->cameraCombo);
    selectorLayout->addWidget(page->refreshButton);

    page->cameraTree = new QTreeWidget(page->widget);
    page->cameraTree->setObjectName(QStringLiteral("FramegrabberCameraFeaturesTree"));
    page->cameraTree->setProperty("treeRole", QStringLiteral("DeviceFeatureTree"));
    page->cameraTree->setHeaderLabels({tr("Feature"), tr("Value")});

    auto* layout = new QVBoxLayout;
    layout->setObjectName(QStringLiteral("FramegrabberCameraLayout"));
    layout->addLayout(selectorLayout);
    layout->addWidget(page->cameraTree);
    page->widget->setLayout(layout);

    CameraPage* pageState = page.get();
    connect(page->refreshButton, &QToolButton::clicked, this, [this, pageState]
    {
        startCameraRefresh(pageState->capability.transport);
    });
    connect(
        page->cameraCombo,
        QOverload<int>::of(&QComboBox::currentIndexChanged),
        this,
        [this, pageState]
    {
        if (_updatingDeviceUi)
        {
            return;
        }
        rebuildCameraTree(*pageState);
    });
    return page;
}

void QFramegrabberWidget::registerCallbacks()
{
    QPointer<QFramegrabberWidget> guard(this);
    _statusCallbackId = _framegrabber->registerStatusCallback(
        [guard](const Framegrabber::Status status, const bool on)
        {
            if (!guard)
            {
                return;
            }
            QMetaObject::invokeMethod(
                guard,
                [guard, status, on]
                {
                    if (!guard)
                    {
                        return;
                    }
                    if (status == Framegrabber::GrabbingStatus)
                    {
                        guard->updateGrabState(on);
                        guard->rebuildAppletTree();
                    }
                    else if (!guard->_operationActive)
                    {
                        guard->applyConnectionState(on);
                    }
                },
                Qt::QueuedConnection);
        });

    _nodeCallbackId = _framegrabber->registerNodeUpdatedCallback(
        [guard](const Framegrabber::FeatureSource source,
                const Framegrabber::CameraTransport transport,
                const unsigned int dmaIndex,
                const std::string&)
        {
            if (!guard)
            {
                return;
            }
            QMetaObject::invokeMethod(
                guard,
                [guard, source, transport, dmaIndex]
                {
                    if (!guard)
                    {
                        return;
                    }
                    if (guard->_updatingDeviceUi)
                    {
                        return;
                    }
                    if (source == Framegrabber::FeatureSource::Applet
                        && guard->_appletDmaCombo->currentData().toUInt() == dmaIndex)
                    {
                        guard->rebuildAppletTree();
                    }
                    else if (source == Framegrabber::FeatureSource::Camera)
                    {
                        CameraPage* page = guard->cameraPage(transport);
                        if (page && page->cameraCombo->currentData().toUInt() == dmaIndex)
                        {
                            guard->rebuildCameraTree(*page);
                        }
                    }
                },
                Qt::QueuedConnection);
        });
}

void QFramegrabberWidget::startAutomaticAppletLoad()
{
    if (!_framegrabber
        || _boardCombo->currentIndex() < 0
        || _operationThread
        || _shuttingDown)
    {
        return;
    }

    const QString boardName = _boardCombo->currentText();
    const auto path = std::make_shared<std::string>();
    const auto success = std::make_shared<bool>(false);
    QPointer<QFramegrabberWidget> guard(this);
    QThread* worker = QThread::create(
        [framegrabber = _framegrabber, boardName, path, success]
        {
            *path = framegrabber->getBoardAppletPath(boardName.toStdString());
            if (!path->empty())
            {
                *success = framegrabber->loadApplet(
                    *path,
                    boardName.toStdString());
            }
        });
    _operationThread = worker;
    setOperationActive(true);
    showStatusMessage(tr("Checking the board applet..."));
    connect(worker, &QThread::finished, this, [guard, worker, path, success]
    {
        worker->deleteLater();
        if (!guard)
        {
            return;
        }

        guard->_operationThread = nullptr;
        guard->_connectionAttempted = !path->empty();
        guard->setOperationActive(false);
        guard->_appletPathEdit->setText(
            QString::fromStdString(guard->_framegrabber->appletPath()));
        guard->applyConnectionState(*success);
        if (*success)
        {
            guard->showStatusMessage(tr("Board applet loaded."));
        }
        else if (path->empty())
        {
            guard->showStatusMessage(
                tr("No active or power-up applet is available."));
        }
        else
        {
            guard->showStatusMessage(
                tr("Failed to load the board applet."),
                true);
        }
    });
    worker->start();
}

void QFramegrabberWidget::startAppletLoad(const QString& path)
{
    if (!_framegrabber || _operationThread || _shuttingDown)
    {
        return;
    }
    if (_boardCombo->currentIndex() < 0)
    {
        showStatusMessage(tr("Select a frame grabber board first."), true);
        return;
    }

    const QString boardName = _boardCombo->currentText();
    const auto success = std::make_shared<bool>(false);
    QPointer<QFramegrabberWidget> guard(this);
    QThread* worker = QThread::create(
        [framegrabber = _framegrabber, boardName, path, success]
        {
            *success = framegrabber->loadApplet(
                path.toStdString(),
                boardName.toStdString());
        });
    _operationThread = worker;
    _connectionAttempted = true;
    setOperationActive(true);
    showStatusMessage(tr("Loading applet..."));
    connect(worker, &QThread::finished, this, [guard, worker, success]
    {
        worker->deleteLater();
        if (!guard)
        {
            return;
        }
        guard->_operationThread = nullptr;
        guard->setOperationActive(false);
        guard->_appletPathEdit->setText(
            QString::fromStdString(guard->_framegrabber->appletPath()));
        guard->applyConnectionState(guard->_framegrabber->isOpened());
        guard->showStatusMessage(
            *success ? tr("Applet loaded.") : tr("Failed to load the applet."),
            !*success);
    });
    worker->start();
}

void QFramegrabberWidget::startAppletUnload()
{
    if (!_framegrabber || _operationThread || _shuttingDown)
    {
        return;
    }

    QPointer<QFramegrabberWidget> guard(this);
    QThread* worker = QThread::create([framegrabber = _framegrabber]
    {
        framegrabber->requestStop();
        framegrabber->close();
    });
    _operationThread = worker;
    _connectionAttempted = true;
    setOperationActive(true);
    showStatusMessage(tr("Unloading applet..."));
    connect(worker, &QThread::finished, this, [guard, worker]
    {
        worker->deleteLater();
        if (!guard)
        {
            return;
        }
        guard->_operationThread = nullptr;
        guard->setOperationActive(false);
        guard->applyConnectionState(false);
        guard->showStatusMessage(tr("Applet unloaded."));
    });
    worker->start();
}

void QFramegrabberWidget::startCameraRefresh(
    const Framegrabber::CameraTransport transport)
{
    CameraPage* page = cameraPage(transport);
    if (!_framegrabber || !page || _operationThread || _shuttingDown)
    {
        return;
    }

    const auto success = std::make_shared<bool>(false);
    QPointer<QFramegrabberWidget> guard(this);
    QThread* worker = QThread::create([framegrabber = _framegrabber, transport, success]
    {
        *success = framegrabber->refreshCameras(transport);
    });
    _operationThread = worker;
    setOperationActive(true);
    showStatusMessage(
        tr("Scanning and connecting %1 cameras...")
            .arg(cameraTransportName(transport)));
    connect(worker, &QThread::finished, this, [guard, worker, transport, success]
    {
        worker->deleteLater();
        if (!guard)
        {
            return;
        }
        guard->_operationThread = nullptr;
        guard->setOperationActive(false);
        CameraPage* currentPage = guard->cameraPage(transport);
        if (!currentPage)
        {
            return;
        }
        guard->refreshCameraSelector(*currentPage);
        guard->showStatusMessage(
            *success
                ? tr("%1 camera setup finished.").arg(cameraTransportName(transport))
                : tr("%1 camera setup failed.").arg(cameraTransportName(transport)),
            !*success);
    });
    worker->start();
}

void QFramegrabberWidget::setOperationActive(const bool active)
{
    _operationActive = active;
    const bool opened = _framegrabber && _framegrabber->isOpened();
    _boardCombo->setEnabled(!active && !opened);
    _loadAppletButton->setEnabled(!active && !_grabbing);
    for (const auto& page : _cameraPages)
    {
        page->refreshButton->setEnabled(
            !active && opened && !_grabbing && page->capability.canDiscover);
    }
    updateStatusLabel();
}

void QFramegrabberWidget::applyConnectionState(const bool opened)
{
    const bool wasUpdatingDeviceUi = _updatingDeviceUi;
    _updatingDeviceUi = true;
    _boardCombo->setEnabled(!opened && !_operationActive);
    _loadAppletButton->setText(opened ? tr("Unload Applet") : tr("Load Applet"));
    _grabOneButton->setEnabled(opened && !_grabbing);
    _grabLiveButton->setEnabled(opened);

    if (opened)
    {
        refreshDmaSelectors();
        rebuildCameraTabs();
        rebuildAppletTree();
    }
    else
    {
        _appletDmaCombo->clear();
        _appletTree->clear();
        clearCameraTabs();
    }
    _updatingDeviceUi = wasUpdatingDeviceUi;
    setOperationActive(_operationActive);
    updateStatusLabel();
}

void QFramegrabberWidget::updateGrabState(const bool grabbing)
{
    _grabbing = grabbing;
    {
        QSignalBlocker blocker(_grabLiveButton);
        _grabLiveButton->setChecked(grabbing);
    }
    _grabOneButton->setEnabled(_framegrabber && _framegrabber->isOpened() && !grabbing);
    _loadAppletButton->setEnabled(!grabbing && !_operationActive);
    for (const auto& page : _cameraPages)
    {
        page->refreshButton->setEnabled(
            !grabbing && !_operationActive && page->capability.canDiscover);
    }
    updateStatusLabel();
}

void QFramegrabberWidget::updateStatusLabel()
{
    const bool opened = _framegrabber && _framegrabber->isOpened();

    if (_operationActive) {
        _statusLabel->setText(tr("Loading"));
        _statusLabel->setProperty("status", "idle");
    } else {
        if (!opened && !_connectionAttempted)
        {
            _statusLabel->setText(tr("Idle"));
            _statusLabel->setProperty("status", "idle");
        }
        else if (!opened)
        {
            _statusLabel->setText(tr("Disconnected"));
            _statusLabel->setProperty("status", "disconnected");
        }
        else if (_grabbing)
        {
            _statusLabel->setText(tr("Live"));
            _statusLabel->setProperty("status", "grabbing");
        }
        else
        {
            _statusLabel->setText(tr("Connected"));
            _statusLabel->setProperty("status", "connected");
        }
    }
    _statusLabel->style()->unpolish(_statusLabel);
    _statusLabel->style()->polish(_statusLabel);
}

void QFramegrabberWidget::refreshDmaSelectors()
{
    const int count = _framegrabber ? _framegrabber->getDMACount() : 0;
    const QVariant previous = _appletDmaCombo->currentData();
    _appletDmaCombo->clear();
    for (int index = 0; index < count; ++index)
    {
        _appletDmaCombo->addItem(tr("DMA %1").arg(index), index);
    }
    const int previousIndex = _appletDmaCombo->findData(previous);
    if (previousIndex >= 0)
    {
        _appletDmaCombo->setCurrentIndex(previousIndex);
    }
}

void QFramegrabberWidget::rebuildCameraTabs()
{
    clearCameraTabs();
    if (!_framegrabber)
    {
        return;
    }

    const auto capabilities = _framegrabber->cameraControlCapabilities();
    for (const Framegrabber::CameraControlCapability& capability : capabilities)
    {
        if (capability.transport == Framegrabber::CameraTransport::None)
        {
            continue;
        }
        auto page = createCameraPage(capability);
        CameraPage* pageState = page.get();
        _tabs->addTab(
            page->widget,
            tr("%1 Camera").arg(cameraTransportName(capability.transport)));
        _cameraPages.push_back(std::move(page));
        refreshCameraSelector(*pageState);
    }
}

void QFramegrabberWidget::clearCameraTabs()
{
    for (const auto& page : _cameraPages)
    {
        QSignalBlocker comboBlocker(page->cameraCombo);
        const int index = _tabs->indexOf(page->widget);
        if (index >= 0)
        {
            _tabs->removeTab(index);
        }
        delete page->widget;
    }
    _cameraPages.clear();
}

QFramegrabberWidget::CameraPage*
QFramegrabberWidget::cameraPage(const Framegrabber::CameraTransport transport) const
{
    for (const auto& page : _cameraPages)
    {
        if (page->capability.transport == transport)
        {
            return page.get();
        }
    }
    return nullptr;
}

void QFramegrabberWidget::refreshCameraSelector(CameraPage& page)
{
    const QVariant previous = page.cameraCombo->currentData();
    QSignalBlocker comboBlocker(page.cameraCombo);
    page.cameraCombo->clear();
    if (!_framegrabber)
    {
        page.cameraTree->clear();
        return;
    }
    for (const Framegrabber::CameraInfo& camera :
         _framegrabber->getCachedCameraList(page.capability.transport))
    {
        page.cameraCombo->addItem(
            tr("DMA %1 - %2")
                .arg(camera.dmaIndex)
                .arg(QString::fromStdString(camera.displayName())),
            camera.dmaIndex);
    }
    const int previousIndex = page.cameraCombo->findData(previous);
    if (previousIndex >= 0)
    {
        page.cameraCombo->setCurrentIndex(previousIndex);
    }
    rebuildCameraTree(page);
}

void QFramegrabberWidget::rebuildAppletTree()
{
    if (!_framegrabber || !_framegrabber->isOpened()
        || _appletDmaCombo->currentIndex() < 0)
    {
        _appletTree->clear();
        return;
    }
    const unsigned int dmaIndex = _appletDmaCombo->currentData().toUInt();
    const auto model = _framegrabber->getAppletFeatureModel(dmaIndex);
    if (!model.empty())
    {
        populateAppletFeatureTree(_appletTree, model, dmaIndex);
        return;
    }

    populateFeatureTree(
        _appletTree,
        QString::fromUtf8(_framegrabber->getAppletFeatureXml(dmaIndex).c_str()),
        TreeSource::Applet,
        Framegrabber::CameraTransport::None,
        dmaIndex);
}

void QFramegrabberWidget::rebuildCameraTree(CameraPage& page)
{
    if (!_framegrabber || !_framegrabber->isOpened()
        || !page.capability.canReadFeatures
        || page.cameraCombo->currentIndex() < 0)
    {
        page.cameraTree->clear();
        return;
    }
    const unsigned int dmaIndex = page.cameraCombo->currentData().toUInt();
    const QString xml = QString::fromUtf8(
        _framegrabber->getCameraFeatureXml(
            page.capability.transport,
            dmaIndex).c_str());
    if (xml.isEmpty())
    {
        page.cameraTree->clear();
        return;
    }
    populateFeatureTree(
        page.cameraTree,
        xml,
        TreeSource::Camera,
        page.capability.transport,
        dmaIndex);
}

void QFramegrabberWidget::populateAppletFeatureTree(
    QTreeWidget* tree,
    const std::vector<Framegrabber::AppletFeatureNode>& nodes,
    const unsigned int dmaIndex)
{
    const TreeState state = captureTreeState(tree);
    tree->clear();
    for (const Framegrabber::AppletFeatureNode& node : nodes)
    {
        addAppletFeatureNode(tree, nullptr, node, dmaIndex);
    }
    restoreTreeState(tree, state);
}

void QFramegrabberWidget::addAppletFeatureNode(
    QTreeWidget* tree,
    QTreeWidgetItem* parent,
    const Framegrabber::AppletFeatureNode& node,
    const unsigned int dmaIndex)
{
    QTreeWidgetItem* item = parent
                                ? new QTreeWidgetItem(parent)
                                : new QTreeWidgetItem(tree);
    const QString name = QString::fromStdString(node.name);
    const QString displayName = QString::fromStdString(node.displayName);
    item->setText(0, displayName.isEmpty() ? name : displayName);
    item->setData(0, Qt::UserRole, name);

    QString toolTip = QString::fromStdString(node.toolTip);
    const QString description = QString::fromStdString(node.description);
    if (toolTip.isEmpty())
    {
        toolTip = description;
    }
    else if (!description.isEmpty() && description != toolTip)
    {
        toolTip += QStringLiteral("\n\n") + description;
    }
    item->setToolTip(0, toolTip);

    if (node.kind == Framegrabber::AppletFeatureKind::Category)
    {
        for (const Framegrabber::AppletFeatureNode& child : node.children)
        {
            addAppletFeatureNode(tree, item, child, dmaIndex);
        }
        return;
    }

    if (QWidget* editor = createAppletFeatureEditor(node, dmaIndex))
    {
        editor->setToolTip(toolTip);
        tree->setItemWidget(item, 1, editor);
    }
}

QWidget* QFramegrabberWidget::createAppletFeatureEditor(
    const Framegrabber::AppletFeatureNode& node,
    const unsigned int dmaIndex)
{
    const QString featureName = QString::fromStdString(
        node.accessName.empty() ? node.name : node.accessName);
    Framegrabber::ParameterValue current = std::int64_t{0};
    if (node.kind == Framegrabber::AppletFeatureKind::Float)
    {
        current = 0.0;
    }
    else if (node.kind == Framegrabber::AppletFeatureKind::Boolean)
    {
        current = std::uint32_t{0};
    }
    else if (node.kind == Framegrabber::AppletFeatureKind::String
             || node.kind == Framegrabber::AppletFeatureKind::Enumeration)
    {
        current = std::string{};
    }

    if (node.kind != Framegrabber::AppletFeatureKind::Command
        && node.parameterId < 0)
    {
        return new QLabel(tr("Unbound"), this);
    }
    if (node.kind != Framegrabber::AppletFeatureKind::Command
        && !_framegrabber->getAppletParameterById(
            node.parameterId,
            dmaIndex,
            current,
            false))
    {
        return new QLabel(tr("Read failed"), this);
    }

    if (node.kind == Framegrabber::AppletFeatureKind::Boolean)
    {
        auto* checkBox = new QCheckBox(this);
        checkBox->setChecked(std::visit(
            [](const auto& value)
            {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_integral_v<T>)
                {
                    return value != 0;
                }
                return false;
            },
            current));
        checkBox->setEnabled(node.writable);
        connect(checkBox, &QCheckBox::toggled, this, [=](const bool value)
        {
            checkBox->setEnabled(false);
            Framegrabber::ParameterValue updated = current;
            std::visit(
                [value](auto& typed)
                {
                    using T = std::decay_t<decltype(typed)>;
                    if constexpr (std::is_integral_v<T>)
                    {
                        typed = static_cast<T>(value ? 1 : 0);
                    }
                },
                updated);
            runAsyncWrite(
                [=]() {
                    return _framegrabber && _framegrabber->setAppletParameterById(node.parameterId, dmaIndex, updated);
                },
                [=](bool success) {
                    checkBox->setEnabled(node.writable);
                    if (!success) {
                        QSignalBlocker blocker(checkBox);
                        checkBox->setChecked(!value);
                        showStatusMessage(tr("Failed to update parameter."), true);
                    }
                }
            );
        });
        return checkBox;
    }

    if (node.kind == Framegrabber::AppletFeatureKind::Enumeration)
    {
        auto* combo = new QComboBox(this);
        for (const Framegrabber::AppletEnumEntry& entry : node.enumEntries)
        {
            const QString name = QString::fromStdString(entry.name);
            const QString displayName = QString::fromStdString(entry.displayName);
            combo->addItem(
                displayName.isEmpty() ? name : displayName,
                QString::number(entry.value));
        }
        const int currentIndex = combo->findData(valueText(current));
        if (currentIndex >= 0)
        {
            combo->setCurrentIndex(currentIndex);
        }
        combo->setEnabled(node.writable);
        connect(combo, &QComboBox::currentTextChanged, this, [=]
        {
            combo->setEnabled(false);
            Framegrabber::ParameterValue updated = current;
            if (updateValueFromText(updated, combo->currentData().toString()))
            {
                runAsyncWrite(
                    [=]() {
                        return _framegrabber && _framegrabber->setAppletParameterById(node.parameterId, dmaIndex, updated);
                    },
                    [=](bool success) {
                        combo->setEnabled(node.writable);
                        if (!success) {
                            showStatusMessage(tr("Failed to update parameter."), true);
                        }
                    }
                );
            } else {
                combo->setEnabled(node.writable);
            }
        });
        return combo;
    }

    if (node.kind == Framegrabber::AppletFeatureKind::Command)
    {
        auto* button = new QPushButton(tr("Execute"), this);
        button->setEnabled(node.writable);
        connect(button, &QPushButton::clicked, this, [=]
        {
            button->setEnabled(false);
            runAsyncWrite(
                [=]() {
                    return _framegrabber
                        && node.parameterId >= 0
                        && _framegrabber->executeAppletCommandById(dmaIndex, node.parameterId);
                },
                [=](bool success) {
                    button->setEnabled(node.writable);
                    showStatusMessage(
                        success ? tr("Command executed.") : tr("Command execution failed."),
                        !success);
                }
            );
        });
        return button;
    }

    auto* edit = new QLineEdit(valueText(current), this);
    edit->setEnabled(node.writable);
    connect(edit, &QLineEdit::editingFinished, this, [=]() mutable
    {
        edit->setEnabled(false);
        Framegrabber::ParameterValue updated = current;
        if (!updateValueFromText(updated, edit->text()))
        {
            QSignalBlocker blocker(edit);
            edit->setText(valueText(current));
            edit->setEnabled(node.writable);
            return;
        }

        runAsyncWrite(
            [=]() {
                return _framegrabber && _framegrabber->setAppletParameterById(node.parameterId, dmaIndex, updated);
            },
            [=](bool success) mutable {
                edit->setEnabled(node.writable);
                if (!success) {
                    QSignalBlocker blocker(edit);
                    edit->setText(valueText(current));
                    showStatusMessage(tr("Failed to update '%1'.").arg(featureName), true);
                } else {
                    current = std::move(updated);
                }
            }
        );
    });
    return edit;
}

void QFramegrabberWidget::populateFeatureTree(QTreeWidget* tree,
                                              const QString& xml,
                                              const TreeSource source,
                                              const Framegrabber::CameraTransport transport,
                                              const unsigned int dmaIndex)
{
    const TreeState state = captureTreeState(tree);
    tree->clear();
    if (xml.isEmpty())
    {
        return;
    }

    QDomDocument document;
    if (!document.setContent(xml))
    {
        showStatusMessage(tr("Feature XML could not be parsed."), true);
        return;
    }
    const QDomElement root = document.documentElement();
    QSet<QString> visiting;

    QDomElement rootCategory = findCategoryElement(root, QStringLiteral("Root"));
    if (!rootCategory.isNull())
    {
        addCategory(
            tree,
            nullptr,
            root,
            rootCategory,
            source,
            transport,
            dmaIndex,
            visiting);
    }
    else
    {
        const QDomNodeList categories = root.elementsByTagName(QStringLiteral("Category"));
        for (int index = 0; index < categories.count(); ++index)
        {
            const QDomElement category = categories.at(index).toElement();
            if (!category.parentNode().toElement().isNull()
                && category.parentNode().toElement().tagName() == QStringLiteral("Categories"))
            {
                addCategory(
                    tree,
                    nullptr,
                    root,
                    category,
                    source,
                    transport,
                    dmaIndex,
                    visiting);
            }
        }
    }
    restoreTreeState(tree, state);
}

void QFramegrabberWidget::addCategory(QTreeWidget* tree,
                                      QTreeWidgetItem* parent,
                                      const QDomElement& root,
                                      const QDomElement& category,
                                      const TreeSource source,
                                      const Framegrabber::CameraTransport transport,
                                      const unsigned int dmaIndex,
                                      QSet<QString>& visiting)
{
    const QString categoryName = category.attribute(QStringLiteral("Name"));
    if (categoryName.isEmpty() || visiting.contains(categoryName))
    {
        return;
    }
    visiting.insert(categoryName);

    QTreeWidgetItem* categoryItem = parent;
    if (categoryName != QStringLiteral("Root"))
    {
        categoryItem = parent
                           ? new QTreeWidgetItem(parent)
                           : new QTreeWidgetItem(tree);
        categoryItem->setText(0, featureDisplayName(category, categoryName));
        categoryItem->setData(0, Qt::UserRole, categoryName);
    }

    for (QDomElement child = category.firstChildElement(QStringLiteral("pFeature"));
         !child.isNull();
         child = child.nextSiblingElement(QStringLiteral("pFeature")))
    {
        const QString featureName = child.text().trimmed();
        const QDomElement childCategory = findCategoryElement(root, featureName);
        if (!childCategory.isNull())
        {
            addCategory(
                tree,
                categoryItem,
                root,
                childCategory,
                source,
                transport,
                dmaIndex,
                visiting);
            continue;
        }

        const QDomElement feature = findFeatureElement(root, featureName);
        if (feature.isNull())
        {
            continue;
        }
        QTreeWidgetItem* item = categoryItem
                                    ? new QTreeWidgetItem(categoryItem)
                                    : new QTreeWidgetItem(tree);
        item->setText(0, featureDisplayName(feature, featureName));
        item->setData(0, Qt::UserRole, featureName);
        if (QWidget* editor =
                createFeatureEditor(feature, featureName, source, transport, dmaIndex))
        {
            tree->setItemWidget(item, 1, editor);
        }
    }
    visiting.remove(categoryName);
}

QWidget* QFramegrabberWidget::createFeatureEditor(const QDomElement& node,
                                                  const QString& featureName,
                                                  const TreeSource source,
                                                  const Framegrabber::CameraTransport transport,
                                                  const unsigned int dmaIndex)
{
    const QString tag = node.tagName();
    Framegrabber::ParameterValue current = std::int64_t{0};
    if (tag == QStringLiteral("Float"))
    {
        current = 0.0;
    }
    else if (tag == QStringLiteral("Boolean"))
    {
        current = std::uint32_t{0};
    }
    else if (tag == QStringLiteral("String")
             || tag == QStringLiteral("StringReg")
             || tag == QStringLiteral("Enumeration"))
    {
        current = std::string{};
    }
    if (!readFeature(source, transport, dmaIndex, featureName, current)
        && tag != QStringLiteral("Command"))
    {
        return new QLabel(tr("Unavailable"), this);
    }

    if (tag == QStringLiteral("Boolean"))
    {
        auto* checkBox = new QCheckBox(this);
        const bool checked = std::visit(
            [](const auto& value)
            {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_integral_v<T>)
                {
                    return value != 0;
                }
                return false;
            },
            current);
        checkBox->setChecked(checked);
        connect(checkBox, &QCheckBox::toggled, this, [=](const bool value)
        {
            checkBox->setEnabled(false);
            Framegrabber::ParameterValue updated = current;
            std::visit(
                [value](auto& typed)
                {
                    using T = std::decay_t<decltype(typed)>;
                    if constexpr (std::is_integral_v<T>)
                    {
                        typed = static_cast<T>(value ? 1 : 0);
                    }
                },
                updated);
            runAsyncWrite(
                [=]() {
                    return writeFeature(source, transport, dmaIndex, featureName, updated);
                },
                [=](bool success) {
                    checkBox->setEnabled(true);
                    if (!success) {
                        QSignalBlocker blocker(checkBox);
                        checkBox->setChecked(!value);
                        showStatusMessage(tr("Failed to update parameter."), true);
                    }
                }
            );
        });
        return checkBox;
    }

    if (tag == QStringLiteral("Enumeration"))
    {
        auto* combo = new QComboBox(this);
        const QDomNodeList entries = node.elementsByTagName(QStringLiteral("EnumEntry"));
        for (int index = 0; index < entries.count(); ++index)
        {
            const QDomElement entry = entries.at(index).toElement();
            const QString name = entry.attribute(QStringLiteral("Name"));
            if (source == TreeSource::Applet)
            {
                combo->addItem(
                    featureDisplayName(entry, name),
                    elementText(entry, QStringLiteral("Value")));
            }
            else
            {
                combo->addItem(featureDisplayName(entry, name), name);
            }
        }
        const int currentIndex = combo->findData(valueText(current));
        if (currentIndex >= 0)
        {
            combo->setCurrentIndex(currentIndex);
        }
        connect(combo, &QComboBox::currentTextChanged, this, [=]
        {
            combo->setEnabled(false);
            Framegrabber::ParameterValue updated = current;
            if (source == TreeSource::Applet)
            {
                if (!updateValueFromText(updated, combo->currentData().toString()))
                {
                    combo->setEnabled(true);
                    return;
                }
            }
            else
            {
                updated = combo->currentData().toString().toStdString();
            }
            runAsyncWrite(
                [=]() {
                    return writeFeature(source, transport, dmaIndex, featureName, updated);
                },
                [=](bool success) {
                    combo->setEnabled(true);
                    if (!success) {
                        showStatusMessage(tr("Failed to update parameter."), true);
                    }
                }
            );
        });
        return combo;
    }

    if (tag == QStringLiteral("Command"))
    {
        auto* button = new QPushButton(tr("Execute"), this);
        connect(button, &QPushButton::clicked, this, [=]
        {
            button->setEnabled(false);
            runAsyncWrite(
                [=]() {
                    bool success = false;
                    if (_framegrabber && source == TreeSource::Camera)
                    {
                        success = _framegrabber->executeCameraCommand(
                            transport,
                            dmaIndex,
                            featureName.toStdString());
                    }
                    else if (_framegrabber)
                    {
                        success = _framegrabber->executeAppletCommand(
                            dmaIndex,
                            featureName.toStdString());
                    }
                    return success;
                },
                [=](bool success) {
                    button->setEnabled(true);
                    showStatusMessage(
                        success ? tr("Command executed.") : tr("Command execution failed."),
                        !success);
                }
            );
        });
        return button;
    }

    auto* edit = new QLineEdit(valueText(current), this);
    connect(edit, &QLineEdit::editingFinished, this, [=]() mutable
    {
        edit->setEnabled(false);
        Framegrabber::ParameterValue updated = current;
        if (!updateValueFromText(updated, edit->text()))
        {
            QSignalBlocker blocker(edit);
            edit->setText(valueText(current));
            edit->setEnabled(true);
            return;
        }

        runAsyncWrite(
            [=]() {
                return writeFeature(source, transport, dmaIndex, featureName, updated);
            },
            [=](bool success) mutable {
                edit->setEnabled(true);
                if (!success) {
                    QSignalBlocker blocker(edit);
                    edit->setText(valueText(current));
                    showStatusMessage(tr("Failed to update '%1'.").arg(featureName), true);
                } else {
                    current = std::move(updated);
                    showStatusMessage(tr("Updated '%1'.").arg(featureName));
                }
            }
        );
    });
    return edit;
}

bool QFramegrabberWidget::readFeature(const TreeSource source,
                                      const Framegrabber::CameraTransport transport,
                                      const unsigned int dmaIndex,
                                      const QString& name,
                                      Framegrabber::ParameterValue& value) const
{
    if (!_framegrabber)
    {
        return false;
    }
    if (source == TreeSource::Applet)
    {
        return _framegrabber->getAppletParameter(
            name.toStdString(),
            dmaIndex,
            value,
            true);
    }
    return _framegrabber->getCameraFeature(
        transport,
        dmaIndex,
        name.toStdString(),
        value);
}

bool QFramegrabberWidget::writeFeature(const TreeSource source,
                                       const Framegrabber::CameraTransport transport,
                                       const unsigned int dmaIndex,
                                       const QString& name,
                                       const Framegrabber::ParameterValue& value)
{
    if (!_framegrabber)
    {
        return false;
    }
    if (source == TreeSource::Applet)
    {
        return _framegrabber->setAppletParameter(
            name.toStdString(),
            dmaIndex,
            value);
    }
    return _framegrabber->setCameraFeature(
        transport,
        dmaIndex,
        name.toStdString(),
        value);
}

QFramegrabberWidget::TreeState QFramegrabberWidget::captureTreeState(QTreeWidget* tree) const
{
    TreeState state;
    for (int index = 0; index < tree->topLevelItemCount(); ++index)
    {
        collectExpandedNodes(tree->topLevelItem(index), state.expandedNodes);
    }
    if (tree->currentItem())
    {
        state.currentNode = tree->currentItem()->data(0, Qt::UserRole).toString();
    }
    state.verticalScrollValue = tree->verticalScrollBar()->value();
    state.horizontalScrollValue = tree->horizontalScrollBar()->value();
    if (QTreeWidgetItem* topItem = tree->itemAt(tree->viewport()->rect().topLeft()))
    {
        state.topVisibleNode = topItem->data(0, Qt::UserRole).toString();
        state.topVisibleOffset = tree->visualItemRect(topItem).top();
    }
    return state;
}

void QFramegrabberWidget::restoreTreeState(QTreeWidget* tree, const TreeState& state)
{
    QTreeWidgetItem* topVisibleItem = nullptr;
    QList<QTreeWidgetItem*> stack;
    for (int index = 0; index < tree->topLevelItemCount(); ++index)
    {
        stack.push_back(tree->topLevelItem(index));
    }
    while (!stack.isEmpty())
    {
        QTreeWidgetItem* item = stack.takeLast();
        const QString name = item->data(0, Qt::UserRole).toString();
        item->setExpanded(state.expandedNodes.contains(name));
        if (!state.currentNode.isEmpty() && state.currentNode == name)
        {
            tree->setCurrentItem(item);
        }
        if (!state.topVisibleNode.isEmpty() && state.topVisibleNode == name)
        {
            topVisibleItem = item;
        }
        for (int index = 0; index < item->childCount(); ++index)
        {
            stack.push_back(item->child(index));
        }
    }
    if (topVisibleItem)
    {
        tree->scrollToItem(topVisibleItem, QAbstractItemView::PositionAtTop);
        const int offsetDelta =
            tree->visualItemRect(topVisibleItem).top() - state.topVisibleOffset;
        tree->verticalScrollBar()->setValue(
            tree->verticalScrollBar()->value() + offsetDelta);
    }
    else
    {
        tree->verticalScrollBar()->setValue(state.verticalScrollValue);
    }
    tree->horizontalScrollBar()->setValue(state.horizontalScrollValue);
}

void QFramegrabberWidget::collectExpandedNodes(QTreeWidgetItem* item,
                                               QSet<QString>& nodes) const
{
    if (!item)
    {
        return;
    }
    if (item->isExpanded())
    {
        nodes.insert(item->data(0, Qt::UserRole).toString());
    }
    for (int index = 0; index < item->childCount(); ++index)
    {
        collectExpandedNodes(item->child(index), nodes);
    }
}

void QFramegrabberWidget::showStatusMessage(const QString& message, const bool error)
{
    _messageLabel->setText(message);
    _messageLabel->setToolTip(message);
    _messageLabel->setProperty("messageState", error ? "error" : "normal");
    _messageLabel->style()->unpolish(_messageLabel);
    _messageLabel->style()->polish(_messageLabel);
    _messageLabel->setVisible(!message.isEmpty());
}

#endif // QT_GUI_LIB
