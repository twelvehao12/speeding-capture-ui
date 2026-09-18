#include "BoardApiClient.h"
#include "FileDownloadExecutor.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QUrlQuery>

#include <memory>

namespace rv1126b {
namespace {

constexpr auto ApiPrefix = "/api/v1";

bool isSuccessfulStatus(int status)
{
    return status >= 200 && status < 300;
}

bool sameOrigin(const QUrl& left, const QUrl& right)
{
    return left.scheme() == right.scheme()
        && left.host().compare(right.host(), Qt::CaseInsensitive) == 0
        && left.port(left.scheme() == QStringLiteral("http") ? 80 : -1)
            == right.port(right.scheme() == QStringLiteral("http") ? 80 : -1);
}

QString contentType(QNetworkReply* reply)
{
    return reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
}

ApiResult<QJsonObject> parseJsonObjectPayload(const QByteArray& payload)
{
    QJsonParseError parseError {};
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        ApiError error;
        error.code = QStringLiteral("invalid_json");
        error.message = QStringLiteral("Board returned invalid JSON.");
        error.category = ApiErrorCategory::Protocol;
        return ApiResult<QJsonObject>::failure(std::move(error));
    }
    return ApiResult<QJsonObject>::success(document.object());
}

} // namespace

BoardApiClient::BoardApiClient(
    DeviceProfile profile,
    ISecretStore* secretStore,
    const IApiCodec* codec,
    QObject* parent)
    : IBoardApiClient(parent)
    , profile_(std::move(profile))
    , secretStore_(secretStore)
    , codec_(codec)
    , networkAccessManager_(this)
{
}

DeviceProfile BoardApiClient::profile() const
{
    return profile_;
}

void BoardApiClient::setProfile(DeviceProfile profile)
{
    if (profile.endpoint.apiBaseUrl != profile_.endpoint.apiBaseUrl
        || profile.credentialRef != profile_.credentialRef || profile.deviceId != profile_.deviceId)
        cancelAll();
    profile_ = std::move(profile);
}

BoardApiClient::~BoardApiClient()
{
    cancelAll();
    fileDownloads_.reset();
}

RequestId BoardApiClient::getHealth(QObject* context, ApiCompletion<HealthDto> completion)
{
    ApiError error;
    const QUrl url = endpointUrl(QStringLiteral("/api/v1/health"), &error);
    if (!error.code.isEmpty()) return returnTypedError(context, std::move(completion), error);
    return startDecodedJson(QNetworkAccessManager::GetOperation, url, {}, context, std::move(completion),
        [this](const QByteArray& payload) { return codec_->parseHealth(payload); });
}

RequestId BoardApiClient::listEvents(
    int limit,
    const std::optional<QString>& cursor,
    QObject* context,
    ApiCompletion<EventPageDto> completion)
{
    if (limit < 1 || limit > 100) {
        return returnTypedError(context, std::move(completion), localError(
            QStringLiteral("rv1126b.api.invalid_limit"), QStringLiteral("Event page limit must be between 1 and 100."), ApiErrorCategory::Validation));
    }
    ApiError error;
    QUrl url = endpointUrl(QStringLiteral("/api/v1/events"), &error);
    if (!error.code.isEmpty()) return returnTypedError(context, std::move(completion), error);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("limit"), QString::number(limit));
    if (cursor) query.addQueryItem(QStringLiteral("cursor"), *cursor);
    url.setQuery(query);
    return startDecodedJson(QNetworkAccessManager::GetOperation, url, {}, context, std::move(completion),
        [this](const QByteArray& payload) { return codec_->parseEventPage(payload); });
}

RequestId BoardApiClient::getEventDetail(
    const EventIdentity& identity,
    QObject* context,
    ApiCompletion<EventDetailDto> completion)
{
    ApiError error;
    const QUrl url = eventUrl(identity, QString(), &error);
    if (!error.code.isEmpty()) return returnTypedError(context, std::move(completion), error);
    return startDecodedJson(QNetworkAccessManager::GetOperation, url, {}, context, std::move(completion),
        [this](const QByteArray& payload) { return codec_->parseEventDetail(payload); });
}

RequestId BoardApiClient::downloadEvidenceToPartFile(
    const EventIdentity& identity,
    const QString& evidenceRelativeUrl,
    const QString& partFilePath,
    QObject* context,
    ApiCompletion<EvidenceDownloadResult> completion)
{
    if (partFilePath.isEmpty()) {
        return returnTypedError(context, std::move(completion), localError(
            QStringLiteral("rv1126b.api.invalid_part_path"), QStringLiteral("Evidence part path is required."), ApiErrorCategory::Validation));
    }
    ApiError error;
    const QUrl url = evidenceRelativeUrl.isEmpty()
        ? eventUrl(identity, QStringLiteral("/images/evidence"), &error)
        : endpointUrl(evidenceRelativeUrl, &error);
    if (!error.code.isEmpty()) return returnTypedError(context, std::move(completion), error);
    return startEvidenceRequest(url, partFilePath, context, std::move(completion));
}

