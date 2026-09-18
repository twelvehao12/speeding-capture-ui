#include "../src/ui/Rv1126bDeviceManagementDialog.h"
#include "../src/rv1126b/services/EmbeddedFtpReceiveServer.h"

#include <QCheckBox>
#include <QAbstractButton>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTest>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>

using namespace rv1126b;

namespace {

template<typename T>
RequestId done(ApiCompletion<T> completion, ApiResult<T> result)
{
    const RequestId id = RequestId::createUuid();
    completion(std::move(result));
    return id;
}

class UiBoard final : public IBoardApiClient
{
public:
    EvidenceConfigDto evidence;
    TimeStatusDto time;

    RequestId getHealth(QObject*, ApiCompletion<HealthDto> c) override { return done(std::move(c), ApiResult<HealthDto>::success({})); }
    RequestId listEvents(int, const std::optional<QString>&, QObject*, ApiCompletion<EventPageDto> c) override { return done(std::move(c), ApiResult<EventPageDto>::success({})); }
    RequestId getEventDetail(const EventIdentity&, QObject*, ApiCompletion<EventDetailDto> c) override { return done(std::move(c), ApiResult<EventDetailDto>::success({})); }
    RequestId downloadEvidenceToPartFile(const EventIdentity&, const QString&, const QString&, QObject*, ApiCompletion<EvidenceDownloadResult> c) override { return done(std::move(c), ApiResult<EvidenceDownloadResult>::success({})); }
    RequestId getEvidenceConfig(QObject*, ApiCompletion<EvidenceConfigDto> c) override { return done(std::move(c), ApiResult<EvidenceConfigDto>::success(evidence)); }
    RequestId putEvidenceConfig(const EvidenceConfigUpdate& u, QObject*, ApiCompletion<EvidenceConfigDto> c) override { evidence.evidence = u; return done(std::move(c), ApiResult<EvidenceConfigDto>::success(evidence)); }
    RequestId getTime(QObject*, ApiCompletion<TimeStatusDto> c) override { return done(std::move(c), ApiResult<TimeStatusDto>::success(time)); }
    RequestId putTime(const TimeUpdate& u, QObject*, ApiCompletion<TimeStatusDto> c) override { time.time.epochMs = u.utcEpochMs; return done(std::move(c), ApiResult<TimeStatusDto>::success(time)); }
    RequestId getIspConfig(QObject*, ApiCompletion<QJsonObject> c) override { return done(std::move(c), ApiResult<QJsonObject>::success({})); }
    RequestId saveCurrentIspConfig(QObject*, ApiCompletion<QJsonObject> c) override { return done(std::move(c), ApiResult<QJsonObject>::success({})); }
    RequestId clearIspConfig(QObject*, ApiCompletion<QJsonObject> c) override { return done(std::move(c), ApiResult<QJsonObject>::success({})); }
    RequestId getFtpConfig(QObject*, ApiCompletion<FtpConfigSnapshotDto> c) override { return done(std::move(c), ApiResult<FtpConfigSnapshotDto>::success({})); }
    RequestId putFtpConfig(const FtpConfigUpdate&, QObject*, ApiCompletion<FtpConfigSnapshotDto> c) override { return done(std::move(c), ApiResult<FtpConfigSnapshotDto>::success({})); }
    RequestId rollbackFtpConfig(const QString&, QObject*, ApiCompletion<FtpConfigSnapshotDto> c) override { return done(std::move(c), ApiResult<FtpConfigSnapshotDto>::success({})); }
    RequestId getFtpControl(QObject*, ApiCompletion<FtpControlDto> c) override { return done(std::move(c), ApiResult<FtpControlDto>::success({})); }
    RequestId putFtpControl(const FtpControlUpdate&, QObject*, ApiCompletion<FtpControlDto> c) override { return done(std::move(c), ApiResult<FtpControlDto>::success({})); }
    RequestId createFtpTask(const FtpTaskCreate&, QObject*, ApiCompletion<FtpTaskDetailDto> c) override { return done(std::move(c), ApiResult<FtpTaskDetailDto>::success({})); }
    RequestId listFtpTasks(int, const std::optional<QString>&, QObject*, ApiCompletion<FtpTaskPageDto> c) override { return done(std::move(c), ApiResult<FtpTaskPageDto>::success({})); }
    RequestId getFtpTask(const QString&, QObject*, ApiCompletion<FtpTaskDetailDto> c) override { return done(std::move(c), ApiResult<FtpTaskDetailDto>::success({})); }
    RequestId retryFtpTask(const QString&, QObject*, ApiCompletion<FtpTaskDetailDto> c) override { return done(std::move(c), ApiResult<FtpTaskDetailDto>::success({})); }
    void cancel(const RequestId&) override {}
    void cancelAll() override {}
};

class UiFtp final : public FtpService
{
public:
    FtpConfigSnapshotDto config;
    FtpControlDto control;
    FtpTaskPageDto page;
    FtpTaskDetailDto detail;
    FtpActivationResult activation;
    FtpConfigUpdate lastUpdate;
    bool conflictOnce = false;
    int saveCount = 0;

