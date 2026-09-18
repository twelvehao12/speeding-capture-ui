#include "../src/rv1126b/application/EventViewController.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace rv1126b;

class FakeEventRepository final : public IEventRepository
{
public:
    RequestId initialize(QObject*, ApiCompletion<void> c) override { return done(std::move(c)); }
    RequestId upsertDevice(const DeviceProfile&, QObject*, ApiCompletion<void> c) override { return done(std::move(c)); }
    RequestId loadDeviceProfiles(QObject*, ApiCompletion<QVector<DeviceProfile>> c) override
    { return value(std::move(c), QVector<DeviceProfile>{}); }
    RequestId deleteDeviceProfile(const QString&, QObject*, ApiCompletion<void> c) override
    { return done(std::move(c)); }
    RequestId upsertEvents(const QVector<VehicleEvent>&, QObject*, ApiCompletion<void> c) override { return done(std::move(c)); }
    RequestId saveDetail(const EventDetailSnapshot&, QObject*, ApiCompletion<void> c) override { return done(std::move(c)); }

    RequestId queryEvents(const EventQuery& query, QObject*, ApiCompletion<QVector<VehicleEvent>> c) override
    {
        ++queryCount;
        if (syntheticCount >= 0) {
            QVector<VehicleEvent> page;
            for (int i = query.offset; i < qMin(syntheticCount, query.offset + query.limit); ++i) {
                VehicleEvent row;
                row.identity = {QStringLiteral("stress"), syntheticCount - i, 1};
                page.append(row);
            }
            maxPageSize = qMax(maxPageSize, int(page.size()));
            return value(std::move(c), page);
        }
        QVector<VehicleEvent> rows;
        for (const VehicleEvent& event : events) {
            if (query.deviceId && event.identity.deviceId != *query.deviceId) continue;
            if (query.plateText && !event.plateText.contains(*query.plateText, Qt::CaseInsensitive)) continue;
            if (query.startEpochMs && event.eventTime.epochMs < *query.startEpochMs) continue;
            if (query.endEpochMs && event.eventTime.epochMs >= *query.endEpochMs) continue;
            rows.append(event);
        }
        std::sort(rows.begin(), rows.end(), [query](const auto& a, const auto& b) {
            return query.newestFirst ? a.eventTime.epochMs > b.eventTime.epochMs
                                     : a.eventTime.epochMs < b.eventTime.epochMs;
        });
        rows = rows.mid(query.offset, query.limit);
        return value(std::move(c), rows);
    }

    RequestId loadEvent(const EventIdentity& identity, QObject*, ApiCompletion<std::optional<VehicleEvent>> c) override
    {
        for (const VehicleEvent& event : events) {
            if (event.identity == identity) return value(std::move(c), std::optional<VehicleEvent>(event));
        }
        return value(std::move(c), std::optional<VehicleEvent>{});
    }

    RequestId deleteEvent(const EventIdentity& identity, QObject*, ApiCompletion<void> c) override
    {
        if (syntheticCount >= 0) { --syntheticCount; return done(std::move(c)); }
        for (int i = 0; i < events.size(); ++i) {
            if (events.at(i).identity == identity) {
                events.removeAt(i);
                break;
            }
        }
        deleted.append(identity);
        return done(std::move(c));
    }

    RequestId loadNonTerminalEvents(const QString&, QObject*, ApiCompletion<QVector<VehicleEvent>> c) override
    { return value(std::move(c), QVector<VehicleEvent>{}); }
    RequestId saveEvidenceState(const EvidenceCacheEntry&, QObject*, ApiCompletion<void> c) override { return done(std::move(c)); }
    RequestId loadEvidenceState(const EventIdentity& identity, const QString&, QObject*,
                                ApiCompletion<std::optional<EvidenceCacheEntry>> c) override
    { return value(std::move(c), evidence.value(identity)); }
    RequestId loadSyncAnchor(const QString&, QObject*, ApiCompletion<std::optional<SyncAnchor>> c) override
    { return value(std::move(c), std::optional<SyncAnchor>{}); }
    RequestId saveSyncAnchor(const SyncAnchor&, QObject*, ApiCompletion<void> c) override { return done(std::move(c)); }
    RequestId saveFtpTaskSnapshot(const StoredFtpTask&, QObject*, ApiCompletion<void> c) override { return done(std::move(c)); }
    RequestId loadFtpTaskSnapshots(const FtpTaskQuery&, QObject*, ApiCompletion<QVector<StoredFtpTask>> c) override
    { return value(std::move(c), QVector<StoredFtpTask>{}); }
    void cancel(const RequestId&) override { ++cancelCount; }
    void cancelAll() override { ++cancelAllCount; }