RequestId BoardApiClient::downloadFileToPartFile(
    const QString& relativeUrl,
    const QString& partFilePath,
    QObject* context,
    ApiCompletion<EvidenceDownloadResult> completion)
{
    if (partFilePath.isEmpty()) {
        return returnTypedError(context, std::move(completion), localError(
            QStringLiteral("rv1126b.api.invalid_part_path"), QStringLiteral("Download part path is required."), ApiErrorCategory::Validation));
    }
    ApiError error;
    const QUrl url = endpointUrl(relativeUrl, &error);
    if (!error.code.isEmpty()) return returnTypedError(context, std::move(completion), error);
    return startFileRequest(
        url,
        partFilePath,
        QByteArrayLiteral("*/*"),
        QString(),
        context,
        std::move(completion));
}

RequestId BoardApiClient::putClientAck(
    const EventIdentity& identity,
    const ClientAckCreate& request,
    QObject* context,
    ApiCompletion<ClientAckDto> completion)
{
    if (!codec_) return returnTypedError(context, std::move(completion), localError(
        QStringLiteral("rv1126b.api.invalid_dependencies"), QStringLiteral("Missing API codec."), ApiErrorCategory::Validation));
    const auto body = codec_->encodeClientAck(request);
    if (!body.isSuccess()) return returnTypedError(context, std::move(completion), body.error());
    ApiError error;
    const QUrl url = eventUrl(identity, QStringLiteral("/ack"), &error);
    if (!error.code.isEmpty()) return returnTypedError(context, std::move(completion), error);
    return startDecodedJson(QNetworkAccessManager::PutOperation, url, body.value(), context, std::move(completion),
        [this](const QByteArray& payload) { return codec_->parseClientAck(payload); });
}

RequestId BoardApiClient::getVideoStreamsConfig(QObject* context, ApiCompletion<VideoStreamsConfigDto> completion)
{
    ApiError error;
    const QUrl url = endpointUrl(QStringLiteral("/api/v1/config/video-streams"), &error);
    if (!error.code.isEmpty()) return returnTypedError(context, std::move(completion), error);
    return startDecodedJson(QNetworkAccessManager::GetOperation, url, {}, context, std::move(completion),
        [this](const QByteArray& payload) { return codec_->parseVideoStreamsConfig(payload); });
}

RequestId BoardApiClient::putVideoStreamsConfig(const VideoStreamsUpdate& update, QObject* context,
                                             ApiCompletion<VideoStreamsConfigDto> completion)
{
    if (!codec_) return returnTypedError(context, std::move(completion), localError(
        QStringLiteral("rv1126b.api.invalid_dependencies"), QStringLiteral("Missing API codec."), ApiErrorCategory::Validation));
    const auto body = codec_->encodeVideoStreamsConfig(update);
    if (!body) return returnTypedError(context, std::move(completion), body.error());
    ApiError error;
    const QUrl url = endpointUrl(QStringLiteral("/api/v1/config/video-streams"), &error);
    if (!error.code.isEmpty()) return returnTypedError(context, std::move(completion), error);
    return startDecodedJson(QNetworkAccessManager::PutOperation, url, body.value(), context, std::move(completion),
        [this](const QByteArray& payload) { return codec_->parseVideoStreamsConfig(payload); });
}

RequestId BoardApiClient::getEvidenceConfig(QObject* context, ApiCompletion<EvidenceConfigDto> completion)
{
    ApiError error;
    const QUrl url = endpointUrl(QStringLiteral("/api/v1/config/evidence"), &error);
    if (!error.code.isEmpty()) return returnTypedError(context, std::move(completion), error);
    return startDecodedJson(QNetworkAccessManager::GetOperation, url, {}, context, std::move(completion),
        [this](const QByteArray& payload) { return codec_->parseEvidenceConfig(payload); });
}

RequestId BoardApiClient::putEvidenceConfig(
    const EvidenceConfigUpdate& update,
    QObject* context,
    ApiCompletion<EvidenceConfigDto> completion)
{
    if (!codec_) return returnTypedError(context, std::move(completion), localError(
        QStringLiteral("rv1126b.api.invalid_dependencies"), QStringLiteral("Missing API codec."), ApiErrorCategory::Validation));
    const auto body = codec_->encodeEvidenceConfig(update);
    if (!body.isSuccess()) return returnTypedError(context, std::move(completion), body.error());
    ApiError error;
    const QUrl url = endpointUrl(QStringLiteral("/api/v1/config/evidence"), &error);
    if (!error.code.isEmpty()) return returnTypedError(context, std::move(completion), error);
    return startDecodedJson(QNetworkAccessManager::PutOperation, url, body.value(), context, std::move(completion),
        [this](const QByteArray& payload) { return codec_->parseEvidenceConfig(payload); });
}

