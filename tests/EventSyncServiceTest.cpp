#include "../src/rv1126b/services/BoardEventSyncService.h"
#include "../src/rv1126b/storage/SqliteEventRepository.h"

#include <QDir>
#include <QMap>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <functional>
#include <optional>

using namespace rv1126b;

class EventSyncServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void pollsPagePersistsEventsAndEmitsSignals();
    void refreshesQueuedEventsWithDetail();
    void usesSnapshotImageWhenEvidenceImageIsMissing();
    void ignoresMissingDetailForStaleNonTerminalEvent();
    void skipsOverlappingPolls();
    void catchUpPaginatesUntilStoredAnchorAndSavesNewHead();
    void retriesAfterTemporaryFailure();
    void stopCancelsPendingRequest();
};

namespace {

constexpr auto DeviceId = "rv-device-001";

ApiError makeError(const QString& code)
{
    ApiError error;
    error.code = code;
    error.message = QStringLiteral("test error");
    error.category = ApiErrorCategory::Network;
    error.retryable = true;
    return error;
}

ApiError makeNotFoundError(const QString& code)
{
    ApiError error;
    error.httpStatus = 404;
    error.code = code;
    error.message = QStringLiteral("test not found");
    error.category = ApiErrorCategory::NotFound;
    error.retryable = false;
    return error;
}

EventSummaryDto makeSummary(qint64 eventId, qint64 trackId, OcrStatus status, qint64 sourceEpochMs = 1784280000123)
{
    EventSummaryDto summary;
    summary.eventId = eventId;
    summary.trackId = trackId;
    summary.eventTime.epochMs = sourceEpochMs;
    summary.eventTime.sourceEpochMs = sourceEpochMs;
    summary.eventTime.quality.value = TimeQuality::NativeUtc;
    summary.eventTime.quality.rawValue = QStringLiteral("native_utc");
    summary.motionDirection = QStringLiteral("north_to_south");
    summary.speedKmh = 72;
    summary.speedValid = true;
    summary.speedStatus = QStringLiteral("overspeed");
    summary.ocrStatus.value = status;
    summary.ocrStatus.rawValue = status == OcrStatus::Queued ? QStringLiteral("queued") : QStringLiteral("matched");
    summary.plateText = status == OcrStatus::Queued ? QString() : QStringLiteral("ABC123");
    summary.plateAscii = status == OcrStatus::Queued ? QString() : QStringLiteral("ABC123");
    summary.plateColor = status == OcrStatus::Queued ? QString() : QStringLiteral("blue");
    summary.evidenceStatus = QStringLiteral("available");
    summary.evidenceAvailable = true;
    summary.detailRelativeUrl = QStringLiteral("/api/v1/events/%1/%2").arg(eventId).arg(trackId);
    summary.evidenceRelativeUrl = QStringLiteral("/api/v1/events/%1/%2/evidence.jpg").arg(eventId).arg(trackId);
    return summary;
}

EventPageDto pageWith(const QVector<EventSummaryDto>& items)
{
    EventPageDto page;
    page.apiVersion = QStringLiteral("v1");
    page.items = items;
    page.count = items.size();
    page.hasMore = false;
    return page;
}

EventDetailDto detailFor(const EventSummaryDto& summary)
{
    EventDetailDto detail;
    detail.summary = summary;
    detail.triggerMode = QStringLiteral("radar");
    detail.captureReason = QStringLiteral("overspeed");
    detail.vehicle.insert(QStringLiteral("type"), QStringLiteral("car"));
    detail.radar.insert(QStringLiteral("speedKmh"), summary.speedKmh);
    detail.ocr.insert(QStringLiteral("plateText"), summary.plateText);
    detail.images.insert(QStringLiteral("evidence"), summary.evidenceRelativeUrl);
    detail.evidenceRelativeUrl = summary.evidenceRelativeUrl;
    return detail;
}

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
    return QDir(tempDir.path()).filePath(QStringLiteral("rv1126b-sync.sqlite"));
}

