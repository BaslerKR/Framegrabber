#include <QtTest/QtTest>

#include "DeviceRuntime/Protocol/DeviceParameterLoopback.h"
#include "DeviceRuntime/Protocol/DeviceRuntimeProtocol.h"
#include "FramegrabberSourceController.h"
#include "FramegrabberTestBackend.h"

#include <QJsonObject>

#include <atomic>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

/**
 * @brief Runs the Playground parameter transport against the Framegrabber controller.
 *
 * Every request, admission, completion and notification crosses the Playground JSON codec.
 * The fixture is synthetic: no board, applet or camera is discovered or opened; the module
 * seams copy the same applet/camera shapes the real SDK produces.
 */
class FramegrabberParameterLoopbackTests final : public QObject
{
    Q_OBJECT
private slots:
    void codecCarriesTreeSnapshotDeltaAndResync();
    void toolDescriptorsRoundTripAndBoardlessLoadRefusal();
};

namespace {

bool waitFor(const std::function<bool()>& predicate)
{
    return QTest::qWaitFor(predicate, 5000);
}

DeviceParameterResult admissionOf(const QJsonObject& response)
{
    const auto result = response.value(QStringLiteral("result")).toObject()
        .value(QStringLiteral("admission")).toObject();
    DeviceParameterResult admission;
    QString error;
    if (!DeviceRuntimeProtocol::decodeResult(result, &admission, &error))
        throw std::runtime_error("Malformed loopback admission");
    return admission;
}

struct Collected
{
    std::mutex mutex;
    std::vector<QJsonObject> completions;
    std::vector<DeviceParameterChange> changes;
};

const DeviceParameterDescriptor* findParameter(
    const std::vector<DeviceParameterDescriptor>& parameters, const char* scope, const char* name)
{
    for (const auto& parameter : parameters)
        if (parameter.target.scope == scope && parameter.target.name == name) return &parameter;
    return nullptr;
}

const ToolDescriptor* findTool(const QList<ToolDescriptor>& tools, const char* name)
{
    for (const auto& tool : tools)
        if (tool.name == QLatin1String(name)) return &tool;
    return nullptr;
}

} // namespace