    template<typename T>
    RequestId value(ApiCompletion<T> c, T result)
    {
        const RequestId id = QUuid::createUuid();
        c(ApiResult<T>::success(std::move(result)));
        return id;
    }

    RequestId done(ApiCompletion<void> c)
    {
        const RequestId id = QUuid::createUuid();
        c(ApiResult<void>::success());
        return id;
    }

    QVector<VehicleEvent> events;
    QHash<EventIdentity, std::optional<EvidenceCacheEntry>> evidence;
    QVector<EventIdentity> deleted;
    int queryCount = 0;
    int cancelCount = 0;
    int cancelAllCount = 0;
    int syntheticCount = -1;
    int maxPageSize = 0;
};

class FakeEvidenceCache final : public EvidenceCache
{
public:
    void enqueue(const VehicleEvent& event) override { enqueued.append(event.identity); }
    RequestId removeLocal(const VehicleEvent& event, QObject*, ApiCompletion<void> c) override
    {
        removed.append(event.identity);
        if (removeFailures.contains(event.identity)) {
            ApiError error;
            error.code = QStringLiteral("cache_remove_failed");
            error.category = ApiErrorCategory::Storage;
            c(ApiResult<void>::failure(error));
        } else {
            c(ApiResult<void>::success());
        }
        return QUuid::createUuid();
    }
    void cancel(const EventIdentity&) override {}
    void cancelDevice(const QString& id) override { cancelledDevices.append(id); }
    void cancelAll() override { ++cancelAllCount; }
    QString finalPathFor(const VehicleEvent& event) const override
    { return QStringLiteral("/%1.jpg").arg(event.identity.eventId); }

    QVector<EventIdentity> enqueued;
    QVector<EventIdentity> removed;
    QSet<EventIdentity> removeFailures;
    QStringList cancelledDevices;
    int cancelAllCount = 0;
};

class FakeEventSync final : public EventSyncService
{
public:
    explicit FakeEventSync(QString id) : id_(std::move(id)) {}
    QString deviceId() const override { return id_; }
    void start() override { running_ = true; ++startCount; }
    void stop() override { running_ = false; ++stopCount; }
    bool isRunning() const override { return running_; }
    void setPollIntervalMs(int value) override { interval_ = value; }
    int pollIntervalMs() const override { return interval_; }
    void pollNow() override { ++pollNowCount; }
    void syncAllExisting() override { ++syncAllCount; }
    void markCurrentHeadAsSynced() override { ++markCurrentCount; }
    void change(const EventIdentity& identity) { emit eventChanged(identity); }
    QString id_;
    bool running_ = false;
    int interval_ = DefaultPollIntervalMs;
    int startCount = 0;
    int stopCount = 0;
    int pollNowCount = 0;
    int syncAllCount = 0;
    int markCurrentCount = 0;
};

class EventViewControllerTest final : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void exactChangesUpsertAndCacheEvidence();
    void historyFiltersAndEvidenceOfflineState();
    void deleteAndClearPreserveRowsWhenImageRemovalFails();
    void exportUsesAllPagesAndContainsCompositeIdentity();
    void disconnectAndShutdownReleaseResources();
    void hundredThousandRowsExportAndClearInBoundedBatches();
    void bulkCancellationDiscardsPartialExportAndStopsClear();

private:
    static VehicleEvent event(QString device, qint64 eventId, qint64 trackId,
                              qint64 epoch, OcrStatus status = OcrStatus::Queued);
};

void EventViewControllerTest::initTestCase()
{
    qRegisterMetaType<VehicleEvent>();
    qRegisterMetaType<EvidenceCacheEntry>();
    qRegisterMetaType<EventIdentity>();
}