class FakeBoardApiClient final : public IBoardApiClient
{
public:
    EventPageDto nextPage;
    QMap<QString, EventPageDto> pagesByCursor;
    QStringList requestedCursors;
    std::optional<ApiError> listFailure;
    QMap<EventIdentity, EventDetailDto> details;
    bool holdListCompletion = false;
    int listEventsCalls = 0;
    int detailCalls = 0;
    int cancelAllCalls = 0;
    ApiCompletion<EventPageDto> pendingListCompletion;

    RequestId getHealth(QObject*, ApiCompletion<HealthDto> completion) override
    {
        completion(ApiResult<HealthDto>::failure(makeError(QStringLiteral("not_implemented"))));
        return RequestId::createUuid();
    }

    RequestId listEvents(
        int,
        const std::optional<QString>& cursor,
        QObject*,
        ApiCompletion<EventPageDto> completion) override
    {
        ++listEventsCalls;
        const QString cursorKey = cursor.value_or(QString());
        requestedCursors.append(cursorKey);
        if (holdListCompletion) {
            pendingListCompletion = std::move(completion);
            return RequestId::createUuid();
        }

        const auto failure = listFailure;
        const auto page = pagesByCursor.contains(cursorKey) ? pagesByCursor.value(cursorKey) : nextPage;
        QTimer::singleShot(0, [completion = std::move(completion), failure, page]() mutable {
            if (failure) {
                completion(ApiResult<EventPageDto>::failure(*failure));
            } else {
                completion(ApiResult<EventPageDto>::success(page));
            }
        });
        return RequestId::createUuid();
    }

    void completeHeldList()
    {
        auto completion = std::move(pendingListCompletion);
        pendingListCompletion = {};
        if (listFailure) {
            completion(ApiResult<EventPageDto>::failure(*listFailure));
        } else {
            completion(ApiResult<EventPageDto>::success(nextPage));
        }
    }

    RequestId getEventDetail(
        const EventIdentity& identity,
        QObject*,
        ApiCompletion<EventDetailDto> completion) override
    {
        ++detailCalls;
        const auto detail = details.value(identity);
        const bool found = details.contains(identity);
        QTimer::singleShot(0, [completion = std::move(completion), detail, found]() mutable {
            if (!found) {
                completion(ApiResult<EventDetailDto>::failure(makeNotFoundError(QStringLiteral("event_not_found"))));
                return;
            }
            completion(ApiResult<EventDetailDto>::success(detail));
        });
        return RequestId::createUuid();
    }

    RequestId downloadEvidenceToPartFile(
        const EventIdentity&,
        const QString&,
        const QString&,
        QObject*,
        ApiCompletion<EvidenceDownloadResult> completion) override
    {
        completion(ApiResult<EvidenceDownloadResult>::failure(makeError(QStringLiteral("not_implemented"))));
        return RequestId::createUuid();
    }

    RequestId getEvidenceConfig(QObject*, ApiCompletion<EvidenceConfigDto> completion) override
    {
        completion(ApiResult<EvidenceConfigDto>::failure(makeError(QStringLiteral("not_implemented"))));
        return RequestId::createUuid();
    }

    RequestId putEvidenceConfig(const EvidenceConfigUpdate&, QObject*, ApiCompletion<EvidenceConfigDto> completion) override
    {
        completion(ApiResult<EvidenceConfigDto>::failure(makeError(QStringLiteral("not_implemented"))));
        return RequestId::createUuid();
    }

    RequestId getTime(QObject*, ApiCompletion<TimeStatusDto> completion) override
    {
        completion(ApiResult<TimeStatusDto>::failure(makeError(QStringLiteral("not_implemented"))));
        return RequestId::createUuid();
    }

    RequestId putTime(const TimeUpdate&, QObject*, ApiCompletion<TimeStatusDto> completion) override
    {
        completion(ApiResult<TimeStatusDto>::failure(makeError(QStringLiteral("not_implemented"))));
        return RequestId::createUuid();
    }

