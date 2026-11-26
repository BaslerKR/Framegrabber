#include <QtTest/QtTest>

#include "FramegrabberSourceController.h"
#include "FramegrabberTestBackend.h"
#include "engine/GraphicsFrameBuilder.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <limits>
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
    void hostSelectedAppletFileLoadsAndValidatesPath();
    void hostSelectedUnicodePathUsesNativeEncoding();
    void hostSelectedNonAsciiConfigurationPath();
    void configurationPathParserHandlesSdkFormatting();
    void nonCatalogAppletMetadataGatesCameraControl();
    void boardlessLoadsUseTheSessionBoard();
    void parameterTreeCoversAppletAndCameraScopes();
    void parameterWriteRefusesUnrepresentableSdkType();
    void parameterChangesDeliverSnapshotAndDelta();
    void parameterAdmissionsStayAvailableDuringObservation();
    void cameraRefreshReplacesTheCachedFeatureTree();
    void notificationQueueOverflowsToResync();
    void terminalChangeFollowsInitialSnapshot();
    void samplesImageRejectionsPerSourceAndRun();
};

namespace {

std::vector<DeviceImageRejectionObservation> imageRejections;

void recordImageRejection(const DeviceImageRejectionObservation& observation) noexcept
{
    try { imageRejections.push_back(observation); } catch (...) {}
}

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

/** @brief The narrow SDK path is local 8-bit on Windows and UTF-8 elsewhere. */
std::string expectedSdkPath(const QString& path)
{
#ifdef Q_OS_WIN
    const QByteArray local = path.toLocal8Bit();
    return std::string(local.constData(), static_cast<std::size_t>(local.size()));
#else
    return path.toStdString();
#endif
}

} // namespace

/** @brief Calls the pure catalog/metadata resolution with an injected metadata result. */
struct FramegrabberCatalogTestAccess
{
    static Framegrabber::AppletInfo entry(const std::string& path, const std::string& board,
        const std::function<FramegrabberSystem::AppletMetadata(const std::string&, const std::string&)>& lookup,
        const std::function<void(const std::string&, bool)>& log = {})
    {
        return Framegrabber::resolveCatalogEntry({}, path, board, lookup, log);
    }
};

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
        "framegrabber.applet-load-file", "framegrabber.configuration-load",
        "framegrabber.configuration-save", "framegrabber.cameras-refresh"};
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
    QCOMPARE(load->inputSchema.value("required").toArray(), QJsonArray{QStringLiteral("applet")});
    QVERIFY(load->inputSchema.value("properties").toObject().contains(QStringLiteral("board")));
    QVERIFY(load->description.contains(QStringLiteral("connected board")));
    const auto applet = load->inputSchema.value("properties").toObject().value("applet").toObject();
    QCOMPARE(applet.value("format").toString(), QStringLiteral("enum"));
    const auto values = applet.value("enum").toArray();
    QCOMPARE(values.size(), 3);
    QVERIFY(values.contains(QStringLiteral("Acquisition")));
    // A duplicated applet name falls back to the exact catalog path so the option can load.
    QVERIFY(values.contains(QStringLiteral("C:/applets/Second.hap")));
    QVERIFY(values.contains(QStringLiteral("C:/applets/Second-alt.hap")));

    const auto* fileLoad = findDescriptor(descriptors, "framegrabber.applet-load-file");
    QVERIFY(fileLoad != nullptr);
    QCOMPARE(fileLoad->inputSchema.value("required").toArray(),
        QJsonArray{QStringLiteral("appletFile")});
    QVERIFY(fileLoad->inputSchema.value("properties").toObject().contains(QStringLiteral("board")));
    QVERIFY(fileLoad->inputSchema.value("properties").toObject()
        .contains(QStringLiteral("connectionId")));
    const auto appletFile = fileLoad->inputSchema.value("properties").toObject()
        .value("appletFile").toObject();
    QCOMPARE(appletFile.value("format").toString(), QStringLiteral("file-open"));
    QCOMPARE(appletFile.value("maxLength").toInt(), 4096);
    QCOMPARE(appletFile.value("title").toString(), QStringLiteral("Applet file"));
    QVERIFY(appletFile.value("nameFilters").toArray().contains(
        QStringLiteral("Applet libraries (*.hap *.dll *.so)")));

    const auto* configuration = findDescriptor(descriptors, "framegrabber.configuration-load");
    QVERIFY(configuration != nullptr);
    QCOMPARE(configuration->inputSchema.value("required").toArray(),
        QJsonArray{QStringLiteral("configuration")});
    QVERIFY(configuration->inputSchema.value("properties").toObject().contains(QStringLiteral("board")));
    QVERIFY(configuration->description.contains(QStringLiteral("connected board")));
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

    // A session without a connected board refuses before asking for a prompt argument.
    auto boardless = invoke("framegrabber.applet-load", QJsonObject{});
    QVERIFY(!boardless.admission->succeeded());
    QCOMPARE(boardless.admission->error()->code, ApiErrorCode::Unavailable);
    QVERIFY(boardless.admission->error()->message.contains(QStringLiteral("No board is connected")));
    QVERIFY(!boardless.result.has_value());

    auto boardlessFile = invoke("framegrabber.configuration-load", QJsonObject{});
    QVERIFY(!boardlessFile.admission->succeeded());
    QCOMPARE(boardlessFile.admission->error()->code, ApiErrorCode::Unavailable);
    QVERIFY(boardlessFile.admission->error()->message.contains(
        QStringLiteral("No board is connected")));
    QVERIFY(!boardlessFile.result.has_value());

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

