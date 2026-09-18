#include "EventViewController.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSharedPointer>
#include <QTextStream>
#include <QSaveFile>

#include <algorithm>

namespace rv1126b {
namespace {

QString csvEscape(QString value)
{
    value.replace(QStringLiteral("\""), QStringLiteral("\"\""));
    if (value.contains(QLatin1Char(',')) || value.contains(QLatin1Char('\"'))
        || value.contains(QLatin1Char('\n')) || value.contains(QLatin1Char('\r'))) {
        return QStringLiteral("\"%1\"").arg(value);
    }
    return value;
}

QString timeQualityText(TimeQuality quality)
{
    switch (quality) {
    case TimeQuality::NativeUtc:
        return QStringLiteral("native_utc");
    case TimeQuality::ConfiguredOffset:
        return QStringLiteral("configured_offset");
    case TimeQuality::BoardEpochUnverified:
        return QStringLiteral("board_epoch_unverified");
    case TimeQuality::AppApiSetCurrentBoot:
        return QStringLiteral("app_api_set_current_boot");
    case TimeQuality::RtcRestoredCurrentBoot:
        return QStringLiteral("rtc_restored_current_boot");
    case TimeQuality::Unknown:
    default:
        return QStringLiteral("unknown");
    }
}

QString ocrStatusText(const WireEnum<OcrStatus>& status)
{
    if (!status.rawValue.isEmpty()) {
        return status.rawValue;
    }
    switch (status.value) {
    case OcrStatus::Queued: return QStringLiteral("queued");
    case OcrStatus::Matched: return QStringLiteral("matched");
    case OcrStatus::NoPlate: return QStringLiteral("no_plate");
    case OcrStatus::Ambiguous: return QStringLiteral("ambiguous");
    case OcrStatus::NoVehicle: return QStringLiteral("no_vehicle");
    case OcrStatus::Failed: return QStringLiteral("failed");
    case OcrStatus::TimedOut: return QStringLiteral("timed_out");
    case OcrStatus::QueueFull: return QStringLiteral("queue_full");
    case OcrStatus::Unknown:
    default: return QStringLiteral("unknown");
    }
}

bool sameRemoteImageUrl(const QString& left, const QString& right)
{
    return left.trimmed() == right.trimmed();
}

} // namespace

class CsvExportWriter final : public QObject
{
public:
    QString begin(const QString& path)
    {
        file_.reset();
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) return QStringLiteral("无法创建导出目录");
        file_ = std::make_unique<QSaveFile>(path);
        if (!file_->open(QIODevice::WriteOnly)) return file_->errorString();
        file_->write("\xEF\xBB\xBF", 3);
        file_->write("device_id,event_id,track_id,epoch_ms,source_epoch_ms,offset_applied_ms,time_quality,ocr_status,plate,plate_color,speed_kmh,speed_valid,direction,evidence_status\n");
        return {};
    }
    QString append(const QVector<VehicleEvent>& events)
    {
        if (!file_) return QStringLiteral("导出已取消");
        QTextStream out(file_.get());
        out.setEncoding(QStringConverter::Utf8);
        for (const auto& event : events) {
            const QStringList values {
                event.identity.deviceId, QString::number(event.identity.eventId), QString::number(event.identity.trackId),
                QString::number(event.eventTime.epochMs), QString::number(event.eventTime.sourceEpochMs),
                QString::number(event.eventTime.offsetAppliedMs), timeQualityText(event.eventTime.quality.value),
                ocrStatusText(event.ocrStatus), event.plateText, event.plateColor, QString::number(event.speedKmh),
                event.speedValid ? QStringLiteral("true") : QStringLiteral("false"),
                event.motionDirection, event.evidenceStatus
            };
            QStringList escaped;
            for (const auto& value : values) escaped.append(csvEscape(value));
            out << escaped.join(',') << '\n';
        }
        out.flush();
        return out.status() == QTextStream::Ok && file_->error() == QFileDevice::NoError
            ? QString() : file_->errorString();
    }
    QString finish()
    {
        if (!file_) return QStringLiteral("导出已取消");
        const bool ok = file_->commit();
        const QString error = ok ? QString() : file_->errorString();
        file_.reset();
        return error;
    }
    void cancel() { file_.reset(); }
private:
    std::unique_ptr<QSaveFile> file_;
};
EventViewController::EventViewController(EventViewDependencies dependencies, QObject* parent)
    : QObject(parent)
    , dependencies_(dependencies)
{
    changeTimer_.setSingleShot(true);
    changeTimer_.setInterval(100);
    connect(&changeTimer_, &QTimer::timeout, this, &EventViewController::flushEventChanges);
    if (dependencies_.evidenceCache) {
        connect(dependencies_.evidenceCache, &EvidenceCache::stateChanged,
                this, &EventViewController::evidenceChanged);
        connect(dependencies_.evidenceCache, &EvidenceCache::cacheError,
                this, [this](const EventIdentity&, const ApiError& error) {
                    reportError(error, QStringLiteral("图片缓存失败"));
                });
    }
}

