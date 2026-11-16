#include <QtTest/QtTest>

#include "FramegrabberSourceController.h"
#include "FramegrabberTestBackend.h"

#include <QJsonArray>
#include <QJsonObject>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class FramegrabberControllerTests final : public QObject
{
    Q_OBJECT
private slots:
    void declaresHierarchicalBoardConnection();
    void connectLoadsOpeningAppletAndOwnsDmaSources();
    void declaredActionsCarryPresentationAndPromptKinds();
    void typedArgumentRequiredPrompts();
    void parameterTreeCoversAppletAndCameraScopes();
    void parameterChangesDeliverSnapshotAndDelta();
    void notificationQueueOverflowsToResync();
};

namespace {

template<class Value>
struct Sink
{
    std::mutex mutex;
    std::optional<Value> result;
    std::atomic<bool> done{false};

    std::function<void(Value)> completion()
    {
        return [this](Value value) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                result = std::move(value);
            }
            done.store(true, std::memory_order_release);
        };
    }

    Value take()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return std::move(*result);
    }

    bool wait()
    {
        return QTest::qWaitFor([this] { return done.load(std::memory_order_acquire); }, 5000);
    }
};

struct ConnectOutcome
{
    QString error;
    DeviceConnectionResult result;
};

ConnectOutcome connectBoard(FramegrabberSourceController& controller)
{
    ConnectOutcome outcome;
    Sink<DeviceConnectionTargets> targets;
    const auto targetsAdmission = controller.requestConnectionTargets(targets.completion());
    if (targetsAdmission.error != DeviceConnectionError::None)
    {
        outcome.error = QString::fromStdString(targetsAdmission.message);
        return outcome;
    }
    if (!targets.wait())
    {
        outcome.error = QStringLiteral("Target catalog did not complete.");
        return outcome;
    }
    const auto catalog = targets.take();
    if (catalog.targets.empty())
    {
        outcome.error = QStringLiteral("No synthetic target.");
        return outcome;
    }
    Sink<DeviceConnectionResult> connected;
    const auto admission = controller.requestConnect(catalog.targets.front().targetId,
        connected.completion());
    if (admission.error != DeviceConnectionError::None)
    {
        outcome.error = QString::fromStdString(admission.message);
        return outcome;
    }
    if (!connected.wait())
    {
        outcome.error = QStringLiteral("Connect did not complete.");
        return outcome;
    }
    outcome.result = connected.take();
    if (outcome.result.error != DeviceConnectionError::None)
        outcome.error = QString::fromStdString(outcome.result.message);
    return outcome;
}

const ToolDescriptor* findDescriptor(const QList<ToolDescriptor>& descriptors, const char* name)
{
    for (const auto& descriptor : descriptors)
        if (descriptor.name == QLatin1String(name)) return &descriptor;
    return nullptr;
}

const DeviceParameterDescriptor* findParameter(
    const std::vector<DeviceParameterDescriptor>& parameters, const char* scope, const char* name)
{
    for (const auto& parameter : parameters)
        if (parameter.target.scope == scope && parameter.target.name == name) return &parameter;
    return nullptr;
}

} // namespace

void FramegrabberControllerTests::declaresHierarchicalBoardConnection()
{
    FramegrabberTestBackend fake;
    FramegrabberSourceController controller(nullptr);
    FramegrabberControllerTestAccess::install(controller, fake);

    const auto capabilities = controller.connectionCapabilities();
    QCOMPARE(capabilities.connectionModel, DeviceConnectionModel::Hierarchical);
    QVERIFY(capabilities.targets.available);
    QVERIFY(capabilities.connect.available);
    QVERIFY(capabilities.disconnect.available);
    QVERIFY(capabilities.single.available);
    QVERIFY(!capabilities.discover.available);
    QVERIFY(!capabilities.live.available);
    QVERIFY(!capabilities.discover.reason.empty());

    Sink<DeviceConnectionTargets> targets;
    QCOMPARE(controller.requestConnectionTargets(targets.completion()).error, DeviceConnectionError::None);
    QVERIFY(targets.wait());
    const auto catalog = targets.take();
    QCOMPARE(catalog.result.error, DeviceConnectionError::None);
    QCOMPARE(catalog.targets.size(), std::size_t(1));
    const auto& target = catalog.targets.front();
    QCOMPARE(target.targetId, std::string("Test Board (SN1)"));
    QCOMPARE(target.kind, DeviceConnectionTargetKind::Interface);
    QVERIFY(target.parentTargetId.empty());
    QVERIFY(target.interfaceInfo.has_value());
    QVERIFY(target.interfaceInfo->exclusive);
    QVERIFY(!target.interfaceInfo->type.empty());

    Sink<DeviceConnectionResult> state;
    QCOMPARE(controller.requestConnectionState(state.completion()).error, DeviceConnectionError::None);
    QVERIFY(state.wait());
    const auto observed = state.take();
    QCOMPARE(observed.error, DeviceConnectionError::None);
    QVERIFY(observed.state.has_value());
    QVERIFY(!observed.state->connected);
    QVERIFY(observed.state->connectionId.empty());

    Sink<DeviceConnectionResult> refused;
    QCOMPARE(controller.requestConnect("missing board", refused.completion()).error,
        DeviceConnectionError::NotFound);
    QVERIFY(!refused.done.load());
}

