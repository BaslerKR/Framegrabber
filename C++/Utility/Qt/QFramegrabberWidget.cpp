#include "QFramegrabberWidget.h"

#ifdef QT_GUI_LIB

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDomDocument>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
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
    for (const std::string& name : _framegrabber->getCachedFramegrabberList())
    {
        _boardCombo->addItem(QString::fromStdString(name));
    }
    _configurationPathEdit->setText(
        QString::fromStdString(_framegrabber->configurationPath()));
    _bufferCountSpin->setValue(static_cast<int>(_framegrabber->dmaBufferCount()));
    applyConnectionState(_framegrabber->isOpened());
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

    _refreshBoardsButton = new QToolButton(this);
    _refreshBoardsButton->setIcon(QIcon(QStringLiteral(":/Resources/Icons/icons8-refresh-48.png")));
    _refreshBoardsButton->setToolTip(tr("Refresh boards"));

    _connectButton = new QToolButton(this);
    _connectButton->setCheckable(true);
    _connectButton->setToolTip(tr("Open or close board"));
    QIcon connectionIcon;
    connectionIcon.addFile(
        QStringLiteral(":/Resources/Icons/icons8-connect-48.png"),
        QSize(),
        QIcon::Normal,
        QIcon::Off);
    connectionIcon.addFile(
        QStringLiteral(":/Resources/Icons/icons8-disconnected-48.png"),
        QSize(),
        QIcon::Normal,
        QIcon::On);
    _connectButton->setIcon(connectionIcon);

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
    selectorLayout->addWidget(_refreshBoardsButton);

    auto* toolLayout = new QHBoxLayout;
    toolLayout->setObjectName(QStringLiteral("DeviceToolLayout"));
    toolLayout->addWidget(_connectButton);
    toolLayout->addWidget(_grabOneButton);
    toolLayout->addWidget(_grabLiveButton);

    auto* topLayout = new QHBoxLayout;
    topLayout->setObjectName(QStringLiteral("DeviceTopBarLayout"));
    topLayout->addLayout(selectorLayout);
    topLayout->addLayout(toolLayout);

    auto* tabs = new QTabWidget(this);
    tabs->setObjectName(QStringLiteral("FramegrabberControlTabs"));
    tabs->addTab(createAppletTab(), tr("Applet"));
    tabs->addTab(createCameraTab(), tr("CXP Camera"));
    tabs->addTab(createConfigurationTab(), tr("Configuration"));

    _statusBar = new QStatusBar(this);
    _statusBar->setObjectName(QStringLiteral("FramegrabberStatusBar"));
    _statusBar->setSizeGripEnabled(false);
    _statusLabel = new QLabel(this);
    _statusLabel->setObjectName(QStringLiteral("FramegrabberStatusLabel"));
    _statusLabel->setAlignment(Qt::AlignCenter);
    _messageLabel = new QLabel(this);
    _messageLabel->setObjectName(QStringLiteral("FramegrabberMessageLabel"));
    _messageLabel->setProperty("messageState", "normal");
    _messageLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    _statusBar->addWidget(_statusLabel);
    _statusBar->addWidget(_messageLabel, 1);

    auto* rootLayout = new QVBoxLayout;
    rootLayout->setObjectName(QStringLiteral("DeviceRootLayout"));
    rootLayout->addLayout(topLayout);
    rootLayout->addWidget(tabs);
    rootLayout->addWidget(_statusBar);
    setLayout(rootLayout);

    connect(_refreshBoardsButton, &QToolButton::clicked, this, [this]
    {
        startBoardRefresh();
    });
    connect(_connectButton, &QToolButton::toggled, this, [this](const bool checked)
    {
        startOpenOperation(checked);
    });
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
}

