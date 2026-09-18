#pragma once

#include "../ports/IEventRepository.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>

namespace rv1126b {

class SqliteEventStore final : public IEventRepository
{
public:
    explicit SqliteEventStore(QObject* parent = nullptr);
    explicit SqliteEventStore(const QString& databasePath, QObject* parent = nullptr);
    ~SqliteEventStore() override;

    RequestId initialize(QObject* context, ApiCompletion<void> completion) override;
    RequestId upsertDevice(
        const DeviceProfile& profile,
        QObject* context,
        ApiCompletion<void> completion) override;
    RequestId loadDeviceProfiles(
        QObject* context,
        ApiCompletion<QVector<DeviceProfile>> completion) override;
    RequestId deleteDeviceProfile(
        const QString& deviceId,
        QObject* context,
        ApiCompletion<void> completion) override;
    RequestId upsertEvents(
        const QVector<VehicleEvent>& events,
        QObject* context,
        ApiCompletion<void> completion) override;
    RequestId saveDetail(
        const EventDetailSnapshot& detail,
        QObject* context,
        ApiCompletion<void> completion) override;
    RequestId queryEvents(
        const EventQuery& query,
        QObject* context,
        ApiCompletion<QVector<VehicleEvent>> completion) override;
    RequestId loadEvent(
        const EventIdentity& identity,
        QObject* context,
        ApiCompletion<std::optional<VehicleEvent>> completion) override;
    RequestId deleteEvent(
        const EventIdentity& identity,
        QObject* context,
        ApiCompletion<void> completion) override;
    RequestId loadNonTerminalEvents(
        const QString& deviceId,
        QObject* context,
        ApiCompletion<QVector<VehicleEvent>> completion) override;
    RequestId saveEvidenceState(
        const EvidenceCacheEntry& evidence,
        QObject* context,
        ApiCompletion<void> completion) override;
    RequestId loadEvidenceState(
        const EventIdentity& identity,
        const QString& role,
        QObject* context,
        ApiCompletion<std::optional<EvidenceCacheEntry>> completion) override;
    RequestId loadSyncAnchor(
        const QString& deviceId,
        QObject* context,
        ApiCompletion<std::optional<SyncAnchor>> completion) override;
    RequestId saveSyncAnchor(
        const SyncAnchor& anchor,
        QObject* context,
        ApiCompletion<void> completion) override;
    RequestId saveFtpTaskSnapshot(
        const StoredFtpTask& task,
        QObject* context,
        ApiCompletion<void> completion) override;
    RequestId loadFtpTaskSnapshots(
        const FtpTaskQuery& query,
        QObject* context,
        ApiCompletion<QVector<StoredFtpTask>> completion) override;

    void cancel(const RequestId& requestId) override;
    void cancelAll() override;
    RequestId loadPendingEvidence(int limit, const QStringList& deviceIds,
        const QVector<EventIdentity>& excluded, QObject* context,
        ApiCompletion<QVector<VehicleEvent>> completion) override;

private:
    bool openDatabase(QString* errorMessage);
    bool ensureSchema(QString* errorMessage);
    bool migrateEventSchema(QString* errorMessage);
    bool migrateEvidenceCacheSchema(QString* errorMessage);
    bool execSql(const QString& sql, QString* errorMessage);
    bool beginTransaction(QString* errorMessage);
    bool commitTransaction(QString* errorMessage);
    void rollbackTransaction();

    ApiError storageError(const QString& message) const;

    bool upsertDeviceInternal(const DeviceProfile& profile, QString* errorMessage);
    bool upsertEventInternal(const VehicleEvent& event, QString* errorMessage);
    bool saveEvidenceStateInternal(const EvidenceCacheEntry& evidence, QString* errorMessage);
    bool saveFtpTaskSnapshotInternal(const StoredFtpTask& task, QString* errorMessage);
    bool loadFtpTaskTargets(StoredFtpTask* task, QString* errorMessage) const;

    DeviceProfile deviceProfileFromQuery(const QSqlQuery& query) const;
    VehicleEvent eventFromQuery(const QSqlQuery& query) const;
    EvidenceCacheEntry evidenceFromQuery(const QSqlQuery& query) const;
    StoredFtpTask ftpTaskFromQuery(const QSqlQuery& query) const;
    StoredFtpTargetStatus ftpTargetFromQuery(const QSqlQuery& query) const;

    template<typename T>
    RequestId finish(QObject* context, ApiCompletion<T> completion, ApiResult<T> result)
    {
        Q_UNUSED(context)
        const RequestId requestId = RequestId::createUuid();
        if (completion) {
            completion(std::move(result));
        }
        return requestId;
    }

    QString databasePath_;
    QString connectionName_;
    QSqlDatabase database_;
};

} // namespace rv1126b