void FramegrabberControllerTests::connectLoadsOpeningAppletAndOwnsDmaSources()
{
    FramegrabberTestBackend fake;
    FramegrabberSourceController controller(nullptr);
    FramegrabberControllerTestAccess::install(controller, fake);

    const auto outcome = connectBoard(controller);
    QVERIFY2(outcome.error.isEmpty(), qPrintable(outcome.error));
    QCOMPARE(outcome.result.error, DeviceConnectionError::None);
    QCOMPARE(outcome.result.effect, DeviceConnectionEffect::Applied);
    QVERIFY(outcome.result.state.has_value());
    const auto& state = *outcome.result.state;
    QVERIFY(state.connected);
    QCOMPARE(state.targetId, std::string("Test Board (SN1)"));
    QCOMPARE(state.kind, DeviceConnectionTargetKind::Interface);
    QVERIFY(state.parentConnectionId.empty());
    QVERIFY(state.interfaceInfo.has_value());
    QVERIFY(state.interfaceInfo->exclusive);
    QVERIFY(state.displayName.find("Acquisition") != std::string::npos);
    QCOMPARE(state.sources.size(), std::size_t(2));
    QCOMPARE(state.sources[0].sourceId, std::string("dma.0"));
    QCOMPARE(state.sources[0].sourceIndex, 0U);
    QCOMPARE(state.sources[1].sourceId, std::string("dma.1"));
    QCOMPARE(state.sources[1].sourceIndex, 1U);
    QVERIFY(!state.connectionId.empty());
    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        QVERIFY(fake.state.opened);
        QCOMPARE(fake.state.appletName, std::string("Acquisition"));
        QCOMPARE(fake.state.generation, std::uint64_t(1));
    }

    Sink<DeviceConnectionResult> second;
    QCOMPARE(controller.requestConnect("Test Board (SN1)", second.completion()).error,
        DeviceConnectionError::Conflict);
    QVERIFY(!second.done.load());

    Sink<DeviceConnectionResult> disconnected;
    QCOMPARE(controller.requestDisconnect(state.connectionId, disconnected.completion()).error,
        DeviceConnectionError::None);
    QVERIFY(disconnected.wait());
    const auto closed = disconnected.take();
    QCOMPARE(closed.error, DeviceConnectionError::None);
    QCOMPARE(closed.effect, DeviceConnectionEffect::Applied);
    QVERIFY(closed.state.has_value());
    QVERIFY(!closed.state->connected);
    QVERIFY(closed.state->connectionId.empty());
}