void FramegrabberParameterLoopbackTests::codecCarriesTreeSnapshotDeltaAndResync()
{
    FramegrabberTestBackend fake;
    FramegrabberSourceController controller(nullptr);
    FramegrabberControllerTestAccess::install(controller, fake);

    DeviceConnectionTargets targets;
    bool targetsDone = false;
    QCOMPARE(controller.requestConnectionTargets([&](DeviceConnectionTargets value) {
        targets = std::move(value);
        targetsDone = true;
    }).error, DeviceConnectionError::None);
    QVERIFY(waitFor([&] { return targetsDone; }));
    QCOMPARE(targets.targets.size(), std::size_t(1));
    DeviceConnectionResult connected;
    std::atomic<bool> connectedDone{false};
    QCOMPARE(controller.requestConnect(targets.targets.front().targetId,
        [&](DeviceConnectionResult value) {
            connected = std::move(value);
            connectedDone.store(true, std::memory_order_release);
        }).error, DeviceConnectionError::None);
    QVERIFY(waitFor([&] { return connectedDone.load(std::memory_order_acquire); }));
    QCOMPARE(connected.error, DeviceConnectionError::None);
    QVERIFY(connected.state.has_value());
    const std::string connection = connected.state->connectionId;

    Collected collected;
    DeviceParameterLoopback loopback(QStringLiteral("framegrabber-loopback"), controller,
        [&collected](const QString& name, const QJsonObject& data) {
            if (name == QLatin1String("completion")) {
                std::lock_guard<std::mutex> lock(collected.mutex);
                collected.completions.push_back(data);
            } else if (name == QLatin1String("parameters.changed")) {
                DeviceParameterChange change;
                QString error;
                if (!DeviceRuntimeProtocol::decodeChange(
                        data.value(QStringLiteral("change")).toObject(), &change, &error))
                    return;
                std::lock_guard<std::mutex> lock(collected.mutex);
                collected.changes.push_back(std::move(change));
            }
        });

    const auto request = [&](const QString& id, const QString& op, const QJsonObject& args) {
        std::optional<DeviceParameterResult> admission;
        if (!loopback.request(id, op, args, [&admission](const QJsonObject& response) {
                admission = admissionOf(response);
            }))
            throw std::runtime_error("The loopback transport refused the request.");
        if (!admission) throw std::runtime_error("The loopback transport completed no admission.");
        return *admission;
    };

    // parameters.inventory: both scopes and their tree fields cross encode/decode unchanged.
    QCOMPARE(request(QStringLiteral("inventory"), QStringLiteral("parameters.inventory"), {}).error,
        DeviceParameterError::None);
    QVERIFY(waitFor([&] {
        std::lock_guard<std::mutex> lock(collected.mutex);
        for (const auto& completion : collected.completions)
            if (completion.value(QStringLiteral("op")).toString()
                == QStringLiteral("parameters.inventory")) return true;
        return false;
    }));
    DeviceParameterInventory inventory;
    {
        std::lock_guard<std::mutex> lock(collected.mutex);
        QString error;
        for (const auto& completion : collected.completions) {
            if (completion.value(QStringLiteral("op")).toString()
                != QStringLiteral("parameters.inventory")) continue;
            QVERIFY(DeviceRuntimeProtocol::decodeInventory(
                completion.value(QStringLiteral("inventory")).toObject(), &inventory, &error));
        }
    }
    QCOMPARE(inventory.result.error, DeviceParameterError::None);
    const auto* appletWidth = findParameter(inventory.parameters, "Applet:0", "Width");
    QVERIFY(appletWidth != nullptr);
    QVERIFY(appletWidth->categoryPath == std::vector<std::string>({"Acquisition"}));
    QCOMPARE(appletWidth->representation, DeviceParameterRepresentation::Number);
    const auto* cameraWidth = findParameter(inventory.parameters, "Camera:cxp:0", "Width");
    QVERIFY(cameraWidth != nullptr);
    QVERIFY(cameraWidth->categoryPath == std::vector<std::string>({"Image"}));
    QVERIFY(cameraWidth->value.has_value());
    QCOMPARE(std::get<std::int64_t>(*cameraWidth->value), std::int64_t(1280));

    // parameters.watch: the initial snapshot must arrive through parameters.changed.
    QCOMPARE(request(QStringLiteral("watch"), QStringLiteral("parameters.watch"),
        QJsonObject{{QStringLiteral("connectionId"), QString::fromStdString(connection)},
                    {QStringLiteral("subscriptionId"), QStringLiteral("loopback-watch")}}).error,
        DeviceParameterError::None);
    QVERIFY(waitFor([&] {
        std::lock_guard<std::mutex> lock(collected.mutex);
        return !collected.changes.empty();
    }));
    {
        std::lock_guard<std::mutex> lock(collected.mutex);
        QCOMPARE(collected.changes.front().kind, DeviceParameterChangeKind::Snapshot);
        QCOMPARE(collected.changes.front().sequence, std::uint64_t{1});
        QVERIFY(findParameter(collected.changes.front().descriptors, "Applet:0", "Width") != nullptr);
        QVERIFY(findParameter(collected.changes.front().descriptors, "Camera:cxp:0", "Width") != nullptr);
        collected.changes.clear();
    }

    // parameters.write: value, effect and readback cross the codec; the watcher gets a delta.
    QCOMPARE(request(QStringLiteral("write"), QStringLiteral("parameters.write"),
        QJsonObject{{QStringLiteral("target"), DeviceRuntimeProtocol::encodeTarget(appletWidth->target)},
                    {QStringLiteral("value"), DeviceRuntimeProtocol::encodeValue(std::int64_t{900})},
                    {QStringLiteral("expectedRevision"), QString::number(appletWidth->revision)}}).error,
        DeviceParameterError::None);
    QVERIFY(waitFor([&] {
        std::lock_guard<std::mutex> lock(collected.mutex);
        for (const auto& completion : collected.completions)
            if (completion.value(QStringLiteral("op")).toString() == QStringLiteral("parameters.write"))
                return true;
        return false;
    }));
    DeviceParameterResult written;
    {
        std::lock_guard<std::mutex> lock(collected.mutex);
        QString error;
        for (const auto& completion : collected.completions) {
            if (completion.value(QStringLiteral("op")).toString() != QStringLiteral("parameters.write"))
                continue;
            QVERIFY(DeviceRuntimeProtocol::decodeResult(
                completion.value(QStringLiteral("result")).toObject(), &written, &error));
        }
    }
    QCOMPARE(written.error, DeviceParameterError::None);
    QCOMPARE(written.effect, DeviceParameterEffect::Applied);
    QVERIFY(written.descriptor.has_value());
    QCOMPARE(std::get<std::int64_t>(*written.descriptor->value), std::int64_t(900));
    QVERIFY(waitFor([&] {
        std::lock_guard<std::mutex> lock(collected.mutex);
        return !collected.changes.empty();
    }));
    {
        std::lock_guard<std::mutex> lock(collected.mutex);
        QCOMPARE(collected.changes.front().kind, DeviceParameterChangeKind::Delta);
        QCOMPARE(collected.changes.front().sequence, std::uint64_t{2});
        QCOMPARE(collected.changes.front().descriptors.size(), std::size_t{1});
        QCOMPARE(collected.changes.front().descriptors.front().target.name, std::string("Width"));
        collected.changes.clear();
    }

    // parameters.execute: a camera command crosses the codec and the command was dispatched.
    const auto* cameraCommand = findParameter(inventory.parameters, "Camera:cxp:0", "Test");
    QVERIFY(cameraCommand != nullptr);
    QCOMPARE(request(QStringLiteral("execute"), QStringLiteral("parameters.execute"),
        QJsonObject{{QStringLiteral("target"), DeviceRuntimeProtocol::encodeTarget(cameraCommand->target)},
                    {QStringLiteral("expectedRevision"), QString::number(cameraCommand->revision)}}).error,
        DeviceParameterError::None);
    QVERIFY(waitFor([&] {
        std::lock_guard<std::mutex> lock(collected.mutex);
        for (const auto& completion : collected.completions)
            if (completion.value(QStringLiteral("op")).toString() == QStringLiteral("parameters.execute"))
                return true;
        return false;
    }));
    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        QCOMPARE(fake.state.cameraCommands, 1);
    }

    // A queue overflow is delivered as one resync snapshot, never a silent sequence gap.
    auto descriptor = *written.descriptor;
    for (int index = 0; index < 20; ++index) {
        descriptor.revision = 10 + index;
        FramegrabberControllerTestAccess::publishDelta(controller, connection, {descriptor});
    }
    FramegrabberControllerTestAccess::drain(controller);
    QVERIFY(waitFor([&] {
        std::lock_guard<std::mutex> lock(collected.mutex);
        for (const auto& change : collected.changes)
            if (change.resync) return true;
        return false;
    }));
    {
        std::lock_guard<std::mutex> lock(collected.mutex);
        std::uint64_t previous = 2;
        bool resync = false;
        for (const auto& change : collected.changes) {
            QVERIFY(change.sequence > previous);
            previous = change.sequence;
            if (!change.resync) continue;
            resync = true;
            QCOMPARE(change.kind, DeviceParameterChangeKind::Snapshot);
            QVERIFY(!change.overflowReason.empty());
        }
        QVERIFY(resync);
        collected.changes.clear();
    }

    // parameters.unwatch: delivery stops through the same transport.
    QCOMPARE(request(QStringLiteral("unwatch"), QStringLiteral("parameters.unwatch"),
        QJsonObject{{QStringLiteral("connectionId"), QString::fromStdString(connection)},
                    {QStringLiteral("subscriptionId"), QStringLiteral("loopback-watch")}}).error,
        DeviceParameterError::None);
    QVERIFY(waitFor([&] {
        std::lock_guard<std::mutex> lock(collected.mutex);
        for (const auto& completion : collected.completions)
            if (completion.value(QStringLiteral("op")).toString() == QStringLiteral("parameters.unwatch"))
                return true;
        return false;
    }));
    FramegrabberControllerTestAccess::publishDelta(controller, connection, {descriptor});
    FramegrabberControllerTestAccess::drain(controller);
    QTest::qWait(100);
    {
        std::lock_guard<std::mutex> lock(collected.mutex);
        QVERIFY(collected.changes.empty());
    }

    QVERIFY(loopback.roundTrips() > 0);
    loopback.close();
}