    RequestId getFtpConfig(QObject*, ApiCompletion<FtpConfigSnapshotDto> completion) override
    {
        completion(ApiResult<FtpConfigSnapshotDto>::failure(makeError(QStringLiteral("not_implemented"))));
        return RequestId::createUuid();
    }

    RequestId putFtpConfig(const FtpConfigUpdate&, QObject*, ApiCompletion<FtpConfigSnapshotDto> completion) override
    {
        completion(ApiResult<FtpConfigSnapshotDto>::failure(makeError(QStringLiteral("not_implemented"))));
        return RequestId::createUuid();
    }

    RequestId rollbackFtpConfig(const QString&, QObject*, ApiCompletion<FtpConfigSnapshotDto> completion) override
    {
        completion(ApiResult<FtpConfigSnapshotDto>::failure(makeError(QStringLiteral("not_implemented"))));
        return RequestId::createUuid();
    }

    RequestId getFtpControl(QObject*, ApiCompletion<FtpControlDto> completion) override
    {
        completion(ApiResult<FtpControlDto>::failure(makeError(QStringLiteral("not_implemented"))));
        return RequestId::createUuid();
    }

    RequestId putFtpControl(const FtpControlUpdate&, QObject*, ApiCompletion<FtpControlDto> completion) override
    {
        completion(ApiResult<FtpControlDto>::failure(makeError(QStringLiteral("not_implemented"))));
        return RequestId::createUuid();
    }

    RequestId createFtpTask(const FtpTaskCreate&, QObject*, ApiCompletion<FtpTaskDetailDto> completion) override
    {
        completion(ApiResult<FtpTaskDetailDto>::failure(makeError(QStringLiteral("not_implemented"))));
        return RequestId::createUuid();
    }

    RequestId listFtpTasks(int, const std::optional<QString>&, QObject*, ApiCompletion<FtpTaskPageDto> completion) override
    {
        completion(ApiResult<FtpTaskPageDto>::failure(makeError(QStringLiteral("not_implemented"))));
        return RequestId::createUuid();
    }

    RequestId getFtpTask(const QString&, QObject*, ApiCompletion<FtpTaskDetailDto> completion) override
    {
        completion(ApiResult<FtpTaskDetailDto>::failure(makeError(QStringLiteral("not_implemented"))));
        return RequestId::createUuid();
    }

    RequestId retryFtpTask(const QString&, QObject*, ApiCompletion<FtpTaskDetailDto> completion) override
    {
        completion(ApiResult<FtpTaskDetailDto>::failure(makeError(QStringLiteral("not_implemented"))));
        return RequestId::createUuid();
    }

    void cancel(const RequestId&) override
    {
    }

    void cancelAll() override
    {
        ++cancelAllCalls;
    }
};

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

QVector<VehicleEvent> loadEvents(SqliteEventRepository& repository, QObject* context)
{
    EventQuery query;
    query.deviceId = QString::fromUtf8(DeviceId);
    query.limit = 20;
    auto rows = callResult<QVector<VehicleEvent>>([&](ApiCompletion<QVector<VehicleEvent>> completion) {
        return repository.queryEvents(query, context, std::move(completion));
    });
    if (!rows || !rows->isSuccess()) {
        QTest::qFail("Failed to load persisted events", __FILE__, __LINE__);
        return {};
    }
    return rows->value();
}

std::optional<SyncAnchor> loadAnchor(SqliteEventRepository& repository, QObject* context)
{
    auto result = callResult<std::optional<SyncAnchor>>([&](ApiCompletion<std::optional<SyncAnchor>> completion) {
        return repository.loadSyncAnchor(QString::fromUtf8(DeviceId), context, std::move(completion));
    });
    if (!result || !result->isSuccess()) {
        QTest::qFail("Failed to load sync anchor", __FILE__, __LINE__);
        return std::nullopt;
    }
    return result->value();
}

