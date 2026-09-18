#pragma once

#include "EvidenceCache.h"
#include "CacheFileLease.h"

#include "../ports/IBoardApiClient.h"
#include "../ports/IEventRepository.h"

#include <QHash>
#include <QPointer>
#include <QSet>
#include <QVector>
#include <QTimer>

#include <functional>

namespace rv1126b {

class BoardEvidenceCache final : public EvidenceCache
{
    Q_OBJECT

public:
    using ApiClientResolver = std::function<IBoardApiClient*(const QString& deviceId)>;

    BoardEvidenceCache(
        IBoardApiClient* apiClient,
        IEventRepository* repository,
        QObject* parent = nullptr);
    BoardEvidenceCache(
        IBoardApiClient* apiClient,
        IEventRepository* repository,
        QString cacheRootPath,
        QObject* parent = nullptr);
    BoardEvidenceCache(
        ApiClientResolver apiClientResolver,
        IEventRepository* repository,
        QString cacheRootPath,
        QObject* parent = nullptr);

    void enqueue(const VehicleEvent& event) override;
    RequestId removeLocal(
        const VehicleEvent& event,
        QObject* context,
        ApiCompletion<void> completion) override;
    void cancel(const EventIdentity& identity) override;
    void cancelDevice(const QString& deviceId) override;
    void cancelAll() override;
    QString finalPathFor(const VehicleEvent& event) const override;
    bool setCacheRootPath(const QString& cacheRootPath);
    static constexpr int MaxQueuedDownloads = 256;

private:
    struct ActiveDownload {
        VehicleEvent event;
        EvidenceCacheEntry entry;
        QString partFilePath;
        RequestId requestId;
        QPointer<IBoardApiClient> apiClient;
        quint64 generation = 0;
        std::shared_ptr<CacheFileLease> lease;
    };

    void drainQueue();
    void enqueueAfterLookup(const VehicleEvent& event, quint64 generation,
                            ApiResult<std::optional<EvidenceCacheEntry>> result);
    void refillQueue();
    bool canStart(const VehicleEvent& event) const;
    void startDownload(const VehicleEvent& event);
    void handleDownloadFinished(const EventIdentity& identity, quint64 generation, ApiResult<EvidenceDownloadResult> result);
    void finishActive(const EventIdentity& identity);
    void scheduleRetry(const VehicleEvent& event);
    void cancelRetry(const EventIdentity& identity);
    void cancelRetriesForDevice(const QString& deviceId);
    int nextRetryDelayMs(const EventIdentity& identity);
    void persistState(const EvidenceCacheEntry& entry);
    void persistFailure(const VehicleEvent& event, EvidenceCacheStatus status, const ApiError& error);
    EvidenceCacheEntry entryFor(const VehicleEvent& event, EvidenceCacheStatus status) const;
    QString partPathFor(const VehicleEvent& event) const;
    QString eventFileStem(const VehicleEvent& event) const;
    QString sanitizePathSegment(const QString& value) const;
    bool ensureCacheDirectory(const VehicleEvent& event, ApiError* error) const;
    bool validateDownloadedFile(
        const EvidenceDownloadResult& result,
        const QString& finalPath,
        ApiError* error) const;
    bool promotePartFile(const QString& partFilePath, const QString& finalPath, ApiError* error) const;
    ApiError makeCacheError(const QString& code, const QString& message, bool retryable = false) const;
    EvidenceCacheStatus statusForError(const ApiError& error) const;

    ApiClientResolver apiClientResolver_;
    IEventRepository* repository_ = nullptr;
    QString cacheRootPath_;
    QVector<VehicleEvent> queue_;
    QSet<EventIdentity> queuedIdentities_;
    QHash<EventIdentity, quint64> scheduledRetries_;
    QHash<EventIdentity, ActiveDownload> active_;
    QHash<QString, int> activePerDevice_;
    QHash<EventIdentity, int> retryAttempts_;
    QHash<EventIdentity, quint64> lookups_;
    QHash<EventIdentity, quint64> stateNotifications_;
    QSet<QString> suspendedDevices_;
    QSet<QString> eligibleDevices_;
    QTimer refillTimer_;
    quint64 nextGeneration_ = 0;
    quint64 refillGeneration_ = 0;
    bool refillPending_ = false;
    bool stopped_ = false;
};

} // namespace rv1126b