EventViewController::~EventViewController()
{
    shutdown();
    exportThread_.quit();
    exportThread_.wait();
}

bool EventViewController::servicesAvailable() const
{
    return dependencies_.repository && dependencies_.evidenceCache;
}

bool EventViewController::isPaused() const { return paused_; }
int EventViewController::pendingChangeCount() const { return pendingChangeCount_; }

void EventViewController::attachSyncService(EventSyncService* service)
{
    if (!service || service->deviceId().trimmed().isEmpty()) {
        return;
    }
    const QString deviceId = service->deviceId();
    if (syncServices_.value(deviceId) == service) {
        return;
    }
    syncServices_.insert(deviceId, service);
    connect(service, &EventSyncService::eventChanged,
            this, &EventViewController::handleEventChanged);
    connect(service, &EventSyncService::initialCatchUpFinished, this,
            [this](const QString& healthyDeviceId) {
                emit syncHealthy(healthyDeviceId);
                if (!paused_) {
                    realtimeMode_ ? refreshRealtime(currentQuery_)
                                  : queryHistory(currentQuery_);
                }
            });
    connect(service, &EventSyncService::syncError, this,
            [this](const QString&, const ApiError& error) {
                reportError(error, QStringLiteral("事件同步失败"));
            });
    connect(service, &QObject::destroyed, this, [this, deviceId]() {
        syncServices_.remove(deviceId);
    });
    if (syncEnabled_ && sessionStates_.value(deviceId) == DeviceSessionState::Online && !service->isRunning()) {
        service->start();
    }
}

void EventViewController::setDeviceSession(const DeviceSessionSnapshot& snapshot)
{
    const QString deviceId = snapshot.profile.deviceId;
    sessionStates_.insert(deviceId, snapshot.state);
    EventSyncService* service = syncServices_.value(deviceId);
    if (!service) {
        return;
    }
    if (syncEnabled_ && snapshot.state == DeviceSessionState::Online) {
        if (!service->isRunning()) service->start();
    } else if (snapshot.state == DeviceSessionState::Disconnected
               || snapshot.state == DeviceSessionState::AuthenticationFailed) {
        stopDevice(deviceId);
    }
}

void EventViewController::setSyncEnabled(bool enabled)
{
    if (syncEnabled_ == enabled) return;
    syncEnabled_ = enabled;
    for (EventSyncService* service : std::as_const(syncServices_)) {
        if (!service) continue;
        if (!enabled) service->stop();
        else if (sessionStates_.value(service->deviceId()) == DeviceSessionState::Online)
            service->start();
    }
}

bool EventViewController::syncEnabled() const { return syncEnabled_; }

void EventViewController::refreshRealtime(const EventQuery& query)
{
    currentQuery_ = query;
    realtimeMode_ = true;
    queryHistory(query);
    realtimeMode_ = true;
}