void saveAnchor(SqliteEventRepository& repository, QObject* context, const EventSortKey& key)
{
    SyncAnchor anchor;
    anchor.deviceId = QString::fromUtf8(DeviceId);
    anchor.previousHead = key;
    anchor.savedEpochMs = 1784280000000;
    auto result = callResult<void>([&](ApiCompletion<void> completion) {
        return repository.saveSyncAnchor(anchor, context, std::move(completion));
    });
    QVERIFY(result.has_value());
    if (!result->isSuccess()) {
        QFAIL(qPrintable(result->error().message));
    }
}

} // namespace

void EventSyncServiceTest::initTestCase()
{
    qRegisterMetaType<rv1126b::ApiError>();
    qRegisterMetaType<rv1126b::EventIdentity>();
}

void EventSyncServiceTest::pollsPagePersistsEventsAndEmitsSignals()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SqliteEventRepository repository(databasePath(tempDir));
    initializeRepository(repository, this);

    FakeBoardApiClient client;
    client.nextPage = pageWith({makeSummary(101, 7, OcrStatus::Matched)});

    BoardEventSyncService service(QString::fromUtf8(DeviceId), &client, &repository);
    service.setPollIntervalMs(EventSyncService::MaximumPollIntervalMs);
    QSignalSpy changedSpy(&service, &EventSyncService::eventChanged);
    QSignalSpy catchUpSpy(&service, &EventSyncService::initialCatchUpFinished);
    QSignalSpy errorSpy(&service, &EventSyncService::syncError);

    service.start();

    QTRY_COMPARE(catchUpSpy.size(), 1);
    QCOMPARE(errorSpy.size(), 0);
    QCOMPARE(changedSpy.size(), 1);
    QCOMPARE(client.listEventsCalls, 1);

    const QVector<VehicleEvent> rows = loadEvents(repository, this);
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.first().identity.eventId, 101);
    QCOMPARE(rows.first().plateText, QStringLiteral("ABC123"));

    service.stop();
}

void EventSyncServiceTest::refreshesQueuedEventsWithDetail()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString dbPath = databasePath(tempDir);
    SqliteEventRepository repository(dbPath);
    initializeRepository(repository, this);

    const EventSummaryDto queued = makeSummary(202, 9, OcrStatus::Queued);
    const EventSummaryDto matched = makeSummary(202, 9, OcrStatus::Matched);
    EventIdentity identity {QString::fromUtf8(DeviceId), 202, 9};

    FakeBoardApiClient client;
    client.nextPage = pageWith({queued});
    client.details.insert(identity, detailFor(matched));

    BoardEventSyncService service(QString::fromUtf8(DeviceId), &client, &repository);
    service.setPollIntervalMs(EventSyncService::MaximumPollIntervalMs);
    QSignalSpy changedSpy(&service, &EventSyncService::eventChanged);
    QSignalSpy catchUpSpy(&service, &EventSyncService::initialCatchUpFinished);
    QSignalSpy errorSpy(&service, &EventSyncService::syncError);

    service.start();

    QTRY_COMPARE(catchUpSpy.size(), 1);
    QCOMPARE(errorSpy.size(), 0);
    QCOMPARE(client.detailCalls, 1);
    QCOMPARE(changedSpy.size(), 2);

    const QVector<VehicleEvent> rows = loadEvents(repository, this);
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.first().ocrStatus.value, OcrStatus::Matched);
    QCOMPARE(rows.first().plateText, QStringLiteral("ABC123"));

    const QString connectionName = QStringLiteral("verify-detail-%1").arg(reinterpret_cast<quintptr>(this));
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(dbPath);
        QVERIFY(database.open());

        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral("SELECT COUNT(*) FROM rv_event_details WHERE event_id = 202 AND track_id = 9")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 1);
        database.close();
    }
    QSqlDatabase::removeDatabase(connectionName);

    service.stop();
}

