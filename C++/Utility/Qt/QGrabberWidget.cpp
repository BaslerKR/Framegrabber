#include "QGrabberWidget.h"
#ifdef QT_GUI_LIB
#include <QVBoxLayout>
#include <QFileDialog>
#include <QHeaderView>
#include <QDomDocument>
#include <QSignalBlocker>
#include <QHash>
#include <QMessageBox>
#include <QStyledItemDelegate>
#include <functional>
#include <string>

QGrabberWidget::QGrabberWidget(QWidget *parent, Framegrabber *grabber) : QWidget(parent), _grabber(grabber)
{
    setWindowTitle("Basler Framegrabber Configuration");
    setWindowIcon(this->windowIcon());

    // Create the features widget
    _featuresWidget = new QTreeWidget;
    _featuresWidget->setHeaderLabels(QStringList() << "Features" << "Value");

    _toolGrabOne = new QToolButton(this);
    _toolGrabOne->setIcon(QIcon(":/Resources/Icons/icons8-camera-48.png"));
    _toolGrabOne->setEnabled(false);
    connect(_toolGrabOne, &QToolButton::clicked, this, [this](){
        _grabber->grabAll(1);
    });

    _toolGrabLive= new QToolButton(this);
    _toolGrabLive->setCheckable(true);
    _toolGrabLive->setEnabled(false);
    {
        QIcon icon;
        icon.addFile(":/Resources/Icons/icons8-cameras-48.png", QSize(), QIcon::Normal, QIcon::Off);
        icon.addFile(":/Resources/Icons/icons8-pause-48.png", QSize(), QIcon::Normal, QIcon::On);
        _toolGrabLive->setIcon(icon);
    }
    connect(_toolGrabLive, &QToolButton::toggled, this, [this](bool toggled){
        if(toggled){
            _grabber->grabAll();
        }else{
            _grabber->stopAll();
        }
    });

    setLayout(new QVBoxLayout(this));
    layout()->setContentsMargins(0,0,0,0);
    setMinimumWidth(270);

    QHBoxLayout *layoutLoadApplet = new QHBoxLayout;
    QToolButton *buttonLoadApplet = new QToolButton;
    buttonLoadApplet->setAutoRaise(true);
    buttonLoadApplet->setIconSize(QSize(18,18));
    buttonLoadApplet->setDefaultAction(new QAction(QIcon(":/Resources/Icons/icons8-network-card-48-5.png"), "Load applet"));
    connect(buttonLoadApplet, &QToolButton::triggered, this, [=](){
        auto get = QFileDialog::getOpenFileName(this, "Load an applet", QDir::homePath(), "*.hap *.dll *.so");
        if(get.isEmpty()) return;

        this->lineLoadApplet->setText(get);

        // Loading the applet
        auto initApplet = _grabber->loadApplet(get.toStdString());
        if(initApplet){
            generateFeaturesWidget(_grabber->getDMACount());
            _grabber->initCXPModule();
            _grabber->updateCXPCameraList();

            _toolGrabOne->setEnabled(true);
            _toolGrabLive->setEnabled(true);
        }else{
            _toolGrabOne->setEnabled(false);
            _toolGrabLive->setEnabled(false);
        }
    });
    lineLoadApplet = new QLineEdit;
    lineLoadApplet->setPlaceholderText("Load an applet");
    layoutLoadApplet->addWidget(buttonLoadApplet);
    layoutLoadApplet->addWidget(lineLoadApplet);
    layoutLoadApplet->setContentsMargins(9,9,9,0);

    QHBoxLayout *layoutLoadConfig = new QHBoxLayout;
    QToolButton *buttonEditMCF = new QToolButton;
    buttonEditMCF->setEnabled(false);
    buttonEditMCF->setAutoRaise(true);
    buttonEditMCF->setDefaultAction(new QAction(QIcon(":/Resources/Icons/icons8-note-48.png"),"MCF Editor"));
    buttonEditMCF->setIconSize(QSize(18,18));
    buttonEditMCF->setEnabled(false);
    connect(buttonEditMCF, &QToolButton::triggered, this, [&](){
        if(mcfEditor!=nullptr) mcfEditor->exec();
    });
    QToolButton *buttonLoadConfig = new QToolButton;
    buttonLoadConfig->setAutoRaise(true);
    buttonLoadConfig->setDefaultAction(new QAction(QIcon(":/Resources/Icons/icons8-network-card-48-4.png"), "Load configuration file"));
    buttonLoadConfig->setIconSize(QSize(18,18));
    connect(buttonLoadConfig, &QToolButton::triggered, this, [=](){
        auto get = QFileDialog::getOpenFileName(this, "Load a configuration file", QDir::homePath(), "*.mcf");
        if(get.isEmpty()) return;

        this->lineLoadMCF->setText(get);
        auto initConfig = _grabber->loadMCF(get.toStdString());
        if(initConfig){
            generateFeaturesWidget(_grabber->getDMACount());
            createMCFEditor(get);
        }
        buttonEditMCF->setEnabled(initConfig);
    });
    lineLoadMCF = new QLineEdit;
    lineLoadMCF->setPlaceholderText("Load a configuration file");
    layoutLoadConfig->addWidget(buttonLoadConfig);
    layoutLoadConfig->addWidget(lineLoadMCF);
    layoutLoadConfig->addWidget(buttonEditMCF);
    layoutLoadConfig->setContentsMargins(9,0,9,0);
    dynamic_cast<QVBoxLayout*>(layout())->addLayout(layoutLoadApplet);
    dynamic_cast<QVBoxLayout*>(layout())->addLayout(layoutLoadConfig);

    QHBoxLayout *layoutIcons = new QHBoxLayout;
    layoutIcons->setContentsMargins(2,2,2,2);
    layoutIcons->addStretch();

    layoutIcons->addSpacerItem(new QSpacerItem(5,5));
    layoutIcons->addWidget(_toolGrabOne);
    layoutIcons->addWidget(_toolGrabLive);
    layoutIcons->setSpacing(-1);
    dynamic_cast<QVBoxLayout*>(layout())->addLayout(layoutIcons);

    QVBoxLayout *layoutFeatures = new QVBoxLayout;
    layoutFeatures->addWidget(_featuresWidget);
    layoutFeatures->setContentsMargins(9,0,9,0);
    dynamic_cast<QVBoxLayout*>(layout())->addLayout(layoutFeatures);

    _statusBar = new QStatusBar(this);
    _statusBar->setContentsMargins(0,0,0,0);
    layout()->addWidget(_statusBar);
}