void FramegrabberControllerTests::hostSelectedAppletFileLoadsAndValidatesPath()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString hapPath = QDir::toNativeSeparators(directory.filePath(QStringLiteral("chosen.hap")));
    QFile hap(hapPath);
    QVERIFY(hap.open(QIODevice::WriteOnly));
    hap.close();

    FramegrabberTestBackend fake;
    FramegrabberSourceController controller(nullptr);
    FramegrabberControllerTestAccess::install(controller, fake);

    struct ToolRun
    {
        std::optional<ApiResult<void>> admission;
        std::optional<ApiResult<QJsonValue>> result;
    };
    const auto invoke = [&controller](const QString& path) {
        ToolRun run;
        Sink<ApiResult<QJsonValue>> sink;
        run.admission = controller.requestTool(QStringLiteral("framegrabber.applet-load-file"), 1,
            QJsonObject{{QStringLiteral("board"), QStringLiteral("Test Board (SN1)")},
                        {QStringLiteral("appletFile"), path}}, sink.completion());
        if (run.admission->succeeded() && sink.wait()) run.result = sink.take();
        return run;
    };

    auto missing = invoke(QStringLiteral("C:/missing/nowhere.hap"));
    QVERIFY2(missing.admission && missing.admission->succeeded(), missing.admission->error()
        ? qPrintable(missing.admission->error()->message) : "");
    QVERIFY(missing.result.has_value());
    QVERIFY(!missing.result->succeeded());
    QCOMPARE(missing.result->error()->code, ApiErrorCode::InvalidArgument);
    QVERIFY(missing.result->error()->message.contains(QStringLiteral("does not exist")));

    auto wrongExtension = invoke(QStringLiteral("C:/missing/nowhere.txt"));
    QVERIFY(wrongExtension.result.has_value());
    QVERIFY(!wrongExtension.result->succeeded());
    QCOMPARE(wrongExtension.result->error()->code, ApiErrorCode::InvalidArgument);
    QVERIFY(wrongExtension.result->error()->message.contains(
        QStringLiteral("Unsupported applet file extension")));

    // A valid path outside the installed catalog reaches Fg_Init and reports its load reason.
    auto loadFailure = invoke(hapPath);
    QVERIFY(loadFailure.result.has_value());
    QVERIFY(!loadFailure.result->succeeded());
    QCOMPARE(loadFailure.result->error()->code, ApiErrorCode::Unavailable);
    QVERIFY(loadFailure.result->error()->message.contains(
        QStringLiteral("Failed to load the applet from")));

    // An existing file outside the catalog reaches the backend without being cataloged.
    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        fake.state.acceptAnyAppletFile = true;
    }
    auto loaded = invoke(hapPath);
    QVERIFY2(loaded.admission->succeeded(), loaded.admission->error()
        ? qPrintable(loaded.admission->error()->message) : "");
    QVERIFY(loaded.result.has_value());
    QVERIFY2(loaded.result->succeeded(), loaded.result->error()
        ? qPrintable(loaded.result->error()->message) : "");
    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        QCOMPARE(fake.state.appletPath, expectedSdkPath(hapPath));
    }
}

void FramegrabberControllerTests::hostSelectedUnicodePathUsesNativeEncoding()
{
    // Pick a character the active code page can encode: Hangul on CP949, U+00E9 on CP1252.
    const QList<QChar> candidates{QChar(0xD55C), QChar(0x00E9), QChar(0x4E2D), QChar(0x0416)};
    const auto roundTrips = [](const QChar character) {
        const QString value = QStringLiteral("applet-") + character;
        return QString::fromLocal8Bit(value.toLocal8Bit()) == value;
    };
    QChar chosen;
    for (const QChar candidate : candidates) {
#ifndef Q_OS_WIN
        chosen = candidate;
        break;
#else
        if (roundTrips(candidate)) { chosen = candidate; break; }
#endif
    }
    QVERIFY(!chosen.isNull());
    const QString templatePath = QDir::tempPath() + QStringLiteral("/applet-")
        + chosen + QStringLiteral("-XXXXXX");
    QTemporaryDir directory(templatePath);
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    const QString hapPath = QDir::toNativeSeparators(directory.filePath(QStringLiteral("unicode.hap")));
    QFile hap(hapPath);
    QVERIFY(hap.open(QIODevice::WriteOnly));
    hap.close();

    FramegrabberTestBackend fake;
    FramegrabberSourceController controller(nullptr);
    FramegrabberControllerTestAccess::install(controller, fake);
    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        fake.state.acceptAnyAppletFile = true;
    }
    struct ToolRun
    {
        std::optional<ApiResult<void>> admission;
        std::optional<ApiResult<QJsonValue>> result;
    };
    const auto invoke = [&controller](const QString& path, const QString& connection = {}) {
        QJsonObject arguments{{QStringLiteral("board"), QStringLiteral("Test Board (SN1)")},
                              {QStringLiteral("appletFile"), path}};
        if (!connection.isEmpty()) arguments.insert(QStringLiteral("connectionId"), connection);
        ToolRun run;
        Sink<ApiResult<QJsonValue>> sink;
        run.admission = controller.requestTool(QStringLiteral("framegrabber.applet-load-file"), 1,
                                               std::move(arguments), sink.completion());
        if (run.admission->succeeded() && sink.wait()) run.result = sink.take();
        return run;
    };

    auto run = invoke(hapPath);
    QVERIFY2(run.admission->succeeded(), run.admission->error()
        ? qPrintable(run.admission->error()->message) : "");
    QVERIFY(run.result.has_value());
    QVERIFY2(run.result->succeeded(), run.result->error()
        ? qPrintable(run.result->error()->message) : "");
    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        QCOMPARE(fake.state.appletPath, expectedSdkPath(hapPath));
    }
