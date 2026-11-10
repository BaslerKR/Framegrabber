#include "Utility/Qt/QFramegrabberWidget.h"
#include <QApplication>
#include <QLabel>
#include <iostream>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QFramegrabberWidget widget;
    auto* fps = widget.findChild<QLabel*>("FramegrabberFpsLabel");
    auto* failed = widget.findChild<QLabel*>("FramegrabberFailedLabel");
    if (!fps || !failed || !fps->isHidden() || !failed->isHidden()) return 1;
    widget.setRuntimeDiagnostics({true, 0, 0});
    if (fps->isHidden() || !failed->isHidden()) return 2;
    widget.setRuntimeDiagnostics({true, 9, 3});
    if (failed->isHidden() || failed->text() != "3 failed"
        || failed->property("failedState").toString() != "warning") return 3;
    widget.setRuntimeDiagnostics({false, 9, 3});
    if (!fps->isHidden() || !failed->isHidden()) return 4;
    widget.setRuntimeDiagnostics({true, 0, 0});
    if (fps->isHidden() || !failed->isHidden()) return 5;
    std::cout << "Live, failure, stop and restart diagnostics passed.\n";
}