QWidget* QFramegrabberWidget::createAppletTab()
{
    auto* page = new QWidget(this);
    _appletDmaCombo = new QComboBox(page);
    _refreshAppletButton = new QToolButton(page);
    _refreshAppletButton->setIcon(
        QIcon(QStringLiteral(":/Resources/Icons/icons8-refresh-48.png")));
    _refreshAppletButton->setToolTip(tr("Refresh applet features"));

    auto* selectorLayout = new QHBoxLayout;
    selectorLayout->setObjectName(QStringLiteral("FramegrabberAppletSelectorLayout"));
    selectorLayout->addWidget(_appletDmaCombo);
    selectorLayout->addWidget(_refreshAppletButton);

    _appletTree = new QTreeWidget(page);
    _appletTree->setObjectName(QStringLiteral("FramegrabberAppletFeaturesTree"));
    _appletTree->setProperty("treeRole", QStringLiteral("DeviceFeatureTree"));
    _appletTree->setHeaderLabels({tr("Feature"), tr("Value")});
    _appletTree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    _appletTree->header()->setSectionResizeMode(1, QHeaderView::Stretch);

    auto* layout = new QVBoxLayout;
    layout->setObjectName(QStringLiteral("FramegrabberAppletLayout"));
    layout->addLayout(selectorLayout);
    layout->addWidget(_appletTree);
    page->setLayout(layout);

    connect(_appletDmaCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]
    {
        rebuildAppletTree();
    });
    connect(_refreshAppletButton, &QToolButton::clicked, this, [this]
    {
        rebuildAppletTree();
    });
    return page;
}

QWidget* QFramegrabberWidget::createCameraTab()
{
    auto* page = new QWidget(this);
    _cameraCombo = new QComboBox(page);

    _refreshCamerasButton = new QToolButton(page);
    _refreshCamerasButton->setIcon(
        QIcon(QStringLiteral(":/Resources/Icons/icons8-refresh-48.png")));
    _refreshCamerasButton->setToolTip(tr("Discover CXP cameras"));

    _connectCameraButton = new QToolButton(page);
    _connectCameraButton->setIcon(
        QIcon(QStringLiteral(":/Resources/Icons/icons8-connect-48.png")));
    _connectCameraButton->setToolTip(tr("Connect selected CXP camera"));

    auto* selectorLayout = new QHBoxLayout;
    selectorLayout->setObjectName(QStringLiteral("FramegrabberCameraSelectorLayout"));
    selectorLayout->addWidget(_cameraCombo);
    selectorLayout->addWidget(_refreshCamerasButton);
    selectorLayout->addWidget(_connectCameraButton);

    _cameraTree = new QTreeWidget(page);
    _cameraTree->setObjectName(QStringLiteral("FramegrabberCameraFeaturesTree"));
    _cameraTree->setProperty("treeRole", QStringLiteral("DeviceFeatureTree"));
    _cameraTree->setHeaderLabels({tr("Feature"), tr("Value")});
    _cameraTree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    _cameraTree->header()->setSectionResizeMode(1, QHeaderView::Stretch);

    auto* layout = new QVBoxLayout;
    layout->setObjectName(QStringLiteral("FramegrabberCameraLayout"));
    layout->addLayout(selectorLayout);
    layout->addWidget(_cameraTree);
    page->setLayout(layout);

    connect(_refreshCamerasButton, &QToolButton::clicked, this, [this]
    {
        startCameraRefresh();
    });
    connect(_connectCameraButton, &QToolButton::clicked, this, [this]
    {
        if (!_framegrabber || _cameraCombo->currentIndex() < 0)
        {
            return;
        }
        const unsigned int dmaIndex = _cameraCombo->currentData().toUInt();
        if (_framegrabber->connectCamera(dmaIndex))
        {
            rebuildCameraTree();
            showStatusMessage(tr("CXP camera connected."));
        }
        else
        {
            showStatusMessage(tr("Failed to connect the CXP camera."), true);
        }
    });
    connect(_cameraCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]
    {
        rebuildCameraTree();
    });
    return page;
}