#ifdef Q_OS_WIN
    for (const QChar candidate : candidates) {
        if (roundTrips(candidate)) continue;
        const QString unsupported = QDir::toNativeSeparators(directory.filePath(
            QStringLiteral("reject-") + candidate + QStringLiteral(".hap")));
        QFile file(unsupported);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        const QString connection = run.result->value().toObject()
            .value(QStringLiteral("connectionId")).toString();
        QVERIFY(!connection.isEmpty());
        auto refused = invoke(unsupported, connection);
        QVERIFY(refused.result.has_value());
        QVERIFY(!refused.result->succeeded());
        QCOMPARE(refused.result->error()->code, ApiErrorCode::InvalidArgument);
        QVERIFY(refused.result->error()->message.contains(QStringLiteral("Windows code page")));
        break;
    }
#endif
}

void FramegrabberControllerTests::hostSelectedNonAsciiConfigurationPath()
{
    const QList<QChar> candidates{QChar(0xD55C), QChar(0x00E9), QChar(0x4E2D), QChar(0x0416)};
    const auto roundTrips = [](const QChar character) {
        const QString value = QStringLiteral("mcf-") + character;
        return QString::fromLocal8Bit(value.toLocal8Bit()) == value;
    };
    QChar chosen;
    for (const QChar candidate : candidates) {
#ifndef Q_OS_WIN
        chosen = candidate;
        break;
#else
        if (roundTrips(candidate)) { chosen = candidate; break; }
#endif
    }
    QVERIFY(!chosen.isNull());
    QTemporaryDir directory(QDir::tempPath() + QStringLiteral("/mcf-") + chosen + QStringLiteral("-XXXXXX"));
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    const QString mcfPath = QDir::toNativeSeparators(directory.filePath(QStringLiteral("unicode.mcf")));
    QFile mcf(mcfPath);
    QVERIFY(mcf.open(QIODevice::WriteOnly));
    mcf.close();

    FramegrabberTestBackend fake;
    FramegrabberSourceController controller(nullptr);
    FramegrabberControllerTestAccess::install(controller, fake);
    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        fake.state.acceptAnyConfiguration = true;
    }
    struct ToolRun
    {
        std::optional<ApiResult<void>> admission;
        std::optional<ApiResult<QJsonValue>> result;
    };
    const auto invoke = [&controller](const QString& name, const QString& path,
                                      const QString& connection = {}) {
        QJsonObject arguments{{QStringLiteral("configuration"), path}};
        if (name == QLatin1String("framegrabber.configuration-load"))
            arguments.insert(QStringLiteral("board"), QStringLiteral("Test Board (SN1)"));
        if (!connection.isEmpty()) arguments.insert(QStringLiteral("connectionId"), connection);
        ToolRun run;
        Sink<ApiResult<QJsonValue>> sink;
        run.admission = controller.requestTool(name, 1, std::move(arguments), sink.completion());
        if (run.admission->succeeded() && sink.wait()) run.result = sink.take();
        return run;
    };

    auto load = invoke(QStringLiteral("framegrabber.configuration-load"), mcfPath);
    QVERIFY2(load.admission->succeeded(), load.admission->error()
        ? qPrintable(load.admission->error()->message) : "");
    QVERIFY(load.result.has_value());
    QVERIFY2(load.result->succeeded(), load.result->error()
        ? qPrintable(load.result->error()->message) : "");
    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        QCOMPARE(fake.state.configuration, expectedSdkPath(mcfPath));
    }
    const QString connection = load.result->value().toObject()
        .value(QStringLiteral("connectionId")).toString();
    QVERIFY(!connection.isEmpty());
    auto save = invoke(QStringLiteral("framegrabber.configuration-save"), mcfPath, connection);
    QVERIFY2(save.admission->succeeded(), save.admission->error()
        ? qPrintable(save.admission->error()->message) : "");
    QVERIFY(save.result.has_value());
    QVERIFY2(save.result->succeeded(), save.result->error()
        ? qPrintable(save.result->error()->message) : "");
    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        QCOMPARE(fake.state.savedConfiguration, expectedSdkPath(mcfPath));
    }

    // A non-catalog applet named by the non-ASCII MCF must still offer the enum prompt.
    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        fake.state.mcfReferencesOutsideApplet = true;
    }
    auto outside = invoke(QStringLiteral("framegrabber.configuration-load"), mcfPath, connection);
    QVERIFY(outside.result.has_value());
    QVERIFY(!outside.result->succeeded());
    QCOMPARE(outside.result->error()->code, ApiErrorCode::ArgumentRequired);
    QCOMPARE(outside.result->error()->details.value(QStringLiteral("argumentRequired")).toObject()
                 .value(QStringLiteral("argument")).toString(), QStringLiteral("applet"));

#ifdef Q_OS_WIN
    for (const QChar candidate : candidates) {
        if (roundTrips(candidate)) continue;
        const QString unsupported = QDir::toNativeSeparators(directory.filePath(
            QStringLiteral("reject-") + candidate + QStringLiteral(".mcf")));
        QFile file(unsupported);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        auto refused = invoke(QStringLiteral("framegrabber.configuration-load"), unsupported);
        QVERIFY(refused.admission.has_value());
        QVERIFY(!refused.admission->succeeded());
        QCOMPARE(refused.admission->error()->code, ApiErrorCode::InvalidArgument);
        QVERIFY(refused.admission->error()->message.contains(QStringLiteral("file path cannot be represented")));
        break;
    }
