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

#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

class QComboBox;
class QDomElement;
class QLabel;
class QLineEdit;
class QPushButton;
class QStatusBar;
class QTabWidget;
#include <QThread>
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
        QString topVisibleNode;
        int topVisibleOffset = 0;
        int verticalScrollValue = 0;
        int horizontalScrollValue = 0;
    };

    struct CameraPage
    {
        Framegrabber::CameraControlCapability capability;
        QWidget* widget = nullptr;
        QComboBox* cameraCombo = nullptr;
        QTreeWidget* cameraTree = nullptr;
        QToolButton* refreshButton = nullptr;
    };

    Framegrabber* _framegrabber = nullptr;
    Framegrabber::CallbackId _statusCallbackId = 0;
    Framegrabber::CallbackId _nodeCallbackId = 0;
    QThread* _operationThread = nullptr;
    bool _shuttingDown = false;
    bool _operationActive = false;
    bool _connectionAttempted = false;
    bool _grabbing = false;
    bool _updatingDeviceUi = false;

    QComboBox* _boardCombo = nullptr;
    QToolButton* _grabOneButton = nullptr;
    QToolButton* _grabLiveButton = nullptr;

    QComboBox* _appletDmaCombo = nullptr;
    QTreeWidget* _appletTree = nullptr;
    QLineEdit* _appletPathEdit = nullptr;
    QPushButton* _loadAppletButton = nullptr;


    QTabWidget* _tabs = nullptr;
    std::vector<std::unique_ptr<CameraPage>> _cameraPages;

    QStatusBar* _statusBar = nullptr;
    QLabel* _statusLabel = nullptr;
    QLabel* _messageLabel = nullptr;
    QLabel* _loadingLabel = nullptr;

    void buildUi();
    QWidget* createSetupTab();
    std::unique_ptr<CameraPage> createCameraPage(
        const Framegrabber::CameraControlCapability& capability);
    void registerCallbacks();

    void startAutomaticAppletLoad();
    void startAppletLoad(const QString& path);
    void startAppletUnload();
    void startCameraRefresh(Framegrabber::CameraTransport transport);
    void setOperationActive(bool active);
    void applyConnectionState(bool opened);
    void updateGrabState(bool grabbing);
    void updateStatusBubble();
    void refreshDmaSelectors();
    void rebuildCameraTabs();
    void clearCameraTabs();
    CameraPage* cameraPage(Framegrabber::CameraTransport transport) const;
    void refreshCameraSelector(CameraPage& page);

    void rebuildAppletTree();
    void rebuildCameraTree(CameraPage& page);
    void populateAppletFeatureTree(
        QTreeWidget* tree,
        const std::vector<Framegrabber::AppletFeatureNode>& nodes,
        unsigned int dmaIndex);
    void addAppletFeatureNode(QTreeWidget* tree,
                              QTreeWidgetItem* parent,
                              const Framegrabber::AppletFeatureNode& node,
                              unsigned int dmaIndex);
    QWidget* createAppletFeatureEditor(
        const Framegrabber::AppletFeatureNode& node,
        unsigned int dmaIndex);
    void populateFeatureTree(QTreeWidget* tree,
                             const QString& xml,
                             TreeSource source,
                             Framegrabber::CameraTransport transport,
                             unsigned int dmaIndex);
    void addCategory(QTreeWidget* tree,
                     QTreeWidgetItem* parent,
                     const QDomElement& root,
                     const QDomElement& category,
                     TreeSource source,
                     Framegrabber::CameraTransport transport,
                     unsigned int dmaIndex,
                     QSet<QString>& visiting);
    QWidget* createFeatureEditor(const QDomElement& node,
                                 const QString& featureName,
                                 TreeSource source,
                                 Framegrabber::CameraTransport transport,
                                 unsigned int dmaIndex);
    bool readFeature(TreeSource source,
                     Framegrabber::CameraTransport transport,
                     unsigned int dmaIndex,
                     const QString& name,
                     Framegrabber::ParameterValue& value) const;
    bool writeFeature(TreeSource source,
                      Framegrabber::CameraTransport transport,
                      unsigned int dmaIndex,
                      const QString& name,
                      const Framegrabber::ParameterValue& value);

    TreeState captureTreeState(QTreeWidget* tree) const;
    void restoreTreeState(QTreeWidget* tree, const TreeState& state);
    void collectExpandedNodes(QTreeWidgetItem* item, QSet<QString>& nodes) const;
    void showStatusMessage(const QString& message, bool error = false);

    template <typename Func, typename Cleanup>
    void runAsyncWrite(Func&& writeFunc, Cleanup&& cleanupFunc) {
        setOperationActive(true);
        auto* success = new bool(false);
        auto write = std::make_shared<std::decay_t<Func>>(std::forward<Func>(writeFunc));
        auto cleanup = std::make_shared<std::decay_t<Cleanup>>(std::forward<Cleanup>(cleanupFunc));
        QThread* worker = QThread::create([write, success]() {
            *success = (*write)();
        });
        connect(worker, &QThread::finished, this, [this, success, cleanup, worker]() {
            setOperationActive(false);
            (*cleanup)(*success);
            delete success;
            worker->deleteLater();
        });
        worker->start();
    }
};

#endif // QT_GUI_LIB
#endif // QFRAMEGRABBERWIDGET_H