RequestId BoardApiClient::getTime(QObject* context, ApiCompletion<TimeStatusDto> completion)
{
    ApiError error;
    const QUrl url = endpointUrl(QStringLiteral("/api/v1/time"), &error);
    if (!error.code.isEmpty()) return returnTypedError(context, std::move(completion), error);
    return startDecodedJson(QNetworkAccessManager::GetOperation, url, {}, context, std::move(completion),
        [this](const QByteArray& payload) { return codec_->parseTimeStatus(payload); });
}

RequestId BoardApiClient::putTime(const TimeUpdate& update, QObject* context, ApiCompletion<TimeStatusDto> completion)
{
    if (!codec_) return returnTypedError(context, std::move(completion), localError(
        QStringLiteral("rv1126b.api.invalid_dependencies"), QStringLiteral("Missing API codec."), ApiErrorCategory::Validation));
    const auto body = codec_->encodeTimeUpdate(update);
    if (!body.isSuccess()) return returnTypedError(context, std::move(completion), body.error());
    ApiError error;
    const QUrl url = endpointUrl(QStringLiteral("/api/v1/time"), &error);
    if (!error.code.isEmpty()) return returnTypedError(context, std::move(completion), error);
    return startDecodedJson(QNetworkAccessManager::PutOperation, url, body.value(), context, std::move(completion),
        [this](const QByteArray& payload) { return codec_->parseTimeStatus(payload); });
}

RequestId BoardApiClient::getTriggerModeConfig(QObject* context, ApiCompletion<TriggerModeConfigDto> completion)
{
    ApiError error;
    const QUrl url = endpointUrl(QStringLiteral("/api/v1/config/trigger-mode"), &error);
    if (!error.code.isEmpty()) return returnTypedError(context, std::move(completion), error);
    return startDecodedJson(QNetworkAccessManager::GetOperation, url, {}, context, std::move(completion),
        [this](const QByteArray& payload) { return codec_->parseTriggerModeConfig(payload); });
}

RequestId BoardApiClient::putTriggerModeConfig(
    const TriggerModeUpdate& update,
    QObject* context,
    ApiCompletion<TriggerModeConfigDto> completion)
{
    if (!codec_) return returnTypedError(context, std::move(completion), localError(
        QStringLiteral("rv1126b.api.invalid_dependencies"), QStringLiteral("Missing API codec."), ApiErrorCategory::Validation));
    const auto body = codec_->encodeTriggerModeConfig(update);
    if (!body.isSuccess()) return returnTypedError(context, std::move(completion), body.error());
    ApiError error;
    const QUrl url = endpointUrl(QStringLiteral("/api/v1/config/trigger-mode"), &error);
    if (!error.code.isEmpty()) return returnTypedError(context, std::move(completion), error);
    return startDecodedJson(QNetworkAccessManager::PutOperation, url, body.value(), context, std::move(completion),
        [this](const QByteArray& payload) { return codec_->parseTriggerModeConfig(payload); },
        ConfigApplyFirstResponseTimeoutMs,
        ConfigApplyRequestTimeoutMs);
}

RequestId BoardApiClient::getLineRegionConfig(QObject* context, ApiCompletion<LineRegionConfigDto> completion)
{
    ApiError error;
    const QUrl url = endpointUrl(QStringLiteral("/api/v1/config/line-region"), &error);
    if (!error.code.isEmpty()) return returnTypedError(context, std::move(completion), error);
    return startDecodedJson(QNetworkAccessManager::GetOperation, url, {}, context, std::move(completion),
        [this](const QByteArray& payload) { return codec_->parseLineRegionConfig(payload); });
}

RequestId BoardApiClient::putLineRegionConfig(
    const LineRegionUpdate& update,
    QObject* context,
    ApiCompletion<LineRegionConfigDto> completion)
{
    if (!codec_) return returnTypedError(context, std::move(completion), localError(
        QStringLiteral("rv1126b.api.invalid_dependencies"), QStringLiteral("Missing API codec."), ApiErrorCategory::Validation));
    const auto body = codec_->encodeLineRegionConfig(update);
    if (!body.isSuccess()) return returnTypedError(context, std::move(completion), body.error());
    ApiError error;
    const QUrl url = endpointUrl(QStringLiteral("/api/v1/config/line-region"), &error);
    if (!error.code.isEmpty()) return returnTypedError(context, std::move(completion), error);
    return startDecodedJson(QNetworkAccessManager::PutOperation, url, body.value(), context, std::move(completion),
        [this](const QByteArray& payload) { return codec_->parseLineRegionConfig(payload); },
        ConfigApplyFirstResponseTimeoutMs,
        ConfigApplyRequestTimeoutMs);
}

