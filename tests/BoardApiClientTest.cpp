#include "../src/rv1126b/network/BoardApiClient.h"
#include "../src/rv1126b/protocol/BoardApiCodec.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

#include <optional>

using namespace rv1126b;

namespace {

class TestSecretStore final : public ISecretStore
{
public:
    explicit TestSecretStore(QByteArray token, QObject* parent = nullptr)
        : ISecretStore(parent)
        , token_(std::move(token))
    {
    }

    QString credentialKeyForDevice(const QString& deviceId) const override
    {
        return QStringLiteral("test/") + deviceId;
    }

    ApiResult<QString> storeToken(const QString&, SecretValue) override
    {
        return ApiResult<QString>::failure(error(QStringLiteral("unsupported")));
    }

    ApiResult<SecretValue> loadToken(const QString&) const override
    {
        return ApiResult<SecretValue>::success(SecretValue(token_));
    }

    ApiResult<void> removeToken(const QString&) override
    {
        return ApiResult<void>::success();
    }

private:
    static ApiError error(const QString& code)
    {
        ApiError value;
        value.code = code;
        value.category = ApiErrorCategory::Storage;
        return value;
    }

    QByteArray token_;
};

class TestHttpServer final : public QTcpServer
{
public:
    struct Response {
        int status = 200;
        QByteArray contentType = "application/json";
        QByteArray body;
        std::optional<qint64> advertisedContentLength;
        bool includeContentLength = true;
    };

    explicit TestHttpServer(QObject* parent = nullptr)
        : QTcpServer(parent)
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                QTcpSocket* socket = nextPendingConnection();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { handleReadyRead(socket); });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    bool start()
    {
        return listen(QHostAddress::LocalHost, 0);
    }

    Response response;
    bool holdResponse = false;
    QByteArray lastRequest;

private:
    void handleReadyRead(QTcpSocket* socket)
    {
        QByteArray& buffer = buffers_[socket];
        buffer.append(socket->readAll());
        if (!buffer.contains("\r\n\r\n") || holdResponse) {
            return;
        }

        const auto headerEnd = buffer.indexOf("\r\n\r\n") + 4;
        qint64 requestLength = 0;
        for (const auto& header : buffer.left(headerEnd).split('\n')) {
            if (header.toLower().startsWith("content-length:"))
                requestLength = header.mid(sizeof("content-length:") - 1).trimmed().toLongLong();
        }
        if (buffer.size() < headerEnd + requestLength) return;
        lastRequest = buffer;
        const QByteArray statusText = response.status == 200 ? "OK" : "Error";
        QByteArray headers = QByteArrayLiteral("HTTP/1.1 ") + QByteArray::number(response.status)
            + ' ' + statusText + "\r\nContent-Type: " + response.contentType;
        if (response.includeContentLength) {
            headers += "\r\nContent-Length: " + QByteArray::number(
                response.advertisedContentLength.value_or(response.body.size()));
        }
        headers += "\r\nConnection: close\r\n\r\n";
        socket->write(headers);
        socket->write(response.body);
        socket->disconnectFromHost();
        buffers_.remove(socket);
    }

    QHash<QTcpSocket*, QByteArray> buffers_;
};

QByteArray healthPayload()
{
    return QByteArrayLiteral(R"({
        "api_version":"v1",
        "device_id":"rv1126b_001",
        "device_model":"RV1126B",
        "release_version":"test",
        "server_time":{"epoch_ms":1784002847389,"source_epoch_ms":1784002847389,"offset_applied_ms":0,"quality":"native_utc"},
        "pipeline_health_available":true,
        "pipeline":{},
        "application_api":{"alive":true,"http_port":18080,"discovery_port":18081,"auth_required":true}
    })");
}

DeviceProfile profileFor(const TestHttpServer& server)
{
    DeviceProfile profile;
    profile.deviceId = QStringLiteral("rv1126b_001");
    profile.credentialRef = QStringLiteral("test/rv1126b_001");
    profile.endpoint.apiBaseUrl = QUrl(QStringLiteral("http://127.0.0.1:%1/api/v1").arg(server.serverPort()));
    return profile;
}

} // namespace