void FramegrabberControllerTests::declaredActionsCarryPresentationAndPromptKinds()
{
    FramegrabberTestBackend fake;
    FramegrabberSourceController controller(nullptr);
    FramegrabberControllerTestAccess::install(controller, fake);

    const auto descriptors = controller.toolDescriptors();
    const QStringList actions{"framegrabber.applets-refresh", "framegrabber.applet-load",
        "framegrabber.configuration-load", "framegrabber.configuration-save",
        "framegrabber.cameras-refresh"};
    for (const auto* name : {"framegrabber.state", "framegrabber.boards", "framegrabber.applets",
             "framegrabber.cameras"})
    {
        const auto* descriptor = findDescriptor(descriptors, name);
        QVERIFY2(descriptor != nullptr, name);
        QVERIFY2(descriptor->presentation.actionId.isEmpty(), name);
        QVERIFY2(descriptor->presentation.iconId.isEmpty(), name);
    }
    for (const auto& action : actions)
    {
        const auto* descriptor = findDescriptor(descriptors, action.toLatin1().constData());
        QVERIFY2(descriptor != nullptr, qPrintable(action));
        QCOMPARE(descriptor->presentation.actionId, action);
        QVERIFY2(!descriptor->presentation.iconId.isEmpty(), qPrintable(action));
        QCOMPARE(descriptor->presentation.group, QStringLiteral("framegrabber"));
        QVERIFY2(toolPresentationError(descriptor->presentation).isEmpty(),
            qPrintable(toolPresentationError(descriptor->presentation)));
        QVERIFY2(!descriptor->presentation.actionId.contains(QLatin1Char('/')),
            "Action ids are semantic, never resource paths.");
        QVERIFY(descriptor->presentation.order != 0);
    }

    const auto* load = findDescriptor(descriptors, "framegrabber.applet-load");
    QVERIFY(load != nullptr);
    const auto applet = load->inputSchema.value("properties").toObject().value("applet").toObject();
    QCOMPARE(applet.value("format").toString(), QStringLiteral("enum"));
    const auto values = applet.value("enum").toArray();
    QCOMPARE(values.size(), 2);
    QVERIFY(values.contains(QStringLiteral("Acquisition")));
    QVERIFY(values.contains(QStringLiteral("Second")));

    const auto* configuration = findDescriptor(descriptors, "framegrabber.configuration-load");
    QVERIFY(configuration != nullptr);
    const auto configurationProperty = configuration->inputSchema.value("properties").toObject()
        .value("configuration").toObject();
    QCOMPARE(configurationProperty.value("format").toString(), QStringLiteral("file-open"));
    QCOMPARE(configurationProperty.value("maxLength").toInt(), 4096);
    const auto appletProperty = configuration->inputSchema.value("properties").toObject()
        .value("applet").toObject();
    QCOMPARE(appletProperty.value("format").toString(), QStringLiteral("enum"));

    const auto* save = findDescriptor(descriptors, "framegrabber.configuration-save");
    QVERIFY(save != nullptr);
    QCOMPARE(save->inputSchema.value("properties").toObject().value("configuration").toObject()
        .value("format").toString(), QStringLiteral("file-save"));
}

void FramegrabberControllerTests::typedArgumentRequiredPrompts()
{
    FramegrabberTestBackend fake;
    FramegrabberSourceController controller(nullptr);
    FramegrabberControllerTestAccess::install(controller, fake);

    struct ToolRun
    {
        std::optional<ApiResult<void>> admission;
        std::optional<ApiResult<QJsonValue>> result;
    };
    const auto invoke = [&controller](const char* name, QJsonObject arguments) {
        ToolRun run;
        Sink<ApiResult<QJsonValue>> sink;
        run.admission = controller.requestTool(QLatin1String(name), 1, std::move(arguments),
            sink.completion());
        if (run.admission->succeeded() && sink.wait()) run.result = sink.take();
        return run;
    };

    auto run = invoke("framegrabber.applet-load",
        QJsonObject{{"board", "Test Board (SN1)"}});
    QVERIFY2(run.admission->succeeded(),
        run.admission->error() ? qPrintable(run.admission->error()->message) : "");
    QVERIFY(run.result.has_value());
    auto result = std::move(*run.result);
    QVERIFY(!result.succeeded());
    QCOMPARE(result.error()->code, ApiErrorCode::ArgumentRequired);
    auto details = result.error()->details.value("argumentRequired").toObject();
    QCOMPARE(details.value("argument").toString(), QStringLiteral("applet"));
    QCOMPARE(details.value("format").toString(), QStringLiteral("enum"));
    QVERIFY(details.value("options").toArray().contains(QStringLiteral("Acquisition")));

    auto arbitrary = invoke("framegrabber.applet-load",
        QJsonObject{{"board", "Test Board (SN1)"}, {"applet", "C:/outside/evil.hap"}});
    QVERIFY(arbitrary.admission->succeeded());
    QVERIFY(arbitrary.result.has_value());
    QVERIFY(!arbitrary.result->succeeded());
    QCOMPARE(arbitrary.result->error()->code, ApiErrorCode::ArgumentRequired);

    auto file = invoke("framegrabber.configuration-load",
        QJsonObject{{"board", "Test Board (SN1)"}});
    QVERIFY(file.admission->succeeded());
    QVERIFY(file.result.has_value());
    QVERIFY(!file.result->succeeded());
    QCOMPARE(file.result->error()->code, ApiErrorCode::ArgumentRequired);
    QCOMPARE(file.result->error()->details.value("argumentRequired").toObject()
        .value("format").toString(), QStringLiteral("file-open"));

    auto mcf = invoke("framegrabber.configuration-load",
        QJsonObject{{"board", "Test Board (SN1)"}, {"configuration", "outside.mcf"}});
    QVERIFY(mcf.admission->succeeded());
    QVERIFY(mcf.result.has_value());
    QVERIFY(!mcf.result->succeeded());
    QCOMPARE(mcf.result->error()->code, ApiErrorCode::ArgumentRequired);
    QCOMPARE(mcf.result->error()->details.value("argumentRequired").toObject()
        .value("argument").toString(), QStringLiteral("applet"));

    auto load = invoke("framegrabber.applet-load",
        QJsonObject{{"board", "Test Board (SN1)"}, {"applet", "Acquisition"}});
    QVERIFY2(load.admission->succeeded(),
        load.admission->error() ? qPrintable(load.admission->error()->message) : "");
    QVERIFY(load.result.has_value());
    QVERIFY2(load.result->succeeded(),
        load.result->error() ? qPrintable(load.result->error()->message) : "");
    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        QVERIFY(fake.state.opened);
        QCOMPARE(fake.state.appletName, std::string("Acquisition"));
    }
}