    RequestId loadConfig(QObject*, ApiCompletion<FtpConfigSnapshotDto> c) override { return done(std::move(c), ApiResult<FtpConfigSnapshotDto>::success(config)); }
    RequestId saveConfigAndEnableNewEvents(const FtpConfigUpdate& u, QObject*, ApiCompletion<FtpActivationResult> c) override
    {
        ++saveCount;
        lastUpdate = u;
        if (conflictOnce) {
            conflictOnce = false;
            config.revision = QStringLiteral("remote-revision");
            ApiError error;
            error.code = QStringLiteral("config_revision_conflict");
            error.category = ApiErrorCategory::Conflict;
            return done(std::move(c), ApiResult<FtpActivationResult>::failure(error));
        }
        return done(std::move(c), ApiResult<FtpActivationResult>::success(activation));
    }
    RequestId rollbackConfig(const QString&, QObject*, ApiCompletion<FtpConfigSnapshotDto> c) override { return done(std::move(c), ApiResult<FtpConfigSnapshotDto>::success(config)); }
    RequestId loadControl(QObject*, ApiCompletion<FtpControlDto> c) override { return done(std::move(c), ApiResult<FtpControlDto>::success(control)); }
    RequestId updateControl(const FtpControlUpdate& u, QObject*, ApiCompletion<FtpControlDto> c) override { control.enabled = u.enabled; control.scope = u.scope; return done(std::move(c), ApiResult<FtpControlDto>::success(control)); }
    RequestId createTask(const FtpTaskCreate&, QObject*, ApiCompletion<FtpTaskDetailDto> c) override { return done(std::move(c), ApiResult<FtpTaskDetailDto>::success(detail)); }
    RequestId listTasks(int, const std::optional<QString>&, QObject*, ApiCompletion<FtpTaskPageDto> c) override { return done(std::move(c), ApiResult<FtpTaskPageDto>::success(page)); }
    RequestId loadTask(const QString&, QObject*, ApiCompletion<FtpTaskDetailDto> c) override { return done(std::move(c), ApiResult<FtpTaskDetailDto>::success(detail)); }
    RequestId retryTask(const QString&, QObject*, ApiCompletion<FtpTaskDetailDto> c) override { return done(std::move(c), ApiResult<FtpTaskDetailDto>::success(detail)); }
    void cancel(const RequestId&) override {}
    void cancelAll() override {}
};

FtpTargetSnapshotDto target(int index)
{
    FtpTargetSnapshotDto value;
    value.id = QStringLiteral("server%1").arg(index + 1);
    value.enabled = true;
    value.host = QStringLiteral("192.0.2.%1").arg(index + 10);
    value.port = 21;
    value.user = QStringLiteral("upload");
    value.remoteDir = QStringLiteral("/events");
    value.passwordConfigured = true;
    return value;
}

void configure(UiBoard& board, UiFtp& ftp, int targetCount = 1)
{
    board.evidence.evidence.siteName = QStringLiteral("测试点位");
    board.evidence.evidence.speedLimitKmh = 60;
    board.time.time.epochMs = 1784000000000LL;
    board.time.time.sourceEpochMs = 1783971200000LL;
    board.time.time.offsetAppliedMs = 28800000;
    board.time.time.quality.value = TimeQuality::BoardEpochUnverified;
    board.time.ntpStatus = QStringLiteral("vendor_time_chain_unverified");
    board.time.timeSetEnabled = false;
    ftp.config.revision = QStringLiteral("rev-1");
    ftp.config.deviceId = QStringLiteral("device-a");
    for (int i = 0; i < targetCount; ++i) ftp.config.targets.append(target(i));
    ftp.control.revision = QStringLiteral("control-1");
    ftp.control.enabled = true;
    ftp.control.scope.value = FtpControlScope::NewEventsOnly;
    ftp.activation.configSaved = true;
    ftp.activation.autoEnabled = false;
    ftp.activation.newRevision = QStringLiteral("rev-2");

    FtpTaskSummaryDto summary;
    summary.taskId = QStringLiteral("task-1");
    summary.state.value = FtpTaskState::Failed;
    summary.targetIds = {QStringLiteral("server1"), QStringLiteral("server2")};
    ftp.page.items.append(summary);
    ftp.page.count = 1;
    ftp.detail.summary = summary;
    FtpTaskTargetStatusDto success;
    success.targetId = QStringLiteral("server1");
    success.state.value = FtpTaskState::Done;
    success.total = success.done = 4;
    FtpTaskTargetStatusDto failed;
    failed.targetId = QStringLiteral("server2");
    failed.state.value = FtpTaskState::Failed;
    failed.total = failed.failed = 4;
    failed.lastError = QStringLiteral("connection refused");
    ftp.detail.targets = {success, failed};
}

} // namespace