class BoardApiClientTest final : public QObject
{
    Q_OBJECT

private slots:
    void repeatedRequestsReleaseContextConnectionsAndReplies();
    void fileCancellationAndContextDestructionReleaseResources();
    void videoStreamsGetAndPutUseContract();
    void videoStreamsErrors_data();
    void videoStreamsErrors();
    void healthRequestUsesBearerAndDecodesResponse();
    void errorResponseUsesStableBoardError();
    void errorResponsesUseStableCategories_data();
    void errorResponsesUseStableCategories();
    void evidenceWritesPartFile();
    void clientAckPutsExpectedPayload();
    void evidenceRejectsMismatchedContentLength();
    void unsafeEvidenceUrlIsRejectedBeforeNetworkRequest();
    void cancellationCompletesExactlyOnce();
    void destroyedContextSuppressesCompletion();
};

void BoardApiClientTest::healthRequestUsesBearerAndDecodesResponse()
{
    TestHttpServer server;
    QVERIFY(server.start());
    server.response.body = healthPayload();
    TestSecretStore secretStore(QByteArrayLiteral("test-token"));
    BoardApiCodec codec;
    BoardApiClient client(profileFor(server), &secretStore, &codec);
    std::optional<ApiResult<HealthDto>> result;

    client.getHealth(this, [&result](ApiResult<HealthDto> value) { result = std::move(value); });

    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 1000);
    QVERIFY(result->isSuccess());
    QCOMPARE(result->value().deviceId, QStringLiteral("rv1126b_001"));
    QVERIFY(server.lastRequest.startsWith("GET /api/v1/health HTTP/1.1\r\n"));
    QVERIFY(server.lastRequest.toLower().contains("authorization: bearer test-token\r\n"));
}

void BoardApiClientTest::errorResponseUsesStableBoardError()
{
    TestHttpServer server;
    QVERIFY(server.start());
    server.response.status = 401;
    server.response.body = QByteArrayLiteral(R"({"error":{"code":"invalid_token","message":"token text must not be surfaced"}})");
    TestSecretStore secretStore(QByteArrayLiteral("test-token"));
    BoardApiCodec codec;
    BoardApiClient client(profileFor(server), &secretStore, &codec);
    std::optional<ApiResult<HealthDto>> result;

    client.getHealth(this, [&result](ApiResult<HealthDto> value) { result = std::move(value); });

    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 1000);
    QVERIFY(!result->isSuccess());
    QCOMPARE(result->error().code, QStringLiteral("invalid_token"));
    QCOMPARE(result->error().category, ApiErrorCategory::Authentication);
}

void BoardApiClientTest::errorResponsesUseStableCategories_data()
{
    QTest::addColumn<int>("status");
    QTest::addColumn<QString>("code");
    QTest::addColumn<ApiErrorCategory>("category");
    QTest::addColumn<bool>("retryable");

    QTest::newRow("capability-disabled") << 403 << QStringLiteral("ftp_config_write_disabled")
                                           << ApiErrorCategory::CapabilityDisabled << false;
    QTest::newRow("not-found") << 404 << QStringLiteral("event_not_found")
                                 << ApiErrorCategory::NotFound << false;
    QTest::newRow("revision-conflict") << 409 << QStringLiteral("config_revision_conflict")
                                        << ApiErrorCategory::Conflict << false;
    QTest::newRow("temporary-server-failure") << 500 << QStringLiteral("ftp_task_read_failed")
                                               << ApiErrorCategory::Temporary << true;
}

void BoardApiClientTest::errorResponsesUseStableCategories()
{
    QFETCH(int, status);
    QFETCH(QString, code);
    QFETCH(ApiErrorCategory, category);
    QFETCH(bool, retryable);
    TestHttpServer server;
    QVERIFY(server.start());
    server.response.status = status;
    server.response.body = QStringLiteral(R"({"error":{"code":"%1","message":"server detail"}})").arg(code).toUtf8();
    TestSecretStore secretStore(QByteArrayLiteral("test-token"));
    BoardApiCodec codec;
    BoardApiClient client(profileFor(server), &secretStore, &codec);
    std::optional<ApiResult<HealthDto>> result;

    client.getHealth(this, [&result](ApiResult<HealthDto> value) { result = std::move(value); });

    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 1000);
    QVERIFY(!result->isSuccess());
    QCOMPARE(result->error().code, code);
    QCOMPARE(result->error().category, category);
    QCOMPARE(result->error().retryable, retryable);
}