void FramegrabberControllerTests::parameterTreeCoversAppletAndCameraScopes()
{
    FramegrabberTestBackend fake;
    FramegrabberSourceController controller(nullptr);
    FramegrabberControllerTestAccess::install(controller, fake);
    const auto outcome = connectBoard(controller);
    QVERIFY2(outcome.error.isEmpty(), qPrintable(outcome.error));

    const auto inventory = [&controller] {
        Sink<DeviceParameterInventory> sink;
        const auto admission = controller.requestParameters(sink.completion());
        if (admission.error != DeviceParameterError::None) return DeviceParameterInventory{admission};
        if (!sink.wait()) return DeviceParameterInventory{};
        return sink.take();
    };
    auto parameters = inventory();
    QCOMPARE(parameters.result.error, DeviceParameterError::None);
    const auto* locatedWidth = findParameter(parameters.parameters, "Applet:0", "Width");
    QVERIFY(locatedWidth != nullptr);
    const auto width = *locatedWidth;
    QCOMPARE(width.kind, DeviceParameterKind::Integer);
    QVERIFY(width.categoryPath == std::vector<std::string>({"Acquisition"}));
    QCOMPARE(width.representation, DeviceParameterRepresentation::Number);
    QVERIFY(width.integerMinimum == std::optional<std::int64_t>(1));
    QVERIFY(width.integerMaximum == std::optional<std::int64_t>(65535));
    QVERIFY(width.value.has_value());
    QCOMPARE(std::get<std::int64_t>(*width.value), std::int64_t(640));
    const auto* locatedMode = findParameter(parameters.parameters, "Applet:0", "Mode");
    QVERIFY(locatedMode != nullptr);
    const auto mode = *locatedMode;
    QCOMPARE(mode.kind, DeviceParameterKind::Enumeration);
    QCOMPARE(mode.representation, DeviceParameterRepresentation::Choice);
    QVERIFY(mode.enumEntries == std::vector<std::string>({"Off", "On"}));
    const auto* locatedAcquire = findParameter(parameters.parameters, "Applet:0", "Acquire");
    QVERIFY(locatedAcquire != nullptr);
    const auto acquire = *locatedAcquire;
    QCOMPARE(acquire.kind, DeviceParameterKind::Command);
    QVERIFY(findParameter(parameters.parameters, "Applet:1", "Width") != nullptr);

    const auto* locatedCameraWidth = findParameter(parameters.parameters, "Camera:cxp:0", "Width");
    QVERIFY(locatedCameraWidth != nullptr);
    const auto cameraWidth = *locatedCameraWidth;
    QCOMPARE(cameraWidth.kind, DeviceParameterKind::Integer);
    QVERIFY(cameraWidth.categoryPath == std::vector<std::string>({"Image"}));
    QVERIFY(cameraWidth.unit == "px");
    QVERIFY(cameraWidth.integerMaximum == std::optional<std::int64_t>(8192));
    QVERIFY(cameraWidth.value.has_value());
    QCOMPARE(std::get<std::int64_t>(*cameraWidth.value), std::int64_t(1280));
    const auto* locatedPixelFormat = findParameter(parameters.parameters, "Camera:cxp:0", "PixelFormat");
    QVERIFY(locatedPixelFormat != nullptr);
    const auto pixelFormat = *locatedPixelFormat;
    QCOMPARE(pixelFormat.kind, DeviceParameterKind::Enumeration);
    QCOMPARE(pixelFormat.visibility, DeviceParameterVisibility::Advanced);
    QVERIFY(pixelFormat.enumEntries == std::vector<std::string>({"Mono8", "RGB8"}));
    const auto* locatedCommand = findParameter(parameters.parameters, "Camera:cxp:0", "Test");
    QVERIFY(locatedCommand != nullptr);
    const auto command = *locatedCommand;
    QCOMPARE(command.kind, DeviceParameterKind::Command);
    QCOMPARE(command.visibility, DeviceParameterVisibility::Expert);
    QVERIFY(command.writable);
    QVERIFY(!command.readable);

    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        fake.state.grabbing = true;
    }
    parameters = inventory();
    QCOMPARE(parameters.result.error, DeviceParameterError::None);
    const auto* grabbingWidth = findParameter(parameters.parameters, "Camera:cxp:0", "Width");
    QVERIFY(grabbingWidth != nullptr);
    QVERIFY(!grabbingWidth->writable);

    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        fake.state.grabbing = false;
    }
    parameters = inventory();
    const auto* stoppedWidth = findParameter(parameters.parameters, "Camera:cxp:0", "Width");
    QVERIFY(stoppedWidth != nullptr);
    QVERIFY(stoppedWidth->writable);
    const auto currentCameraWidth = *stoppedWidth;
    const auto* currentPixelFormat = findParameter(parameters.parameters, "Camera:cxp:0", "PixelFormat");
    QVERIFY(currentPixelFormat != nullptr);
    const auto currentCommand = *findParameter(parameters.parameters, "Camera:cxp:0", "Test");

    const auto write = [&controller](DeviceParameterTarget target, DeviceParameterValue value,
                                     std::optional<std::uint64_t> revision) {
        Sink<DeviceParameterResult> sink;
        const auto admission = controller.requestWriteParameter(std::move(target), std::move(value),
            revision, sink.completion());
        if (admission.error != DeviceParameterError::None) return DeviceParameterResult{admission};
        if (!sink.wait()) return DeviceParameterResult{};
        return sink.take();
    };
    auto written = write(width.target, std::int64_t(800), width.revision);
    QCOMPARE(written.error, DeviceParameterError::None);
    QCOMPARE(written.effect, DeviceParameterEffect::Applied);
    QVERIFY(written.descriptor.has_value());
    QCOMPARE(std::get<std::int64_t>(*written.descriptor->value), std::int64_t(800));
    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        QCOMPARE(fake.state.integers[1], std::int64_t(800));
    }
    const auto stale = write(width.target, std::int64_t(810), width.revision);
    QCOMPARE(stale.error, DeviceParameterError::Conflict);
    const auto outOfRange = write(width.target, std::int64_t(70000), written.descriptor->revision);
    QCOMPARE(outOfRange.error, DeviceParameterError::InvalidArgument);

    Sink<DeviceParameterResult> executed;
    QCOMPARE(controller.requestExecuteParameter(acquire.target, acquire.revision,
        executed.completion()).error, DeviceParameterError::None);
    QVERIFY(executed.wait());
    const auto executedResult = executed.take();
    QCOMPARE(executedResult.error, DeviceParameterError::None);
    QCOMPARE(executedResult.effect, DeviceParameterEffect::Applied);
    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        QCOMPARE(fake.state.appletCommands, 1);
    }

    Sink<DeviceParameterResult> readPixel;
    QCOMPARE(controller.requestReadParameter(currentPixelFormat->target, readPixel.completion()).error,
        DeviceParameterError::None);
    QVERIFY(readPixel.wait());
    const auto readPixelResult = readPixel.take();
    QCOMPARE(readPixelResult.error, DeviceParameterError::None);
    QVERIFY(readPixelResult.descriptor.has_value());
    QCOMPARE(std::get<std::string>(*readPixelResult.descriptor->value), std::string("Mono8"));

    auto cameraWritten = write(currentCameraWidth.target, std::int64_t(640), currentCameraWidth.revision);
    QCOMPARE(cameraWritten.error, DeviceParameterError::None);
    QCOMPARE(cameraWritten.effect, DeviceParameterEffect::Applied);
    QVERIFY(cameraWritten.descriptor.has_value());
    QCOMPARE(std::get<std::int64_t>(*cameraWritten.descriptor->value), std::int64_t(640));

    Sink<DeviceParameterResult> cameraCommand;
    QCOMPARE(controller.requestExecuteParameter(currentCommand.target, currentCommand.revision,
        cameraCommand.completion()).error, DeviceParameterError::None);
    QVERIFY(cameraCommand.wait());
    QCOMPARE(cameraCommand.take().error, DeviceParameterError::None);
    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        QCOMPARE(fake.state.cameraCommands, 1);
    }
}