RequestId BoardApiClient::applyRuntimeConfig(
    const RuntimeApplyUpdate& update,
    QObject* context,
    ApiCompletion<RuntimeApplyDto> completion)
{
    if (!codec_) return returnTypedError(context, std::move(completion), localError(
        QStringLiteral("rv1126b.api.invalid_dependencies"), QStringLiteral("Missing API codec."), ApiErrorCategory::Validation));
    const auto body = codec_->encodeRuntimeApply(update);
    if (!body.isSuccess()) return returnTypedError(context, std::move(completion), body.error());
    ApiError error;
    const QUrl url = endpointUrl(QStringLiteral("/api/v1/config/runtime/apply"), &error);
    if (!error.code.isEmpty()) return returnTypedError(context, std::move(completion), error);
    return startDecodedJson(QNetworkAccessManager::PostOperation, url, body.value(), context, std::move(completion),
        [this](const QByteArray& payload) { return codec_->parseRuntimeApply(payload); },
        ConfigApplyFirstResponseTimeoutMs,
        ConfigApplyRequestTimeoutMs);
}

RequestId BoardApiClient::getIspConfig(QObject* context, ApiCompletion<QJsonObject> completion)
{
    ApiError error;
    const QUrl url = endpointUrl(QStringLiteral("/api/v1/config/isp"), &error);
    if (!error.code.isEmpty()) return returnTypedError(context, std::move(completion), error);
    return startDecodedJson(QNetworkAccessManager::GetOperation, url, {}, context, std::move(completion),
        [](const QByteArray& payload) { return parseJsonObjectPayload(payload); });
}

RequestId BoardApiClient::saveCurrentIspConfig(QObject* context, ApiCompletion<QJsonObject> completion)
{
    ApiError error;
    const QUrl url = endpointUrl(QStringLiteral("/api/v1/config/isp/save-current"), &error);
    if (!error.code.isEmpty()) return returnTypedError(context, std::move(completion), error);
    return startDecodedJson(QNetworkAccessManager::PostOperation, url, {}, context, std::move(completion),
        [](const QByteArray& payload) { return parseJsonObjectPayload(payload); },
        ConfigApplyFirstResponseTimeoutMs,
        ConfigApplyRequestTimeoutMs);
}

RequestId BoardApiClient::clearIspConfig(QObject* context, ApiCompletion<QJsonObject> completion)
{
    ApiError error;
    const QUrl url = endpointUrl(QStringLiteral("/api/v1/config/isp/clear"), &error);
    if (!error.code.isEmpty()) return returnTypedError(context, std::move(completion), error);
    return startDecodedJson(QNetworkAccessManager::PostOperation, url, {}, context, std::move(completion),
        [](const QByteArray& payload) { return parseJsonObjectPayload(payload); },
        ConfigApplyFirstResponseTimeoutMs,
        ConfigApplyRequestTimeoutMs);
}

RequestId BoardApiClient::getFtpConfig(QObject* context, ApiCompletion<FtpConfigSnapshotDto> completion)
{
    ApiError error;
    const QUrl url = endpointUrl(QStringLiteral("/api/v1/ftp/config"), &error);
    if (!error.code.isEmpty()) return returnTypedError(context, std::move(completion), error);
    return startDecodedJson(QNetworkAccessManager::GetOperation, url, {}, context, std::move(completion),
        [this](const QByteArray& payload) { return codec_->parseFtpConfig(payload); });
}

RequestId BoardApiClient::putFtpConfig(
    const FtpConfigUpdate& update,
    QObject* context,
    ApiCompletion<FtpConfigSnapshotDto> completion)
{
    if (!codec_) return returnTypedError(context, std::move(completion), localError(
        QStringLiteral("rv1126b.api.invalid_dependencies"), QStringLiteral("Missing API codec."), ApiErrorCategory::Validation));
    const auto body = codec_->encodeFtpConfig(update);
    if (!body.isSuccess()) return returnTypedError(context, std::move(completion), body.error());
    ApiError error;
    const QUrl url = endpointUrl(QStringLiteral("/api/v1/ftp/config"), &error);
    if (!error.code.isEmpty()) return returnTypedError(context, std::move(completion), error);
    return startDecodedJson(QNetworkAccessManager::PutOperation, url, body.value(), context, std::move(completion),
        [this](const QByteArray& payload) { return codec_->parseFtpConfig(payload); });
}

RequestId BoardApiClient::rollbackFtpConfig(
    const QString& expectedRevision,
    QObject* context,
    ApiCompletion<FtpConfigSnapshotDto> completion)
{
    if (!codec_) return returnTypedError(context, std::move(completion), localError(
        QStringLiteral("rv1126b.api.invalid_dependencies"), QStringLiteral("Missing API codec."), ApiErrorCategory::Validation));
    const auto body = codec_->encodeExpectedRevision(expectedRevision);
    if (!body.isSuccess()) return returnTypedError(context, std::move(completion), body.error());
    ApiError error;
    const QUrl url = endpointUrl(QStringLiteral("/api/v1/ftp/config/rollback"), &error);
    if (!error.code.isEmpty()) return returnTypedError(context, std::move(completion), error);
    return startDecodedJson(QNetworkAccessManager::PostOperation, url, body.value(), context, std::move(completion),
        [this](const QByteArray& payload) { return codec_->parseFtpConfig(payload); });
}