void EventViewController::queryHistory(const EventQuery& query)
{
    if (!servicesAvailable() || shutdown_) {
        emit userError(QStringLiteral("event_services_unavailable"),
                       QStringLiteral("真实事件仓储和图片缓存服务尚未装配"));
        return;
    }
    if (query.startEpochMs && query.endEpochMs && *query.startEpochMs >= *query.endEpochMs) {
        emit userError(QStringLiteral("invalid_time_range"),
                       QStringLiteral("历史查询开始时间必须早于结束时间"));
        return;
    }
    currentQuery_ = query;
    realtimeMode_ = false;
    changeTimer_.stop();
    pendingEvents_.clear();
    const quint64 generation = ++queryGeneration_;
    if (!activeListRequest_.isNull()) {
        dependencies_.repository->cancel(activeListRequest_);
        activeRequests_.remove(activeListRequest_);
        activeListRequest_ = RequestId();
    }
    auto holder = QSharedPointer<RequestId>::create();
    auto completed = QSharedPointer<bool>::create(false);
    const RequestId id = dependencies_.repository->queryEvents(
        query, this,
        [this, holder, completed, generation](ApiResult<QVector<VehicleEvent>> result) {
            *completed = true;
            if (!holder->isNull()) activeRequests_.remove(*holder);
            if (activeListRequest_ == *holder) activeListRequest_ = RequestId();
            if (generation != queryGeneration_ || shutdown_) return;
            if (!result) {
                reportError(result.error(), QStringLiteral("读取本地事件失败"));
                return;
            }
            emit eventsReset(result.value());
            emit queryFinished(result.value().size());
            for (const VehicleEvent& event : result.value()) {
                if (event.evidenceAvailable) dependencies_.evidenceCache->enqueue(event);
            }
        });
    *holder = id;
    if (!*completed && !id.isNull()) {
        activeListRequest_ = id;
        activeRequests_.insert(id, query.deviceId.value_or(QString()));
    }
}

void EventViewController::setPaused(bool paused)
{
    if (paused_ == paused) return;
    paused_ = paused;
    if (!paused_) {
        pendingChangeCount_ = 0;
        emit pendingChangeCountChanged(0);
        if (realtimeMode_) {
            refreshRealtime(currentQuery_);
        } else {
            queryHistory(currentQuery_);
            realtimeMode_ = false;
        }
    }
}

void EventViewController::requestEvidence(const VehicleEvent& event)
{
    if (!servicesAvailable() || shutdown_) return;
    auto holder = QSharedPointer<RequestId>::create();
    auto completed = QSharedPointer<bool>::create(false);
    const RequestId id = dependencies_.repository->loadEvidenceState(
        event.identity, QStringLiteral("evidence"), this,
        [this, holder, completed, event](ApiResult<std::optional<EvidenceCacheEntry>> result) {
            *completed = true;
            if (!holder->isNull()) activeRequests_.remove(*holder);
            if (shutdown_) return;
            if (!result) {
                reportError(result.error(), QStringLiteral("读取图片缓存状态失败"));
                return;
            }
            if (result.value().has_value()
                && result.value()->status == EvidenceCacheStatus::Available
                && QFileInfo::exists(result.value()->localFilePath)
                && sameRemoteImageUrl(result.value()->remoteRelativeUrl, event.evidenceRelativeUrl)) {
                emit evidenceChanged(*result.value());
                return;
            }
            if (result.value().has_value()
                && result.value()->status != EvidenceCacheStatus::Available) {
                emit evidenceChanged(*result.value());
            }
            const bool shouldEnqueue = !result.value().has_value()
                || result.value()->status == EvidenceCacheStatus::NotRequested
                || result.value()->status == EvidenceCacheStatus::Missing
                || result.value()->status == EvidenceCacheStatus::Available;
            if (shouldEnqueue && event.evidenceAvailable && isDeviceOnline(event.identity.deviceId)) {
                dependencies_.evidenceCache->enqueue(event);
                EvidenceCacheEntry queued;
                queued.identity = event.identity;
                queued.remoteRelativeUrl = event.evidenceRelativeUrl;
                queued.status = EvidenceCacheStatus::Queued;
                emit evidenceChanged(queued);
            } else if (!result.value().has_value()
                       || result.value()->status == EvidenceCacheStatus::Available) {
                EvidenceCacheEntry missing;
                missing.identity = event.identity;
                missing.status = EvidenceCacheStatus::Missing;
                missing.failureCode = isDeviceOnline(event.identity.deviceId)
                    ? QStringLiteral("evidence_not_available")
                    : QStringLiteral("offline_not_cached");
                emit evidenceChanged(missing);
            }
        });
    *holder = id;
    if (!*completed && !id.isNull()) activeRequests_.insert(id, event.identity.deviceId);
}

void EventViewController::deleteLocalEvent(const VehicleEvent& event)
{
    if (!servicesAvailable() || shutdown_) return;
    dependencies_.evidenceCache->removeLocal(
        event, this, [this, event](ApiResult<void> cacheResult) {
            if (!cacheResult) {
                reportError(cacheResult.error(), QStringLiteral("删除本地图片失败，事件记录已保留"));
                emit deleteFinished(0, 1);
                return;
            }
            dependencies_.repository->deleteEvent(
                event.identity, this, [this, event](ApiResult<void> result) {
                    if (!result) {
                        reportError(result.error(), QStringLiteral("删除本地事件失败"));
                        emit deleteFinished(0, 1);
                        return;
                    }
                    pendingEvents_.remove(event.identity);
                    emit eventDeleted(event.identity);
                    emit deleteFinished(1, 0);
                });
        });
}