static QDomElement findFeature(const QDomElement &root, const QString &name)
{
    QStringList tags = {"Category","Integer","Float","Enumeration","String","StringReg","Boolean","Command"};

    for (auto &tag : tags) {
        auto nodes = root.elementsByTagName(tag);
        for (int i=0; i<nodes.count(); ++i) {
            QDomElement el = nodes.at(i).toElement();
            if (el.attribute("Name") == name)
                return el;
        }
    }
    return QDomElement();
}

static QString genicamDisplayName(const QDomElement &el, const QString &fallback)
{
    QString displayName = el.firstChildElement("DisplayName").text().trimmed();
    if (!displayName.isEmpty()) return displayName;

    QString description = el.firstChildElement("Description").text().trimmed();
    if (!description.isEmpty()) return description;

    return fallback;
}

void QGrabberWidget::generateFeaturesWidget(int dmaCnt)
{
    _featuresWidget->clear();
    auto fg = _grabber->getFg();

    for(int i=0; i<dmaCnt; ++i){
        QTreeWidgetItem *dma = new QTreeWidgetItem(_featuresWidget, QStringList() << "DMA: " + QString::number(i));

        size_t xmlSize = 0;
        int rc = Fg_getParameterInfoXML(fg, i, nullptr, &xmlSize);
        if (xmlSize == 0) {
            qDebug() << "No XML returned for port" << i;
            return;
        }

        std::vector<char> xmlBuf(xmlSize + 1, 0);
        rc = Fg_getParameterInfoXML(fg, i, xmlBuf.data(), &xmlSize);
        if (rc != FG_OK) {
            qDebug() << "Fg_getParameterInfoXML failed, rc=" << rc;
            return;
        }

        QString xml = QString::fromUtf8(xmlBuf.data());
        QDomDocument dom;
        if (!dom.setContent(xml)) {
            qDebug() << "XML parse failed";
            return;
        }
        QDomElement root = dom.documentElement();

        std::function<void(const QDomElement&, QTreeWidgetItem*)> addCategory;
        addCategory = [&](const QDomElement &catEl, QTreeWidgetItem *parentItem) {
            const QString catName = catEl.attribute("Name");
            QTreeWidgetItem *categoryItem = parentItem;
            if (!catName.isEmpty() && catName != "Root") {
                categoryItem = new QTreeWidgetItem(parentItem, QStringList() << catName);
            }
            for (QDomElement child = catEl.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
                if (child.tagName() != "pFeature") continue;

                const QString featureName = child.text().trimmed();
                if (featureName.isEmpty()) continue;

                QDomElement featEl = findFeature(root, featureName);
                if (featEl.isNull()) continue;

                if (featEl.tagName() == "Category") {
                    addCategory(featEl, categoryItem);
                    continue;
                }

                QString displayName = genicamDisplayName(featEl, featureName);

                QTreeWidgetItem *item = new QTreeWidgetItem(categoryItem, QStringList() << displayName);
                _featuresWidget->setItemWidget(item, item->columnCount(), createNodeWidget(featEl, featureName, i));
            }
        };

        QDomElement categoriesContainer = root.firstChildElement("Categories");
        if (!categoriesContainer.isNull()) {
            for (QDomElement cat = categoriesContainer.firstChildElement("Category"); !cat.isNull(); cat = cat.nextSiblingElement("Category")) {
                addCategory(cat, dma);
            }
        } else {
            for (QDomElement cat = root.firstChildElement("Category"); !cat.isNull(); cat = cat.nextSiblingElement("Category")) {
                addCategory(cat, dma);
            }
        }
    }
}

