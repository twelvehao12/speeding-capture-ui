#include "../src/rv1126b/services/LocalEvidenceService.h"
#include "../src/rv1126b/storage/SqliteEventRepository.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <functional>
#include <optional>

using namespace rv1126b;

class LocalEvidenceServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void returnsExistingLocalFileWithoutQueueingDownload();
    void queuesDownloadWhenOnlineAndCacheMissing();
    void reportsNotRequestedWhenOfflineAndCacheMissing();
};

namespace {

VehicleEvent makeEvent(qint64 eventId = 701)
{
    VehicleEvent event;
    event.identity.deviceId = QStringLiteral("rv-device-001");
    event.identity.eventId = eventId;
    event.identity.trackId = 7;
    event.evidenceAvailable = true;
    event.evidenceRelativeUrl = QStringLiteral("/api/v1/events/%1/7/evidence.jpg").arg(eventId);
    return event;
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
    return QDir(tempDir.path()).filePath(QStringLiteral("rv1126b-local-evidence.sqlite"));
}

class FakeEvidenceCache final : public EvidenceCache
{
public:
    explicit FakeEvidenceCache(QString cacheRootPath)
        : cacheRootPath_(std::move(cacheRootPath))
    {
    }

    void enqueue(const VehicleEvent& event) override
    {
        ++enqueueCount;
        enqueuedEvents.append(event);
    }

    RequestId removeLocal(const VehicleEvent&, QObject*, ApiCompletion<void> completion) override
    {
        const RequestId requestId = RequestId::createUuid();
        completion(ApiResult<void>::success());
        return requestId;
    }

    void cancel(const EventIdentity&) override
    {
    }

    void cancelDevice(const QString&) override
    {
    }

    void cancelAll() override
    {
    }

    QString finalPathFor(const VehicleEvent& event) const override
    {
        return QDir(cacheRootPath_).filePath(QStringLiteral("%1_%2_%3.jpg")
            .arg(event.identity.deviceId)
            .arg(event.identity.eventId)
            .arg(event.identity.trackId));
    }

    int enqueueCount = 0;
    QVector<VehicleEvent> enqueuedEvents;

private:
    QString cacheRootPath_;
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

LocalEvidenceResult resolve(
    LocalEvidenceService& service,
    const VehicleEvent& event,
    bool online,
    QObject* context)
{
    auto result = callResult<LocalEvidenceResult>([&](ApiCompletion<LocalEvidenceResult> completion) {
        return service.resolve(event, online, context, std::move(completion));
    });
    if (!result || !result->isSuccess()) {
        QTest::qFail("Failed to resolve local evidence", __FILE__, __LINE__);
        return {};
    }
    return result->value();
}

} // namespace

void LocalEvidenceServiceTest::returnsExistingLocalFileWithoutQueueingDownload()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SqliteEventRepository repository(databasePath(tempDir));
    initializeRepository(repository, this);

    FakeEvidenceCache cache(QDir(tempDir.path()).filePath(QStringLiteral("cache")));
    LocalEvidenceService service(&repository, &cache);
    const VehicleEvent event = makeEvent();

    QDir().mkpath(QFileInfo(cache.finalPathFor(event)).absolutePath());
    QFile file(cache.finalPathFor(event));
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write("cached");
    file.close();

    const LocalEvidenceResult result = resolve(service, event, false, this);
    QCOMPARE(result.status, EvidenceCacheStatus::Available);
    QCOMPARE(result.localFilePath, cache.finalPathFor(event));
    QCOMPARE(cache.enqueueCount, 0);
}

void LocalEvidenceServiceTest::queuesDownloadWhenOnlineAndCacheMissing()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SqliteEventRepository repository(databasePath(tempDir));
    initializeRepository(repository, this);

    FakeEvidenceCache cache(QDir(tempDir.path()).filePath(QStringLiteral("cache")));
    LocalEvidenceService service(&repository, &cache);
    const VehicleEvent event = makeEvent(702);

    const LocalEvidenceResult result = resolve(service, event, true, this);
    QCOMPARE(result.status, EvidenceCacheStatus::Queued);
    QVERIFY(result.downloadQueued);
    QCOMPARE(cache.enqueueCount, 1);
    QCOMPARE(cache.enqueuedEvents.first().identity, event.identity);
}

void LocalEvidenceServiceTest::reportsNotRequestedWhenOfflineAndCacheMissing()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SqliteEventRepository repository(databasePath(tempDir));
    initializeRepository(repository, this);

    FakeEvidenceCache cache(QDir(tempDir.path()).filePath(QStringLiteral("cache")));
    LocalEvidenceService service(&repository, &cache);
    const VehicleEvent event = makeEvent(703);

    const LocalEvidenceResult result = resolve(service, event, false, this);
    QCOMPARE(result.status, EvidenceCacheStatus::NotRequested);
    QVERIFY(result.localFilePath.isEmpty());
    QVERIFY(!result.downloadQueued);
    QCOMPARE(cache.enqueueCount, 0);
}

QTEST_MAIN(LocalEvidenceServiceTest)

#include "LocalEvidenceServiceTest.moc"