void EventSyncServiceTest::usesSnapshotImageWhenEvidenceImageIsMissing()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SqliteEventRepository repository(databasePath(tempDir));
    initializeRepository(repository, this);

    EventSummaryDto queued = makeSummary(204, 9, OcrStatus::Queued);
    queued.evidenceAvailable = false;
    queued.evidenceRelativeUrl.clear();
    queued.rawJson.insert(QStringLiteral("evidence_url"), QJsonValue());
    queued.rawJson.insert(QStringLiteral("snapshot_url"), QStringLiteral("/api/v1/events/204/9/files/snapshot"));

    EventDetailDto detail = detailFor(queued);
    detail.summary = queued;
    detail.images.insert(QStringLiteral("evidence"), QJsonValue());
    detail.images.insert(QStringLiteral("snapshot"), QStringLiteral("/api/v1/events/204/9/files/snapshot"));
    detail.evidenceRelativeUrl = std::nullopt;

    EventIdentity identity {QString::fromUtf8(DeviceId), 204, 9};
    FakeBoardApiClient client;
    client.nextPage = pageWith({queued});
    client.details.insert(identity, detail);

    BoardEventSyncService service(QString::fromUtf8(DeviceId), &client, &repository);
    service.setPollIntervalMs(EventSyncService::MaximumPollIntervalMs);
    QSignalSpy catchUpSpy(&service, &EventSyncService::initialCatchUpFinished);
    QSignalSpy errorSpy(&service, &EventSyncService::syncError);

    service.start();

    QTRY_COMPARE(catchUpSpy.size(), 1);
    QCOMPARE(errorSpy.size(), 0);

    const QVector<VehicleEvent> rows = loadEvents(repository, this);
    QCOMPARE(rows.size(), 1);
    QVERIFY(rows.first().evidenceAvailable);
    QCOMPARE(rows.first().evidenceRelativeUrl, QStringLiteral("/api/v1/events/204/9/files/snapshot"));

    service.stop();
}

void EventSyncServiceTest::ignoresMissingDetailForStaleNonTerminalEvent()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SqliteEventRepository repository(databasePath(tempDir));
    initializeRepository(repository, this);

    FakeBoardApiClient client;
    client.nextPage = pageWith({makeSummary(203, 9, OcrStatus::Queued)});

    BoardEventSyncService service(QString::fromUtf8(DeviceId), &client, &repository);
    service.setPollIntervalMs(EventSyncService::MaximumPollIntervalMs);
    QSignalSpy catchUpSpy(&service, &EventSyncService::initialCatchUpFinished);
    QSignalSpy errorSpy(&service, &EventSyncService::syncError);

    service.start();

    QTRY_COMPARE(catchUpSpy.size(), 1);
    QCOMPARE(client.detailCalls, 1);
    QCOMPARE(errorSpy.size(), 0);
    QCOMPARE(loadEvents(repository, this).size(), 1);

    service.stop();
}

void EventSyncServiceTest::skipsOverlappingPolls()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SqliteEventRepository repository(databasePath(tempDir));
    initializeRepository(repository, this);

    FakeBoardApiClient client;
    client.holdListCompletion = true;
    client.nextPage = pageWith({makeSummary(303, 2, OcrStatus::Matched)});

    BoardEventSyncService service(QString::fromUtf8(DeviceId), &client, &repository);
    service.setPollIntervalMs(EventSyncService::MaximumPollIntervalMs);
    QSignalSpy catchUpSpy(&service, &EventSyncService::initialCatchUpFinished);

    service.start();
    QTRY_COMPARE(client.listEventsCalls, 1);

    service.pollNow();
    QTest::qWait(20);
    QCOMPARE(client.listEventsCalls, 1);

    client.completeHeldList();
    QTRY_COMPARE(catchUpSpy.size(), 1);
    QCOMPARE(client.listEventsCalls, 1);

    service.stop();
}