void QGrabberWidget::setAppletPath(QString path)
{
    lineLoadApplet->setText(path);
}

void QGrabberWidget::setMCFPath(QString path)
{
    lineLoadMCF->setText(path);
}

QWidget *QGrabberWidget::createNodeWidget(const QDomElement &node, QString featureName, int dmaIndex)
{
    QString tag = node.tagName();
    auto fg = _grabber->getFg();
    auto id = Fg_getParameterIdByName(_grabber->getFg(), featureName.toStdString().c_str());

    // Integer
    if (tag == "Integer") {
        QSpinBox *sb = new QSpinBox;
        sb->setMinimum(0);
        sb->setMaximum(0);
        sb->setMinimum(node.firstChildElement("Min").text().toInt());
        sb->setMaximum(node.firstChildElement("Max").text().toInt());
        sb->setSingleStep(node.firstChildElement("Inc").text().toInt());
        int32_t value = 0;
        Fg_getParameterPropertyWithType(_grabber->getFg(), id, FgProperty::PROP_ID_VALUE, &value);

        sb->setValue(value);

        connect(sb, QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int number){
            auto value = number;
            if(!_grabber->setParameter(id, dmaIndex, &value)){
                int current;
                _grabber->getParameter(id, dmaIndex, &current);
                QSignalBlocker block(sb);
                sb->setValue(current);
            }
        });
        return sb;
    }
    // Float
    if (tag == "Float") {
        QDoubleSpinBox *ds = new QDoubleSpinBox;
        ds->setMinimum(node.firstChildElement("Min").text().toDouble());
        ds->setMaximum(node.firstChildElement("Max").text().toDouble());

        double value = 0;
        Fg_getParameterPropertyWithType(_grabber->getFg(), id, FgProperty::PROP_ID_VALUE, &value);

        ds->setValue(value);
        connect(ds, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [=](double number){
            auto value = number;
            if(!_grabber->setParameter(id, dmaIndex, &value)){
                double current;
                _grabber->getParameter(id, dmaIndex, &current);
                QSignalBlocker block(ds);
                ds->setValue(current);
            }
        });
        return ds;
    }

    if (tag == "Enumeration") {
        QComboBox *cb = new QComboBox;
        struct EnumInfo { QString name; int value; };
        QVector<EnumInfo> list;

        auto entries = node.elementsByTagName("EnumEntry");
        for (int i = 0; i < entries.count(); ++i) {
            QDomElement e = entries.at(i).toElement();
            QString name = e.attribute("Name");
            int value = e.firstChildElement("Value").text().toInt();
            list.push_back({name, value});
            cb->addItem(name, value);
        }

        int32_t cur = 0;
        Fg_getParameterPropertyWithType(fg, id, FgProperty::PROP_ID_VALUE, &cur);

        for (int i = 0; i < list.size(); ++i) {
            if (list[i].value == cur) {
                cb->setCurrentIndex(i);
                break;
            }
        }
        connect(cb, &QComboBox::currentTextChanged, this, [=]{
            int current = cb->currentData().toInt();
            qDebug() << cb << cb->currentData() << cb->currentText() << current;
            if(!_grabber->setParameter(id, dmaIndex, &current)){
                int val;
                _grabber->getParameter(id, dmaIndex, &val);

                QSignalBlocker block(cb);
                QVariant var(val);
                cb->setCurrentIndex(cb->findData(var));
            }

        });

        return cb;
    }
    if (tag == "StringReg"){
        QLineEdit *edit = new QLineEdit;
        std::string buf;
        Fg_getParameterPropertyWithType(fg, id, FgProperty::PROP_ID_VALUE, buf);

        edit->setText(QString::fromUtf8(buf));
        connect(edit, &QLineEdit::editingFinished, this, [=]{
            std::string value = edit->text().toStdString();
            if(!_grabber->setParameter(id, dmaIndex, &value)){
                QSignalBlocker block(edit);

                std::string current;
                _grabber->getParameter(id, dmaIndex, &current);
                edit->setText(current.c_str());
            }
        });

        return edit;
    }
    if(tag== "Boolean"){
        qDebug() << "Boolean coming";
    }
    if(tag=="Command"){
        qDebug() << "Command coming";
    }

    // Default fallback
    return new QLabel(tag);
}

