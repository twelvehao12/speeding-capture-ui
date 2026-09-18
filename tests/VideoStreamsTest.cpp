#include "../src/rv1126b/application/VideoStreamsController.h"
#include "../src/ui/LivePreviewPanel.h"

#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QtTest>

using namespace rv1126b;

namespace {

ApiError failure(ApiErrorCategory category, int status = 0)
{
    ApiError error;
    error.category = category;
    error.httpStatus = status;
    return error;
}

class FakeApi final : public IBoardApiClient
{
public:
    VideoStreamsConfigDto config{QStringLiteral("v1"), QStringLiteral("video-1"),
        QStringLiteral("runtime-1"), true, false, mainVideoStreamDefaults(), subVideoStreamDefaults()};
    VideoStreamsUpdate lastUpdate;
    RuntimeApplyUpdate lastApply;
    int reads = 0;
    int writes = 0;
    int applies = 0;
    bool restartRequired = true;
    bool holdRead = false;
    bool holdWrite = false;
    bool holdApply = false;
    bool applyDespiteError = false;
    bool remainPending = false;
    int failReadsAfter = -1;
    std::optional<ApiError> readError;
    std::optional<ApiError> writeError;
    std::optional<ApiError> applyError;
    QVector<std::function<void()>> pending;
    QVector<ApiCompletion<TriggerModeConfigDto>> modeReads;
    QVector<ApiCompletion<LineRegionConfigDto>> lineReads;
    QVector<ApiCompletion<TriggerModeConfigDto>> modeWrites;
    int lineWrites = 0;

    RequestId getTriggerModeConfig(QObject*, ApiCompletion<TriggerModeConfigDto> done) override
    { modeReads.append(done); return RequestId::createUuid(); }
    RequestId getLineRegionConfig(QObject*, ApiCompletion<LineRegionConfigDto> done) override
    { lineReads.append(done); return RequestId::createUuid(); }
    RequestId putTriggerModeConfig(const TriggerModeUpdate&, QObject*, ApiCompletion<TriggerModeConfigDto> done) override
    { modeWrites.append(done); return RequestId::createUuid(); }
    RequestId putLineRegionConfig(const LineRegionUpdate&, QObject*, ApiCompletion<LineRegionConfigDto>) override
    { ++lineWrites; return RequestId::createUuid(); }

    RequestId getVideoStreamsConfig(QObject*, ApiCompletion<VideoStreamsConfigDto> done) override
    {
        ++reads;
        auto result = readError ? ApiResult<VideoStreamsConfigDto>::failure(*readError)
            : failReadsAfter >= 0 && reads > failReadsAfter
                ? ApiResult<VideoStreamsConfigDto>::failure(failure(ApiErrorCategory::Network))
                : ApiResult<VideoStreamsConfigDto>::success(config);
        auto deliver = [done, result] { done(result); };
        if (holdRead) pending.append(deliver); else deliver();
        return RequestId::createUuid();
    }
    RequestId putVideoStreamsConfig(const VideoStreamsUpdate& update, QObject*,
                                   ApiCompletion<VideoStreamsConfigDto> done) override
    {
        ++writes;
        lastUpdate = update;
        if (!writeError) {
            config.main = update.main;
            config.sub = update.sub;
            config.revision = QStringLiteral("video-2");
            config.runtimeRevision = QStringLiteral("runtime-2");
            config.restartRequired = restartRequired;
        }
        auto result = writeError ? ApiResult<VideoStreamsConfigDto>::failure(*writeError)
                                 : ApiResult<VideoStreamsConfigDto>::success(config);
        auto deliver = [done, result] { done(result); };
        if (holdWrite) pending.append(deliver); else deliver();
        return RequestId::createUuid();
    }
    RequestId applyRuntimeConfig(const RuntimeApplyUpdate& update, QObject*,
                                ApiCompletion<RuntimeApplyDto> done) override
    {
        ++applies;
        lastApply = update;
        if ((!applyError || applyDespiteError) && !remainPending) config.restartRequired = false;
        auto result = applyError ? ApiResult<RuntimeApplyDto>::failure(*applyError)
                                 : ApiResult<RuntimeApplyDto>::success(RuntimeApplyDto{});
        auto deliver = [done, result] { done(result); };
        if (holdApply) pending.append(deliver); else deliver();
        return RequestId::createUuid();
    }

