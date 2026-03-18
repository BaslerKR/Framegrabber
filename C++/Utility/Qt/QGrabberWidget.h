#ifndef QGRABBERWIDGET_H
#define QGRABBERWIDGET_H

#include <QWidget>
#include <QObject>
#include <QTreeWidget>
#include <QPushButton>
#include <QComboBox>
#include <QLabel>
#include <QSpinBox>
#include <QToolButton>
#include <QStatusBar>
#include <QCheckBox>
#include <QLineEdit>
#include <QDomElement>
#include "Framegrabber.h"

class QGrabberWidget : public QWidget
{
    Q_OBJECT
public:
    QGrabberWidget(QWidget *parent=nullptr, Framegrabber* grabber=nullptr);

    void generateFeaturesWidget(int dmaCnt);
    void generateChildItem(QDomElement value);

    void setAppletPath(QString path);
    void setMCFPath(QString path);
    QWidget *createNodeWidget(const QDomElement &node, QString featureName, int dmaIndex);
    void createMCFEditor(QString mcfPath);
    void refreshMCFEditor();


private:
    Framegrabber *_grabber = nullptr;
    QTreeWidget *_featuresWidget = nullptr;

    QToolButton *_toolConnect = nullptr;
    QToolButton *_toolGrabOne = nullptr;
    QToolButton *_toolGrabLive = nullptr;

    QLineEdit* lineLoadApplet = nullptr;
    QLineEdit* lineLoadMCF = nullptr;

    QStatusBar *_statusBar = nullptr;

    QDialog *mcfEditor = nullptr;
};

#endif // QGRABBERWIDGET_H