void FramegrabberControllerTests::parameterChangesDeliverSnapshotAndDelta()
{
    FramegrabberTestBackend fake;
    FramegrabberSourceController controller(nullptr);
    FramegrabberControllerTestAccess::install(controller, fake);
    const auto outcome = connectBoard(controller);
    QVERIFY2(outcome.error.isEmpty(), qPrintable(outcome.error));
    const auto connection = outcome.result.state->connectionId;

    std::mutex changeMutex;
    std::vector<DeviceParameterChange> changes;
    const auto collect = [&](DeviceParameterChange change) {
        std::lock_guard<std::mutex> lock(changeMutex);
        changes.push_back(std::move(change));
    };
    QCOMPARE(controller.requestParameterChanges(connection, "watch-1", collect).error,
        DeviceParameterError::None);
    QVERIFY(QTest::qWaitFor([&] {
        std::lock_guard<std::mutex> lock(changeMutex);
        return !changes.empty();
    }, 5000));
    std::uint64_t snapshotRevision = 0;
    {
        std::lock_guard<std::mutex> lock(changeMutex);
        QCOMPARE(changes.size(), std::size_t(1));
        const auto& snapshot = changes.front();
        QCOMPARE(snapshot.kind, DeviceParameterChangeKind::Snapshot);
        QCOMPARE(snapshot.connectionId, connection);
        QCOMPARE(snapshot.subscriptionId, std::string("watch-1"));
        QCOMPARE(snapshot.sequence, std::uint64_t(1));
        QVERIFY(findParameter(snapshot.descriptors, "Applet:0", "Width") != nullptr);
        QVERIFY(findParameter(snapshot.descriptors, "Camera:cxp:0", "Width") != nullptr);
        snapshotRevision = snapshot.revision;
        changes.clear();
    }

    DeviceParameterTarget widthTarget;
    {
        Sink<DeviceParameterInventory> sink;
        QCOMPARE(controller.requestParameters(sink.completion()).error, DeviceParameterError::None);
        QVERIFY(sink.wait());
        const auto inventory = sink.take();
        const auto* width = findParameter(inventory.parameters, "Applet:0", "Width");
        QVERIFY(width != nullptr);
        widthTarget = width->target;
    }
    Sink<DeviceParameterResult> write;
    QCOMPARE(controller.requestWriteParameter(widthTarget, std::int64_t(801),
        std::nullopt, write.completion()).error, DeviceParameterError::None);
    QVERIFY(write.wait());
    QCOMPARE(write.take().error, DeviceParameterError::None);
    QVERIFY(QTest::qWaitFor([&] {
        std::lock_guard<std::mutex> lock(changeMutex);
        return !changes.empty();
    }, 5000));
    {
        std::lock_guard<std::mutex> lock(changeMutex);
        QCOMPARE(changes.size(), std::size_t(1));
        const auto& delta = changes.front();
        QCOMPARE(delta.kind, DeviceParameterChangeKind::Delta);
        QCOMPARE(delta.sequence, std::uint64_t(2));
        QCOMPARE(delta.descriptors.size(), std::size_t(1));
        QCOMPARE(delta.descriptors.front().target.name, std::string("Width"));
        QVERIFY(delta.descriptors.front().revision > snapshotRevision);
        changes.clear();
    }

    Sink<DeviceParameterResult> released;
    QCOMPARE(controller.releaseParameterChanges(connection, "watch-1",
        released.completion()).error, DeviceParameterError::None);
    QVERIFY(released.wait());
    QCOMPARE(released.take().error, DeviceParameterError::None);
    Sink<DeviceParameterResult> lateWrite;
    QCOMPARE(controller.requestWriteParameter(widthTarget, std::int64_t(802),
        std::nullopt, lateWrite.completion()).error, DeviceParameterError::None);
    QVERIFY(lateWrite.wait());
    QCOMPARE(lateWrite.take().error, DeviceParameterError::None);
    QTest::qWait(200);
    {
        std::lock_guard<std::mutex> lock(changeMutex);
        QVERIFY(changes.empty());
    }

    QCOMPARE(controller.requestParameterChanges(connection, "watch-1", collect).error,
        DeviceParameterError::None);
    QCOMPARE(controller.requestParameterChanges(connection, "watch-1", collect).error,
        DeviceParameterError::Conflict);
    QCOMPARE(controller.requestParameterChanges("stale", "watch-2", collect).error,
        DeviceParameterError::Conflict);
    Sink<DeviceParameterResult> releasedAgain;
    QCOMPARE(controller.releaseParameterChanges(connection, "watch-1",
        releasedAgain.completion()).error, DeviceParameterError::None);
    QVERIFY(releasedAgain.wait());
}