RequestId BoardApiClient::getFtpControl(QObject* context, ApiCompletion<FtpControlDto> completion)
{
    ApiError error;
    const QUrl url = endpointUrl(QStringLiteral("/api/v1/ftp/control"), &error);
    if (!error.code.isEmpty()) return returnTypedError(context, std::move(completion), error);
    return startDecodedJson(QNetworkAccessManager::GetOperation, url, {}, context, std::move(completion),
        [this](const QByteArray& payload) { return codec_->parseFtpControl(payload); });
}

RequestId BoardApiClient::putFtpControl(
    const FtpControlUpdate& update,
    QObject* context,
    ApiCompletion<FtpControlDto> completion)
{
    if (!codec_) return returnTypedError(context, std::move(completion), localError(
        QStringLiteral("rv1126b.api.invalid_dependencies"), QStringLiteral("Missing API codec."), ApiErrorCategory::Validation));
    const auto body = codec_->encodeFtpControl(update);
    if (!body.isSuccess()) return returnTypedError(context, std::move(completion), body.error());
    ApiError error;
    const QUrl url = endpointUrl(QStringLiteral("/api/v1/ftp/control"), &error);
    if (!error.code.isEmpty()) return returnTypedError(context, std::move(completion), error);
    return startDecodedJson(QNetworkAccessManager::PutOperation, url, body.value(), context, std::move(completion),
        [this](const QByteArray& payload) { return codec_->parseFtpControl(payload); });
}

RequestId BoardApiClient::createFtpTask(
    const FtpTaskCreate& request,
    QObject* context,
    ApiCompletion<FtpTaskDetailDto> completion)
{
    if (!codec_) return returnTypedError(context, std::move(completion), localError(
        QStringLiteral("rv1126b.api.invalid_dependencies"), QStringLiteral("Missing API codec."), ApiErrorCategory::Validation));
    const auto body = codec_->encodeFtpTaskCreate(request);
    if (!body.isSuccess()) return returnTypedError(context, std::move(completion), body.error());
    ApiError error;
    const QUrl url = endpointUrl(QStringLiteral("/api/v1/ftp/tasks"), &error);
    if (!error.code.isEmpty()) return returnTypedError(context, std::move(completion), error);
    return startDecodedJson(QNetworkAccessManager::PostOperation, url, body.value(), context, std::move(completion),
        [this](const QByteArray& payload) { return codec_->parseFtpTaskDetail(payload); });
}

RequestId BoardApiClient::listFtpTasks(
    int limit,
    const std::optional<QString>& cursor,
    QObject* context,
    ApiCompletion<FtpTaskPageDto> completion)
{
    if (limit < 1 || limit > 100) {
        return returnTypedError(context, std::move(completion), localError(
            QStringLiteral("rv1126b.api.invalid_limit"), QStringLiteral("FTP task page limit must be between 1 and 100."), ApiErrorCategory::Validation));
    }
    ApiError error;
    QUrl url = endpointUrl(QStringLiteral("/api/v1/ftp/tasks"), &error);
    if (!error.code.isEmpty()) return returnTypedError(context, std::move(completion), error);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("limit"), QString::number(limit));
    if (cursor) query.addQueryItem(QStringLiteral("cursor"), *cursor);
    url.setQuery(query);
    return startDecodedJson(QNetworkAccessManager::GetOperation, url, {}, context, std::move(completion),
        [this](const QByteArray& payload) { return codec_->parseFtpTaskPage(payload); });
}

RequestId BoardApiClient::getFtpTask(const QString& taskId, QObject* context, ApiCompletion<FtpTaskDetailDto> completion)
{
    if (taskId.isEmpty()) return returnTypedError(context, std::move(completion), localError(
        QStringLiteral("rv1126b.api.invalid_task_id"), QStringLiteral("FTP task id is required."), ApiErrorCategory::Validation));
    ApiError error;
    const QUrl url = endpointUrl(QStringLiteral("/api/v1/ftp/tasks/%1").arg(QString::fromUtf8(QUrl::toPercentEncoding(taskId))), &error);
    if (!error.code.isEmpty()) return returnTypedError(context, std::move(completion), error);
    return startDecodedJson(QNetworkAccessManager::GetOperation, url, {}, context, std::move(completion),
        [this](const QByteArray& payload) { return codec_->parseFtpTaskDetail(payload); });
}