    RequestId getHealth(QObject*, ApiCompletion<HealthDto>) override { return {}; }
    RequestId listEvents(int, const std::optional<QString>&, QObject*, ApiCompletion<EventPageDto>) override { return {}; }
    RequestId getEventDetail(const EventIdentity&, QObject*, ApiCompletion<EventDetailDto>) override { return {}; }
    RequestId downloadEvidenceToPartFile(const EventIdentity&, const QString&, const QString&, QObject*, ApiCompletion<EvidenceDownloadResult>) override { return {}; }
    RequestId getEvidenceConfig(QObject*, ApiCompletion<EvidenceConfigDto>) override { return {}; }
    RequestId putEvidenceConfig(const EvidenceConfigUpdate&, QObject*, ApiCompletion<EvidenceConfigDto>) override { return {}; }
    RequestId getTime(QObject*, ApiCompletion<TimeStatusDto>) override { return {}; }
    RequestId putTime(const TimeUpdate&, QObject*, ApiCompletion<TimeStatusDto>) override { return {}; }
    RequestId getFtpConfig(QObject*, ApiCompletion<FtpConfigSnapshotDto>) override { return {}; }
    RequestId putFtpConfig(const FtpConfigUpdate&, QObject*, ApiCompletion<FtpConfigSnapshotDto>) override { return {}; }
    RequestId rollbackFtpConfig(const QString&, QObject*, ApiCompletion<FtpConfigSnapshotDto>) override { return {}; }
    RequestId getFtpControl(QObject*, ApiCompletion<FtpControlDto>) override { return {}; }
    RequestId putFtpControl(const FtpControlUpdate&, QObject*, ApiCompletion<FtpControlDto>) override { return {}; }
    RequestId createFtpTask(const FtpTaskCreate&, QObject*, ApiCompletion<FtpTaskDetailDto>) override { return {}; }
    RequestId listFtpTasks(int, const std::optional<QString>&, QObject*, ApiCompletion<FtpTaskPageDto>) override { return {}; }
    RequestId getFtpTask(const QString&, QObject*, ApiCompletion<FtpTaskDetailDto>) override { return {}; }
    RequestId retryFtpTask(const QString&, QObject*, ApiCompletion<FtpTaskDetailDto>) override { return {}; }
    void cancel(const RequestId&) override {}
    void cancelAll() override {}
};

class FakePlayer final : public IRtspPlayer
{
public:
    void open(const RtspStreamSpec&) override {}
    void stop() override {}
    RtspPlayerState state() const override { return RtspPlayerState::Idle; }
    QWidget* outputWidget() const override { return nullptr; }
};

} // namespace

class VideoStreamsTest final : public QObject
{
    Q_OBJECT
private slots:
    void draftsAndFixedResolutionMigration();
    void saveFailurePreservesDraft();
    void failedApplyRetriesWithoutAnotherWrite();
    void lostApplyResponseIsConfirmedWithoutRepeatingPost();
    void conflictRefreshesRevisionAndPreservesDraft();
    void unsupportedAndReadOnlyDisableWrites();
    void lateResponsesAreIgnored_data();
    void lateResponsesAreIgnored();
    void delayedVerificationDoesNotCrossDevices();
    void verificationRetriesAreBounded();
    void destroyingApiResetsState();
    void uiSeparatesRoleDraftsAndShowsActualResolution();
    void detectionWaitsForBothReadsAndIgnoresOldDeviceCallbacks();
};

void VideoStreamsTest::draftsAndFixedResolutionMigration()
{
    FakeApi api;
    api.config.main.width = 3840;
    api.config.main.height = 2160;
    api.config.sub.width = 1280;
    api.config.sub.height = 720;
    api.config.main.codec = QStringLiteral("h265");
    VideoStreamsController controller;
    QSignalSpy restart(&controller, &VideoStreamsController::previewRestartRequested);
    controller.setDevice(QStringLiteral("a"), &api);
    QVERIFY(controller.canApply());
    QCOMPARE(controller.codec(RtspStreamRole::Main), QStringLiteral("h265"));
    QCOMPARE(controller.codec(RtspStreamRole::Sub), QStringLiteral("h264"));
    QCOMPARE(api.writes, 0);
    controller.apply();
    QCOMPARE(api.writes, 1);
    QCOMPARE(api.lastUpdate.main.width, 2560);
    QCOMPARE(api.lastUpdate.main.height, 1440);
    QCOMPARE(api.lastUpdate.sub.width, 1920);
    QCOMPARE(api.lastUpdate.sub.height, 1080);
    QCOMPARE(api.lastUpdate.main.codec, QStringLiteral("h265"));
    QCOMPARE(api.lastApply.expectedRevision, QStringLiteral("runtime-2"));
    QCOMPARE(api.lastApply.scope, QStringLiteral("rkipc"));
    QCOMPARE(restart.count(), 1);
    QCOMPARE(restart.at(0).at(0).toString(), QStringLiteral("a"));
    QVERIFY(!controller.busy());
    QVERIFY(!controller.canApply());
}