QWidget* QFramegrabberWidget::createConfigurationTab()
{
    auto* page = new QWidget(this);
    _configurationPathEdit = new QLineEdit(page);
    _configurationPathEdit->setReadOnly(true);

    _loadConfigurationButton = new QPushButton(tr("Load"), page);
    _reloadConfigurationButton = new QPushButton(tr("Reload"), page);
    _saveConfigurationButton = new QPushButton(tr("Save"), page);
    _saveConfigurationAsButton = new QPushButton(tr("Save As"), page);

    auto* pathLayout = new QHBoxLayout;
    pathLayout->setObjectName(QStringLiteral("FramegrabberConfigurationPathLayout"));
    pathLayout->addWidget(_configurationPathEdit);
    pathLayout->addWidget(_loadConfigurationButton);

    auto* actionLayout = new QHBoxLayout;
    actionLayout->setObjectName(QStringLiteral("FramegrabberConfigurationActionLayout"));
    actionLayout->addWidget(_reloadConfigurationButton);
    actionLayout->addWidget(_saveConfigurationButton);
    actionLayout->addWidget(_saveConfigurationAsButton);

    _configurationStateLabel = new QLabel(tr("No configuration loaded"), page);
    _configurationStateLabel->setObjectName(
        QStringLiteral("FramegrabberConfigurationStateLabel"));
    _configurationStateLabel->setProperty("state", "idle");

    _bufferCountSpin = new QSpinBox(page);
    _bufferCountSpin->setRange(1, 128);

    auto* formLayout = new QFormLayout;
    formLayout->setObjectName(QStringLiteral("FramegrabberConfigurationFormLayout"));
    formLayout->addRow(tr("Configuration"), pathLayout);
    formLayout->addRow(tr("DMA buffers"), _bufferCountSpin);
    formLayout->addRow(QString(), actionLayout);
    formLayout->addRow(tr("State"), _configurationStateLabel);
    page->setLayout(formLayout);

    connect(_loadConfigurationButton, &QPushButton::clicked, this, [this]
    {
        const QString path = QFileDialog::getOpenFileName(
            this,
            tr("Load frame grabber configuration"),
            _configurationPathEdit->text(),
            tr("Frame Grabber Configuration (*.mcf *.hap *.dll *.so);;All Files (*)"));
        if (!path.isEmpty())
        {
            startConfigurationLoad(path);
        }
    });
    connect(_reloadConfigurationButton, &QPushButton::clicked, this, [this]
    {
        if (!_configurationPathEdit->text().isEmpty())
        {
            startConfigurationLoad(_configurationPathEdit->text());
        }
    });
    connect(_saveConfigurationButton, &QPushButton::clicked, this, [this]
    {
        if (!_framegrabber || _configurationPathEdit->text().isEmpty())
        {
            return;
        }
        if (_framegrabber->saveConfiguration(_configurationPathEdit->text().toStdString()))
        {
            markConfigurationDirty(false);
            showStatusMessage(tr("Configuration saved."));
        }
        else
        {
            showStatusMessage(tr("Failed to save the configuration."), true);
        }
    });
    connect(_saveConfigurationAsButton, &QPushButton::clicked, this, [this]
    {
        if (!_framegrabber)
        {
            return;
        }
        const QString path = QFileDialog::getSaveFileName(
            this,
            tr("Save frame grabber configuration"),
            _configurationPathEdit->text(),
            tr("MCF Configuration (*.mcf)"));
        if (!path.isEmpty() && _framegrabber->saveConfiguration(path.toStdString()))
        {
            _configurationPathEdit->setText(path);
            _framegrabber->setConfigurationPath(path.toStdString());
            markConfigurationDirty(false);
            showStatusMessage(tr("Configuration saved."));
        }
    });
    connect(_bufferCountSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](const int count)
    {
        if (_framegrabber)
        {
            _framegrabber->setDMABufferCount(static_cast<std::size_t>(count));
        }
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
                const unsigned int dmaIndex,
                const std::string&)
        {
            if (!guard)
            {
                return;
            }
            QMetaObject::invokeMethod(
                guard,
                [guard, source, dmaIndex]
                {
                    if (!guard)
                    {
                        return;
                    }
                    guard->markConfigurationDirty(true);
                    if (source == Framegrabber::FeatureSource::Applet
                        && guard->_appletDmaCombo->currentData().toUInt() == dmaIndex)
                    {
                        guard->rebuildAppletTree();
                    }
                    else if (source == Framegrabber::FeatureSource::Camera
                             && guard->_cameraCombo->currentData().toUInt() == dmaIndex)
                    {
                        guard->rebuildCameraTree();
                    }
                },
                Qt::QueuedConnection);
        });
}