void EventViewControllerTest::exactChangesUpsertAndCacheEvidence()
{
    FakeEventRepository repository;
    FakeEvidenceCache cache;
    FakeEventSync sync(QStringLiteral("dev-a"));
    VehicleEvent queued = event(QStringLiteral("dev-a"), 1, 2, 1000);
    repository.events = {queued};
    EventViewController controller({&repository, &cache});
    controller.attachSyncService(&sync);
    DeviceSessionSnapshot snapshot;
    snapshot.profile.deviceId = QStringLiteral("dev-a");
    snapshot.state = DeviceSessionState::Online;
    controller.setDeviceSession(snapshot);
    QCOMPARE(sync.startCount, 1);

    EventQuery realtimeQuery;
    realtimeQuery.deviceId = QStringLiteral("dev-a");
    realtimeQuery.limit = 100;
    controller.refreshRealtime(realtimeQuery);
    QSignalSpy upsertSpy(&controller, &EventViewController::eventUpserted);
    repository.events[0].ocrStatus.value = OcrStatus::Matched;
    repository.events[0].plateText = QStringLiteral("粤B12345");
    repository.events[0].evidenceAvailable = true;
    sync.change(queued.identity);

    QTRY_COMPARE(upsertSpy.size(), 1);
    QCOMPARE(upsertSpy.takeFirst().at(0).value<VehicleEvent>().plateText, QStringLiteral("粤B12345"));
    QCOMPARE(cache.enqueued.size(), 1);
}

void EventViewControllerTest::historyFiltersAndEvidenceOfflineState()
{
    FakeEventRepository repository;
    FakeEvidenceCache cache;
    repository.events = {
        event(QStringLiteral("dev-a"), 1, 1, 1000),
        event(QStringLiteral("dev-b"), 2, 1, 2000)
    };
    repository.events[0].plateText = QStringLiteral("ABC");
    EventViewController controller({&repository, &cache});
    EventQuery query;
    query.deviceId = QStringLiteral("dev-a");
    query.plateText = QStringLiteral("ab");
    query.startEpochMs = 500;
    query.endEpochMs = 1500;
    QSignalSpy rowsSpy(&controller, &EventViewController::eventsReset);
    controller.queryHistory(query);
    QCOMPARE(rowsSpy.size(), 1);
    QCOMPARE(rowsSpy.takeFirst().at(0).value<QVector<VehicleEvent>>().size(), 1);

    QSignalSpy evidenceSpy(&controller, &EventViewController::evidenceChanged);
    controller.requestEvidence(repository.events[0]);
    QCOMPARE(evidenceSpy.size(), 1);
    const EvidenceCacheEntry state = evidenceSpy.takeFirst().at(0).value<EvidenceCacheEntry>();
    QCOMPARE(state.status, EvidenceCacheStatus::Missing);
    QCOMPARE(state.failureCode, QStringLiteral("offline_not_cached"));
}

void EventViewControllerTest::deleteAndClearPreserveRowsWhenImageRemovalFails()
{
    FakeEventRepository repository;
    FakeEvidenceCache cache;
    const VehicleEvent first = event(QStringLiteral("dev-a"), 1, 1, 1000);
    const VehicleEvent second = event(QStringLiteral("dev-a"), 2, 1, 2000);
    repository.events = {first, second};
    cache.removeFailures.insert(first.identity);
    EventViewController controller({&repository, &cache});
    QSignalSpy finishedSpy(&controller, &EventViewController::deleteFinished);

    controller.deleteLocalEvent(first);
    QCOMPARE(repository.events.size(), 2);
    QCOMPARE(finishedSpy.takeFirst().at(1).toInt(), 1);

    EventQuery query;
    query.deviceId = QStringLiteral("dev-a");
    controller.clearLocalHistory(query);
    QTRY_COMPARE(repository.events.size(), 1);
    QCOMPARE(repository.events.first().identity, first.identity);
    QTRY_VERIFY(finishedSpy.size() >= 1);
    const QList<QVariant> result = finishedSpy.takeLast();
    QCOMPARE(result.at(0).toInt(), 1);
    QCOMPARE(result.at(1).toInt(), 1);
}

void EventViewControllerTest::exportUsesAllPagesAndContainsCompositeIdentity()
{
    FakeEventRepository repository;
    FakeEvidenceCache cache;
    for (int i = 0; i < 105; ++i) repository.events.append(event(QStringLiteral("dev-a"), i, i + 1, i));
    EventViewController controller({&repository, &cache});
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("events.csv"));
    QSignalSpy exportSpy(&controller, &EventViewController::exportFinished);
    controller.exportHistory(EventQuery{}, path);
    QTRY_COMPARE(exportSpy.size(), 1);
    QCOMPARE(exportSpy.first().at(1).toInt(), 105);
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray csv = file.readAll();
    QVERIFY(csv.contains("device_id,event_id,track_id"));
    QVERIFY(csv.contains("dev-a"));
    QVERIFY(repository.queryCount >= 2);
}