void BoardApiClientTest::evidenceWritesPartFile()
{
    TestHttpServer server;
    QVERIFY(server.start());
    server.response.contentType = QByteArrayLiteral("image/jpeg");
    server.response.body = QByteArray::fromHex("ffd8ffd9");
    TestSecretStore secretStore(QByteArrayLiteral("test-token"));
    BoardApiCodec codec;
    BoardApiClient client(profileFor(server), &secretStore, &codec);
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString partPath = temporaryDirectory.filePath(QStringLiteral("evidence.part"));
    std::optional<ApiResult<EvidenceDownloadResult>> result;
    EventIdentity identity { QStringLiteral("rv1126b_001"), 1, 1 };

    client.downloadEvidenceToPartFile(
        identity,
        QStringLiteral("/api/v1/events/1/1/images/evidence"),
        partPath,
        this,
        [&result](ApiResult<EvidenceDownloadResult> value) { result = std::move(value); });

    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 1000);
    QVERIFY(result->isSuccess());
    QCOMPARE(result->value().partFilePath, partPath);
    QCOMPARE(result->value().receivedBytes, 4);
    QFile file(partPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray::fromHex("ffd8ffd9"));
}

void BoardApiClientTest::clientAckPutsExpectedPayload()
{
    TestHttpServer server;
    QVERIFY(server.start());
    server.response.body = QByteArrayLiteral(R"({
        "api_version":"v1",
        "ack":{"schema_version":1,"device_id":"rv1126b_001","client_id":"qt_primary","event_id":1,"track_id":2,"evidence_size":4096,"persisted_epoch_ms":1784002847389}
    })");
    TestSecretStore secretStore(QByteArrayLiteral("test-token"));
    BoardApiCodec codec;
    BoardApiClient client(profileFor(server), &secretStore, &codec);
    std::optional<ApiResult<ClientAckDto>> result;
    EventIdentity identity { QStringLiteral("rv1126b_001"), 1, 2 };
    ClientAckCreate request;
    request.clientId = QStringLiteral("qt_primary");
    request.evidenceSize = 4096;

    client.putClientAck(identity, request, this,
        [&result](ApiResult<ClientAckDto> value) { result = std::move(value); });

    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 1000);
    QVERIFY(result->isSuccess());
    QCOMPARE(result->value().clientId, QStringLiteral("qt_primary"));
    QVERIFY(server.lastRequest.startsWith("PUT /api/v1/events/1/2/ack HTTP/1.1\r\n"));
    QVERIFY(server.lastRequest.contains("\"client_id\":\"qt_primary\""));
    QVERIFY(server.lastRequest.contains("\"evidence_size\":4096"));
}

void BoardApiClientTest::evidenceRejectsMismatchedContentLength()
{
    TestHttpServer server;
    QVERIFY(server.start());
    server.response.contentType = QByteArrayLiteral("image/jpeg");
    server.response.body = QByteArray::fromHex("ffd8ffd9");
    server.response.includeContentLength = false;
    TestSecretStore secretStore(QByteArrayLiteral("test-token"));
    BoardApiCodec codec;
    BoardApiClient client(profileFor(server), &secretStore, &codec);
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    std::optional<ApiResult<EvidenceDownloadResult>> result;
    const EventIdentity identity { QStringLiteral("rv1126b_001"), 1, 1 };

    client.downloadEvidenceToPartFile(
        identity,
        QStringLiteral("/api/v1/events/1/1/images/evidence"),
        temporaryDirectory.filePath(QStringLiteral("invalid.part")),
        this,
        [&result](ApiResult<EvidenceDownloadResult> value) { result = std::move(value); });

    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 1000);
    QVERIFY(!result->isSuccess());
    QCOMPARE(result->error().code, QStringLiteral("invalid_content_length"));
    QCOMPARE(result->error().category, ApiErrorCategory::Protocol);
}

void BoardApiClientTest::unsafeEvidenceUrlIsRejectedBeforeNetworkRequest()
{
    TestHttpServer server;
    QVERIFY(server.start());
    TestSecretStore secretStore(QByteArrayLiteral("test-token"));
    BoardApiCodec codec;
    BoardApiClient client(profileFor(server), &secretStore, &codec);
    std::optional<ApiResult<EvidenceDownloadResult>> result;
    EventIdentity identity { QStringLiteral("rv1126b_001"), 1, 1 };

    client.downloadEvidenceToPartFile(
        identity,
        QStringLiteral("http://untrusted.example/api/v1/evidence"),
        QDir::temp().filePath(QStringLiteral("unsafe.part")),
        this,
        [&result](ApiResult<EvidenceDownloadResult> value) { result = std::move(value); });

    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 500);
    QVERIFY(!result->isSuccess());
    QCOMPARE(result->error().category, ApiErrorCategory::Validation);
    QVERIFY(server.lastRequest.isEmpty());
}