void QFramegrabberWidget::startBoardRefresh()
{
    if (!_framegrabber || _operationThread || _shuttingDown)
    {
        return;
    }

    const auto names = std::make_shared<std::vector<std::string>>();
    QPointer<QFramegrabberWidget> guard(this);
    QThread* worker = QThread::create([framegrabber = _framegrabber, names]
    {
        *names = framegrabber->getUpdatedFramegrabberList();
    });
    _operationThread = worker;
    setOperationActive(true);
    showStatusMessage(tr("Scanning frame grabber boards..."));
    connect(worker, &QThread::finished, this, [guard, worker, names]
    {
        worker->deleteLater();
        if (!guard)
        {
            return;
        }
        guard->_operationThread = nullptr;
        guard->_boardCombo->clear();
        for (const std::string& name : *names)
        {
            guard->_boardCombo->addItem(QString::fromStdString(name));
        }
        guard->setOperationActive(false);
        guard->showStatusMessage(tr("Board scan finished."));
    });
    worker->start();
}

void QFramegrabberWidget::startOpenOperation(const bool open)
{
    if (!_framegrabber || _operationThread || _shuttingDown)
    {
        QSignalBlocker blocker(_connectButton);
        _connectButton->setChecked(_framegrabber && _framegrabber->isOpened());
        return;
    }

    if (open && _configurationPathEdit->text().isEmpty())
    {
        QSignalBlocker blocker(_connectButton);
        _connectButton->setChecked(false);
        showStatusMessage(tr("Load an applet or MCF configuration first."), true);
        return;
    }

    const QString boardName = _boardCombo->currentText();
    const auto success = std::make_shared<bool>(false);
    QPointer<QFramegrabberWidget> guard(this);
    QThread* worker = QThread::create(
        [framegrabber = _framegrabber, boardName, open, success]
        {
            if (open)
            {
                *success = framegrabber->open(boardName.toStdString());
            }
            else
            {
                framegrabber->close();
                *success = true;
            }
        });
    _operationThread = worker;
    _connectionAttempted = true;
    setOperationActive(true);
    showStatusMessage(open ? tr("Opening frame grabber...") : tr("Closing frame grabber..."));
    connect(worker, &QThread::finished, this, [guard, worker, open, success]
    {
        worker->deleteLater();
        if (!guard)
        {
            return;
        }
        guard->_operationThread = nullptr;
        guard->setOperationActive(false);
        guard->applyConnectionState(open && *success);
        if (open && *success)
        {
            guard->markConfigurationDirty(false);
            guard->showStatusMessage(tr("Frame grabber opened."));
        }
        else if (open)
        {
            guard->showStatusMessage(tr("Failed to open the frame grabber."), true);
        }
        else
        {
            guard->showStatusMessage(tr("Frame grabber closed."));
        }
    });
    worker->start();
}

void QFramegrabberWidget::startConfigurationLoad(const QString& path)
{
    if (!_framegrabber || _operationThread || _shuttingDown)
    {
        return;
    }

    const auto success = std::make_shared<bool>(false);
    QPointer<QFramegrabberWidget> guard(this);
    QThread* worker = QThread::create(
        [framegrabber = _framegrabber, path, success]
        {
            *success = framegrabber->loadConfiguration(path.toStdString());
        });
    _operationThread = worker;
    setOperationActive(true);
    showStatusMessage(tr("Loading configuration..."));
    connect(worker, &QThread::finished, this, [guard, worker, path, success]
    {
        worker->deleteLater();
        if (!guard)
        {
            return;
        }
        guard->_operationThread = nullptr;
        guard->setOperationActive(false);
        if (*success)
        {
            guard->_configurationPathEdit->setText(path);
            guard->markConfigurationDirty(false);
            guard->applyConnectionState(guard->_framegrabber->isOpened());
            guard->showStatusMessage(tr("Configuration loaded."));
        }
        else
        {
            guard->showStatusMessage(tr("Failed to load the configuration."), true);
        }
    });
    worker->start();
}

void QFramegrabberWidget::startCameraRefresh()
{
    if (!_framegrabber || _operationThread || _shuttingDown)
    {
        return;
    }

    const auto success = std::make_shared<bool>(false);
    QPointer<QFramegrabberWidget> guard(this);
    QThread* worker = QThread::create([framegrabber = _framegrabber, success]
    {
        *success = framegrabber->refreshCameras();
    });
    _operationThread = worker;
    setOperationActive(true);
    showStatusMessage(tr("Scanning CXP cameras..."));
    connect(worker, &QThread::finished, this, [guard, worker, success]
    {
        worker->deleteLater();
        if (!guard)
        {
            return;
        }
        guard->_operationThread = nullptr;
        guard->setOperationActive(false);
        guard->refreshCameraSelector();
        guard->showStatusMessage(
            *success ? tr("CXP camera scan finished.") : tr("CXP camera scan failed."),
            !*success);
    });
    worker->start();
}

