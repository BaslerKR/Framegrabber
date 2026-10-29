#include "FramegrabberControlUi.h"
#include <QtWidgets>


void FramegrabberControlUi::create(QWidget* owner) {
    owner->setProperty("actionDensity", "compact");
    owner->setProperty("controlRole", "devicePanel");
    owner->setWindowTitle(QCoreApplication::translate("QFramegrabberWidget", "Basler Frame Grabber Configuration"));
    owner->setMinimumSize(300, 350);

    _boardCombo = new QComboBox(owner);
    _boardCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    _grabOneButton = new QToolButton(owner);
    _grabOneButton->setIcon(QIcon(QStringLiteral(":/Resources/icon/camera-48.png")));
    _grabOneButton->setToolTip(QCoreApplication::translate("QFramegrabberWidget", "Grab one frame per DMA"));

    _grabLiveButton = new QToolButton(owner);
    _grabLiveButton->setCheckable(true);
    QIcon grabIcon;
    grabIcon.addFile(
        QStringLiteral(":/Resources/icon/cameras-48.png"),
        QSize(),
        QIcon::Normal,
        QIcon::Off);
    grabIcon.addFile(
        QStringLiteral(":/Resources/icon/pause-48.png"),
        QSize(),
        QIcon::Normal,
        QIcon::On);
    _grabLiveButton->setIcon(grabIcon);
    _grabLiveButton->setToolTip(QCoreApplication::translate("QFramegrabberWidget", "Start or stop live acquisition"));

    auto* selectorLayout = new QHBoxLayout;
    selectorLayout->setObjectName(QStringLiteral("DeviceSelectorLayout"));
    selectorLayout->setProperty("layoutRole", "row");
    selectorLayout->addWidget(_boardCombo);

    auto* toolLayout = new QHBoxLayout;
    toolLayout->setObjectName(QStringLiteral("DeviceToolLayout"));
    toolLayout->setProperty("layoutRole", "actions");
    toolLayout->addWidget(_grabOneButton);
    toolLayout->addWidget(_grabLiveButton);

    auto* topLayout = new QHBoxLayout;
    topLayout->setObjectName(QStringLiteral("DeviceTopBarLayout"));
    topLayout->setProperty("layoutRole", "content");
    topLayout->addLayout(selectorLayout);
    topLayout->addLayout(toolLayout);

    _tabs = new QTabWidget(owner);
    _tabs->setObjectName(QStringLiteral("FramegrabberControlTabs"));
    _tabs->addTab(createSetupTab(owner), QCoreApplication::translate("QFramegrabberWidget", "Setup"));

    _statusBar = new QStatusBar(owner);
    _statusBar->setObjectName(QStringLiteral("FramegrabberStatusBar"));
    _statusBar->setSizeGripEnabled(false);
    _statusLabel = new QLabel(owner);
    _statusLabel->setObjectName(QStringLiteral("FramegrabberStatusLabel"));
    _statusLabel->setAlignment(Qt::AlignCenter);
    _statusBar->addWidget(_statusLabel);

    auto* rootLayout = new QVBoxLayout;
    rootLayout->setObjectName(QStringLiteral("DeviceRootLayout"));
    rootLayout->setProperty("layoutRole", "flush");
    rootLayout->addLayout(topLayout);
    rootLayout->addWidget(_tabs);
    rootLayout->addWidget(_statusBar);
    owner->setLayout(rootLayout);


}

QWidget* FramegrabberControlUi::createSetupTab(QWidget* owner) {

    auto* page = new QWidget(owner);
    const auto iconButton = [page](const QString& icon, const char* toolTip) {
        auto* button = new QToolButton(page);
        button->setIcon(QIcon(QStringLiteral(":/Resources/icon/") + icon));
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setIconSize(QSize(16, 16));
        button->setToolTip(QCoreApplication::translate("QFramegrabberWidget", toolTip));
        return button;
    };

    _appletCombo = new QComboBox(page);
    _appletCombo->setObjectName(QStringLiteral("FramegrabberAppletCombo"));
    _appletCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    _appletCombo->setToolTip(QCoreApplication::translate("QFramegrabberWidget", "Loadable applets for the selected board"));
    _appletRefreshButton = iconButton(QStringLiteral("refresh-48.png"), "Refresh the loadable applet list");
    _appletLoadButton = iconButton(QStringLiteral("connect-48.png"), "Load the selected applet");
    _closeButton = iconButton(QStringLiteral("disconnected-48.png"), "Release the board and unload the applet");

    auto* catalogLayout = new QHBoxLayout;
    catalogLayout->setObjectName(QStringLiteral("FramegrabberAppletCatalogLayout"));
    catalogLayout->addWidget(_appletCombo);
    catalogLayout->addWidget(_appletRefreshButton);
    catalogLayout->addWidget(_appletLoadButton);
    catalogLayout->addWidget(_closeButton);

    _appletPathEdit = new QLineEdit(page);
    _appletPathEdit->setReadOnly(true);
    _loadButton = iconButton(QStringLiteral("opened-folder-48.png"), "Load an applet file or configuration");

    _saveConfigurationButton = iconButton(QStringLiteral("save-48.png"), "Save current applet configuration");

    auto* pathLayout = new QHBoxLayout;
    pathLayout->setObjectName(QStringLiteral("FramegrabberAppletPathLayout"));
    pathLayout->addWidget(_loadButton);
    pathLayout->addWidget(_saveConfigurationButton);
    pathLayout->addWidget(_appletPathEdit);

    _appletDmaCombo = new QComboBox(page);

    auto* selectorLayout = new QHBoxLayout;
    selectorLayout->setObjectName(QStringLiteral("FramegrabberAppletSelectorLayout"));
    selectorLayout->addWidget(_appletDmaCombo);

    _appletTree = new QTreeWidget(page);
    _appletTree->setObjectName(QStringLiteral("FramegrabberAppletFeaturesTree"));
    _appletTree->setProperty("treeRole", QStringLiteral("DeviceFeatureTree"));
    _appletTree->setHeaderLabels({QCoreApplication::translate("QFramegrabberWidget", "Feature"), QCoreApplication::translate("QFramegrabberWidget", "Value")});

    auto* layout = new QVBoxLayout;
    layout->setObjectName(QStringLiteral("FramegrabberAppletLayout"));
    layout->addLayout(catalogLayout);
    layout->addLayout(pathLayout);
    layout->addLayout(selectorLayout);
    layout->addWidget(_appletTree);
    page->setLayout(layout);




    return page;

}
