#pragma once

#include "../core/Result.h"
#include "../domain/Models.h"

#include <QObject>
#include <QVector>

#include <optional>

namespace rv1126b {

class IEventRepository : public QObject
{
public:
    explicit IEventRepository(QObject* parent = nullptr)
        : QObject(parent)
    {
    }

    ~IEventRepository() override = default;

    virtual RequestId initialize(QObject* context, ApiCompletion<void> completion) = 0;
    virtual RequestId upsertDevice(
        const DeviceProfile& profile,
        QObject* context,
        ApiCompletion<void> completion) = 0;
    virtual RequestId loadDeviceProfiles(
        QObject* context,
        ApiCompletion<QVector<DeviceProfile>> completion) = 0;
    virtual RequestId deleteDeviceProfile(
        const QString& deviceId,
        QObject* context,
        ApiCompletion<void> completion) = 0;
    virtual RequestId upsertEvents(
        const QVector<VehicleEvent>& events,
        QObject* context,
        ApiCompletion<void> completion) = 0;
    virtual RequestId saveDetail(
        const EventDetailSnapshot& detail,
        QObject* context,
        ApiCompletion<void> completion) = 0;
    virtual RequestId queryEvents(
        const EventQuery& query,
        QObject* context,
        ApiCompletion<QVector<VehicleEvent>> completion) = 0;
    virtual RequestId loadEvent(
        const EventIdentity& identity,
        QObject* context,
        ApiCompletion<std::optional<VehicleEvent>> completion) = 0;
    virtual RequestId deleteEvent(
        const EventIdentity& identity,
        QObject* context,
        ApiCompletion<void> completion) = 0;
    virtual RequestId loadNonTerminalEvents(
        const QString& deviceId,
        QObject* context,
        ApiCompletion<QVector<VehicleEvent>> completion) = 0;
    virtual RequestId saveEvidenceState(
        const EvidenceCacheEntry& evidence,
        QObject* context,
        ApiCompletion<void> completion) = 0;
    virtual RequestId loadEvidenceState(
        const EventIdentity& identity,
        const QString& role,
        QObject* context,
        ApiCompletion<std::optional<EvidenceCacheEntry>> completion) = 0;
    virtual RequestId loadSyncAnchor(
        const QString& deviceId,
        QObject* context,
        ApiCompletion<std::optional<SyncAnchor>> completion) = 0;
    virtual RequestId saveSyncAnchor(
        const SyncAnchor& anchor,
        QObject* context,
        ApiCompletion<void> completion) = 0;
    virtual RequestId saveFtpTaskSnapshot(
        const StoredFtpTask& task,
        QObject* context,
        ApiCompletion<void> completion) = 0;
    virtual RequestId loadFtpTaskSnapshots(
        const FtpTaskQuery& query,
        QObject* context,
        ApiCompletion<QVector<StoredFtpTask>> completion) = 0;

    virtual void cancel(const RequestId& requestId) = 0;
    virtual void cancelAll() = 0;

    // Bounded recovery of persisted download work. Test/alternate repositories may opt out.
    virtual RequestId loadPendingEvidence(int limit, const QStringList& deviceIds,
        const QVector<EventIdentity>& excluded, QObject* context,
        ApiCompletion<QVector<VehicleEvent>> completion)
    {
        Q_UNUSED(limit) Q_UNUSED(deviceIds) Q_UNUSED(excluded) Q_UNUSED(context)
        if (completion) completion(ApiResult<QVector<VehicleEvent>>::success({}));
        return RequestId::createUuid();
    }
};

} // namespace rv1126b