#endif
}

void FramegrabberControllerTests::configurationPathParserHandlesSdkFormatting()
{
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    const auto writeFile = [&directory](const QString& name, const QByteArray& contents) {
        QFile file(directory.filePath(name));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        if (file.write(contents) != contents.size()) return false;
        file.close();
        return true;
    };
    QVERIFY(writeFile(QStringLiteral("Acq_SingleCXP12Area"), "applet"));
    QVERIFY(writeFile(QStringLiteral("First.hap"), "first"));
    QVERIFY(writeFile(QStringLiteral("Second.hap"), "second"));

    const auto sdkPath = [](const QString& path) { return expectedSdkPath(path); };
    Framegrabber board(nullptr);

    const QString sdkMcf = directory.filePath(QStringLiteral("sdk.mcf"));
    QByteArray sdkContents("\xEF\xBB\xBF[fglib5]\r\n");
    sdkContents += "  FileName = \"Acq_SingleCXP12Area\" ;  \r\n";
    QVERIFY(writeFile(QStringLiteral("sdk.mcf"), sdkContents));
    QCOMPARE(board.appletPathFromConfiguration(sdkPath(sdkMcf)),
        sdkPath(QDir::toNativeSeparators(directory.filePath(QStringLiteral("Acq_SingleCXP12Area")))));

    const QString precedenceMcf = directory.filePath(QStringLiteral("precedence.mcf"));
    QVERIFY(writeFile(QStringLiteral("precedence.mcf"),
        "[Other]\nHapName='Second.hap';\n[fglib5]\nFileName='First.hap';\n"));
    QCOMPARE(board.appletPathFromConfiguration(sdkPath(precedenceMcf)),
        sdkPath(QDir::toNativeSeparators(directory.filePath(QStringLiteral("Second.hap")))));

    const QString relativeMcf = directory.filePath(QStringLiteral("relative.mcf"));
    QVERIFY(writeFile(QStringLiteral("relative.mcf"),
        "[fglib5]\nFileName='missing/../Second.hap';\n"));
    QCOMPARE(board.appletPathFromConfiguration(sdkPath(relativeMcf)),
        sdkPath(QDir::toNativeSeparators(directory.filePath(QStringLiteral("Second.hap")))));

    const QString wrongExtension = directory.filePath(QStringLiteral("wrong.txt"));
    QVERIFY(writeFile(QStringLiteral("wrong.txt"), "[fglib5]\nFileName='First.hap';\n"));
    QVERIFY(board.appletPathFromConfiguration(sdkPath(wrongExtension)).empty());
    QVERIFY(board.appletPathFromConfiguration(sdkPath(directory.filePath(QStringLiteral("missing.mcf")))).empty());
}

void FramegrabberControllerTests::nonCatalogAppletMetadataGatesCameraControl()
{
    const std::string path = "C:/custom/chosen.hap";
    const std::string board = "Test Board (SN1)";
    QString loggedLimitation;
    const auto log = [&loggedLimitation](const std::string& message, bool warning) {
        QVERIFY(warning);
        loggedLimitation = QString::fromStdString(message);
    };
    const auto hit = FramegrabberCatalogTestAccess::entry(path, board,
        [](const std::string&, const std::string&) {
            FramegrabberSystem::AppletMetadata metadata;
            metadata.supportsCameraControl = true;
            metadata.version = "9.9";
            return metadata;
        });
    QCOMPARE(hit.path, path);
    QVERIFY(hit.supportsCameraControl); // gates Sgc_initBoard in adoptBoard
    QCOMPARE(hit.version, std::string("9.9"));
    const auto miss = FramegrabberCatalogTestAccess::entry(path, board,
        [](const std::string&, const std::string&) { return FramegrabberSystem::AppletMetadata{}; }, log);
    QVERIFY(!miss.supportsCameraControl);
    QVERIFY(miss.version.empty());
    QVERIFY(loggedLimitation.contains(QStringLiteral("No SDK camera-control tag")));
}