RequestId BoardApiClient::retryFtpTask(const QString& taskId, QObject* context, ApiCompletion<FtpTaskDetailDto> completion)
{
    if (taskId.isEmpty()) return returnTypedError(context, std::move(completion), localError(
        QStringLiteral("rv1126b.api.invalid_task_id"), QStringLiteral("FTP task id is required."), ApiErrorCategory::Validation));
    ApiError error;
    const QUrl url = endpointUrl(QStringLiteral("/api/v1/ftp/tasks/%1/retry").arg(QString::fromUtf8(QUrl::toPercentEncoding(taskId))), &error);
    if (!error.code.isEmpty()) return returnTypedError(context, std::move(completion), error);
    return startDecodedJson(QNetworkAccessManager::PostOperation, url, {}, context, std::move(completion),
        [this](const QByteArray& payload) { return codec_->parseFtpTaskDetail(payload); });
}

void BoardApiClient::cancel(const RequestId& requestId)
{
    failPending(requestId, localError(
        QStringLiteral("cancelled"), QStringLiteral("Request cancelled."), ApiErrorCategory::Cancelled), true);
}

void BoardApiClient::cancelAll()
{
    const QList<RequestId> requestIds = pendingRequests_.keys();
    for (const RequestId& requestId : requestIds) {
        cancel(requestId);
    }
}