void EventViewControllerTest::disconnectAndShutdownReleaseResources()
{
    FakeEventRepository repository;
    FakeEvidenceCache cache;
    FakeEventSync sync(QStringLiteral("dev-a"));
    EventViewController controller({&repository, &cache});
    controller.attachSyncService(&sync);
    sync.start();
    controller.stopDevice(QStringLiteral("dev-a"));
    QCOMPARE(sync.stopCount, 1);
    QCOMPARE(cache.cancelledDevices, QStringList{QStringLiteral("dev-a")});
    controller.shutdown();
    controller.shutdown();
    QCOMPARE(cache.cancelAllCount, 1);
    QCOMPARE(sync.stopCount, 2);
}

VehicleEvent EventViewControllerTest::event(QString device, qint64 eventId, qint64 trackId,
                                             qint64 epoch, OcrStatus status)
{
    VehicleEvent value;
    value.identity = {std::move(device), eventId, trackId};
    value.eventTime.epochMs = epoch;
    value.eventTime.sourceEpochMs = epoch;
    value.eventTime.quality.value = TimeQuality::NativeUtc;
    value.ocrStatus.value = status;
    return value;
}

void EventViewControllerTest::hundredThousandRowsExportAndClearInBoundedBatches()
{
    FakeEventRepository repository;
    repository.syntheticCount = 100000;
    FakeEvidenceCache cache;
    EventViewController controller({&repository, &cache});
    QTemporaryDir dir;
    const auto path = dir.filePath(QStringLiteral("stress.csv"));
    QSignalSpy exported(&controller, &EventViewController::exportFinished);
    controller.exportHistory(EventQuery{}, path);
    QTRY_COMPARE_WITH_TIMEOUT(exported.size(), 1, 60000);
    QCOMPARE(exported.first().at(1).toInt(), 100000);
    QCOMPARE(repository.maxPageSize, 100);
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    int lines = 0;
    while (!file.atEnd()) { file.readLine(); ++lines; }
    QCOMPARE(lines, 100001);
    int heartbeats = 0;
    QTimer timer;
    connect(&timer, &QTimer::timeout, this, [&] { ++heartbeats; });
    timer.start(1);
    QSignalSpy cleared(&controller, &EventViewController::deleteFinished);
    controller.clearLocalHistory(EventQuery{});
    QTRY_COMPARE_WITH_TIMEOUT(cleared.size(), 1, 60000);
    QCOMPARE(repository.syntheticCount, 0);
    QCOMPARE(cleared.first().at(0).toInt(), 100000);
    QVERIFY(heartbeats > 0);
}

void EventViewControllerTest::bulkCancellationDiscardsPartialExportAndStopsClear()
{
    FakeEventRepository repository;
    repository.syntheticCount = 100000;
    FakeEvidenceCache cache;
    EventViewController controller({&repository, &cache});
    QTemporaryDir dir;
    const auto path = dir.filePath(QStringLiteral("cancelled.csv"));
    QFile existing(path);
    QVERIFY(existing.open(QIODevice::WriteOnly));
    existing.write("original");
    existing.close();
    connect(&controller, &EventViewController::bulkProgress, this, [&](const QString& operation, int count) {
        if (count < 100) return;
        if (operation == QStringLiteral("export")) controller.cancelExport();
        else controller.cancelClear();
    });
    QSignalSpy finished(&controller, &EventViewController::bulkFinished);
    controller.exportHistory(EventQuery{}, path);
    QTRY_COMPARE(finished.size(), 1);
    QVERIFY(finished.last().at(1).toBool());
    QVERIFY(existing.open(QIODevice::ReadOnly));
    QCOMPARE(existing.readAll(), QByteArrayLiteral("original"));
    controller.clearLocalHistory(EventQuery{});
    QTRY_COMPARE(finished.size(), 2);
    QVERIFY(finished.last().at(1).toBool());
    QTest::qWait(30);
    QCOMPARE(repository.syntheticCount, 99900);
}

QTEST_MAIN(EventViewControllerTest)
#include "EventViewControllerTest.moc"