void VideoStreamsTest::saveFailurePreservesDraft()
{
    FakeApi api;
    VideoStreamsController controller;
    controller.setDevice(QStringLiteral("a"), &api);
    controller.setCodec(RtspStreamRole::Sub, QStringLiteral("h265"));
    api.writeError = failure(ApiErrorCategory::Network);
    controller.apply();
    QCOMPARE(controller.codec(RtspStreamRole::Sub), QStringLiteral("h265"));
    QVERIFY(controller.canApply());
    QVERIFY(controller.hasError());
    QCOMPARE(api.applies, 0);
    api.writeError.reset();
    api.restartRequired = false;
    controller.apply();
    QCOMPARE(api.writes, 2);
    QCOMPARE(api.applies, 0);
    QVERIFY(controller.status().contains(QStringLiteral("已生效")));
}

void VideoStreamsTest::failedApplyRetriesWithoutAnotherWrite()
{
    FakeApi api;
    VideoStreamsController controller;
    controller.setDevice(QStringLiteral("a"), &api);
    controller.setCodec(RtspStreamRole::Main, QStringLiteral("h265"));
    api.applyError = failure(ApiErrorCategory::CapabilityDisabled);
    controller.apply();
    QCOMPARE(api.writes, 1);
    QVERIFY(controller.status().contains(QStringLiteral("已保存，待应用")));
    QVERIFY(controller.canApply());
    api.applyError.reset();
    controller.apply();
    QCOMPARE(api.writes, 1);
    QCOMPARE(api.applies, 2);
    QVERIFY(!controller.canApply());
}

void VideoStreamsTest::lostApplyResponseIsConfirmedWithoutRepeatingPost()
{
    FakeApi api;
    api.applyError = failure(ApiErrorCategory::Network);
    api.applyDespiteError = true;
    VideoStreamsController controller;
    QSignalSpy restart(&controller, &VideoStreamsController::previewRestartRequested);
    controller.setDevice(QStringLiteral("a"), &api);
    controller.setCodec(RtspStreamRole::Main, QStringLiteral("h265"));
    controller.apply();
    QCOMPARE(api.applies, 1);
    QCOMPARE(restart.count(), 1);
    QVERIFY(!controller.hasError());
}

void VideoStreamsTest::conflictRefreshesRevisionAndPreservesDraft()
{
    FakeApi api;
    VideoStreamsController controller;
    controller.setDevice(QStringLiteral("a"), &api);
    controller.setCodec(RtspStreamRole::Sub, QStringLiteral("h265"));
    api.config.revision = QStringLiteral("another-client-revision");
    api.writeError = failure(ApiErrorCategory::Conflict, 409);
    controller.apply();
    QCOMPARE(api.reads, 2);
    QCOMPARE(api.applies, 0);
    QCOMPARE(controller.codec(RtspStreamRole::Sub), QStringLiteral("h265"));
    QVERIFY(controller.canApply());
    QVERIFY(controller.status().contains(QStringLiteral("重新应用")));
    api.writeError.reset();
    controller.apply();
    QCOMPARE(api.lastUpdate.expectedRevision, QStringLiteral("another-client-revision"));
}

void VideoStreamsTest::unsupportedAndReadOnlyDisableWrites()
{
    FakeApi api;
    api.readError = failure(ApiErrorCategory::NotFound, 404);
    VideoStreamsController controller;
    controller.setDevice(QStringLiteral("a"), &api);
    QVERIFY(!controller.editable());
    QVERIFY(controller.status().contains(QStringLiteral("不支持")));
    controller.apply();
    QCOMPARE(api.writes, 0);
    api.readError.reset();
    api.config.writeEnabled = false;
    controller.refresh();
    QVERIFY(!controller.editable());
    QVERIFY(!controller.canApply());
    QVERIFY(controller.status().contains(QStringLiteral("禁止写入")));
}

void VideoStreamsTest::lateResponsesAreIgnored_data()
{
    QTest::addColumn<QString>("stage");
    QTest::newRow("read") << QStringLiteral("read");
    QTest::newRow("write") << QStringLiteral("write");
    QTest::newRow("apply") << QStringLiteral("apply");
}