RequestId BoardApiClient::startJsonRequest(
    QNetworkAccessManager::Operation operation,
    const QUrl& url,
    const QByteArray& body,
    QObject* context,
    ApiCompletion<QByteArray> completion,
    int firstResponseTimeoutMs,
    int transferTimeoutMs)
{
    const RequestId requestId = RequestId::createUuid();
    if (!codec_ || !secretStore_) {
        queueCompletion(context, std::move(completion), ApiResult<QByteArray>::failure(localError(
            QStringLiteral("rv1126b.api.invalid_dependencies"), QStringLiteral("Missing API codec or secret store."), ApiErrorCategory::Validation)));
        return requestId;
    }
    if (!url.isValid() || url.isEmpty()) {
        queueCompletion(context, std::move(completion), ApiResult<QByteArray>::failure(localError(
            QStringLiteral("rv1126b.api.invalid_url"), QStringLiteral("Invalid board API URL."), ApiErrorCategory::Validation)));
        return requestId;
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Accept", "application/json");
    request.setTransferTimeout(transferTimeoutMs);
    ApiError authorizationError;
    if (!applyAuthorization(&request, &authorizationError)) {
        queueCompletion(context, std::move(completion), ApiResult<QByteArray>::failure(authorizationError));
        return requestId;
    }

    QNetworkReply* reply = issueRequest(operation, request, body);
    PendingRequest pending;
    pending.reply = reply;
    const auto completionHolder = std::make_shared<ApiCompletion<QByteArray>>(std::move(completion));
    pending.fail = [context, completionHolder](const ApiError& error) mutable {
        queueCompletion(context, std::move(*completionHolder), ApiResult<QByteArray>::failure(error));
    };
    pending.succeedJson = [context, completionHolder](const QByteArray& payload) mutable {
        queueCompletion(context, std::move(*completionHolder), ApiResult<QByteArray>::success(payload));
    };
    auto* timer = new QTimer(reply);
    timer->setSingleShot(true);
    pending.connectTimer = timer;
    pendingRequests_.insert(requestId, std::move(pending));

    connect(timer, &QTimer::timeout, this, [this, requestId] {
        failPending(requestId, localError(
            QStringLiteral("network_connect_timeout"), QStringLiteral("Board connection timed out."), ApiErrorCategory::Network, true), true);
    });
    connect(reply, &QNetworkReply::metaDataChanged, this, [this, requestId] { stopConnectTimer(requestId); });
    connect(reply, &QNetworkReply::finished, this, [this, requestId] { completeJsonRequest(requestId); });
    watchContext(requestId, context);
    timer->start(firstResponseTimeoutMs);
    return requestId;
}

RequestId BoardApiClient::startEvidenceRequest(
    const QUrl& url,
    const QString& partFilePath,
    QObject* context,
    ApiCompletion<EvidenceDownloadResult> completion)
{
    return startFileRequest(
        url,
        partFilePath,
        QByteArrayLiteral("image/jpeg"),
        QStringLiteral("image/jpeg"),
        context,
        std::move(completion));
}

RequestId BoardApiClient::startFileRequest(
    const QUrl& url,
    const QString& partFilePath,
    const QByteArray& acceptHeader,
    const QString& requiredContentTypePrefix,
    QObject* context,
    ApiCompletion<EvidenceDownloadResult> completion)
{
    const RequestId requestId = RequestId::createUuid();
    if (!codec_ || !secretStore_) {
        queueCompletion(context, std::move(completion), ApiResult<EvidenceDownloadResult>::failure(localError(
            QStringLiteral("rv1126b.api.invalid_dependencies"), QStringLiteral("Missing API codec or secret store."), ApiErrorCategory::Validation)));
        return requestId;
    }
    if (!url.isValid() || url.isEmpty()) {
        queueCompletion(context, std::move(completion), ApiResult<EvidenceDownloadResult>::failure(localError(
            QStringLiteral("rv1126b.api.invalid_url"), QStringLiteral("Invalid evidence URL."), ApiErrorCategory::Validation)));
        return requestId;
    }

    QNetworkRequest request(url);
    request.setRawHeader("Accept", acceptHeader);
    request.setTransferTimeout(EvidenceRequestTimeoutMs);
    ApiError authorizationError;
    if (!applyAuthorization(&request, &authorizationError)) {
        queueCompletion(context, std::move(completion), ApiResult<EvidenceDownloadResult>::failure(authorizationError));
        return requestId;
    }

    if (!fileDownloads_) fileDownloads_ = std::make_unique<FileDownloadExecutor>();
    PendingRequest pending;
    pending.fileDownload = true;
    const auto holder = std::make_shared<ApiCompletion<EvidenceDownloadResult>>(std::move(completion));
    pending.fail = [context, holder](const ApiError& error) mutable {
        queueCompletion(context, std::move(*holder), ApiResult<EvidenceDownloadResult>::failure(error));
    };
    pending.succeedEvidence = [context, holder](const EvidenceDownloadResult& result) mutable {
        auto file = std::make_shared<FileDownloadExecutor::PendingFile>();
        file->path = result.partFilePath;
        if (!context) return;
        QMetaObject::invokeMethod(context, [file, holder, result]() mutable {
            file->delivered = true;
            if (*holder) (*holder)(ApiResult<EvidenceDownloadResult>::success(result));
        }, Qt::QueuedConnection);
    };
    pendingRequests_.insert(requestId, std::move(pending));
    watchContext(requestId, context);
    fileDownloads_->start(requestId, request, partFilePath, requiredContentTypePrefix, JsonConnectTimeoutMs,
        [this, requestId](int status, QByteArray payload, ApiResult<EvidenceDownloadResult> result) {
            const auto it = pendingRequests_.find(requestId);
            if (it == pendingRequests_.end()) {
                if (result) QFile::remove(result.value().partFilePath);
                return;
            }
            PendingRequest pending = std::move(it.value());
            pendingRequests_.erase(it);
            disconnect(pending.contextDestroyed);
            setProperty("pendingRequestCount", pendingRequests_.size());
            if (status > 0 && !isSuccessfulStatus(status)) pending.fail(codec_->parseError(status, payload));
            else if (!result) pending.fail(result.error());
            else pending.succeedEvidence(result.value());
        });
    return requestId;
}
QUrl BoardApiClient::endpointUrl(const QString& relativePath, ApiError* error) const
{
    const QUrl base = profile_.endpoint.apiBaseUrl;
    QUrl candidate(relativePath);
    if (!base.isValid() || base.scheme() != QStringLiteral("http") || base.host().isEmpty()
        || !base.userInfo().isEmpty() || !candidate.scheme().isEmpty() || !candidate.host().isEmpty()
        || !candidate.userInfo().isEmpty()) {
        if (error) *error = localError(QStringLiteral("rv1126b.api.invalid_url"), QStringLiteral("Invalid board API endpoint."), ApiErrorCategory::Validation);
        return {};
    }
    const QUrl resolved = base.resolved(candidate);
    if (!resolved.isValid() || !sameOrigin(base, resolved) || !resolved.path().startsWith(QLatin1String(ApiPrefix))) {
        if (error) *error = localError(QStringLiteral("rv1126b.api.unsafe_url"), QStringLiteral("Rejected unsafe board API endpoint."), ApiErrorCategory::Validation);
        return {};
    }
    return resolved;
}

QUrl BoardApiClient::eventUrl(const EventIdentity& identity, const QString& suffix, ApiError* error) const
{
    if (identity.deviceId != profile_.deviceId || identity.eventId <= 0 || identity.trackId <= 0) {
        if (error) *error = localError(QStringLiteral("rv1126b.api.invalid_event_identity"), QStringLiteral("Invalid event identity."), ApiErrorCategory::Validation);
        return {};
    }
    return endpointUrl(
        QStringLiteral("/api/v1/events/%1/%2%3")
            .arg(identity.eventId)
            .arg(identity.trackId)
            .arg(suffix),
        error);
}

bool BoardApiClient::applyAuthorization(QNetworkRequest* request, ApiError* error) const
{
    if (!request || !secretStore_ || profile_.credentialRef.isEmpty()) {
        if (error) *error = localError(QStringLiteral("rv1126b.api.missing_credential"), QStringLiteral("Board credential is not configured."), ApiErrorCategory::Authentication);
        return false;
    }
    auto tokenResult = secretStore_->loadToken(profile_.credentialRef);
    if (!tokenResult.isSuccess()) {
        if (error) *error = tokenResult.error();
        return false;
    }
    SecretValue token = std::move(tokenResult.value());
    if (token.isEmpty()) {
        if (error) *error = localError(QStringLiteral("rv1126b.api.missing_credential"), QStringLiteral("Board credential is empty."), ApiErrorCategory::Authentication);
        return false;
    }
    const QByteArrayView tokenView = token.view();
    QByteArray authorization("Bearer ");
    authorization.append(tokenView.data(), tokenView.size());
    request->setRawHeader("Authorization", authorization);
    authorization.fill('\0');
    token.clear();
    return true;
}

QNetworkReply* BoardApiClient::issueRequest(
    QNetworkAccessManager::Operation operation,
    const QNetworkRequest& request,
    const QByteArray& body)
{
    switch (operation) {
    case QNetworkAccessManager::GetOperation:
        return networkAccessManager_.get(request);
    case QNetworkAccessManager::PutOperation:
        return networkAccessManager_.put(request, body);
    case QNetworkAccessManager::PostOperation:
        return networkAccessManager_.post(request, body);
    default:
        return networkAccessManager_.sendCustomRequest(request, QByteArrayLiteral("GET"), body);
    }
}

void BoardApiClient::completeJsonRequest(const RequestId& requestId)
{
    const auto it = pendingRequests_.find(requestId);
    if (it == pendingRequests_.end()) return;
    PendingRequest pending = std::move(it.value());
    pendingRequests_.erase(it);
    disconnect(pending.contextDestroyed);
    setProperty("pendingRequestCount", pendingRequests_.size());
    if (pending.connectTimer) pending.connectTimer->stop();
    if (!pending.reply) return;

    QNetworkReply* reply = pending.reply;
    const QByteArray payload = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status <= 0 && reply->error() != QNetworkReply::NoError) {
        pending.fail(networkError(reply));
    } else if (!isSuccessfulStatus(status)) {
        pending.fail(codec_->parseError(status, payload));
    } else if (reply->error() != QNetworkReply::NoError) {
        pending.fail(networkError(reply));
    } else if (!contentType(reply).startsWith(QStringLiteral("application/json"))) {
        pending.fail(localError(QStringLiteral("invalid_content_type"), QStringLiteral("Expected JSON response."), ApiErrorCategory::Protocol));
    } else {
        pending.succeedJson(payload);
    }
    reply->deleteLater();
}