void QGrabberWidget::createMCFEditor(QString mcfPath)
{
    QFile file(mcfPath);
    QString sectionParser = "[";
    QString valueParser = "=";


    if(mcfEditor != nullptr) mcfEditor->deleteLater();
    mcfEditor = new QDialog(this);
    QVBoxLayout *mcfLayout = new QVBoxLayout;
    mcfEditor->setLayout(mcfLayout);
    mcfEditor->setWindowTitle("MCF Editor");
    mcfEditor->setWindowIcon(this->windowIcon());

    QLineEdit *lineEditSearch = new QLineEdit;
    lineEditSearch->setFrame(true);
    lineEditSearch->setPlaceholderText("Search:");
    lineEditSearch->setClearButtonEnabled(true);
    lineEditSearch->setStyleSheet("QLineEdit{ border: 1px solid gray; height: 20px; }");


    QTreeWidget *widget = new QTreeWidget(mcfEditor);
    widget->setHeaderLabels(QStringList() << "Parameter" << "Value");
    widget->header()->resizeSection(0, 200);



    QHBoxLayout *layoutButtons = new QHBoxLayout;

    QToolButton *saveButton = new QToolButton;
    saveButton->setAutoRaise(true);
    saveButton->setDefaultAction(new QAction(QIcon(":/Resources/Icons/icons8-save-as-48.png"), "Save"));
    saveButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    saveButton->setIconSize(QSize(20,20));
    saveButton->setFocusPolicy(Qt::FocusPolicy::NoFocus);
    saveButton->setStyleSheet(R"(
        color: black;
    )");
    connect(saveButton, &QToolButton::triggered, this, [=]{
        auto val = QMessageBox::warning(this, "MCF Editor", "Are you sure to overwrite mcf file whith this settings?",
                                        QMessageBox::Yes | QMessageBox::No);
        if(val == QMessageBox::Yes){
            _grabber->saveConfig(this->lineLoadMCF->text().toStdString());
        }else return;
    });

    QToolButton *saveAsButton = new QToolButton;
    saveAsButton->setAutoRaise(true);
    saveAsButton->setDefaultAction(new QAction(QIcon(":/Resources/Icons/icons8-save-as-48.png"), "Save as"));
    saveAsButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    saveAsButton->setIconSize(QSize(20,20));
    saveAsButton->setFocusPolicy(Qt::FocusPolicy::NoFocus);
    saveAsButton->setStyleSheet(R"(
        color: black;
    )");
    connect(saveAsButton, &QToolButton::triggered, this, [=]{
        QString savePath = QFileDialog::getSaveFileName(this, "Save MCF", QDir::currentPath(), "MCF File (*.mcf)");
        if (!savePath.isEmpty()) {
            if(_grabber->saveConfig(savePath.toStdString())){
                QMessageBox::information(this, "MCF Editor", "File saved successfully.");
            }else{
                QMessageBox::warning(this, "MCF Editor", "Failed to save the mcf file.");
            }
        }
    });

    QToolButton *buttonRefresh = new QToolButton;
    buttonRefresh->setAutoRaise(true);
    buttonRefresh->setDefaultAction(new QAction(QIcon(":/Resources/Icons/icons8-refresh-48.png"), "Refresh"));
    buttonRefresh->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    buttonRefresh->setIconSize(QSize(20,20));
    buttonRefresh->setStyleSheet(R"(
        color: black;
    )");
    connect(buttonRefresh, &QToolButton::triggered, this, [=]{
        refreshMCFEditor();
    });

    layoutButtons->addWidget(saveButton);
    layoutButtons->addWidget(saveAsButton);
    layoutButtons->addWidget(buttonRefresh);
    layoutButtons->addStretch(100);

    mcfLayout->addLayout(layoutButtons);
    mcfLayout->addWidget(lineEditSearch);
    mcfLayout->addWidget(widget);

    mcfEditor->setMinimumSize(400,300);
    widget->setMinimumSize(400,300);
    mcfEditor->setWindowFlags(Qt::Window);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        QTreeWidgetItem* sectionItem = nullptr;

        while (!in.atEnd()) {
            QString line = in.readLine().trimmed();
            if (line.isEmpty()) continue;

            if (line.startsWith("[") && line.endsWith("]")) {
                sectionItem = new QTreeWidgetItem(widget);
                sectionItem->setText(0, line.mid(1, line.length() - 2));
                continue;
            }

            auto values = line.split(valueParser); // e.g. '='
            if (values.size() < 2) continue;

            QString fullKey = values.first().trimmed();  // e.g., Device1_Process0_AppletProperties_Width
            QString value = values.last().remove(";").trimmed();

            QStringList parts = fullKey.split("_");
            if (parts.isEmpty()) continue;

            QTreeWidgetItem* parent = sectionItem;
            QString lastPrefix;

            QStringList cleanedKeys;
            for (const QString& part : parts) {
                if (!lastPrefix.isEmpty() && part.startsWith(lastPrefix)) {
                    cleanedKeys << part.mid(lastPrefix.length());
                } else {
                    cleanedKeys << part;
                }
                lastPrefix += part;
            }

            for (int i = 0; i < cleanedKeys.size() - 1; ++i) {
                QString key = cleanedKeys[i];
                QTreeWidgetItem* existing = nullptr;

                for (int j = 0; j < parent->childCount(); ++j) {
                    if (parent->child(j)->text(0) == key) {
                        existing = parent->child(j);
                        break;
                    }
                }

                if (existing) {
                    parent = existing;
                } else {
                    QTreeWidgetItem* node = new QTreeWidgetItem(parent);
                    node->setText(0, key);
                    parent = node;
                }
            }

            QString paramKey = cleanedKeys.last();
            QTreeWidgetItem* leaf = new QTreeWidgetItem(parent);
            leaf->setText(0, paramKey);

            QLineEdit* lineEdit = new QLineEdit(value);
            lineEdit->setFrame(false);
            lineEdit->setObjectName(fullKey);

            connect(lineEdit, &QLineEdit::returnPressed, this, [=](){
                int r = QMessageBox::question(
                    mcfEditor, "MCF Editor",
                    "Are you sure to change this value?",
                    QMessageBox::Cancel | QMessageBox::Ok, QMessageBox::Cancel);

                if (r == QMessageBox::Cancel) {
                    lineEdit->undo();
                    return;
                }
                QString category = sectionItem ? sectionItem->text(0) : "";
                QString parameterName = lineEdit->objectName();

                int dmaIndex = 0;
                QString filterString = "ID";
                if (category.startsWith(filterString)) {
                    category.remove(filterString);

                    bool isInt = false;
                    dmaIndex = category.toInt(&isInt);
                    if(!isInt){
                        qDebug() << "Searching DMA failed.";
                        return;
                    }
                }
                switch(_grabber->getParameterProperty(parameterName.toStdString(), dmaIndex)){
                case FG_PARAM_TYPE_SIZE_T:
                case FG_PARAM_TYPE_INT32_T:
                case FG_PARAM_TYPE_UINT32_T:
                case FG_PARAM_TYPE_INT64_T:
                case FG_PARAM_TYPE_UINT64_T:{
                    size_t temp = lineEdit->text().toInt();
                    _grabber->setParameter(parameterName.toStdString(), dmaIndex, &temp, true);
                    break;
                }break;
                case FG_PARAM_TYPE_DOUBLE:{
                    double temp = lineEdit->text().toDouble();
                    _grabber->setParameter(parameterName.toStdString(), dmaIndex, &temp, true);
                }break;
                case FG_PARAM_TYPE_CHAR_PTR:{
                    std::string temp = lineEdit->text().toStdString();
                    _grabber->setParameter(parameterName.toStdString(), dmaIndex, &temp, true);
                }break;
                case FG_PARAM_TYPE_CHAR_PTR_PTR: qDebug() << "CHAR_PTR_PTR Unsupported parameter." << parameterName; break;
                case FG_PARAM_TYPE_STRUCT_FIELDPARAMACCESS: qDebug() << "STRUCT_FIELDPARAMACCESS Unsupported parameter." << parameterName; break;
                case FG_PARAM_TYPE_STRUCT_FIELDPARAMINT: qDebug() << "TYPE_STRUCT_FIELDPARAMINT Unsupported parameter." << parameterName; break;
                case FG_PARAM_TYPE_STRUCT_FIELDPARAMINT64: qDebug() << "TYPE_STRUCT_FIELDPARAMINT64 Unsupported parameter." << parameterName; break;
                case FG_PARAM_TYPE_STRUCT_FIELDPARAMDOUBLE: qDebug() << "TYPE_STRUCT_FIELDPARAMDOUBLE Unsupported parameter." << parameterName; break;
                case FG_PARAM_TYPE_COMPLEX_DATATYPE: qDebug() << "TYPE_COMPLEX_DATATYPE Unsupported parameter." << parameterName; break;
                case FG_PARAM_TYPE_AUTO: qDebug() << "TYPE_AUTO Unsupported parameter." << parameterName; break;
                case FG_PARAM_TYPE_INVALID: qDebug() << "INVALID Unsupported parameter." << parameterName; break;
                }
                refreshMCFEditor();
            });
            widget->setItemWidget(leaf, 1, lineEdit);
        }
    }
    widget->setItemDelegate(new QStyledItemDelegate(widget));

    auto filterItems = [widget](const QString &searchText) {
        std::function<bool(QTreeWidgetItem*)> filterRecursive = [&](QTreeWidgetItem *item) -> bool {
            if (!item) return false;

            QString originalText = item->data(0, Qt::UserRole).toString();
            if (originalText.isEmpty()) {
                originalText = item->text(0);
                item->setData(0, Qt::UserRole, originalText);
            }
            item->setText(0, originalText);

            bool selfMatch = originalText.contains(searchText, Qt::CaseInsensitive);

            if (selfMatch && !searchText.isEmpty()) {
                item->setBackground(0, QBrush(Qt::yellow));
            } else {
                item->setBackground(0, QBrush());
            }

            QWidget *editor = widget->itemWidget(item, 1);
            bool editorMatch = false;
            if (editor) {
                QLineEdit *lineEdit = qobject_cast<QLineEdit*>(editor);
                if (lineEdit) {
                    editorMatch = lineEdit->text().contains(searchText, Qt::CaseInsensitive);
                }
            }

            bool childMatch = false;
            for (int j = 0; j < item->childCount(); ++j) {
                childMatch |= filterRecursive(item->child(j));
            }

            bool finalMatch = selfMatch || editorMatch || childMatch;

            item->setHidden(!finalMatch);
            item->setExpanded(finalMatch);

            if ((selfMatch || editorMatch) && item->childCount() > 0) {
                for (int j = 0; j < item->childCount(); ++j) {
                    item->child(j)->setHidden(false);
                }
            }
            return finalMatch;
        };

        widget->setUpdatesEnabled(false);
        for (int i = 0; i < widget->topLevelItemCount(); ++i) {
            filterRecursive(widget->topLevelItem(i));
        }

        widget->setUpdatesEnabled(true);
    };
    connect(lineEditSearch, &QLineEdit::textChanged, filterItems);
}

