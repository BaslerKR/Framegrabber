#include "Utility/Qt/FramegrabberNumericFeatureEditor.h"
#include "Utility/Qt/QFramegrabberWidget.h"
#include <QDomDocument>
#include <QPushButton>
#include <QtTest>
#include <atomic>

using namespace framegrabber::presentation;

class FramegrabberFeatureEditorTest : public QObject
{
    Q_OBJECT
private slots:
    void signedAndUnsignedBoundaries() {
        FeatureSpinBox<std::int64_t> signedSpin((std::numeric_limits<std::int64_t>::max)());
        signedSpin.stepBy(1);
        QCOMPARE(signedSpin.value(), (std::numeric_limits<std::int64_t>::max)());
        signedSpin.stepBy(-1);
        QCOMPARE(signedSpin.value(), (std::numeric_limits<std::int64_t>::max)() - 1);
        signedSpin.setValue((std::numeric_limits<std::int64_t>::min)());
        signedSpin.stepBy(-1);
        QCOMPARE(signedSpin.value(), (std::numeric_limits<std::int64_t>::min)());
        signedSpin.stepBy(1);
        QCOMPARE(signedSpin.value(), (std::numeric_limits<std::int64_t>::min)() + 1);
        signedSpin.setValue(-1);
        signedSpin.stepBy(2);
        QCOMPARE(signedSpin.value(), std::int64_t{1});

        FeatureSpinBox<std::uint64_t> unsignedSpin(UINT64_MAX - 1);
        unsignedSpin.stepBy(2);
        QCOMPARE(unsignedSpin.value(), UINT64_MAX);
        QCOMPARE(unsignedSpin.commitText().value(), QStringLiteral("18446744073709551615"));
        QVERIFY(unsignedSpin.toolTip().contains(QStringLiteral("18446744073709551615")));
        unsignedSpin.findChild<QLineEdit*>()->setText("-1");
        QVERIFY(!unsignedSpin.commitText());
        QCOMPARE(unsignedSpin.value(), UINT64_MAX);
        unsignedSpin.setValue(0);
        unsignedSpin.stepBy(-1);
        QCOMPARE(unsignedSpin.value(), std::uint64_t{0});
    }

    void incrementAndInvalidInput() {
        FeatureSpinBox<std::int64_t> spin(5, -3, 14, 4);
        spin.stepBy(99);
        QCOMPARE(spin.value(), std::int64_t{13});
        auto* input = spin.findChild<QLineEdit*>();
        for (const auto& text : {"14", "6", "9223372036854775808", "abc"}) {
            input->setText(text);
            QVERIFY(!spin.commitText());
            QCOMPARE(spin.value(), std::int64_t{13});
        }
        input->setText("-3");
        QVERIFY(spin.commitText());
        spin.stepBy((std::numeric_limits<int>::min)());
        QCOMPARE(spin.value(), std::int64_t{-3});
        FeatureSpinBox<std::int32_t> narrow(0);
        narrow.findChild<QLineEdit*>()->setText("4294967296");
        QVERIFY(!narrow.commitText());
        narrow.setEnabled(false);
        narrow.stepBy(1);
        QCOMPARE(narrow.value(), std::int32_t{0});
    }

    void floatingPointRoundTrip() {
        const double exact = 1.2345678901234567e-20;
        FeatureSpinBox<double> spin(exact);
        QCOMPARE(spin.commitText()->toDouble(), exact);
        auto* input = spin.findChild<QLineEdit*>();
        for (const auto& text : {"nan", "inf", "1e999", "not a number"}) {
            input->setText(text);
            QVERIFY(!spin.commitText());
            QCOMPARE(spin.value(), exact);
        }
        FeatureSpinBox<double> ranged(0.3, 0.1, 0.9, 0.1);
        ranged.stepBy(1);
        QVERIFY(std::abs(ranged.value() - 0.4) < 1e-15);
        ranged.findChild<QLineEdit*>()->setText("0.45");
        QVERIFY(!ranged.commitText());
        FeatureSpinBox<double> inconsistent(12.5, 20.0, 1.0, -2.0);
        QVERIFY(inconsistent.commitText());
    }