void FramegrabberControllerTests::boardlessLoadsUseTheSessionBoard()
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
    const auto expectNoEffect = [&fake] {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        QVERIFY(!fake.state.opened);
        QVERIFY(fake.state.loadedBoard.empty());
    };

    // An omitted board with no session connection refuses before any device effect.
    auto refused = invoke("framegrabber.applet-load",
        QJsonObject{{"applet", "Acquisition"}});
    QVERIFY(!refused.admission->succeeded());
    QCOMPARE(refused.admission->error()->code, ApiErrorCode::Unavailable);
    QVERIFY(refused.admission->error()->message.contains(QStringLiteral("No board is connected")));
    QVERIFY(!refused.result.has_value());
    expectNoEffect();

    auto refusedConfiguration = invoke("framegrabber.configuration-load",
        QJsonObject{{"configuration", "installed.mcf"}});
    QVERIFY(!refusedConfiguration.admission->succeeded());
    QCOMPARE(refusedConfiguration.admission->error()->code, ApiErrorCode::Unavailable);
    QVERIFY(refusedConfiguration.admission->error()->message.contains(
        QStringLiteral("No board is connected")));
    QVERIFY(!refusedConfiguration.result.has_value());
    expectNoEffect();

    // A connected session supplies the board for both loads.
    const auto outcome = connectBoard(controller);
    QVERIFY2(outcome.error.isEmpty(), qPrintable(outcome.error));
    QString connection = QString::fromStdString(outcome.result.state->connectionId);
    QVERIFY(!connection.isEmpty());

    // With a connected session the board resolves first, then only the prompt argument is asked.
    auto prompt = invoke("framegrabber.applet-load",
        QJsonObject{{"connectionId", connection}});
    QVERIFY(prompt.admission->succeeded());
    QVERIFY(prompt.result.has_value());
    QVERIFY(!prompt.result->succeeded());
    QCOMPARE(prompt.result->error()->code, ApiErrorCode::ArgumentRequired);
    QCOMPARE(prompt.result->error()->details.value("argumentRequired").toObject()
        .value("argument").toString(), QStringLiteral("applet"));
    QCOMPARE(prompt.result->error()->details.value("argumentRequired").toObject()
        .value("format").toString(), QStringLiteral("enum"));

    auto promptFile = invoke("framegrabber.configuration-load",
        QJsonObject{{"connectionId", connection}});
    QVERIFY(promptFile.admission->succeeded());
    QVERIFY(promptFile.result.has_value());
    QVERIFY(!promptFile.result->succeeded());
    QCOMPARE(promptFile.result->error()->code, ApiErrorCode::ArgumentRequired);
    QCOMPARE(promptFile.result->error()->details.value("argumentRequired").toObject()
        .value("argument").toString(), QStringLiteral("configuration"));
    QCOMPARE(promptFile.result->error()->details.value("argumentRequired").toObject()
        .value("format").toString(), QStringLiteral("file-open"));

    auto load = invoke("framegrabber.applet-load",
        QJsonObject{{"applet", "Acquisition"}, {"connectionId", connection}});
    QVERIFY2(load.admission->succeeded(),
        load.admission->error() ? qPrintable(load.admission->error()->message) : "");
    QVERIFY(load.result.has_value());
    QVERIFY2(load.result->succeeded(),
        load.result->error() ? qPrintable(load.result->error()->message) : "");
    QCOMPARE(load.result->value().toObject().value("board").toString(),
        QStringLiteral("Test Board (SN1)"));
    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        QVERIFY(fake.state.opened);
        QCOMPARE(fake.state.appletName, std::string("Acquisition"));
        QCOMPARE(fake.state.loadedBoard, std::string("Test Board (SN1)"));
    }
    connection = load.result->value().toObject().value("connectionId").toString();
    QVERIFY(!connection.isEmpty());

    auto configuration = invoke("framegrabber.configuration-load",
        QJsonObject{{"configuration", "installed.mcf"}, {"connectionId", connection}});
    QVERIFY2(configuration.admission->succeeded(),
        configuration.admission->error() ? qPrintable(configuration.admission->error()->message) : "");
    QVERIFY(configuration.result.has_value());
    QVERIFY2(configuration.result->succeeded(),
        configuration.result->error() ? qPrintable(configuration.result->error()->message) : "");
    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        QVERIFY(fake.state.opened);
        QCOMPARE(fake.state.appletName, std::string("Configured"));
        QCOMPARE(fake.state.loadedBoard, std::string("Test Board (SN1)"));
        QCOMPARE(fake.state.configuration, std::string("installed.mcf"));
    }
    connection = configuration.result->value().toObject().value("connectionId").toString();
    QVERIFY(!connection.isEmpty());

    // An explicit board that is not the session's board keeps the current refusal.
    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        FramegrabberSystem::BoardInfo second;
        second.index = 1;
        second.name = "Second Board";
        second.serial = "SN2";
        fake.state.boards.push_back(second);
    }
    auto other = invoke("framegrabber.applet-load",
        QJsonObject{{"board", "Second Board (SN2)"}, {"applet", "Acquisition"},
                    {"connectionId", connection}});
    QVERIFY(!other.admission->succeeded());
    QCOMPARE(other.admission->error()->code, ApiErrorCode::Conflict);
    QVERIFY(!other.result.has_value());
    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        QCOMPARE(fake.state.loadedBoard, std::string("Test Board (SN1)"));
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
    // The feature's access mode comes from its pValue register, not a direct attribute.
    QVERIFY(locatedCameraWidth->writable);
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
    // ImposedAccessMode overrides the referenced register's access mode.
    const auto* locatedSerial = findParameter(parameters.parameters, "Camera:cxp:0", "SerialNumber");
    QVERIFY(locatedSerial != nullptr);
    QVERIFY(locatedSerial->readable);
    QVERIFY(!locatedSerial->writable);
    QVERIFY(locatedSerial->available);
    // pIsLocked is read from the device; a locked feature is never reported writable.
    const auto* locatedGain = findParameter(parameters.parameters, "Camera:cxp:0", "Gain");
    QVERIFY(locatedGain != nullptr);
    QVERIFY(locatedGain->readable);
    QVERIFY(!locatedGain->writable);

    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        fake.state.failCameraGateReads = true;
    }
    parameters = inventory();
    QCOMPARE(parameters.result.error, DeviceParameterError::None);
    const auto* gateFailureGain = findParameter(parameters.parameters, "Camera:cxp:0", "Gain");
    QVERIFY(gateFailureGain != nullptr);
    QVERIFY(gateFailureGain->readable);
    QVERIFY(gateFailureGain->available);
    QVERIFY(!gateFailureGain->writable);
    QVERIFY(gateFailureGain->observationError.empty());
    QVERIFY(gateFailureGain->value.has_value());
    Sink<DeviceParameterResult> gateFailureRead;
    QCOMPARE(controller.requestReadParameter(gateFailureGain->target,
        gateFailureRead.completion()).error, DeviceParameterError::None);
    QVERIFY(gateFailureRead.wait());
    QCOMPARE(gateFailureRead.take().error, DeviceParameterError::None);
    Sink<DeviceParameterResult> gateFailureWrite;
    QCOMPARE(controller.requestWriteParameter(gateFailureGain->target, 2.0,
        gateFailureGain->revision, gateFailureWrite.completion()).error, DeviceParameterError::None);
    QVERIFY(gateFailureWrite.wait());
    const auto gateFailureWriteResult = gateFailureWrite.take();
    QCOMPARE(gateFailureWriteResult.error, DeviceParameterError::BackendFailure);
    QVERIFY(gateFailureWriteResult.message.find("lock state could not be read") != std::string::npos);
    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        fake.state.failCameraGateReads = false;
    }

    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        fake.state.failCameraAvailabilityReads = true;
    }
    parameters = inventory();
    const auto* availabilityFailureWidth = findParameter(parameters.parameters, "Camera:cxp:0", "Width");
    QVERIFY(availabilityFailureWidth != nullptr);
    QVERIFY(availabilityFailureWidth->readable);
    QVERIFY(availabilityFailureWidth->available);
    QVERIFY(!availabilityFailureWidth->writable);
    QVERIFY(availabilityFailureWidth->observationError.empty());
    QVERIFY(availabilityFailureWidth->value.has_value());
    Sink<DeviceParameterResult> availabilityFailureRead;
    QCOMPARE(controller.requestReadParameter(availabilityFailureWidth->target,
        availabilityFailureRead.completion()).error, DeviceParameterError::None);
    QVERIFY(availabilityFailureRead.wait());
    QCOMPARE(availabilityFailureRead.take().error, DeviceParameterError::None);
    Sink<DeviceParameterResult> availabilityFailureWrite;
    QCOMPARE(controller.requestWriteParameter(availabilityFailureWidth->target, std::int64_t(640),
        availabilityFailureWidth->revision, availabilityFailureWrite.completion()).error,
        DeviceParameterError::None);
    QVERIFY(availabilityFailureWrite.wait());
    const auto availabilityFailureWriteResult = availabilityFailureWrite.take();
    QCOMPARE(availabilityFailureWriteResult.error, DeviceParameterError::BackendFailure);
    QVERIFY(availabilityFailureWriteResult.message.find("availability state could not be read")
        != std::string::npos);
    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        fake.state.failCameraAvailabilityReads = false;
    }

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