void BoardApiClientTest::cancellationCompletesExactlyOnce()
{
    TestHttpServer server;
    QVERIFY(server.start());
    server.holdResponse = true;
    TestSecretStore secretStore(QByteArrayLiteral("test-token"));
    BoardApiCodec codec;
    BoardApiClient client(profileFor(server), &secretStore, &codec);
    int completionCount = 0;
    std::optional<ApiResult<HealthDto>> result;

    const RequestId requestId = client.getHealth(this, [&completionCount, &result](ApiResult<HealthDto> value) {
        ++completionCount;
        result = std::move(value);
    });
    client.cancel(requestId);

    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 500);
    QVERIFY(!result->isSuccess());
    QCOMPARE(result->error().category, ApiErrorCategory::Cancelled);
    QTest::qWait(50);
    QCOMPARE(completionCount, 1);
}

void BoardApiClientTest::destroyedContextSuppressesCompletion()
{
    TestHttpServer server;
    QVERIFY(server.start());
    server.holdResponse = true;
    TestSecretStore secretStore(QByteArrayLiteral("test-token"));
    BoardApiCodec codec;
    BoardApiClient client(profileFor(server), &secretStore, &codec);
    auto* context = new QObject;
    int completionCount = 0;

    client.getHealth(context, [&completionCount](ApiResult<HealthDto>) { ++completionCount; });
    delete context;
    QTest::qWait(100);

    QCOMPARE(completionCount, 0);
}

void BoardApiClientTest::videoStreamsGetAndPutUseContract()
{
    TestHttpServer server;
    QVERIFY(server.start());
    server.response.body = QByteArrayLiteral(R"({"api_version":"v1","revision":"r1",
        "runtime_revision":"runtime-r1","write_enabled":true,"restart_required":false,
        "main":{"width":2560,"height":1440,"codec":"h265"},
        "sub":{"width":1920,"height":1080,"codec":"h264"}})");
    TestSecretStore secrets(QByteArrayLiteral("video-test-token"));
    BoardApiCodec codec;
    BoardApiClient client(profileFor(server), &secrets, &codec);
    std::optional<ApiResult<VideoStreamsConfigDto>> result;
    client.getVideoStreamsConfig(this, [&](auto value) { result = std::move(value); });
    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 1000);
    QVERIFY(*result);
    QVERIFY(server.lastRequest.startsWith("GET /api/v1/config/video-streams HTTP/1.1\r\n"));
    QVERIFY(server.lastRequest.toLower().contains("authorization: bearer video-test-token\r\n"));
    VideoStreamsUpdate update;
    update.expectedRevision = result->value().revision;
    update.main.codec = QStringLiteral("h265");
    result.reset();
    client.putVideoStreamsConfig(update, this, [&](auto value) { result = std::move(value); });
    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 1000);
    QVERIFY(*result);
    QVERIFY(server.lastRequest.startsWith("PUT /api/v1/config/video-streams HTTP/1.1\r\n"));
    QVERIFY(server.lastRequest.toLower().contains("authorization: bearer video-test-token\r\n"));
    const auto body = QJsonDocument::fromJson(server.lastRequest.mid(server.lastRequest.indexOf("\r\n\r\n") + 4)).object();
    QCOMPARE(body.value(QStringLiteral("expected_revision")).toString(), QStringLiteral("r1"));
    QCOMPARE(body.value(QStringLiteral("main")).toObject().value(QStringLiteral("codec")).toString(), QStringLiteral("h265"));
    QCOMPARE(body.value(QStringLiteral("sub")).toObject().value(QStringLiteral("width")).toInt(), 1920);
    update.main.width = 3840;
    result.reset();
    server.lastRequest.clear();
    client.putVideoStreamsConfig(update, this, [&](auto value) { result = std::move(value); });
    QTRY_VERIFY(result.has_value());
    QVERIFY(!*result);
    QCOMPARE(result->error().category, ApiErrorCategory::Validation);
    QVERIFY(server.lastRequest.isEmpty());
}

void BoardApiClientTest::videoStreamsErrors_data()
{
    QTest::addColumn<int>("status");
    QTest::addColumn<QString>("code");
    QTest::addColumn<ApiErrorCategory>("category");
    QTest::newRow("conflict") << 409 << QStringLiteral("config_revision_conflict") << ApiErrorCategory::Conflict;
    QTest::newRow("write-disabled") << 403 << QStringLiteral("config_write_disabled") << ApiErrorCategory::CapabilityDisabled;
    QTest::newRow("unauthorized") << 401 << QStringLiteral("unauthorized") << ApiErrorCategory::Authentication;
    QTest::newRow("unsupported") << 404 << QStringLiteral("video_streams_config_unsupported") << ApiErrorCategory::NotFound;
}