void FramegrabberControllerTests::notificationQueueOverflowsToResync()
{
    framegrabber::ParameterNotificationHub hub(
        [](const std::string& connection, const std::string& subscription) {
            DeviceParameterChange change;
            change.kind = DeviceParameterChangeKind::Snapshot;
            change.connectionId = connection;
            change.subscriptionId = subscription;
            DeviceParameterDescriptor descriptor;
            descriptor.target = {connection, "Applet:0", "Width", {}};
            descriptor.kind = DeviceParameterKind::Integer;
            descriptor.value = std::int64_t(1);
            descriptor.revision = 1;
            change.descriptors.push_back(descriptor);
            return change;
        });
    std::mutex gateMutex;
    std::condition_variable gate;
    bool released = false;
    hub.setAfterPopHookForTest([&] {
        std::unique_lock<std::mutex> lock(gateMutex);
        gate.wait(lock, [&] { return released; });
    });
    std::mutex collectedMutex;
    std::vector<DeviceParameterChange> collected;
    QCOMPARE(hub.add("connection", "watch", [&](DeviceParameterChange change) {
        std::lock_guard<std::mutex> lock(collectedMutex);
        collected.push_back(std::move(change));
    }).error, DeviceParameterError::None);
    hub.publishSnapshot("connection");
    std::thread drain([&] { hub.drain(); });
    // The snapshot is popped and held in the delivery hook while deltas queue up.
    for (int index = 0; index < 20; ++index)
    {
        DeviceParameterDescriptor descriptor;
        descriptor.target = {"connection", "Applet:0", "Width", {}};
        descriptor.kind = DeviceParameterKind::Integer;
        descriptor.value = std::int64_t(10 + index);
        descriptor.revision = static_cast<std::uint64_t>(10 + index);
        hub.publishDelta("connection", {descriptor}, {});
    }
    {
        std::lock_guard<std::mutex> lock(gateMutex);
        released = true;
    }
    gate.notify_all();
    drain.join();
    std::lock_guard<std::mutex> lock(collectedMutex);
    QVERIFY(collected.size() >= std::size_t(1));
    std::uint64_t previous = 0;
    bool resync = false;
    for (const auto& change : collected)
    {
        QVERIFY(change.sequence > previous);
        previous = change.sequence;
        if (!change.resync) continue;
        resync = true;
        QCOMPARE(change.kind, DeviceParameterChangeKind::Snapshot);
        QVERIFY(!change.overflowReason.empty());
        QVERIFY(change.error == DeviceParameterError::None);
    }
    QVERIFY(resync);
    QCOMPARE(hub.pending("watch"), std::size_t(0));
    hub.close();
}

QTEST_GUILESS_MAIN(FramegrabberControllerTests)
#include "FramegrabberControllerTests.moc"