void QFramegrabberWidget::setOperationActive(const bool active)
{
    _operationActive = active;
    const bool opened = _framegrabber && _framegrabber->isOpened();
    _boardCombo->setEnabled(!active && !opened);
    _refreshBoardsButton->setEnabled(!active && !opened);
    _connectButton->setEnabled(!active);
    _loadConfigurationButton->setEnabled(!active && !_grabbing);
    _reloadConfigurationButton->setEnabled(!active && !_grabbing);
}

void QFramegrabberWidget::applyConnectionState(const bool opened)
{
    {
        QSignalBlocker blocker(_connectButton);
        _connectButton->setChecked(opened);
    }
    _boardCombo->setEnabled(!opened && !_operationActive);
    _refreshBoardsButton->setEnabled(!opened && !_operationActive);
    _grabOneButton->setEnabled(opened && !_grabbing);
    _grabLiveButton->setEnabled(opened);
    _refreshAppletButton->setEnabled(opened);
    _refreshCamerasButton->setEnabled(opened && !_grabbing);
    _connectCameraButton->setEnabled(opened && !_grabbing);
    _saveConfigurationButton->setEnabled(opened);
    _saveConfigurationAsButton->setEnabled(opened);

    if (opened)
    {
        refreshDmaSelectors();
        refreshCameraSelector();
        rebuildAppletTree();
    }
    else
    {
        _appletDmaCombo->clear();
        _cameraCombo->clear();
        _appletTree->clear();
        _cameraTree->clear();
    }
    updateStatusBubble();
}

void QFramegrabberWidget::updateGrabState(const bool grabbing)
{
    _grabbing = grabbing;
    {
        QSignalBlocker blocker(_grabLiveButton);
        _grabLiveButton->setChecked(grabbing);
    }
    _grabOneButton->setEnabled(_framegrabber && _framegrabber->isOpened() && !grabbing);
    _loadConfigurationButton->setEnabled(!grabbing && !_operationActive);
    _reloadConfigurationButton->setEnabled(!grabbing && !_operationActive);
    _refreshCamerasButton->setEnabled(!grabbing && !_operationActive);
    _connectCameraButton->setEnabled(!grabbing && !_operationActive);
    updateStatusBubble();
}

void QFramegrabberWidget::updateStatusBubble()
{
    const bool opened = _framegrabber && _framegrabber->isOpened();
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

void QFramegrabberWidget::refreshCameraSelector()
{
    const QVariant previous = _cameraCombo->currentData();
    _cameraCombo->clear();
    if (!_framegrabber)
    {
        return;
    }
    for (const Framegrabber::CameraInfo& camera : _framegrabber->getCachedCameraList())
    {
        _cameraCombo->addItem(
            tr("DMA %1 - %2")
                .arg(camera.dmaIndex)
                .arg(QString::fromStdString(camera.displayName())),
            camera.dmaIndex);
    }
    const int previousIndex = _cameraCombo->findData(previous);
    if (previousIndex >= 0)
    {
        _cameraCombo->setCurrentIndex(previousIndex);
    }
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
    populateFeatureTree(
        _appletTree,
        QString::fromUtf8(_framegrabber->getAppletFeatureXml(dmaIndex).c_str()),
        TreeSource::Applet,
        dmaIndex);
}

void QFramegrabberWidget::rebuildCameraTree()
{
    if (!_framegrabber || !_framegrabber->isOpened()
        || _cameraCombo->currentIndex() < 0)
    {
        _cameraTree->clear();
        return;
    }
    const unsigned int dmaIndex = _cameraCombo->currentData().toUInt();
    const QString xml = QString::fromUtf8(
        _framegrabber->getCameraFeatureXml(dmaIndex).c_str());
    if (xml.isEmpty())
    {
        _cameraTree->clear();
        return;
    }
    populateFeatureTree(_cameraTree, xml, TreeSource::Camera, dmaIndex);
}

void QFramegrabberWidget::populateFeatureTree(QTreeWidget* tree,
                                              const QString& xml,
                                              const TreeSource source,
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
        addCategory(tree, nullptr, root, rootCategory, source, dmaIndex, visiting);
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
                addCategory(tree, nullptr, root, category, source, dmaIndex, visiting);
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
        if (QWidget* editor = createFeatureEditor(feature, featureName, source, dmaIndex))
        {
            tree->setItemWidget(item, 1, editor);
        }
    }
    visiting.remove(categoryName);
}