void VideoStreamsTest::lateResponsesAreIgnored()
{
    QFETCH(QString, stage);
    FakeApi oldApi;
    FakeApi newApi;
    VideoStreamsController controller;
    QSignalSpy restart(&controller, &VideoStreamsController::previewRestartRequested);
    oldApi.holdRead = stage == QStringLiteral("read");
    oldApi.holdWrite = stage == QStringLiteral("write");
    oldApi.holdApply = stage == QStringLiteral("apply");
    controller.setDevice(QStringLiteral("a"), &oldApi);
    if (stage != QStringLiteral("read")) {
        controller.setCodec(RtspStreamRole::Sub, QStringLiteral("h265"));
        controller.apply();
        QVERIFY(controller.busy());
        controller.apply();
        QCOMPARE(oldApi.writes, 1);
    }
    controller.setDevice(QStringLiteral("b"), &newApi);
    const QString status = controller.status();
    QCOMPARE(oldApi.pending.size(), 1);
    oldApi.pending.takeFirst()(); // Deliberately deliver a stale completion to test generation isolation.
    QCOMPARE(controller.status(), status);
    QCOMPARE(controller.codec(RtspStreamRole::Sub), QStringLiteral("h264"));
    QCOMPARE(newApi.writes, 0);
    QCOMPARE(newApi.applies, 0);
    QCOMPARE(restart.count(), 0);
    if (stage == QStringLiteral("write")) QCOMPARE(oldApi.applies, 0);
}

void VideoStreamsTest::delayedVerificationDoesNotCrossDevices()
{
    FakeApi api;
    FakeApi nextApi;
    api.failReadsAfter = 1;
    VideoStreamsController controller;
    controller.setDevice(QStringLiteral("a"), &api);
    controller.setCodec(RtspStreamRole::Sub, QStringLiteral("h265"));
    controller.apply();
    QVERIFY(controller.busy());
    controller.setDevice(QStringLiteral("b"), &nextApi);
    QTest::qWait(1200);
    QCOMPARE(api.reads, 2);
    QCOMPARE(nextApi.reads, 1);
    QCOMPARE(nextApi.writes, 0);
    QVERIFY(!controller.busy());
}

void VideoStreamsTest::verificationRetriesAreBounded()
{
    FakeApi api;
    api.remainPending = true;
    VideoStreamsController controller;
    QSignalSpy restart(&controller, &VideoStreamsController::previewRestartRequested);
    controller.setDevice(QStringLiteral("a"), &api);
    controller.setCodec(RtspStreamRole::Sub, QStringLiteral("h265"));
    controller.apply();
    QVERIFY(controller.busy());
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 18000);
    QCOMPARE(api.reads, 6); // Initial load + immediate verification + four retries.
    QCOMPARE(api.writes, 1);
    QCOMPARE(api.applies, 1);
    QCOMPARE(restart.count(), 0);
    QVERIFY(controller.hasError());
    QVERIFY(controller.status().contains(QStringLiteral("未确认")));
    api.config.restartRequired = false;
    controller.refresh();
    QCOMPARE(restart.count(), 1);
}

void VideoStreamsTest::destroyingApiResetsState()
{
    auto* api = new FakeApi;
    VideoStreamsController controller;
    controller.setDevice(QStringLiteral("a"), api);
    QVERIFY(controller.editable());
    delete api;
    QVERIFY(!controller.available());
    QVERIFY(!controller.hasConfig());
    QVERIFY(!controller.busy());
}