void FramegrabberParameterLoopbackTests::toolDescriptorsRoundTripAndBoardlessLoadRefusal()
{
    FramegrabberTestBackend fake;
    FramegrabberSourceController controller(nullptr);
    FramegrabberControllerTestAccess::install(controller, fake);

    // The worker snapshot carries the same descriptors, including the optional board.
    QList<ToolDescriptor> descriptors;
    QString error;
    QVERIFY2(DeviceRuntimeProtocol::decodeToolDescriptors(
        DeviceRuntimeProtocol::encodeToolDescriptors(controller.toolDescriptors()),
        &descriptors, &error), qPrintable(error));
    const auto* load = findTool(descriptors, "framegrabber.applet-load");
    QVERIFY(load != nullptr);
    QCOMPARE(load->inputSchema.value(QStringLiteral("required")).toArray(),
        QJsonArray{QStringLiteral("applet")});
    QVERIFY(load->inputSchema.value(QStringLiteral("properties")).toObject()
        .contains(QStringLiteral("board")));
    QVERIFY(load->description.contains(QStringLiteral("connected board")));
    const auto* configuration = findTool(descriptors, "framegrabber.configuration-load");
    QVERIFY(configuration != nullptr);
    QCOMPARE(configuration->inputSchema.value(QStringLiteral("required")).toArray(),
        QJsonArray{QStringLiteral("configuration")});
    QVERIFY(configuration->inputSchema.value(QStringLiteral("properties")).toObject()
        .contains(QStringLiteral("board")));
    QVERIFY(configuration->description.contains(QStringLiteral("connected board")));
    QCOMPARE(load->inputSchema.value(QStringLiteral("properties")).toObject()
        .value(QStringLiteral("applet")).toObject().value(QStringLiteral("format")).toString(),
        QStringLiteral("enum"));
    QCOMPARE(configuration->inputSchema.value(QStringLiteral("properties")).toObject()
        .value(QStringLiteral("configuration")).toObject().value(QStringLiteral("format")).toString(),
        QStringLiteral("file-open"));

    bool completed = false;
    const auto refused = controller.requestTool(QStringLiteral("framegrabber.applet-load"), 1,
        QJsonObject{{QStringLiteral("applet"), QStringLiteral("Acquisition")}},
        [&completed](ApiResult<QJsonValue>) { completed = true; });
    QVERIFY(!refused.succeeded());
    QCOMPARE(refused.error()->code, ApiErrorCode::Unavailable);
    QVERIFY(refused.error()->message.contains(QStringLiteral("No board is connected")));
    QVERIFY(!completed);
    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        QVERIFY(!fake.state.opened);
    }
}

QTEST_GUILESS_MAIN(FramegrabberParameterLoopbackTests)
#include "FramegrabberParameterLoopbackTests.moc"