    void runtimeWriteAndRestore() {
        QFramegrabberWidget widget;
        Framegrabber::AppletParameterLimits limits;
        limits.minimum = std::int64_t{0}; limits.maximum = std::int64_t{100}; limits.increment = std::int64_t{2};
        std::atomic<int> writes{0};
        int refreshes = 0;
        auto* editor = dynamic_cast<NumericFeatureEditor*>(widget.createNumericEditor(std::int64_t{10}, limits, true,
            [&](const Framegrabber::ParameterValue& value) {
                ++writes;
                return std::get<std::int64_t>(value) == 12;
            }, [&] { ++refreshes; }));
        QVERIFY(editor);
        QCOMPARE(writes.load(), 0);
        editor->stepBy(1);
        QTRY_COMPARE(writes.load(), 1);
        QTRY_VERIFY(editor->isEnabled());
        QCOMPARE(editor->commitText().value(), QStringLiteral("12"));
        emit editor->editingFinished();
        QCOMPARE(writes.load(), 1); // Focus-out after an arrow step must not write twice.
        editor->stepBy(1);
        QTRY_COMPARE(refreshes, 1);
        QCOMPARE(writes.load(), 2);
        QCOMPARE(editor->commitText().value(), QStringLiteral("12"));
    }

    void commandUsesButtonWithoutRead() {
        QFramegrabberWidget widget;
        Framegrabber::AppletFeatureNode command;
        command.kind = Framegrabber::AppletFeatureKind::Command;
        command.readable = false; command.writable = true; command.parameterId = 7;
        auto* appletButton = qobject_cast<QPushButton*>(widget.createAppletFeatureEditor(command, 0));
        QVERIFY(appletButton);
        QVERIFY(appletButton->isEnabled());
        command.writable = false;
        QVERIFY(!widget.createAppletFeatureEditor(command, 0)->isEnabled());
        QDomDocument document;
        document.setContent(QStringLiteral("<Command Name='TriggerSoftware'><AccessMode>WO</AccessMode></Command>"));
        QVERIFY(qobject_cast<QPushButton*>(widget.createFeatureEditor(document.documentElement(), "TriggerSoftware",
            QFramegrabberWidget::TreeSource::Camera, Framegrabber::CameraTransport::CoaXPress, 0)));
    }

    void keyboardCommitAndReadOnly() {
        QFramegrabberWidget widget;
        std::atomic<int> writes{0};
        const auto write = [&](const Framegrabber::ParameterValue&) { ++writes; return true; };
        Framegrabber::AppletParameterLimits limits;
        limits.minimum = std::int32_t{0}; limits.maximum = std::int32_t{100}; limits.increment = std::int32_t{2};
        auto* editor = dynamic_cast<NumericFeatureEditor*>(widget.createNumericEditor(std::int32_t{10}, limits, true, write, [] {}));
        QVERIFY(editor);
        widget.show(); editor->show();
        auto* input = editor->findChild<QLineEdit*>();
        input->setFocus();
        input->selectAll();
        QTest::keyClicks(input, "12");
        QCOMPARE(writes.load(), 0);
        QTest::keyClick(input, Qt::Key_Return);
        QTRY_COMPARE(writes.load(), 1);
        QTRY_VERIFY(editor->isEnabled());
        input->setFocus(); input->selectAll();
        QTest::keyClicks(input, "13");
        QTest::keyClick(input, Qt::Key_Return);
        QCOMPARE(writes.load(), 1);
        QCOMPARE(input->text(), QStringLiteral("12"));
        auto* locked = dynamic_cast<NumericFeatureEditor*>(widget.createNumericEditor(std::int32_t{10}, limits, false, write, [] {}));
        QVERIFY(locked && !locked->isEnabled());
        locked->stepBy(1);
        emit locked->editingFinished();
        QCOMPARE(writes.load(), 1);
    }
};

QTEST_MAIN(FramegrabberFeatureEditorTest)
#include "FeatureEditorTest.moc"