class DeviceOperationsUiTest final : public QObject
{
    Q_OBJECT

private slots:
    void showsManagementPagesTimeWarningAndMixedTaskTargets();
    void enforcesTargetLimitAndClearsReplacementPassword();
    void confirmsRevisionConflictAndResubmits();
    void startsEmbeddedReceiverAndSavesUniqueTargetId();
};

void DeviceOperationsUiTest::showsManagementPagesTimeWarningAndMixedTaskTargets()
{
    UiBoard board;
    UiFtp ftp;
    configure(board, ftp, 2);
    DeviceOperationsController controller({[&](const QString&) { return &board; },
                                           [&](const QString&) { return &ftp; }});
    Rv1126bDeviceManagementDialog dialog(QStringLiteral("device-a"), &controller);
    auto* tabs = dialog.findChild<QTabWidget*>(QStringLiteral("deviceOperationsTabs"));
    QVERIFY(tabs);
    QCOMPARE(tabs->count(), 6);
    QCOMPARE(dialog.findChild<QLineEdit*>(QStringLiteral("siteNameEdit"))->text(), QStringLiteral("测试点位"));
    auto* quality = dialog.findChild<QLabel*>(QStringLiteral("timeQualityLabel"));
    QVERIFY(quality->text().contains(QStringLiteral("不可作为可靠 UTC")));
    QVERIFY(quality->styleSheet().contains(QStringLiteral("b00020")));
    QVERIFY(!dialog.findChild<QPushButton*>(QStringLiteral("syncUtcButton"))->isEnabled());

    tabs->setCurrentIndex(5);
    auto* tasks = dialog.findChild<QTableWidget*>(QStringLiteral("ftpTaskTable"));
    QCOMPARE(tasks->rowCount(), 1);
    tasks->selectRow(0);
    QCoreApplication::processEvents();
    auto* details = dialog.findChild<QTableWidget*>(QStringLiteral("ftpTaskDetailTable"));
    QCOMPARE(details->rowCount(), 2);
    QCOMPARE(details->item(0, 1)->text(), QStringLiteral("完成"));
    QCOMPARE(details->item(1, 1)->text(), QStringLiteral("失败"));
    QVERIFY(dialog.findChild<QPushButton*>(QStringLiteral("retryFtpTaskButton"))->isEnabled());
}

void DeviceOperationsUiTest::enforcesTargetLimitAndClearsReplacementPassword()
{
    UiBoard board;
    UiFtp ftp;
    configure(board, ftp, 8);
    DeviceOperationsController controller({[&](const QString&) { return &board; },
                                           [&](const QString&) { return &ftp; }});
    Rv1126bDeviceManagementDialog dialog(QStringLiteral("device-a"), &controller,
        Rv1126bDeviceManagementDialog::InitialPage::FtpConfig);
    dialog.show();
    QCoreApplication::processEvents();
    auto* table = dialog.findChild<QTableWidget*>(QStringLiteral("ftpTargetsTable"));
    QCOMPARE(table->rowCount(), 8);
    QTest::mouseClick(dialog.findChild<QPushButton*>(QStringLiteral("addFtpTargetButton")), Qt::LeftButton);
    QCOMPARE(table->rowCount(), 8);
    QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("deviceOperationsMessage"))->text()
                .contains(QStringLiteral("最多 8 个")));

    auto* action = qobject_cast<QComboBox*>(table->cellWidget(0, 5));
    auto* password = qobject_cast<QLineEdit*>(table->cellWidget(0, 6));
    action->setCurrentIndex(action->findData(static_cast<int>(FtpPasswordAction::Replace)));
    QCOMPARE(password->echoMode(), QLineEdit::Password);
    password->setText(QStringLiteral("not-logged"));
    QTest::mouseClick(dialog.findChild<QPushButton*>(QStringLiteral("saveAndEnableFtpButton")), Qt::LeftButton);
    QCOMPARE(ftp.lastUpdate.targets[0].replacementPassword.value(), QStringLiteral("not-logged"));
    QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("ftpStatusLabel"))->text()
                .contains(QStringLiteral("配置已保存，但自动下发未开启")));
}

