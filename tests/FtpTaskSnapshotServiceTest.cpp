#include "../src/rv1126b/services/BoardFtpTaskSnapshotService.h"
#include "../src/rv1126b/storage/SqliteEventRepository.h"

#include <QDir>
#include <QTemporaryDir>
#include <QtTest>

#include <functional>
#include <optional>

using namespace rv1126b;

class FtpTaskSnapshotServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void mapsAndPersistsTaskDetail();
    void rejectsMissingTaskId();
};

namespace {

template<typename T>
std::optional<ApiResult<T>> callResult(std::function<RequestId(ApiCompletion<T>)> invoker)
{
    auto result = std::make_shared<std::optional<ApiResult<T>>>();
    invoker([result](ApiResult<T> value) {
        result->emplace(std::move(value));
    });
    if (!QTest::qWaitFor([result] { return result->has_value(); }, 10000)) return std::nullopt;
    return std::move(*result);
}

QString databasePath(QTemporaryDir& tempDir)
{
    return QDir(tempDir.path()).filePath(QStringLiteral("rv1126b-ftp-snapshots.sqlite"));
}

void initializeRepository(SqliteEventRepository& repository, QObject* context)
{
    auto init = callResult<void>([&](ApiCompletion<void> completion) {
        return repository.initialize(context, std::move(completion));
    });
    QVERIFY(init.has_value());
    if (!init->isSuccess()) {
        QFAIL(qPrintable(init->error().message));
    }
}

FtpTaskTargetStatusDto makeTarget(const QString& targetId, FtpTaskState state, int done, int failed)
{
    FtpTaskTargetStatusDto target;
    target.targetId = targetId;
    target.state.value = state;
    target.state.rawValue = state == FtpTaskState::Failed ? QStringLiteral("failed") : QStringLiteral("running");
    target.total = 12;
    target.pending = 12 - done - failed;
    target.uploading = state == FtpTaskState::Running ? 1 : 0;
    target.done = done;
    target.failed = failed;
    target.attempts = done + failed;
    target.lastError = failed > 0 ? QStringLiteral("ftp_timeout") : QString();
    return target;
}

FtpTaskDetailDto makeDetail(const QString& taskId = QStringLiteral("ftp-task-001"))
{
    FtpTaskDetailDto detail;
    detail.summary.taskId = taskId;
    detail.summary.state.value = FtpTaskState::Running;
    detail.summary.state.rawValue = QStringLiteral("running");
    detail.summary.startEpochMs = 1784270000000;
    detail.summary.endEpochMs = 1784280000000;
    detail.summary.createdEpochMs = 1784280006000;
    detail.summary.targetIds = {QStringLiteral("target-a"), QStringLiteral("target-b")};
    detail.targets.append(makeTarget(QStringLiteral("target-a"), FtpTaskState::Running, 5, 0));
    detail.targets.append(makeTarget(QStringLiteral("target-b"), FtpTaskState::Failed, 2, 1));
    return detail;
}

} // namespace

void FtpTaskSnapshotServiceTest::mapsAndPersistsTaskDetail()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SqliteEventRepository repository(databasePath(tempDir));
    initializeRepository(repository, this);

    BoardFtpTaskSnapshotService service(QStringLiteral("rv-device-001"), &repository);
    const FtpTaskDetailDto detail = makeDetail();

    const StoredFtpTask mapped = service.snapshotFromDetail(detail, 1784280010000);
    QCOMPARE(mapped.deviceId, QStringLiteral("rv-device-001"));
    QCOMPARE(mapped.taskId, QStringLiteral("ftp-task-001"));
    QCOMPARE(mapped.state.value, FtpTaskState::Running);
    QCOMPARE(mapped.refreshedEpochMs, 1784280010000);
    QCOMPARE(mapped.targets.size(), 2);
    QCOMPARE(mapped.targets.last().targetId, QStringLiteral("target-b"));
    QCOMPARE(mapped.targets.last().failed, 1);
    QCOMPARE(mapped.targets.last().lastError, QStringLiteral("ftp_timeout"));

    auto saved = callResult<StoredFtpTask>([&](ApiCompletion<StoredFtpTask> completion) {
        return service.saveTaskDetail(detail, this, std::move(completion));
    });
    QVERIFY(saved.has_value());
    if (!saved->isSuccess()) {
        QFAIL(qPrintable(saved->error().message));
    }
    QCOMPARE(saved->value().taskId, QStringLiteral("ftp-task-001"));

    FtpTaskQuery query;
    query.deviceId = QStringLiteral("rv-device-001");
    auto loaded = callResult<QVector<StoredFtpTask>>([&](ApiCompletion<QVector<StoredFtpTask>> completion) {
        return service.loadTaskSnapshots(query, this, std::move(completion));
    });
    QVERIFY(loaded.has_value());
    QVERIFY(loaded->isSuccess());
    QCOMPARE(loaded->value().size(), 1);
    QCOMPARE(loaded->value().first().taskId, QStringLiteral("ftp-task-001"));
    QCOMPARE(loaded->value().first().targets.size(), 2);
    QCOMPARE(loaded->value().first().targets.last().targetId, QStringLiteral("target-b"));
}

void FtpTaskSnapshotServiceTest::rejectsMissingTaskId()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SqliteEventRepository repository(databasePath(tempDir));
    initializeRepository(repository, this);

    BoardFtpTaskSnapshotService service(QStringLiteral("rv-device-001"), &repository);
    auto saved = callResult<StoredFtpTask>([&](ApiCompletion<StoredFtpTask> completion) {
        return service.saveTaskDetail(makeDetail(QString()), this, std::move(completion));
    });

    QVERIFY(saved.has_value());
    QVERIFY(!saved->isSuccess());
    QCOMPARE(saved->error().code, QStringLiteral("rv1126b.ftp_snapshot.empty_task_id"));
}

QTEST_MAIN(FtpTaskSnapshotServiceTest)

#include "FtpTaskSnapshotServiceTest.moc"