void FramegrabberControllerTests::parameterWriteRefusesUnrepresentableSdkType()
{
    FramegrabberTestBackend fake;
    FramegrabberSourceController controller(nullptr);
    FramegrabberControllerTestAccess::install(controller, fake);
    const auto outcome = connectBoard(controller);
    QVERIFY2(outcome.error.isEmpty(), qPrintable(outcome.error));
    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        fake.state.widthMaximum = (std::numeric_limits<std::int64_t>::max)();
    }

    Sink<DeviceParameterInventory> inventorySink;
    QCOMPARE(controller.requestParameters(inventorySink.completion()).error, DeviceParameterError::None);
    QVERIFY(inventorySink.wait());
    const auto inventory = inventorySink.take();
    const auto* width = findParameter(inventory.parameters, "Applet:0", "Width");
    QVERIFY(width != nullptr);

    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        fake.state.widthPrototypeOverride = 0.0;
    }
    Sink<DeviceParameterResult> resultSink;
    QCOMPARE(controller.requestWriteParameter(width->target, std::int64_t(800), width->revision,
        resultSink.completion()).error, DeviceParameterError::None);
    QVERIFY(resultSink.wait());
    const auto result = resultSink.take();
    QCOMPARE(result.error, DeviceParameterError::Unsupported);
    QCOMPARE(result.effect, DeviceParameterEffect::None);
    QVERIFY(result.message.find("does not match") != std::string::npos);
    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        QVERIFY(fake.state.parameterWrites.empty());
        QCOMPARE(fake.state.integers[1], std::int64_t(640));
        fake.state.widthPrototypeOverride = std::int32_t{0};
    }
    Sink<DeviceParameterResult> overflowSink;
    QCOMPARE(controller.requestWriteParameter(width->target,
        (std::numeric_limits<std::int64_t>::max)(), width->revision,
        overflowSink.completion()).error, DeviceParameterError::None);
    QVERIFY(overflowSink.wait());
    const auto overflow = overflowSink.take();
    QCOMPARE(overflow.error, DeviceParameterError::InvalidArgument);
    QVERIFY(overflow.message.find("not representable") != std::string::npos);
    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        QVERIFY(fake.state.parameterWrites.empty());
        QCOMPARE(fake.state.integers[1], std::int64_t(640));
        fake.state.widthPrototypeOverride.reset();
        fake.state.widthPrototypeMissing = true;
    }
    Sink<DeviceParameterResult> unsupportedSink;
    QCOMPARE(controller.requestWriteParameter(width->target, std::int64_t(800), width->revision,
        unsupportedSink.completion()).error, DeviceParameterError::None);
    QVERIFY(unsupportedSink.wait());
    const auto unsupported = unsupportedSink.take();
    QCOMPARE(unsupported.error, DeviceParameterError::Unsupported);
    QCOMPARE(unsupported.effect, DeviceParameterEffect::None);
    QVERIFY(unsupported.message.find("not supported") != std::string::npos);
    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        QVERIFY(fake.state.parameterWrites.empty());
        QCOMPARE(fake.state.integers[1], std::int64_t(640));
        fake.state.widthPrototypeMissing = false;
        fake.state.disconnectBeforePrototype = true;
    }
    Sink<DeviceParameterResult> disconnectedSink;
    QCOMPARE(controller.requestWriteParameter(width->target, std::int64_t(800), width->revision,
        disconnectedSink.completion()).error, DeviceParameterError::None);
    QVERIFY(disconnectedSink.wait());
    const auto disconnected = disconnectedSink.take();
    QCOMPARE(disconnected.error, DeviceParameterError::Disconnected);
    QVERIFY(disconnected.message.find("closed before") != std::string::npos);
    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        QVERIFY(fake.state.parameterWrites.empty());
        QCOMPARE(fake.state.integers[1], std::int64_t(640));
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
    // A read and an inventory before the write must not consume the delta.
    Sink<DeviceParameterResult> before;
    QCOMPARE(controller.requestReadParameter(widthTarget, before.completion()).error,
        DeviceParameterError::None);
    QVERIFY(before.wait());
    QCOMPARE(before.take().error, DeviceParameterError::None);
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
        QCOMPARE(delta.descriptors.front().target.scope, std::string("Applet:0"));
        QCOMPARE(delta.descriptors.front().target.name, std::string("Width"));
        QVERIFY(delta.descriptors.front().value.has_value());
        QCOMPARE(std::get<std::int64_t>(*delta.descriptors.front().value), std::int64_t(801));
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