void VideoStreamsTest::uiSeparatesRoleDraftsAndShowsActualResolution()
{
    FakeApi api;
    FakePlayer player;
    LivePreviewPanel panel(&player);
    panel.setCurrentDevice(QStringLiteral("a"));
    panel.setBoardApiClient(&api);
    auto* role = panel.findChild<QComboBox*>(QStringLiteral("rtspStreamRoleCombo"));
    auto* codec = panel.findChild<QComboBox*>(QStringLiteral("rtspVideoCodecCombo"));
    auto* apply = panel.findChild<QPushButton*>(QStringLiteral("applyVideoStreamsButton"));
    auto* resolution = panel.findChild<QLabel*>(QStringLiteral("rtspActualResolutionLabel"));
    QVERIFY(role && codec && apply && resolution);
    QCOMPARE(role->currentData().toInt(), static_cast<int>(RtspStreamRole::Sub));
    QVERIFY(!apply->isEnabled());
    codec->setCurrentIndex(codec->findData(QStringLiteral("h265")));
    role->setCurrentIndex(1);
    QCOMPARE(codec->currentData().toString(), QStringLiteral("h264"));
    role->setCurrentIndex(0);
    QCOMPARE(codec->currentData().toString(), QStringLiteral("h265"));
    QCOMPARE(api.writes, 0);
    QVERIFY(apply->isEnabled());
    apply->click();
    QCOMPARE(api.lastUpdate.main.codec, QStringLiteral("h264"));
    QCOMPARE(api.lastUpdate.sub.codec, QStringLiteral("h265"));
    QVERIFY(!apply->isEnabled());
    emit player.videoFrameReceived(QSize(1280, 720));
    QVERIFY(resolution->text().contains(QStringLiteral("1280×720")));
    QVERIFY(resolution->text().contains(QStringLiteral("与目标不符")));
    emit player.videoFrameReceived(QSize(1920, 1080));
    QVERIFY(!resolution->text().contains(QStringLiteral("与目标不符")));
    role->setCurrentIndex(1);
    QVERIFY(resolution->text().contains(QStringLiteral("等待")));
    emit player.videoFrameReceived(QSize(2560, 1440));
    QVERIFY(!resolution->text().contains(QStringLiteral("与目标不符")));
    const int readsBeforeDisconnect = api.reads;
    panel.setBoardApiClient(&api, false);
    QVERIFY(!codec->isEnabled());
    QVERIFY(!apply->isEnabled());
    panel.setBoardApiClient(&api, true);
    QCOMPARE(api.reads, readsBeforeDisconnect + 1);
    QVERIFY(codec->isEnabled());
}

void VideoStreamsTest::detectionWaitsForBothReadsAndIgnoresOldDeviceCallbacks()
{
    FakeApi a, b;
    LivePreviewPanel panel(nullptr);
    panel.setCurrentDevice(QStringLiteral("a"));
    panel.setBoardApiClient(&a);
    auto* modes = panel.findChild<QComboBox*>(QStringLiteral("triggerModeCombo"));
    auto* save = panel.findChild<QPushButton*>(QStringLiteral("saveLineRegionButton"));
    QVERIFY(modes && save);
    TriggerModeConfigDto mode;
    mode.revision = QStringLiteral("a1");
    mode.triggerMode = QStringLiteral("line");
    mode.supportedModes = {QStringLiteral("line"), QStringLiteral("radar")};
    mode.writeEnabled = true;
    LineRegionConfigDto line;
    line.revision = QStringLiteral("a1");
    line.writeEnabled = true;
    QCOMPARE(a.modeReads.size(), 1);
    a.modeReads.first()(ApiResult<TriggerModeConfigDto>::success(mode));
    QVERIFY(!modes->isEnabled());
    a.lineReads.first()(ApiResult<LineRegionConfigDto>::success(line));
    QVERIFY(modes->isEnabled());
    modes->setCurrentIndex(1);
    QVERIFY(save->isEnabled());
    save->click();
    QCOMPARE(a.modeWrites.size(), 1);
    panel.setCurrentDevice(QStringLiteral("b"));
    panel.setBoardApiClient(&b);
    mode.revision = QStringLiteral("b1");
    mode.triggerMode = QStringLiteral("radar");
    b.modeReads.first()(ApiResult<TriggerModeConfigDto>::success(mode));
    QVERIFY(!modes->isEnabled());
    b.lineReads.first()(ApiResult<LineRegionConfigDto>::success(line));
    QVERIFY(modes->isEnabled());
    mode.triggerMode = QStringLiteral("line");
    a.modeWrites.first()(ApiResult<TriggerModeConfigDto>::success(mode));
    a.modeReads.first()(ApiResult<TriggerModeConfigDto>::success(mode));
    a.lineReads.first()(ApiResult<LineRegionConfigDto>::success(line));
    QCOMPARE(modes->currentData().toString(), QStringLiteral("radar"));
    QCOMPARE(b.lineWrites, 0);
    QCOMPARE(b.applies, 0);
    auto* closing = new LivePreviewPanel(nullptr);
    closing->setCurrentDevice(QStringLiteral("a"));
    closing->setBoardApiClient(&a);
    const auto lateMode = a.modeReads.last();
    const auto lateLine = a.lineReads.last();
    delete closing;
    lateMode(ApiResult<TriggerModeConfigDto>::success(mode));
    lateLine(ApiResult<LineRegionConfigDto>::success(line));
}

QTEST_MAIN(VideoStreamsTest)
#include "VideoStreamsTest.moc"
