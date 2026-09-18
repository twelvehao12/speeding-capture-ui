#include "../src/rv1126b/services/EvidenceCacheMaintenanceService.h"
#include "../src/rv1126b/services/CacheFileLease.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>
#include <QSignalSpy>

using namespace rv1126b;

class EvidenceCacheMaintenanceServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void deletesFilesOlderThanRetentionDays();
    void trimsOldestFilesWhenCapacityExceeded();
    void neverDeletesPartFiles();
    void deletesOldestFilesWhenFreeSpaceIsBelowThreshold();
    void backgroundCleanupSkipsLeasedFilesAndCanCancel();
};

namespace {

QString writeFile(const QString& path, int bytes, int ageDays)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    const bool opened = file.open(QIODevice::WriteOnly | QIODevice::Truncate);
    Q_ASSERT(opened);
    file.write(QByteArray(bytes, 'x'));
    file.close();

    const QDateTime timestamp = QDateTime::currentDateTime().addDays(-ageDays);
    QFile timestampFile(path);
    const bool timestampOpened = timestampFile.open(QIODevice::ReadWrite);
    Q_ASSERT(timestampOpened);
    const bool timestampSet = timestampFile.setFileTime(timestamp, QFileDevice::FileModificationTime);
    Q_ASSERT(timestampSet);
    timestampFile.close();
    return path;
}

EvidenceCacheCleanupPolicy basePolicy()
{
    EvidenceCacheCleanupPolicy policy;
    policy.retentionDays = 30;
    policy.maxCacheBytes = 0;
    policy.minFreeSpaceBytes = 0;
    policy.deleteOldestWhenLowSpace = false;
    return policy;
}

} // namespace

void EvidenceCacheMaintenanceServiceTest::deletesFilesOlderThanRetentionDays()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString oldFile = writeFile(QDir(tempDir.path()).filePath(QStringLiteral("device/old.jpg")), 20, 40);
    const QString freshFile = writeFile(QDir(tempDir.path()).filePath(QStringLiteral("device/fresh.jpg")), 20, 5);

    EvidenceCacheCleanupPolicy policy = basePolicy();
    policy.retentionDays = 30;
    EvidenceCacheMaintenanceService service(tempDir.path());
    const EvidenceCacheCleanupResult result = service.cleanup(policy);

    QCOMPARE(result.scannedFiles, 2);
    QCOMPARE(result.deletedFiles, 1);
    QVERIFY(!QFileInfo::exists(oldFile));
    QVERIFY(QFileInfo::exists(freshFile));
}

void EvidenceCacheMaintenanceServiceTest::trimsOldestFilesWhenCapacityExceeded()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString oldest = writeFile(QDir(tempDir.path()).filePath(QStringLiteral("oldest.jpg")), 60, 20);
    const QString middle = writeFile(QDir(tempDir.path()).filePath(QStringLiteral("middle.jpg")), 60, 10);
    const QString newest = writeFile(QDir(tempDir.path()).filePath(QStringLiteral("newest.jpg")), 60, 1);

    EvidenceCacheCleanupPolicy policy = basePolicy();
    policy.retentionDays = 365;
    policy.maxCacheBytes = 120;
    EvidenceCacheMaintenanceService service(tempDir.path());
    const EvidenceCacheCleanupResult result = service.cleanup(policy);

    QCOMPARE(result.scannedFiles, 3);
    QCOMPARE(result.deletedFiles, 1);
    QVERIFY(!QFileInfo::exists(oldest));
    QVERIFY(QFileInfo::exists(middle));
    QVERIFY(QFileInfo::exists(newest));
}

void EvidenceCacheMaintenanceServiceTest::neverDeletesPartFiles()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString partFile = writeFile(QDir(tempDir.path()).filePath(QStringLiteral("active.jpg.part")), 100, 90);
    const QString oldFile = writeFile(QDir(tempDir.path()).filePath(QStringLiteral("old.jpg")), 100, 90);

    EvidenceCacheCleanupPolicy policy = basePolicy();
    policy.retentionDays = 1;
    policy.maxCacheBytes = 1;
    EvidenceCacheMaintenanceService service(tempDir.path());
    const EvidenceCacheCleanupResult result = service.cleanup(policy);

    QCOMPARE(result.scannedFiles, 1);
    QCOMPARE(result.deletedFiles, 1);
    QVERIFY(QFileInfo::exists(partFile));
    QVERIFY(!QFileInfo::exists(oldFile));
}

void EvidenceCacheMaintenanceServiceTest::deletesOldestFilesWhenFreeSpaceIsBelowThreshold()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString oldest = writeFile(QDir(tempDir.path()).filePath(QStringLiteral("oldest.jpg")), 60, 20);
    const QString newest = writeFile(QDir(tempDir.path()).filePath(QStringLiteral("newest.jpg")), 60, 1);

    EvidenceCacheCleanupPolicy policy = basePolicy();
    policy.retentionDays = 365;
    policy.minFreeSpaceBytes = 100;
    policy.deleteOldestWhenLowSpace = true;
    EvidenceCacheMaintenanceService service(tempDir.path(), [oldest] {
        return QFileInfo::exists(oldest) ? 40LL : 120LL;
    });
    const EvidenceCacheCleanupResult result = service.cleanup(policy);

    QCOMPARE(result.deletedFiles, 1);
    QVERIFY(!QFileInfo::exists(oldest));
    QVERIFY(QFileInfo::exists(newest));
}

void EvidenceCacheMaintenanceServiceTest::backgroundCleanupSkipsLeasedFilesAndCanCancel()
{
    QTemporaryDir dir;
    const auto path = writeFile(dir.filePath(QStringLiteral("leased.jpg")), 32, 60);
    auto lease = CacheFileLease::acquire(path);
    QVERIFY(lease);
    EvidenceCacheMaintenanceService service(dir.path());
    QSignalSpy finished(&service, &EvidenceCacheMaintenanceService::cleanupFinished);
    service.cleanupAsync(basePolicy());
    QTRY_COMPARE(finished.size(), 1);
    QVERIFY(QFileInfo::exists(path));
    lease.reset();
    service.cleanupAsync(basePolicy());
    QTRY_COMPARE(finished.size(), 2);
    QVERIFY(!QFileInfo::exists(path));
    service.cleanupAsync(basePolicy());
    service.cancel();
    QTest::qWait(30);
    QCOMPARE(finished.size(), 2);
}

QTEST_MAIN(EvidenceCacheMaintenanceServiceTest)

#include "EvidenceCacheMaintenanceServiceTest.moc"