void EventSyncServiceTest::catchUpPaginatesUntilStoredAnchorAndSavesNewHead()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SqliteEventRepository repository(databasePath(tempDir));
    initializeRepository(repository, this);

    EventSortKey oldHead;
    oldHead.sourceEpochMs = 1784280001000;
    oldHead.eventId = 100;
    oldHead.trackId = 1;
    saveAnchor(repository, this, oldHead);

    EventPageDto firstPage = pageWith({
        makeSummary(300, 1, OcrStatus::Matched, 1784280003000),
        makeSummary(200, 1, OcrStatus::Matched, 1784280002000),
    });
    firstPage.hasMore = true;
    firstPage.nextCursor = QStringLiteral("page-2");

    EventPageDto secondPage = pageWith({
        makeSummary(100, 1, OcrStatus::Matched, 1784280001000),
        makeSummary(99, 1, OcrStatus::Matched, 1784280000999),
    });
    secondPage.hasMore = true;
    secondPage.nextCursor = QStringLiteral("page-3");

    FakeBoardApiClient client;
    client.pagesByCursor.insert(QString(), firstPage);
    client.pagesByCursor.insert(QStringLiteral("page-2"), secondPage);

    BoardEventSyncService service(QString::fromUtf8(DeviceId), &client, &repository);
    service.setPollIntervalMs(EventSyncService::MaximumPollIntervalMs);
    QSignalSpy catchUpSpy(&service, &EventSyncService::initialCatchUpFinished);
    QSignalSpy errorSpy(&service, &EventSyncService::syncError);

    service.start();

    QTRY_COMPARE(catchUpSpy.size(), 1);
    QCOMPARE(errorSpy.size(), 0);
    QCOMPARE(client.listEventsCalls, 2);
    QCOMPARE(client.requestedCursors, QStringList({QString(), QStringLiteral("page-2")}));

    const QVector<VehicleEvent> rows = loadEvents(repository, this);
    QCOMPARE(rows.size(), 3);
    QCOMPARE(rows.first().identity.eventId, 300);

    const std::optional<SyncAnchor> anchor = loadAnchor(repository, this);
    QVERIFY(anchor.has_value());
    QCOMPARE(anchor->previousHead.sourceEpochMs, 1784280003000);
    QCOMPARE(anchor->previousHead.eventId, 300);
    QCOMPARE(anchor->previousHead.trackId, 1);

    service.stop();
}

void EventSyncServiceTest::retriesAfterTemporaryFailure()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SqliteEventRepository repository(databasePath(tempDir));
    initializeRepository(repository, this);

    FakeBoardApiClient client;
    client.listFailure = makeError(QStringLiteral("network_timeout"));
    client.nextPage = pageWith({makeSummary(401, 1, OcrStatus::Matched)});

    BoardEventSyncService service(QString::fromUtf8(DeviceId), &client, &repository);
    service.setPollIntervalMs(EventSyncService::MaximumPollIntervalMs);
    QSignalSpy errorSpy(&service, &EventSyncService::syncError);
    QSignalSpy catchUpSpy(&service, &EventSyncService::initialCatchUpFinished);
    service.start();

    QTRY_COMPARE(errorSpy.size(), 1);
    client.listFailure.reset();
    QTRY_COMPARE_WITH_TIMEOUT(client.listEventsCalls, 2, 2500);
    QTRY_COMPARE_WITH_TIMEOUT(catchUpSpy.size(), 1, 2500);
    QCOMPARE(loadEvents(repository, this).size(), 1);

    service.stop();
}

void EventSyncServiceTest::stopCancelsPendingRequest()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SqliteEventRepository repository(databasePath(tempDir));
    initializeRepository(repository, this);

    FakeBoardApiClient client;
    client.holdListCompletion = true;
    BoardEventSyncService service(QString::fromUtf8(DeviceId), &client, &repository);
    service.start();
    QTRY_COMPARE(client.listEventsCalls, 1);

    service.stop();
    QCOMPARE(client.cancelAllCalls, 1);
    QVERIFY(!service.isRunning());
}

QTEST_MAIN(EventSyncServiceTest)

#include "EventSyncServiceTest.moc"