void FramegrabberControllerTests::parameterAdmissionsStayAvailableDuringObservation()
{
    FramegrabberTestBackend fake;
    FramegrabberSourceController controller(nullptr);
    FramegrabberControllerTestAccess::install(controller, fake);
    const auto outcome = connectBoard(controller);
    QVERIFY2(outcome.error.isEmpty(), qPrintable(outcome.error));
    const auto connection = outcome.result.state->connectionId;

    std::mutex changeMutex;
    int changes = 0;
    QCOMPARE(controller.requestParameterChanges(connection, "watch-admission",
        [&](DeviceParameterChange) {
            std::lock_guard<std::mutex> lock(changeMutex);
            ++changes;
        }).error, DeviceParameterError::None);
    QVERIFY(QTest::qWaitFor([&] {
        std::lock_guard<std::mutex> lock(changeMutex);
        return changes > 0;
    }, 5000));

    // Block the observation thread inside a parameter read, then admit an inventory while it
    // is running. Before the fix the observation held the shared lifecycle gate and this
    // admission was refused as busy; now it is admitted and waits for the observation pass.
    {
        std::lock_guard<std::mutex> lock(fake.state.observationGateMutex);
        fake.state.blockObservation = true;
        fake.state.observationEntered = false;
    }
    FramegrabberControllerTestAccess::wakeObservation(controller, false);
    QVERIFY(QTest::qWaitFor([&] {
        std::lock_guard<std::mutex> lock(fake.state.observationGateMutex);
        return fake.state.observationEntered;
    }, 5000));
    Sink<DeviceParameterInventory> during;
    QCOMPARE(controller.requestParameters(during.completion()).error, DeviceParameterError::None);
    {
        std::lock_guard<std::mutex> lock(fake.state.observationGateMutex);
        fake.state.blockObservation = false;
    }
    fake.state.observationGate.notify_all();
    QVERIFY(during.wait());
    QCOMPARE(during.take().result.error, DeviceParameterError::None);
    Sink<DeviceParameterInventory> after;
    QCOMPARE(controller.requestParameters(after.completion()).error, DeviceParameterError::None);
    QVERIFY(after.wait());
    QCOMPARE(after.take().result.error, DeviceParameterError::None);
    Sink<DeviceParameterResult> released;
    QCOMPARE(controller.releaseParameterChanges(connection, "watch-admission",
        released.completion()).error, DeviceParameterError::None);
    QVERIFY(released.wait());
}

