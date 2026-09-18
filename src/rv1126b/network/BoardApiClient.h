#pragma once

#include "../domain/Models.h"
#include "../ports/IBoardApiClient.h"
#include "../ports/ISecretStore.h"
#include "../protocol/ApiCodec.h"

#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QTimer>

#include <utility>
#include <memory>

namespace rv1126b {
class FileDownloadExecutor;

class BoardApiClient final : public IBoardApiClient
{
    Q_OBJECT

public:
    static constexpr int JsonConnectTimeoutMs = 3000;
    static constexpr int JsonRequestTimeoutMs = 10000;
    static constexpr int ConfigApplyFirstResponseTimeoutMs = 15000;
    static constexpr int ConfigApplyRequestTimeoutMs = 25000;
    static constexpr int EvidenceRequestTimeoutMs = 30000;

    BoardApiClient(
        DeviceProfile profile,
        ISecretStore* secretStore,
        const IApiCodec* codec,
        QObject* parent = nullptr);
    ~BoardApiClient() override;

    DeviceProfile profile() const;
    void setProfile(DeviceProfile profile);

    RequestId getHealth(QObject* context, ApiCompletion<HealthDto> completion) override;
    RequestId getVideoStreamsConfig(QObject* context, ApiCompletion<VideoStreamsConfigDto> completion) override;
    RequestId putVideoStreamsConfig(const VideoStreamsUpdate& update, QObject* context,
                                   ApiCompletion<VideoStreamsConfigDto> completion) override;
    RequestId listEvents(int limit, const std::optional<QString>& cursor, QObject* context, ApiCompletion<EventPageDto> completion) override;
    RequestId getEventDetail(const EventIdentity& identity, QObject* context, ApiCompletion<EventDetailDto> completion) override;
    RequestId downloadEvidenceToPartFile(
        const EventIdentity& identity,
        const QString& evidenceRelativeUrl,
        const QString& partFilePath,
        QObject* context,
        ApiCompletion<EvidenceDownloadResult> completion) override;
    RequestId downloadFileToPartFile(
        const QString& relativeUrl,
        const QString& partFilePath,
        QObject* context,
        ApiCompletion<EvidenceDownloadResult> completion) override;
    RequestId putClientAck(
        const EventIdentity& identity,
        const ClientAckCreate& request,
        QObject* context,
        ApiCompletion<ClientAckDto> completion) override;
    RequestId getEvidenceConfig(QObject* context, ApiCompletion<EvidenceConfigDto> completion) override;
    RequestId putEvidenceConfig(const EvidenceConfigUpdate& update, QObject* context, ApiCompletion<EvidenceConfigDto> completion) override;
    RequestId getTime(QObject* context, ApiCompletion<TimeStatusDto> completion) override;
    RequestId putTime(const TimeUpdate& update, QObject* context, ApiCompletion<TimeStatusDto> completion) override;
    RequestId getTriggerModeConfig(QObject* context, ApiCompletion<TriggerModeConfigDto> completion) override;
    RequestId putTriggerModeConfig(const TriggerModeUpdate& update, QObject* context, ApiCompletion<TriggerModeConfigDto> completion) override;
    RequestId getLineRegionConfig(QObject* context, ApiCompletion<LineRegionConfigDto> completion) override;
    RequestId putLineRegionConfig(const LineRegionUpdate& update, QObject* context, ApiCompletion<LineRegionConfigDto> completion) override;
    RequestId applyRuntimeConfig(const RuntimeApplyUpdate& update, QObject* context, ApiCompletion<RuntimeApplyDto> completion) override;
    RequestId getIspConfig(QObject* context, ApiCompletion<QJsonObject> completion) override;
    RequestId saveCurrentIspConfig(QObject* context, ApiCompletion<QJsonObject> completion) override;
    RequestId clearIspConfig(QObject* context, ApiCompletion<QJsonObject> completion) override;
    RequestId getFtpConfig(QObject* context, ApiCompletion<FtpConfigSnapshotDto> completion) override;
    RequestId putFtpConfig(const FtpConfigUpdate& update, QObject* context, ApiCompletion<FtpConfigSnapshotDto> completion) override;
    RequestId rollbackFtpConfig(const QString& expectedRevision, QObject* context, ApiCompletion<FtpConfigSnapshotDto> completion) override;
    RequestId getFtpControl(QObject* context, ApiCompletion<FtpControlDto> completion) override;
    RequestId putFtpControl(const FtpControlUpdate& update, QObject* context, ApiCompletion<FtpControlDto> completion) override;
    RequestId createFtpTask(const FtpTaskCreate& request, QObject* context, ApiCompletion<FtpTaskDetailDto> completion) override;
    RequestId listFtpTasks(int limit, const std::optional<QString>& cursor, QObject* context, ApiCompletion<FtpTaskPageDto> completion) override;
    RequestId getFtpTask(const QString& taskId, QObject* context, ApiCompletion<FtpTaskDetailDto> completion) override;
    RequestId retryFtpTask(const QString& taskId, QObject* context, ApiCompletion<FtpTaskDetailDto> completion) override;