QWidget* QFramegrabberWidget::createFeatureEditor(const QDomElement& node,
                                                  const QString& featureName,
                                                  const TreeSource source,
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
    if (!readFeature(source, dmaIndex, featureName, current)
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
            if (!writeFeature(source, dmaIndex, featureName, updated))
            {
                QSignalBlocker blocker(checkBox);
                checkBox->setChecked(!value);
            }
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
            Framegrabber::ParameterValue updated = current;
            if (source == TreeSource::Applet)
            {
                if (!updateValueFromText(updated, combo->currentData().toString()))
                {
                    return;
                }
            }
            else
            {
                updated = combo->currentData().toString().toStdString();
            }
            writeFeature(source, dmaIndex, featureName, updated);
        });
        return combo;
    }

    if (tag == QStringLiteral("Command"))
    {
        auto* button = new QPushButton(tr("Execute"), this);
        connect(button, &QPushButton::clicked, this, [=]
        {
            bool success = false;
            if (_framegrabber && source == TreeSource::Camera)
            {
                success = _framegrabber->executeCameraCommand(
                    dmaIndex,
                    featureName.toStdString());
            }
            else if (_framegrabber)
            {
                success = _framegrabber->executeAppletCommand(
                    dmaIndex,
                    featureName.toStdString());
            }
            showStatusMessage(
                success ? tr("Command executed.") : tr("Command execution failed."),
                !success);
        });
        return button;
    }

    auto* edit = new QLineEdit(valueText(current), this);
    connect(edit, &QLineEdit::editingFinished, this, [=]() mutable
    {
        Framegrabber::ParameterValue updated = current;
        if (!updateValueFromText(updated, edit->text())
            || !writeFeature(source, dmaIndex, featureName, updated))
        {
            QSignalBlocker blocker(edit);
            edit->setText(valueText(current));
            showStatusMessage(tr("Failed to update '%1'.").arg(featureName), true);
        }
        else
        {
            current = std::move(updated);
            showStatusMessage(tr("Updated '%1'.").arg(featureName));
        }
    });
    return edit;
}

bool QFramegrabberWidget::readFeature(const TreeSource source,
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
        Framegrabber::FeatureSource::Camera,
        dmaIndex,
        name.toStdString(),
        value);
}

bool QFramegrabberWidget::writeFeature(const TreeSource source,
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
        Framegrabber::FeatureSource::Camera,
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
    return state;
}

void QFramegrabberWidget::restoreTreeState(QTreeWidget* tree, const TreeState& state)
{
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
        for (int index = 0; index < item->childCount(); ++index)
        {
            stack.push_back(item->child(index));
        }
    }
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

void QFramegrabberWidget::markConfigurationDirty(const bool dirty)
{
    _configurationDirty = dirty;
    if (_configurationPathEdit->text().isEmpty())
    {
        _configurationStateLabel->setText(tr("No configuration loaded"));
        _configurationStateLabel->setProperty("state", "idle");
    }
    else if (dirty)
    {
        _configurationStateLabel->setText(tr("Modified"));
        _configurationStateLabel->setProperty("state", "warning");
    }
    else
    {
        _configurationStateLabel->setText(tr("Saved"));
        _configurationStateLabel->setProperty("state", "normal");
    }
    _configurationStateLabel->style()->unpolish(_configurationStateLabel);
    _configurationStateLabel->style()->polish(_configurationStateLabel);
}

void QFramegrabberWidget::showStatusMessage(const QString& message, const bool error)
{
    _messageLabel->setText(message);
    _messageLabel->setProperty("messageState", error ? "error" : "normal");
    _messageLabel->style()->unpolish(_messageLabel);
    _messageLabel->style()->polish(_messageLabel);
}

#endif // QT_GUI_LIB