void QGrabberWidget::refreshMCFEditor()
{
    if (!mcfEditor) return;
    auto treeWidget = mcfEditor->findChild<QTreeWidget*>();
    if (!treeWidget) return;

    std::function<void(QTreeWidgetItem*)> refreshItemRecursive = [&](QTreeWidgetItem* item) {
        if (!item) return;
        QWidget* cur = treeWidget->itemWidget(item, 1);
        if (cur) {
            QLineEdit* lineEdit = qobject_cast<QLineEdit*>(cur);
            if (lineEdit) {
                QString parameterName = lineEdit->objectName(); // Device1_Process0_AppletProperties_Width
                QString category;
                QTreeWidgetItem* sectionItem = item;
                while (sectionItem && sectionItem->parent()) sectionItem = sectionItem->parent();
                if (sectionItem) category = sectionItem->text(0);

                int dmaIndex = 0;
                QString filterString = "ID";
                if (category.startsWith(filterString)) {
                    category.remove(filterString);

                    bool isInt = false;
                    dmaIndex = category.toInt(&isInt);
                    if(!isInt){
                        qDebug() << "Searching DMA failed.";
                        return;
                    }
                }
                QVariant newValue;
                auto dataType =_grabber->getParameterProperty(parameterName.toStdString(), dmaIndex);
                bool succeed = false;
                switch(dataType){
                case FG_PARAM_TYPE_SIZE_T:
                    size_t sizeTtemp;
                    succeed = _grabber->getParameter(parameterName.toStdString(), dmaIndex, &sizeTtemp,true);
                    newValue = sizeTtemp;
                    break;
                case FG_PARAM_TYPE_INT32_T:
                    int32_t int32temp;
                    succeed = _grabber->getParameter(parameterName.toStdString(), dmaIndex, &int32temp,true);
                    newValue = int32temp;
                    break;
                case FG_PARAM_TYPE_UINT32_T:
                    uint32_t uint32temp;
                    succeed = _grabber->getParameter(parameterName.toStdString(), dmaIndex, &uint32temp,true);
                    newValue = uint32temp;
                    break;
                case FG_PARAM_TYPE_INT64_T:
                    int64_t int64temp;
                    succeed = _grabber->getParameter(parameterName.toStdString(), dmaIndex, &int64temp, true);
                    newValue = int64temp;
                    break;
                case FG_PARAM_TYPE_UINT64_T:
                    uint64_t uint64temp;
                    succeed = _grabber->getParameter(parameterName.toStdString(), dmaIndex, &uint64temp, true);
                    newValue = uint64temp;
                    break;
                case FG_PARAM_TYPE_DOUBLE:
                    double doubleTemp;
                    succeed = _grabber->getParameter(parameterName.toStdString(), dmaIndex, &doubleTemp, true);
                    newValue = doubleTemp;
                    break;
                case FG_PARAM_TYPE_CHAR_PTR_PTR:
                case FG_PARAM_TYPE_CHAR_PTR:{
                    std::string stringTemp;
                    succeed = _grabber->getParameter(parameterName.toStdString(), dmaIndex, &stringTemp, true);
                    newValue = stringTemp.c_str();
                }break;
                case FG_PARAM_TYPE_STRUCT_FIELDPARAMACCESS:
                case FG_PARAM_TYPE_STRUCT_FIELDPARAMINT:
                case FG_PARAM_TYPE_STRUCT_FIELDPARAMINT64:
                case FG_PARAM_TYPE_STRUCT_FIELDPARAMDOUBLE:
                case FG_PARAM_TYPE_COMPLEX_DATATYPE:
                case FG_PARAM_TYPE_AUTO:
                case FG_PARAM_TYPE_INVALID:
                    int temp;
                    succeed = _grabber->getParameter(parameterName.toStdString(), dmaIndex, &temp, true);
                    newValue = temp;
                    break;
                }
                QString newText = newValue.toString();
                if (lineEdit->text() != newText && succeed) lineEdit->setText(newText);
            }
        }


        for (int i = 0; i < item->childCount(); ++i) {
            refreshItemRecursive(item->child(i));
        }
    };

    for (int i = 0; i < treeWidget->topLevelItemCount(); ++i) {
        refreshItemRecursive(treeWidget->topLevelItem(i));
    }
}
#endif