    void cancel(const RequestId& requestId) override;
    void cancelAll() override;

private:
    struct PendingRequest {
        QPointer<QNetworkReply> reply;
        QPointer<QTimer> connectTimer;
        std::function<void(const ApiError&)> fail;
        std::function<void(const QByteArray&)> succeedJson;
        std::function<void(const EvidenceDownloadResult&)> succeedEvidence;
        QMetaObject::Connection contextDestroyed;
        bool fileDownload = false;
    };

    RequestId startJsonRequest(
        QNetworkAccessManager::Operation operation,
        const QUrl& url,
        const QByteArray& body,
        QObject* context,
        ApiCompletion<QByteArray> completion,
        int firstResponseTimeoutMs = JsonConnectTimeoutMs,
        int transferTimeoutMs = JsonRequestTimeoutMs);
    RequestId startEvidenceRequest(
        const QUrl& url,
        const QString& partFilePath,
        QObject* context,
        ApiCompletion<EvidenceDownloadResult> completion);
    RequestId startFileRequest(
        const QUrl& url,
        const QString& partFilePath,
        const QByteArray& acceptHeader,
        const QString& requiredContentTypePrefix,
        QObject* context,
        ApiCompletion<EvidenceDownloadResult> completion);
    QUrl endpointUrl(const QString& relativePath, ApiError* error) const;
    QUrl eventUrl(const EventIdentity& identity, const QString& suffix, ApiError* error) const;
    bool applyAuthorization(QNetworkRequest* request, ApiError* error) const;
    QNetworkReply* issueRequest(QNetworkAccessManager::Operation operation, const QNetworkRequest& request, const QByteArray& body);
    void completeJsonRequest(const RequestId& requestId);
    void watchContext(const RequestId& requestId, QObject* context);
    void stopConnectTimer(const RequestId& requestId);
    void failPending(const RequestId& requestId, const ApiError& error, bool abortReply);
    ApiError localError(const QString& code, const QString& message, ApiErrorCategory category, bool retryable = false) const;
    ApiError networkError(QNetworkReply* reply) const;

    template<typename T>
    static void queueCompletion(QObject* context, ApiCompletion<T> completion, ApiResult<T> result)
    {
        if (!context) {
            return;
        }
        QPointer<QObject> guardedContext(context);
        QMetaObject::invokeMethod(context, [guardedContext, completion = std::move(completion), result = std::move(result)]() mutable {
            if (guardedContext) {
                completion(std::move(result));
            }
        }, Qt::QueuedConnection);
    }

    template<typename T, typename Parser>
    RequestId startDecodedJson(
        QNetworkAccessManager::Operation operation,
        const QUrl& url,
        const QByteArray& body,
        QObject* context,
        ApiCompletion<T> completion,
        Parser parser,
        int firstResponseTimeoutMs = JsonConnectTimeoutMs,
        int transferTimeoutMs = JsonRequestTimeoutMs)
    {
        return startJsonRequest(operation, url, body, context,
            [guard = QPointer<BoardApiClient>(this), completion = std::move(completion), parser = std::move(parser)](ApiResult<QByteArray> result) mutable {
                if (!guard) return;
                if (!result.isSuccess()) {
                    completion(ApiResult<T>::failure(result.error()));
                    return;
                }
                completion(parser(result.value()));
            },
            firstResponseTimeoutMs,
            transferTimeoutMs);
    }

    template<typename T>
    RequestId returnTypedError(QObject* context, ApiCompletion<T> completion, const ApiError& error)
    {
        const RequestId requestId = RequestId::createUuid();
        queueCompletion(context, std::move(completion), ApiResult<T>::failure(error));
        return requestId;
    }

    DeviceProfile profile_;
    ISecretStore* secretStore_ = nullptr;
    const IApiCodec* codec_ = nullptr;
    QNetworkAccessManager networkAccessManager_;
    QHash<RequestId, PendingRequest> pendingRequests_;
    std::unique_ptr<FileDownloadExecutor> fileDownloads_;
};

} // namespace rv1126b
