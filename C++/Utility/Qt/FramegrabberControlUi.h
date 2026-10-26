#pragma once
#include <QWidget>
#include <QString>
class QComboBox; class QToolButton; class QTreeWidget; class QStatusBar; class QLabel;
class QLineEdit; class QTabWidget; class OneGenFeatureTree;

/** @brief Owns the device control composition without SDK or acquisition dependencies. */
struct FramegrabberControlUi {
    /** @brief Creates the same controls used by the runtime widget. */
    void create(QWidget* owner);
    QComboBox* _boardCombo = nullptr;
    QToolButton* _grabOneButton = nullptr;
    QToolButton* _grabLiveButton = nullptr;
    QTabWidget* _tabs = nullptr;
    QStatusBar* _statusBar = nullptr;
    QLabel* _statusLabel = nullptr;
    QComboBox* _appletDmaCombo = nullptr;
    QTreeWidget* _appletTree = nullptr;
    QLineEdit* _appletPathEdit = nullptr;
    QToolButton* _loadButton = nullptr;
    QToolButton* _saveConfigurationButton = nullptr;
    /** @brief Creates the applet configuration tab used by the runtime widget. */
    QWidget* createSetupTab(QWidget* owner);
};