void DeviceOperationsUiTest::confirmsRevisionConflictAndResubmits()
{
    UiBoard board;
    UiFtp ftp;
    configure(board, ftp);
    ftp.conflictOnce = true;
    ftp.activation.autoEnabled = true;
    DeviceOperationsController controller({[&](const QString&) { return &board; },
                                           [&](const QString&) { return &ftp; }});
    Rv1126bDeviceManagementDialog dialog(QStringLiteral("device-a"), &controller,
        Rv1126bDeviceManagementDialog::InitialPage::FtpConfig);
    dialog.show();
    QCoreApplication::processEvents();
    QTimer::singleShot(0, []() {
        for (QWidget* widget : QApplication::topLevelWidgets())
            if (auto* message = qobject_cast<QMessageBox*>(widget))
                if (QAbstractButton* yes = message->button(QMessageBox::Yes)) yes->click();
    });
    QTest::mouseClick(dialog.findChild<QPushButton*>(QStringLiteral("saveAndEnableFtpButton")), Qt::LeftButton);
    QCOMPARE(ftp.saveCount, 2);
    QCOMPARE(ftp.lastUpdate.expectedRevision, QStringLiteral("remote-revision"));
    QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("ftpStatusLabel"))->text()
                .contains(QStringLiteral("all_existing")));
}

void DeviceOperationsUiTest::startsEmbeddedReceiverAndSavesUniqueTargetId()
{
    UiBoard board;
    UiFtp ftp;
    configure(board, ftp);
    ftp.activation.autoEnabled = true;
    QTemporaryDir root;
    QVERIFY(root.isValid());
    EmbeddedFtpReceiveServer server;
    DeviceOperationsController controller({[&](const QString&) { return &board; },
                                           [&](const QString&) { return &ftp; }});
    Rv1126bDeviceManagementDialog dialog(QStringLiteral("device-a"), &controller,
        Rv1126bDeviceManagementDialog::InitialPage::FtpConfig, nullptr, true, &server);
    dialog.show();
    QCoreApplication::processEvents();

    dialog.findChild<QLineEdit*>(QStringLiteral("localFtpRootEdit"))->setText(root.path());
    dialog.findChild<QLineEdit*>(QStringLiteral("localFtpHostEdit"))->setText(QStringLiteral("127.0.0.1"));
    QCOMPARE(dialog.findChild<QLineEdit*>(QStringLiteral("localFtpTargetIdEdit"))->text(), QStringLiteral("pc_127_0_0_1"));
    dialog.findChild<QLineEdit*>(QStringLiteral("localFtpTargetIdEdit"))->setText(QStringLiteral("pc_test_bay"));
    dialog.findChild<QSpinBox*>(QStringLiteral("localFtpPortSpin"))->setValue(22110);
    dialog.findChild<QSpinBox*>(QStringLiteral("localFtpPassiveStartSpin"))->setValue(22111);
    dialog.findChild<QSpinBox*>(QStringLiteral("localFtpPassiveEndSpin"))->setValue(22120);
    const QString password = dialog.findChild<QLineEdit*>(QStringLiteral("localFtpPasswordEdit"))->text();

    QTest::mouseClick(dialog.findChild<QPushButton*>(QStringLiteral("localFtpSaveTargetButton")), Qt::LeftButton);

    QTRY_VERIFY(server.isListening());
    QCOMPARE(ftp.saveCount, 1);
    const auto local = std::find_if(ftp.lastUpdate.targets.cbegin(), ftp.lastUpdate.targets.cend(), [](const FtpTargetUpdate& target) {
        return target.id == QStringLiteral("pc_test_bay");
    });
    QVERIFY(local != ftp.lastUpdate.targets.cend());
    QVERIFY(local->enabled);
    QCOMPARE(local->host, QStringLiteral("127.0.0.1"));
    QCOMPARE(local->port, static_cast<quint16>(22110));
    QCOMPARE(local->user, QStringLiteral("upload"));
    QCOMPARE(local->remoteDir, QStringLiteral("/vehicle_events"));
    QVERIFY(local->passive);
    QCOMPARE(local->passwordAction.value, FtpPasswordAction::Replace);
    QVERIFY(local->replacementPassword.has_value());
    QCOMPARE(*local->replacementPassword, password);
}

QTEST_MAIN(DeviceOperationsUiTest)
#include "DeviceOperationsUiTest.moc"