void EventViewController::clearLocalHistory(const EventQuery& query)
{
    if (!servicesAvailable() || shutdown_ || clearRunning_) return;
    if (query.startEpochMs && query.endEpochMs && *query.startEpochMs >= *query.endEpochMs) {
        emit userError(QStringLiteral("invalid_time_range"),
                       QStringLiteral("清空范围的开始时间必须早于结束时间"));
        return;
    }
    clearRunning_ = true;
    ++clearGeneration_;
    clearContext_ = new QObject(this);
    emit bulkProgress(QStringLiteral("clear"), 0);
    clearQuery_ = query;
    clearQuery_.limit = HistoryPageSize;
    clearQuery_.offset = 0;
    clearDeletedCount_ = 0;
    clearFailedCount_ = 0;
    clearSkippedCount_ = 0;
    clearPausedServices_.clear();
    for (EventSyncService* service : std::as_const(syncServices_)) {
        if (!service || !service->isRunning()) continue;
        if (query.deviceId && service->deviceId() != *query.deviceId) continue;
        service->stop();
        clearPausedServices_.append(service);
    }
    QTimer::singleShot(0, clearContext_, [this] { startClearBatch(); });
}

void EventViewController::startClearBatch()
{
    if (!clearRunning_ || shutdown_) return;
    const auto generation = clearGeneration_;
    clearQuery_.offset = clearSkippedCount_;
    dependencies_.repository->queryEvents(
        clearQuery_, clearContext_, [this, generation](ApiResult<QVector<VehicleEvent>> result) {
            if (generation != clearGeneration_ || !clearRunning_) return;
            if (!result) {
                reportError(result.error(), QStringLiteral("读取待清空事件失败"));
                ++clearFailedCount_;
                finishClear();
                return;
            }
            clearQueue_ = result.value();
            if (clearQueue_.isEmpty()) {
                finishClear();
                return;
            }
            QTimer::singleShot(0, clearContext_, [this] { deleteNextClearEvent(); });
        });
}

void EventViewController::finishClear()
{
    const bool cancelled = !clearContext_;
    clearRunning_ = false;
    delete clearContext_.data();
    if (!shutdown_) {
        for (EventSyncService* service : std::as_const(clearPausedServices_)) {
            if (service) service->start();
        }
    }
    clearPausedServices_.clear();
    emit bulkFinished(QStringLiteral("clear"), cancelled);
    emit deleteFinished(clearDeletedCount_, clearFailedCount_);
}

void EventViewController::deleteNextClearEvent()
{
    if (!clearRunning_ || shutdown_) return;
    const auto generation = clearGeneration_;
    if (clearQueue_.isEmpty()) {
        QTimer::singleShot(0, clearContext_, [this] { startClearBatch(); });
        return;
    }
    const VehicleEvent event = clearQueue_.takeFirst();
    dependencies_.evidenceCache->removeLocal(
        event, clearContext_, [this, event, generation](ApiResult<void> cacheResult) {
            if (generation != clearGeneration_ || !clearRunning_) return;
            if (!cacheResult) {
                ++clearFailedCount_;
                ++clearSkippedCount_;
                QTimer::singleShot(0, clearContext_, [this] { deleteNextClearEvent(); });
                return;
            }
            dependencies_.repository->deleteEvent(
                event.identity, clearContext_, [this, event, generation](ApiResult<void> result) {
                    if (generation != clearGeneration_ || !clearRunning_) return;
                    if (result) {
                        ++clearDeletedCount_;
                        pendingEvents_.remove(event.identity);
                        emit eventDeleted(event.identity);
                    } else {
                        ++clearFailedCount_;
                        ++clearSkippedCount_;
                    }
                    emit bulkProgress(QStringLiteral("clear"), clearDeletedCount_ + clearFailedCount_);
                    if (generation != clearGeneration_ || !clearRunning_) return;
                    QTimer::singleShot(0, clearContext_, [this] { deleteNextClearEvent(); });
                });
        });
}