void BoardApiClient::watchContext(const RequestId& requestId, QObject* context)
{
    setProperty("pendingRequestCount", pendingRequests_.size());
    if (!context) return;
    pendingRequests_[requestId].contextDestroyed = connect(context, &QObject::destroyed, this, [this, requestId] {
        const auto it = pendingRequests_.find(requestId);
        if (it == pendingRequests_.end()) return;
        PendingRequest pending = std::move(it.value());
        pendingRequests_.erase(it);
        disconnect(pending.contextDestroyed);
        setProperty("pendingRequestCount", pendingRequests_.size());
        if (pending.connectTimer) pending.connectTimer->stop();
        if (pending.fileDownload && fileDownloads_) fileDownloads_->cancel(requestId);
        if (pending.reply) {
            pending.reply->abort();
            pending.reply->deleteLater();
        }
    });
}
void BoardApiClient::stopConnectTimer(const RequestId& requestId)
{
    const auto it = pendingRequests_.find(requestId);
    if (it != pendingRequests_.end() && it->connectTimer) {
        it->connectTimer->stop();
    }
}

void BoardApiClient::failPending(const RequestId& requestId, const ApiError& error, bool abortReply)
{
    const auto it = pendingRequests_.find(requestId);
    if (it == pendingRequests_.end()) return;
    PendingRequest pending = std::move(it.value());
    pendingRequests_.erase(it);
    disconnect(pending.contextDestroyed);
    setProperty("pendingRequestCount", pendingRequests_.size());
    if (pending.connectTimer) pending.connectTimer->stop();
    if (pending.fileDownload && fileDownloads_) fileDownloads_->cancel(requestId);
    if (pending.reply) {
        if (abortReply) pending.reply->abort();
        pending.reply->deleteLater();
    }
    pending.fail(error);
}

ApiError BoardApiClient::localError(
    const QString& code,
    const QString& message,
    ApiErrorCategory category,
    bool retryable) const
{
    ApiError error;
    error.code = code;
    error.message = message;
    error.category = category;
    error.retryable = retryable;
    return error;
}

ApiError BoardApiClient::networkError(QNetworkReply* reply) const
{
    const bool timeout = reply && reply->error() == QNetworkReply::TimeoutError;
    return localError(
        timeout ? QStringLiteral("network_timeout") : QStringLiteral("network_error"),
        timeout ? QStringLiteral("Board request timed out.") : QStringLiteral("Board network request failed."),
        ApiErrorCategory::Network,
        true);
}

} // namespace rv1126b
