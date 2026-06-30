#ifndef QFRAMEGRABBERWIDGET_H
#define QFRAMEGRABBERWIDGET_H

/**
 * @file QFramegrabberWidget.h
 * @brief Qt control panel for board selection, configuration snapshots, acquisition, and features.
 */

#ifdef QT_GUI_LIB

#include "Framegrabber.h"

#include <QPointer>
#include <QSet>
#include <QWidget>

class QComboBox;
class QDomElement;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QStatusBar;
class QThread;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

class QFramegrabberWidget final : public QWidget
{
    Q_OBJECT

public:
    explicit QFramegrabberWidget(QWidget* parent = nullptr, Framegrabber* framegrabber = nullptr);
    ~QFramegrabberWidget() override;

    void prepareForShutdown();

private:
    enum class TreeSource
    {
        Applet,
        Camera
    };

    struct TreeState
    {
        QSet<QString> expandedNodes;
        QString currentNode;
    };

    Framegrabber* _framegrabber = nullptr;
    Framegrabber::CallbackId _statusCallbackId = 0;
    Framegrabber::CallbackId _nodeCallbackId = 0;
    QThread* _operationThread = nullptr;
    bool _shuttingDown = false;
    bool _operationActive = false;
    bool _connectionAttempted = false;
    bool _grabbing = false;
    bool _configurationDirty = false;

    QComboBox* _boardCombo = nullptr;
    QToolButton* _refreshBoardsButton = nullptr;
    QToolButton* _connectButton = nullptr;
    QToolButton* _grabOneButton = nullptr;
    QToolButton* _grabLiveButton = nullptr;

    QComboBox* _appletDmaCombo = nullptr;
    QTreeWidget* _appletTree = nullptr;
    QToolButton* _refreshAppletButton = nullptr;

    QComboBox* _cameraCombo = nullptr;
    QTreeWidget* _cameraTree = nullptr;
    QToolButton* _refreshCamerasButton = nullptr;
    QToolButton* _connectCameraButton = nullptr;

    QLineEdit* _configurationPathEdit = nullptr;
    QPushButton* _loadConfigurationButton = nullptr;
    QPushButton* _reloadConfigurationButton = nullptr;
    QPushButton* _saveConfigurationButton = nullptr;
    QPushButton* _saveConfigurationAsButton = nullptr;
    QLabel* _configurationStateLabel = nullptr;
    QSpinBox* _bufferCountSpin = nullptr;

    QStatusBar* _statusBar = nullptr;
    QLabel* _statusLabel = nullptr;
    QLabel* _messageLabel = nullptr;

    void buildUi();
    QWidget* createAppletTab();
    QWidget* createCameraTab();
    QWidget* createConfigurationTab();
    void registerCallbacks();

    void startBoardRefresh();
    void startOpenOperation(bool open);
    void startConfigurationLoad(const QString& path);
    void startCameraRefresh();
    void setOperationActive(bool active);
    void applyConnectionState(bool opened);
    void updateGrabState(bool grabbing);
    void updateStatusBubble();
    void refreshDmaSelectors();
    void refreshCameraSelector();

    void rebuildAppletTree();
    void rebuildCameraTree();
    void populateFeatureTree(QTreeWidget* tree,
                             const QString& xml,
                             TreeSource source,
                             unsigned int dmaIndex);
    void addCategory(QTreeWidget* tree,
                     QTreeWidgetItem* parent,
                     const QDomElement& root,
                     const QDomElement& category,
                     TreeSource source,
                     unsigned int dmaIndex,
                     QSet<QString>& visiting);
    QWidget* createFeatureEditor(const QDomElement& node,
                                 const QString& featureName,
                                 TreeSource source,
                                 unsigned int dmaIndex);
    bool readFeature(TreeSource source,
                     unsigned int dmaIndex,
                     const QString& name,
                     Framegrabber::ParameterValue& value) const;
    bool writeFeature(TreeSource source,
                      unsigned int dmaIndex,
                      const QString& name,
                      const Framegrabber::ParameterValue& value);

    TreeState captureTreeState(QTreeWidget* tree) const;
    void restoreTreeState(QTreeWidget* tree, const TreeState& state);
    void collectExpandedNodes(QTreeWidgetItem* item, QSet<QString>& nodes) const;
    void markConfigurationDirty(bool dirty);
    void showStatusMessage(const QString& message, bool error = false);
};

#endif // QT_GUI_LIB
#endif // QFRAMEGRABBERWIDGET_H