void BoardApiClientTest::videoStreamsErrors()
{
    QFETCH(int, status);
    QFETCH(QString, code);
    QFETCH(ApiErrorCategory, category);
    TestHttpServer server;
    QVERIFY(server.start());
    server.response.status = status;
    server.response.body = QJsonDocument(QJsonObject{{QStringLiteral("error"), QJsonObject{
        {QStringLiteral("code"), code}, {QStringLiteral("message"), QStringLiteral("private board detail")}}}}).toJson();
    TestSecretStore secrets(QByteArrayLiteral("video-test-token"));
    BoardApiCodec codec;
    BoardApiClient client(profileFor(server), &secrets, &codec);
    VideoStreamsUpdate update;
    update.expectedRevision = QStringLiteral("r1");
    std::optional<ApiResult<VideoStreamsConfigDto>> result;
    client.putVideoStreamsConfig(update, this, [&](auto value) { result = std::move(value); });
    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 1000);
    QVERIFY(!*result);
    QCOMPARE(result->error().category, category);
    QCOMPARE(result->error().code, code);
    QVERIFY(!result->error().message.contains(QStringLiteral("private board detail")));
}

void BoardApiClientTest::repeatedRequestsReleaseContextConnectionsAndReplies()
{
    class Context : public QObject { public: using QObject::receivers; } context;
    TestHttpServer server;
    QVERIFY(server.start());
    server.response.body = healthPayload();
    TestSecretStore secrets(QByteArrayLiteral("token"));
    BoardApiCodec codec;
    BoardApiClient client(profileFor(server), &secrets, &codec);
    const int initial = context.receivers(SIGNAL(destroyed(QObject*)));
    int completed = 0;
    for (int i = 0; i < 100; ++i) {
        client.getHealth(&context, [&](auto result) { QVERIFY(result); ++completed; });
        QTRY_COMPARE(completed, i + 1);
        QCOMPARE(context.receivers(SIGNAL(destroyed(QObject*))), initial);
    }
    QTRY_COMPARE(client.property("pendingRequestCount").toInt(), 0);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(client.findChildren<QNetworkReply*>().size(), 0);
}

void BoardApiClientTest::fileCancellationAndContextDestructionReleaseResources()
{
    TestHttpServer server;
    QVERIFY(server.start());
    server.holdResponse = true;
    TestSecretStore secrets(QByteArrayLiteral("token"));
    BoardApiCodec codec;
    BoardApiClient client(profileFor(server), &secrets, &codec);
    QTemporaryDir dir;
    const EventIdentity identity{QStringLiteral("rv1126b_001"), 1, 1};
    int calls = 0;
    const auto path = dir.filePath(QStringLiteral("cancelled.part"));
    auto id = client.downloadEvidenceToPartFile(identity, QStringLiteral("/api/v1/events/1/1/evidence.jpg"),
        path, this, [&](auto result) { QVERIFY(!result); ++calls; });
    client.cancel(id);
    QTRY_COMPARE(calls, 1);
    auto* context = new QObject;
    client.downloadEvidenceToPartFile(identity, QStringLiteral("/api/v1/events/1/1/evidence.jpg"),
        dir.filePath(QStringLiteral("closed.part")), context, [&](auto) { ++calls; });
    delete context;
    QTRY_COMPARE(client.property("pendingRequestCount").toInt(), 0);
    QTest::qWait(100);
    QCOMPARE(calls, 1);
    QVERIFY(QDir(dir.path()).entryList(QDir::Files).isEmpty());
    server.holdResponse = false;
    server.response.contentType = "image/jpeg";
    server.response.body = QByteArray(2 * 1024 * 1024, 'x');
    client.downloadEvidenceToPartFile(identity, QStringLiteral("/api/v1/events/1/1/evidence.jpg"), path,
        this, [&](auto result) { QVERIFY(result); ++calls; });
    QTRY_COMPARE(calls, 2);
    QCOMPARE(QFileInfo(path).size(), qint64(2 * 1024 * 1024));
}

QTEST_MAIN(BoardApiClientTest)

#include "BoardApiClientTest.moc"