void EventViewController::exportHistory(const EventQuery& query, const QString& filePath)
{
    if (!dependencies_.repository || shutdown_ || exportRunning_ || filePath.isEmpty()) return;
    if (query.startEpochMs && query.endEpochMs && *query.startEpochMs >= *query.endEpochMs) {
        emit userError(QStringLiteral("invalid_time_range"), QStringLiteral("导出范围的开始时间必须早于结束时间"));
        return;
    }
    if (!exportWriter_) {
        exportWriter_ = new CsvExportWriter;
        exportWriter_->moveToThread(&exportThread_);
        exportThread_.setObjectName(QStringLiteral("event-csv-export"));
        connect(&exportThread_, &QThread::finished, exportWriter_, &QObject::deleteLater);
        exportThread_.start();
    }
    exportRunning_ = true;
    const auto generation = ++exportGeneration_;
    exportContext_ = new QObject(this);
    exportQuery_ = query;
    exportQuery_.limit = HistoryPageSize;
    exportQuery_.offset = 0;
    exportRowCount_ = 0;
    exportPath_ = filePath;
    emit bulkProgress(QStringLiteral("export"), 0);
    QMetaObject::invokeMethod(exportWriter_, [this, filePath, generation] {
        const QString error = exportWriter_->begin(filePath);
        QMetaObject::invokeMethod(this, [this, error, generation] {
            if (generation != exportGeneration_ || !exportRunning_) return;
            if (!error.isEmpty()) {
                cancelExport();
                emit userError(QStringLiteral("event_export_failed"), error);
            } else startExportPage();
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void EventViewController::startExportPage()
{
    if (!exportRunning_ || shutdown_) return;
    const auto generation = exportGeneration_;
    dependencies_.repository->queryEvents(exportQuery_, exportContext_,
        [this, generation](ApiResult<QVector<VehicleEvent>> result) {
            if (generation != exportGeneration_ || !exportRunning_) return;
            if (!result) {
                cancelExport();
                reportError(result.error(), QStringLiteral("导出查询失败"));
                return;
            }
            const auto events = result.value();
            QMetaObject::invokeMethod(exportWriter_, [this, events, generation] {
                const QString error = exportWriter_->append(events);
                QMetaObject::invokeMethod(this, [this, error, count = events.size(), generation] {
                    if (generation != exportGeneration_ || !exportRunning_) return;
                    if (!error.isEmpty()) {
                        cancelExport();
                        emit userError(QStringLiteral("event_export_failed"), error);
                        return;
                    }
                    exportRowCount_ += count;
                    emit bulkProgress(QStringLiteral("export"), exportRowCount_);
                    if (generation != exportGeneration_ || !exportRunning_) return;
                    if (count < exportQuery_.limit) finishExport();
                    else {
                        exportQuery_.offset += count;
                        QTimer::singleShot(0, exportContext_, [this] { startExportPage(); });
                    }
                }, Qt::QueuedConnection);
            }, Qt::QueuedConnection);
        });
}

void EventViewController::finishExport()
{
    const auto generation = exportGeneration_;
    QMetaObject::invokeMethod(exportWriter_, [this, generation] {
        const QString error = exportWriter_->finish();
        QMetaObject::invokeMethod(this, [this, generation, error] {
            if (generation != exportGeneration_ || !exportRunning_) return;
            exportRunning_ = false;
            delete exportContext_.data();
            emit bulkFinished(QStringLiteral("export"), false);
            if (error.isEmpty()) emit exportFinished(exportPath_, exportRowCount_);
            else emit userError(QStringLiteral("event_export_failed"), error);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void EventViewController::cancelExport()
{
    if (!exportRunning_) return;
    ++exportGeneration_;
    exportRunning_ = false;
    delete exportContext_.data();
    if (exportWriter_) QMetaObject::invokeMethod(exportWriter_, [writer = exportWriter_] { writer->cancel(); }, Qt::QueuedConnection);
    emit bulkFinished(QStringLiteral("export"), true);
}

void EventViewController::cancelClear()
{
    if (!clearRunning_) return;
    ++clearGeneration_;
    delete clearContext_.data();
    clearQueue_.clear();
    finishClear();
}

void EventViewController::flushEventChanges()
{
    const auto events = pendingEvents_.values();
    pendingEvents_.clear();
    if (shutdown_ || paused_ || !realtimeMode_) return;
    emit eventsUpserted(events);
    for (const auto& event : events) emit eventUpserted(event);
}
void EventViewController::stopDevice(const QString& deviceId)
{
    if (EventSyncService* service = syncServices_.value(deviceId)) service->stop();
    cancelRequestsForDevice(deviceId);
    if (dependencies_.evidenceCache) dependencies_.evidenceCache->cancelDevice(deviceId);
}

void EventViewController::shutdown()
{
    if (shutdown_) return;
    shutdown_ = true;
    changeTimer_.stop();
    pendingEvents_.clear();
    cancelClear();
    cancelExport();
    ++queryGeneration_;
    for (EventSyncService* service : std::as_const(syncServices_)) {
        if (service) service->stop();
    }
    if (dependencies_.repository) {
        const QList<RequestId> ids = activeRequests_.keys();
        for (const RequestId& id : ids) dependencies_.repository->cancel(id);
        dependencies_.repository->cancelAll();
    }
    activeRequests_.clear();
    activeListRequest_ = RequestId();
    if (dependencies_.evidenceCache) dependencies_.evidenceCache->cancelAll();
}

void EventViewController::handleEventChanged(const EventIdentity& identity)
{
    loadChangedEvent(identity);
}

void EventViewController::loadChangedEvent(const EventIdentity& identity)
{
    if (!dependencies_.repository || shutdown_) return;
    auto holder = QSharedPointer<RequestId>::create();
    auto completed = QSharedPointer<bool>::create(false);
    const RequestId id = dependencies_.repository->loadEvent(
        identity, this, [this, holder, completed](ApiResult<std::optional<VehicleEvent>> result) {
            *completed = true;
            if (!holder->isNull()) activeRequests_.remove(*holder);
            if (shutdown_) return;
            if (!result) {
                reportError(result.error(), QStringLiteral("刷新事件失败"));
                return;
            }
            if (!result.value().has_value()) return;
            const VehicleEvent event = *result.value();
            emit syncHealthy(event.identity.deviceId);
            if (event.evidenceAvailable && dependencies_.evidenceCache) {
                dependencies_.evidenceCache->enqueue(event);
            }
            if (paused_) {
                ++pendingChangeCount_;
                emit pendingChangeCountChanged(pendingChangeCount_);
            } else if (realtimeMode_ && matchesCurrentQuery(event)) {
                pendingEvents_.insert(event.identity, event);
                if (!changeTimer_.isActive()) changeTimer_.start();
            }
        });
    *holder = id;
    if (!*completed && !id.isNull()) activeRequests_.insert(id, identity.deviceId);
}

bool EventViewController::matchesCurrentQuery(const VehicleEvent& event) const
{
    if (currentQuery_.deviceId.has_value() && event.identity.deviceId != *currentQuery_.deviceId) return false;
    if (currentQuery_.startEpochMs.has_value() && event.eventTime.epochMs < *currentQuery_.startEpochMs) return false;
    if (currentQuery_.endEpochMs.has_value() && event.eventTime.epochMs >= *currentQuery_.endEpochMs) return false;
    if (currentQuery_.plateText.has_value()
        && !event.plateText.contains(*currentQuery_.plateText, Qt::CaseInsensitive)) return false;
    return true;
}

bool EventViewController::isDeviceOnline(const QString& deviceId) const
{
    const DeviceSessionState state = sessionStates_.value(deviceId, DeviceSessionState::Disconnected);
    return state == DeviceSessionState::Online || state == DeviceSessionState::Degraded;
}

void EventViewController::reportError(const ApiError& error, const QString& fallback)
{
    QString message = fallback;
    if (error.category == ApiErrorCategory::Network || error.category == ApiErrorCategory::Temporary)
        message = QStringLiteral("%1：设备网络暂时不可用，本地历史仍可查看").arg(fallback);
    else if (error.category == ApiErrorCategory::Cancelled)
        message = QStringLiteral("操作已取消");
    else if (!error.message.isEmpty())
        message = QStringLiteral("%1：%2").arg(fallback, error.message);
    if (!error.code.isEmpty() && !message.contains(error.code)) {
        message = QStringLiteral("%1（%2）").arg(message, error.code);
    }
    emit userError(error.code.isEmpty() ? QStringLiteral("event_operation_failed") : error.code,
                   message);
}

void EventViewController::cancelRequestsForDevice(const QString& deviceId)
{
    if (!dependencies_.repository) return;
    const QList<RequestId> ids = activeRequests_.keys();
    for (const RequestId& id : ids) {
        if (activeRequests_.value(id) == deviceId) {
            dependencies_.repository->cancel(id);
            activeRequests_.remove(id);
        }
    }
}

} // namespace rv1126b