void FramegrabberControllerTests::cameraRefreshReplacesTheCachedFeatureTree()
{
    FramegrabberTestBackend fake;
    FramegrabberSourceController controller(nullptr);
    FramegrabberControllerTestAccess::install(controller, fake);
    const auto outcome = connectBoard(controller);
    QVERIFY2(outcome.error.isEmpty(), qPrintable(outcome.error));
    const auto connection = outcome.result.state->connectionId;

    const auto inventory = [&controller] {
        Sink<DeviceParameterInventory> sink;
        const auto admission = controller.requestParameters(sink.completion());
        if (admission.error != DeviceParameterError::None) return DeviceParameterInventory{admission};
        if (!sink.wait()) return DeviceParameterInventory{};
        return sink.take();
    };
    auto parameters = inventory();
    QCOMPARE(parameters.result.error, DeviceParameterError::None);
    QVERIFY(findParameter(parameters.parameters, "Camera:cxp:0", "Width") != nullptr);
    QVERIFY(findParameter(parameters.parameters, "Camera:cxp:0", "Height") == nullptr);

    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        fake.state.extendedCameraXml = true;
    }
    Sink<ApiResult<QJsonValue>> refreshed;
    QCOMPARE(controller.requestTool(QStringLiteral("framegrabber.cameras-refresh"), 1,
        QJsonObject{{QStringLiteral("connectionId"), QString::fromStdString(connection)}},
        refreshed.completion()).succeeded(), true);
    QVERIFY(refreshed.wait());
    QVERIFY(refreshed.take().succeeded());
    parameters = inventory();
    QCOMPARE(parameters.result.error, DeviceParameterError::None);
    const auto* height = findParameter(parameters.parameters, "Camera:cxp:0", "Height");
    QVERIFY(height != nullptr);
    QVERIFY(height->categoryPath == std::vector<std::string>({"Image"}));
    QVERIFY(findParameter(parameters.parameters, "Camera:cxp:0", "Width") != nullptr);

    // A temporary empty feature XML drops the camera scope instead of serving the old tree.
    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        fake.state.cameraXmlEmpty = true;
    }
    parameters = inventory();
    QCOMPARE(parameters.result.error, DeviceParameterError::None);
    QVERIFY(findParameter(parameters.parameters, "Camera:cxp:0", "Width") == nullptr);
    {
        std::lock_guard<std::mutex> lock(fake.state.mutex);
        fake.state.cameraXmlEmpty = false;
    }
    parameters = inventory();
    QCOMPARE(parameters.result.error, DeviceParameterError::None);
    QVERIFY(findParameter(parameters.parameters, "Camera:cxp:0", "Width") != nullptr);
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

void FramegrabberControllerTests::terminalChangeFollowsInitialSnapshot()
{
    framegrabber::ParameterNotificationHub hub(
        [](const std::string& connection, const std::string& subscription) {
            DeviceParameterChange change;
            change.kind = DeviceParameterChangeKind::Snapshot;
            change.connectionId = connection;
            change.subscriptionId = subscription;
            return change;
        });
    std::vector<DeviceParameterChange> collected;
    QCOMPARE(hub.add("connection", "watch", [&](DeviceParameterChange change) {
        collected.push_back(std::move(change));
    }).error, DeviceParameterError::None);
    hub.publishTerminal("connection", DeviceParameterError::Disconnected, "closed");
    hub.drain();
    QCOMPARE(collected.size(), std::size_t(2));
    QCOMPARE(collected[0].kind, DeviceParameterChangeKind::Snapshot);
    QCOMPARE(collected[0].error, DeviceParameterError::None);
    QCOMPARE(collected[0].sequence, std::uint64_t(1));
    QCOMPARE(collected[1].kind, DeviceParameterChangeKind::Delta);
    QCOMPARE(collected[1].error, DeviceParameterError::Disconnected);
    QCOMPARE(collected[1].sequence, std::uint64_t(2));
    QCOMPARE(hub.size(), std::size_t(0));
}

void FramegrabberControllerTests::samplesImageRejectionsPerSourceAndRun()
{
    using Access = FramegrabberControllerTestAccess;
    imageRejections.clear();
    FramegrabberSourceController controller(nullptr, nullptr, &recordImageRejection);
    auto bytes = std::make_shared<std::vector<std::uint8_t>>(7U);
    GraphicsFrameBuilder builder;
    auto borrowed = builder.borrow(bytes, bytes->data(), bytes->size(), GraphicsBorrow::Immutable);
    QVERIFY(borrowed.hasValue());
    GraphicsImage image;
    image.pixels = std::move(borrowed).value();
    image.width = 4;
    image.height = 2;
    image.stride = 4;
    image.pixelFormat = GraphicsImagePixelFormat::Mono8;
    Access::beginImageRejectionRun(controller, false);
    const auto truncated = GraphicsImageFailure::TruncatedBytes;
    for (std::uint64_t sequence = 1; sequence <= 101; ++sequence) {
        image.frameSequence = sequence;
        Access::reportImageRejection(controller, image, 2U, truncated);
    }
    QCOMPARE(imageRejections.size(), std::size_t{2});
    QCOMPARE(imageRejections[0].frameSequence, std::uint64_t{1});
    QCOMPARE(imageRejections[1].frameSequence, std::uint64_t{100});
    QCOMPARE(imageRejections.front().sourceIndex, std::uint32_t{2});
    QCOMPARE(imageRejections.front().reason, DeviceImageRejection::TruncatedBytes);
    QCOMPARE(imageRejections.front().readableBytes, std::uint64_t{7});
    QCOMPARE(imageRejections.front().requiredBytes, std::uint64_t{8});
    QCOMPARE(imageRejections.front().reportedStride, std::uint64_t{0});

    Access::reportImageRejection(controller, image, 3U, truncated);
    Access::reportImageRejection(controller, image, 2U, GraphicsImageFailure::MissingBytes);
    Access::reportImageRejection(controller, image, 2U, GraphicsImageFailure::StorageRefused);
    Access::reportImageRejection(controller, image, 2U, GraphicsImageFailure::None);
    QCOMPARE(imageRejections.size(), std::size_t{5});
    QCOMPARE(imageRejections.back().reason, DeviceImageRejection::StorageRefused);

    Access::beginImageRejectionRun(controller, false);
    Access::reportImageRejection(controller, image, 2U, truncated);
    QCOMPARE(imageRejections.size(), std::size_t{6});
    Access::beginImageRejectionRun(controller, true);
    Access::reportImageRejection(controller, image, 2U, truncated);
    Access::reportImageRejection(controller, image, 2U, truncated);
    QCOMPARE(imageRejections.size(), std::size_t{8});
}

QTEST_GUILESS_MAIN(FramegrabberControllerTests)
#include "FramegrabberControllerTests.moc"
