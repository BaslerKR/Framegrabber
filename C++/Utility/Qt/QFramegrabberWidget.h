#ifndef QFRAMEGRABBERWIDGET_H
#define QFRAMEGRABBERWIDGET_H

/**
 * @file QFramegrabberWidget.h
 * @brief Qt control panel for board selection, configuration snapshots, acquisition, and features.
 */

#ifdef QT_GUI_LIB
#include "FramegrabberControlUi.h"

#include "Framegrabber.h"

#include <QPointer>
#include <QSet>
#include <QWidget>

#include <functional>
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

class QFramegrabberWidget final : public QWidget, protected FramegrabberControlUi
{
    Q_OBJECT

public:
    /**
     * @brief Presents an open or save file dialog; returns an empty path when cancelled.
     *
     * Load, Save and missing-applet selection all use it on the GUI thread. Without a
     * presenter the widget uses Qt's non-native file dialog and needs no theme.
     */
    using FileDialogPresenter = std::function<QString(bool save,
                                                      const QString& title,
                                                      const QString& directory,
                                                      const QString& filter)>;

    explicit QFramegrabberWidget(QWidget* parent = nullptr, Framegrabber* framegrabber = nullptr);
    ~QFramegrabberWidget() override;

    void prepareForShutdown();
    /** @brief Replaces the plain Qt file dialog, for example with a consumer-themed one. */
    void setFileDialogPresenter(FileDialogPresenter presenter);

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
    QSet<QThread*> _parameterThreads;
    bool _shuttingDown = false;
    bool _operationActive = false;
    /** @brief Another control surface (for example an API call) holds the lifecycle gate. */
    bool _externalControl = false;
    /** @brief Connection generation the device views were last built for. */
    std::uint64_t _observedGeneration = 0;
    int _pendingParameterWrites = 0;
    bool _grabbing = false;
    bool _updatingDeviceUi = false;
    FileDialogPresenter _fileDialogPresenter;




    std::vector<std::unique_ptr<CameraPage>> _cameraPages;


    void buildUi();
    std::unique_ptr<CameraPage> createCameraPage(
        const Framegrabber::CameraControlCapability& capability);
    void registerCallbacks();

    /**
     * @brief Runs one lifecycle command on a worker while holding the shared control gate.
     * @returns false without side effects when another surface owns the gate.
     */
    bool startControlOperation(const QString& message,
                               std::function<bool()> work,
                               std::function<void(bool)> finished);
    QString presentFileDialog(bool save,
                              const QString& title,
                              const QString& directory,
                              const QString& filter);
    void startAutomaticAppletLoad();
    void startAppletCatalogRefresh();
    void startAppletLoad(const QString& path);
    void startClose();
    void startConfigurationLoad(const QString& configurationPath);
    void startConfigurationLoadWithApplet(const QString& configurationPath,
                                          const QString& appletPath);
    void startConfigurationSave(const QString& configurationPath);
    void startCameraRefresh(Framegrabber::CameraTransport transport);
    void refreshAppletCatalogSelector();
    void reportLastError(const QString& fallback);
    void setOperationActive(bool active);
    void applyConnectionState(bool opened);
    void updateGrabState(bool grabbing);
    void updateStatusLabel();
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
    bool featureVisible(const QDomElement& node, TreeSource source) const;
    bool featureReadable(const QDomElement& node, TreeSource source) const;
    bool featureWritable(const QDomElement& node, TreeSource source) const;
    bool readFeature(TreeSource source,
                     Framegrabber::CameraTransport transport,
                     unsigned int dmaIndex,
                     const QString& name,
                     Framegrabber::ParameterValue& value) const;
    bool writeFeature(TreeSource source,
                      Framegrabber::CameraTransport transport,
                      unsigned int dmaIndex,
                      const QString& name,
                      const Framegrabber::ParameterValue& value,
                      bool verifyReadBack = true);
    void refreshFeatureTree(TreeSource source,
                            Framegrabber::CameraTransport transport,
                            unsigned int dmaIndex);

    TreeState captureTreeState(QTreeWidget* tree) const;
    void restoreTreeState(QTreeWidget* tree, const TreeState& state);
    void collectExpandedNodes(QTreeWidgetItem* item, QSet<QString>& nodes) const;
    /** Routes operation diagnostics to application logs. */
    void logMessage(const QString& message, bool error = false);

    template <typename Func, typename Cleanup>
    void runAsyncWrite(Func&& writeFunc, Cleanup&& cleanupFunc) {
        ++_pendingParameterWrites;
        setOperationActive(true);
        const auto success = std::make_shared<bool>(false);
        auto write = std::make_shared<std::decay_t<Func>>(std::forward<Func>(writeFunc));
        auto cleanup = std::make_shared<std::decay_t<Cleanup>>(std::forward<Cleanup>(cleanupFunc));
        QThread* worker = QThread::create([write, success]() {
            *success = (*write)();
        });
        worker->setParent(this);
        _parameterThreads.insert(worker);
        connect(worker, &QThread::finished, this, [this, success, cleanup, worker]() {
            _parameterThreads.remove(worker);
            if (_pendingParameterWrites > 0) {
                --_pendingParameterWrites;
            }
            if (!_shuttingDown) {
                setOperationActive(_operationThread != nullptr || _pendingParameterWrites > 0);
                (*cleanup)(*success);
            }
            worker->deleteLater();
        });
        worker->start();
    }
};

#endif // QT_GUI_LIB
#endif // QFRAMEGRABBERWIDGET_H
