#include "../src/rv1126b/application/DeviceIntegrationController.h"

#include <QSignalSpy>
#include <QWidget>
#include <QtTest>

using namespace rv1126b;

class FakeDiscoveryService final : public DeviceDiscoveryService
{
public:
    using DeviceDiscoveryService::DeviceDiscoveryService;

    RequestId startScan(int scanWindowMs) override
    {
        ++startCount;
        lastWindowMs = scanWindowMs;
        currentId = QUuid::createUuid();
        return currentId;
    }

    void cancelScan(const RequestId& scanId) override
    {
        ++cancelCount;
        cancelledIds.append(scanId);
    }

    void cancelAll() override
    {
        ++cancelAllCount;
    }

    void sendDevice(const RequestId& scanId, const DiscoveredDeviceDto& device)
    {
        emit deviceFound(scanId, device);
    }

    void finish(const RequestId& scanId)
    {
        emit scanFinished(scanId);
    }

    int startCount = 0;
    int cancelCount = 0;
    int cancelAllCount = 0;
    int lastWindowMs = 0;
    RequestId currentId;
    QVector<RequestId> cancelledIds;
};

class FakeSecretStore final : public ISecretStore
{
public:
    using ISecretStore::ISecretStore;

    QString credentialKeyForDevice(const QString& deviceId) const override
    {
        return QStringLiteral("RV1126B/%1").arg(deviceId);
    }

    ApiResult<QString> storeToken(const QString& deviceId, SecretValue token) override
    {
        ++storeCount;
        lastDeviceId = deviceId;
        lastToken = QByteArray(token.view().data(), token.view().size());
        if (storeShouldFail) {
            ApiError error;
            error.code = QStringLiteral("credential_store_failed");
            error.category = ApiErrorCategory::Storage;
            return ApiResult<QString>::failure(error);
        }
        return ApiResult<QString>::success(credentialKeyForDevice(deviceId));
    }

    ApiResult<SecretValue> loadToken(const QString&) const override
    {
        return ApiResult<SecretValue>::success(SecretValue(QByteArrayLiteral("unused")));
    }

    ApiResult<void> removeToken(const QString&) override
    {
        return ApiResult<void>::success();
    }

    int storeCount = 0;
    bool storeShouldFail = false;
    QString lastDeviceId;
    QByteArray lastToken;
};

class FakeFleetService final : public DeviceFleetService
{
public:
    using DeviceFleetService::DeviceFleetService;

    QVector<DeviceSessionSnapshot> sessions() const override
    {
        return snapshots.values();
    }

    bool upsertDevice(const DeviceProfile& profile) override
    {
        ++upsertCount;
        lastProfile = profile;
        DeviceSessionSnapshot snapshot = snapshots.value(profile.deviceId);
        snapshot.profile = profile;
        snapshots.insert(profile.deviceId, snapshot);
        return upsertResult;
    }

    bool connectDevice(const QString& deviceId) override
    {
        ++connectCount;
        lastConnectedId = deviceId;
        return connectResult;
    }

    void disconnectDevice(const QString& deviceId) override
    {
        ++disconnectCount;
        lastDisconnectedId = deviceId;
    }

    void disconnectAll() override
    {
        ++disconnectAllCount;
    }

    bool selectVideoDevice(const QString& deviceId) override
    {
        ++selectCount;
        if (!selectResult) {
            return false;
        }
        selectedId = deviceId;
        emit selectedVideoDeviceChanged(deviceId);
        return true;
    }

    QString selectedVideoDeviceId() const override
    {
        return selectedId;
    }

    void sendSession(DeviceSessionSnapshot snapshot)
    {
        snapshots.insert(snapshot.profile.deviceId, snapshot);
        emit sessionChanged(snapshot);
    }

    QHash<QString, DeviceSessionSnapshot> snapshots;
    DeviceProfile lastProfile;
    QString selectedId;
    QString lastConnectedId;
    QString lastDisconnectedId;
    int upsertCount = 0;
    int connectCount = 0;
    int disconnectCount = 0;
    int disconnectAllCount = 0;
    int selectCount = 0;
    bool upsertResult = true;
    bool connectResult = true;
    bool selectResult = true;
};

class FakeRtspPlayer final : public IRtspPlayer
{
public:
    FakeRtspPlayer()
        : output_(new QWidget)
    {
    }

    ~FakeRtspPlayer() override
    {
        if (output_ && !output_->parent()) {
            delete output_;
        }
    }

    void open(const RtspStreamSpec& stream) override
    {
        ++openCount;
        lastStream = stream;
        state_ = RtspPlayerState::Opening;
        emit stateChanged(state_);
    }

    void stop() override
    {
        ++stopCount;
        state_ = RtspPlayerState::Stopped;
        emit stateChanged(state_);
    }

    RtspPlayerState state() const override
    {
        return state_;
    }

    QWidget* outputWidget() const override
    {
        return output_;
    }

    void fail(const ApiError& error)
    {
        emit errorOccurred(error);
    }

    QPointer<QWidget> output_;
    RtspStreamSpec lastStream;
    RtspPlayerState state_ = RtspPlayerState::Idle;
    int openCount = 0;
    int stopCount = 0;
};

class DeviceIntegrationControllerTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void unavailableServicesReportStableError();
    void scanDeduplicatesAndIgnoresLateResponses();
    void tokenIsRequiredAndStoredByReference();
    void existingCredentialCanBeReused();
    void onlineSessionOpensSubStreamAndSupportsSwitching();
    void rapidSelectionsCoalesceAndStopCancelsPendingOpen();
    void degradedHttpDoesNotStopVideoAndRtspFailureDoesNotChangeSession();
    void retryableRtspFailureDoesNotRaiseGlobalUserError();
    void rtspFailureReportsGenericStorageProblemFromLastHealth();
    void rtspFailureReportsStoppedRkipcFromLastHealth();
    void authenticationFailureStopsVideoAndPromptsOnce();
    void selectingAnotherDeviceStopsOldVideoBeforeOpeningNewVideo();
    void disconnectAndShutdownReleaseResources();

private:
    static DiscoveredDeviceDto discovered(QString id = QStringLiteral("device-a"),
                                          QString ip = QStringLiteral("192.0.2.10"));
    static DeviceSessionSnapshot snapshot(const DeviceProfile& profile,
                                          DeviceSessionState state);
};

void DeviceIntegrationControllerTest::initTestCase()
{
    qRegisterMetaType<ApiError>();
    qRegisterMetaType<DeviceSessionSnapshot>();
    qRegisterMetaType<RtspPlayerState>();
}

void DeviceIntegrationControllerTest::unavailableServicesReportStableError()
{
    FakeRtspPlayer player;
    DeviceIntegrationController controller({nullptr, nullptr, nullptr, &player});
    QSignalSpy errorSpy(&controller, &DeviceIntegrationController::userError);

    QVERIFY(controller.startScan().isNull());
    QCOMPARE(errorSpy.size(), 1);
    QCOMPARE(errorSpy.at(0).at(0).toString(), QStringLiteral("services_unavailable"));
}

void DeviceIntegrationControllerTest::scanDeduplicatesAndIgnoresLateResponses()
{
    FakeDiscoveryService discoveryService;
    FakeFleetService fleet;
    FakeSecretStore secrets;
    FakeRtspPlayer player;
    DeviceIntegrationController controller({&discoveryService, &fleet, &secrets, &player});
    QSignalSpy upsertSpy(&controller, &DeviceIntegrationController::discoveredDeviceUpserted);

    const RequestId firstScan = controller.startScan();
    discoveryService.sendDevice(firstScan, discovered());
    discoveryService.sendDevice(firstScan, discovered(QStringLiteral("device-a"), QStringLiteral("192.0.2.11")));
    QCOMPARE(controller.discoveredDevices().size(), 1);
    QCOMPARE(controller.discoveredDevices().first().ipv4, QStringLiteral("192.0.2.11"));
    QCOMPARE(upsertSpy.size(), 2);

    const RequestId secondScan = controller.startScan();
    QCOMPARE(discoveryService.cancelCount, 1);
    discoveryService.sendDevice(firstScan, discovered(QStringLiteral("late-device")));
    QVERIFY(controller.discoveredDevices().isEmpty());
    discoveryService.sendDevice(secondScan, discovered(QStringLiteral("device-b"), QStringLiteral("192.0.2.20")));
    QCOMPARE(controller.discoveredDevices().size(), 1);
    discoveryService.finish(secondScan);
    QVERIFY(!controller.isScanning());
}

void DeviceIntegrationControllerTest::tokenIsRequiredAndStoredByReference()
{
    FakeDiscoveryService discoveryService;
    FakeFleetService fleet;
    FakeSecretStore secrets;
    FakeRtspPlayer player;
    DeviceIntegrationController controller({&discoveryService, &fleet, &secrets, &player});
    QSignalSpy errorSpy(&controller, &DeviceIntegrationController::userError);

    const RequestId scanId = controller.startScan();
    discoveryService.sendDevice(scanId, discovered());

    QVERIFY(!controller.connectDiscoveredDevice(QStringLiteral("device-a"), SecretValue {}));
    QCOMPARE(errorSpy.last().at(0).toString(), QStringLiteral("token_required"));
    QCOMPARE(fleet.connectCount, 0);

    QVERIFY(controller.connectDiscoveredDevice(
        QStringLiteral("device-a"), SecretValue(QByteArrayLiteral("top-secret-token"))));
    QCOMPARE(secrets.storeCount, 1);
    QCOMPARE(secrets.lastToken, QByteArrayLiteral("top-secret-token"));
    QCOMPARE(fleet.lastProfile.credentialRef, QStringLiteral("RV1126B/device-a"));
    QCOMPARE(fleet.connectCount, 1);
    QVERIFY(!errorSpy.last().at(1).toString().contains(QStringLiteral("top-secret-token")));
}

void DeviceIntegrationControllerTest::existingCredentialCanBeReused()
{
    FakeDiscoveryService discoveryService;
    FakeFleetService fleet;
    FakeSecretStore secrets;
    FakeRtspPlayer player;
    DeviceSessionSnapshot existing;
    existing.profile.deviceId = QStringLiteral("device-a");
    existing.profile.credentialRef = QStringLiteral("RV1126B/device-a");
    fleet.snapshots.insert(existing.profile.deviceId, existing);
    DeviceIntegrationController controller({&discoveryService, &fleet, &secrets, &player});

    const RequestId scanId = controller.startScan();
    discoveryService.sendDevice(scanId, discovered());

    QVERIFY(controller.hasCredentialForDevice(QStringLiteral("device-a")));
    QVERIFY(controller.connectDiscoveredDevice(QStringLiteral("device-a"), SecretValue {}));
    QCOMPARE(secrets.storeCount, 0);
    QCOMPARE(fleet.lastProfile.credentialRef, QStringLiteral("RV1126B/device-a"));
}

void DeviceIntegrationControllerTest::onlineSessionOpensSubStreamAndSupportsSwitching()
{
    FakeDiscoveryService discoveryService;
    FakeFleetService fleet;
    FakeSecretStore secrets;
    FakeRtspPlayer player;
    DeviceIntegrationController controller({&discoveryService, &fleet, &secrets, &player});

    const RequestId scanId = controller.startScan();
    discoveryService.sendDevice(scanId, discovered());
    QVERIFY(controller.connectDiscoveredDevice(
        QStringLiteral("device-a"), SecretValue(QByteArrayLiteral("token"))));

    fleet.sendSession(snapshot(fleet.lastProfile, DeviceSessionState::Online));
    QTRY_VERIFY(player.openCount > 0);
    QTRY_COMPARE(player.openCount, 1);
    QCOMPARE(player.lastStream.role, RtspStreamRole::Sub);
    QCOMPARE(player.lastStream.url, QUrl(QStringLiteral("rtsp://192.0.2.10/live/1")));

    controller.setStreamRole(RtspStreamRole::Main);
    QTRY_COMPARE(player.openCount, 2);
    QCOMPARE(player.lastStream.role, RtspStreamRole::Main);
    QCOMPARE(player.lastStream.url, QUrl(QStringLiteral("rtsp://192.0.2.10/live/0")));
    controller.restartSelectedStream(QStringLiteral("other-device"));
    QTRY_COMPARE(player.openCount, 2);
    controller.restartSelectedStream(QStringLiteral("device-a"));
    QTRY_COMPARE(player.openCount, 3);
    QCOMPARE(player.lastStream.role, RtspStreamRole::Main);
    controller.setPlaybackSuspended(true);
    controller.restartSelectedStream(QStringLiteral("device-a"));
    QTRY_COMPARE(player.openCount, 3);
    controller.setPlaybackSuspended(false);
    QTRY_COMPARE(player.openCount, 4);
    controller.disconnectDevice(QStringLiteral("device-a"));
    controller.restartSelectedStream(QStringLiteral("device-a"));
    QTRY_COMPARE(player.openCount, 4);
}

void DeviceIntegrationControllerTest::degradedHttpDoesNotStopVideoAndRtspFailureDoesNotChangeSession()
{
    FakeDiscoveryService discoveryService;
    FakeFleetService fleet;
    FakeSecretStore secrets;
    FakeRtspPlayer player;
    DeviceIntegrationController controller({&discoveryService, &fleet, &secrets, &player});
    QSignalSpy sessionSpy(&controller, &DeviceIntegrationController::sessionChanged);
    QSignalSpy playbackErrorSpy(&controller, &DeviceIntegrationController::playbackError);

    const RequestId scanId = controller.startScan();
    discoveryService.sendDevice(scanId, discovered());
    controller.connectDiscoveredDevice(
        QStringLiteral("device-a"), SecretValue(QByteArrayLiteral("token")));
    fleet.sendSession(snapshot(fleet.lastProfile, DeviceSessionState::Online));
    QTRY_VERIFY(player.openCount > 0);
    fleet.sendSession(snapshot(fleet.lastProfile, DeviceSessionState::Degraded));
    QCOMPARE(player.stopCount, 0);

    ApiError rtspError;
    rtspError.code = QStringLiteral("rtsp_open_timeout");
    rtspError.category = ApiErrorCategory::Network;
    rtspError.retryable = true;
    player.fail(rtspError);
    QCOMPARE(playbackErrorSpy.size(), 1);
    QCOMPARE(sessionSpy.size(), 2);
    QCOMPARE(sessionSpy.last().at(0).value<DeviceSessionSnapshot>().state,
             DeviceSessionState::Degraded);
}

void DeviceIntegrationControllerTest::retryableRtspFailureDoesNotRaiseGlobalUserError()
{
    FakeDiscoveryService discoveryService;
    FakeFleetService fleet;
    FakeSecretStore secrets;
    FakeRtspPlayer player;
    DeviceIntegrationController controller({&discoveryService, &fleet, &secrets, &player});
    QSignalSpy playbackErrorSpy(&controller, &DeviceIntegrationController::playbackError);
    QSignalSpy userErrorSpy(&controller, &DeviceIntegrationController::userError);

    const RequestId scanId = controller.startScan();
    discoveryService.sendDevice(scanId, discovered());
    controller.connectDiscoveredDevice(
        QStringLiteral("device-a"), SecretValue(QByteArrayLiteral("token")));
    fleet.sendSession(snapshot(fleet.lastProfile, DeviceSessionState::Online));
    QTRY_VERIFY(player.openCount > 0);

    ApiError rtspError;
    rtspError.code = QStringLiteral("rtsp_open_timeout");
    rtspError.category = ApiErrorCategory::Network;
    rtspError.retryable = true;
    player.fail(rtspError);

    QCOMPARE(playbackErrorSpy.size(), 1);
    QCOMPARE(userErrorSpy.size(), 0);
}

void DeviceIntegrationControllerTest::rtspFailureReportsGenericStorageProblemFromLastHealth()
{
    FakeDiscoveryService discoveryService;
    FakeFleetService fleet;
    FakeSecretStore secrets;
    FakeRtspPlayer player;
    DeviceIntegrationController controller({&discoveryService, &fleet, &secrets, &player});
    QSignalSpy playbackErrorSpy(&controller, &DeviceIntegrationController::playbackError);

    const RequestId scanId = controller.startScan();
    discoveryService.sendDevice(scanId, discovered());
    controller.connectDiscoveredDevice(
        QStringLiteral("device-a"), SecretValue(QByteArrayLiteral("token")));

    DeviceSessionSnapshot online = snapshot(fleet.lastProfile, DeviceSessionState::Online);
    HealthDto health;
    health.deviceId = fleet.lastProfile.deviceId;
    health.pipelineHealthAvailable = true;
    health.pipeline.insert(QStringLiteral("storage"), QJsonObject{
        {QStringLiteral("status"), QStringLiteral("read_only")},
        {QStringLiteral("writable"), false},
    });
    online.lastHealth = health;
    fleet.sendSession(online);

    ApiError rtspError;
    rtspError.code = QStringLiteral("rtsp_open_timeout");
    rtspError.category = ApiErrorCategory::Network;
    rtspError.retryable = true;
    player.fail(rtspError);

    QCOMPARE(playbackErrorSpy.size(), 1);
    const ApiError reported = playbackErrorSpy.last().at(0).value<ApiError>();
    QVERIFY(reported.message.contains(QStringLiteral("事件存储")));
    QVERIFY(!reported.message.contains(QStringLiteral("TF")));
}

void DeviceIntegrationControllerTest::rtspFailureReportsStoppedRkipcFromLastHealth()
{
    FakeDiscoveryService discoveryService;
    FakeFleetService fleet;
    FakeSecretStore secrets;
    FakeRtspPlayer player;
    DeviceIntegrationController controller({&discoveryService, &fleet, &secrets, &player});
    QSignalSpy playbackErrorSpy(&controller, &DeviceIntegrationController::playbackError);

    const RequestId scanId = controller.startScan();
    discoveryService.sendDevice(scanId, discovered());
    controller.connectDiscoveredDevice(
        QStringLiteral("device-a"), SecretValue(QByteArrayLiteral("token")));

    DeviceSessionSnapshot online = snapshot(fleet.lastProfile, DeviceSessionState::Online);
    HealthDto health;
    health.deviceId = fleet.lastProfile.deviceId;
    health.pipelineHealthAvailable = true;
    health.pipeline.insert(QStringLiteral("storage"), QJsonObject{
        {QStringLiteral("status"), QStringLiteral("healthy")},
        {QStringLiteral("writable"), true},
    });
    health.pipeline.insert(QStringLiteral("rkipc"), QJsonObject{
        {QStringLiteral("alive"), false},
    });
    online.lastHealth = health;
    fleet.sendSession(online);

    ApiError rtspError;
    rtspError.code = QStringLiteral("rtsp_open_timeout");
    rtspError.category = ApiErrorCategory::Network;
    rtspError.retryable = true;
    player.fail(rtspError);

    QCOMPARE(playbackErrorSpy.size(), 1);
    const ApiError reported = playbackErrorSpy.last().at(0).value<ApiError>();
    QVERIFY(reported.message.contains(QStringLiteral("rkipc")));
    QVERIFY(reported.message.contains(QStringLiteral("RTSP 554")));
}

void DeviceIntegrationControllerTest::authenticationFailureStopsVideoAndPromptsOnce()
{
    FakeDiscoveryService discoveryService;
    FakeFleetService fleet;
    FakeSecretStore secrets;
    FakeRtspPlayer player;
    DeviceIntegrationController controller({&discoveryService, &fleet, &secrets, &player});
    QSignalSpy errorSpy(&controller, &DeviceIntegrationController::userError);

    const RequestId scanId = controller.startScan();
    discoveryService.sendDevice(scanId, discovered());
    controller.connectDiscoveredDevice(
        QStringLiteral("device-a"), SecretValue(QByteArrayLiteral("never-log-this")));
    fleet.sendSession(snapshot(fleet.lastProfile, DeviceSessionState::Online));
    QTRY_VERIFY(player.openCount > 0);
    fleet.sendSession(snapshot(fleet.lastProfile, DeviceSessionState::AuthenticationFailed));
    fleet.sendSession(snapshot(fleet.lastProfile, DeviceSessionState::AuthenticationFailed));

    QCOMPARE(player.stopCount, 1);
    int unauthorizedCount = 0;
    for (const QList<QVariant>& arguments : errorSpy) {
        if (arguments.at(0).toString() == QStringLiteral("unauthorized")) {
            ++unauthorizedCount;
            QCOMPARE(arguments.at(1).toString(), QStringLiteral("认证失败，请重新配置 Token"));
            QVERIFY(!arguments.at(1).toString().contains(QStringLiteral("never-log-this")));
        }
    }
    QCOMPARE(unauthorizedCount, 1);
}

void DeviceIntegrationControllerTest::selectingAnotherDeviceStopsOldVideoBeforeOpeningNewVideo()
{
    FakeDiscoveryService discoveryService;
    FakeFleetService fleet;
    FakeSecretStore secrets;
    FakeRtspPlayer player;
    DeviceIntegrationController controller({&discoveryService, &fleet, &secrets, &player});

    const RequestId scanId = controller.startScan();
    discoveryService.sendDevice(scanId, discovered(QStringLiteral("device-a"), QStringLiteral("192.0.2.10")));
    discoveryService.sendDevice(scanId, discovered(QStringLiteral("device-b"), QStringLiteral("192.0.2.20")));
    controller.connectDiscoveredDevice(
        QStringLiteral("device-a"), SecretValue(QByteArrayLiteral("token-a")));
    const DeviceProfile profileA = fleet.lastProfile;
    fleet.sendSession(snapshot(profileA, DeviceSessionState::Online));
    QTRY_COMPARE(player.openCount, 1);

    controller.connectDiscoveredDevice(
        QStringLiteral("device-b"), SecretValue(QByteArrayLiteral("token-b")));
    const DeviceProfile profileB = fleet.lastProfile;
    QCOMPARE(player.stopCount, 1);
    QTRY_COMPARE(player.openCount, 1);
    fleet.sendSession(snapshot(profileB, DeviceSessionState::Online));
    QTRY_COMPARE(player.openCount, 2);
    QCOMPARE(player.lastStream.deviceId, QStringLiteral("device-b"));
    QCOMPARE(player.lastStream.url.host(), QStringLiteral("192.0.2.20"));
}

void DeviceIntegrationControllerTest::disconnectAndShutdownReleaseResources()
{
    FakeDiscoveryService discoveryService;
    FakeFleetService fleet;
    FakeSecretStore secrets;
    FakeRtspPlayer player;
    {
        DeviceIntegrationController controller({&discoveryService, &fleet, &secrets, &player});
        const RequestId scanId = controller.startScan();
        discoveryService.sendDevice(scanId, discovered());
        controller.connectDiscoveredDevice(
            QStringLiteral("device-a"), SecretValue(QByteArrayLiteral("token")));
        fleet.sendSession(snapshot(fleet.lastProfile, DeviceSessionState::Online));
    QTRY_VERIFY(player.openCount > 0);

        controller.disconnectDevice(QStringLiteral("device-a"));
        QCOMPARE(player.stopCount, 1);
        QCOMPARE(fleet.disconnectCount, 1);

        controller.shutdown();
        controller.shutdown();
        QCOMPARE(discoveryService.cancelAllCount, 1);
        QCOMPARE(fleet.disconnectAllCount, 1);
    }
    QCOMPARE(discoveryService.cancelAllCount, 1);
    QCOMPARE(fleet.disconnectAllCount, 1);
}

DiscoveredDeviceDto DeviceIntegrationControllerTest::discovered(QString id, QString ip)
{
    DiscoveredDeviceDto value;
    value.magic = QStringLiteral("RV1126B_DISCOVERY");
    value.version = 1;
    value.type = QStringLiteral("discover_response");
    value.deviceId = std::move(id);
    value.deviceModel = QStringLiteral("RV1126B");
    value.ipv4 = std::move(ip);
    value.apiVersion = QStringLiteral("v1");
    value.apiUrl = QUrl(QStringLiteral("http://%1:18080/api/v1").arg(value.ipv4));
    value.releaseVersion = QStringLiteral("test-release");
    value.authRequired = true;
    return value;
}

DeviceSessionSnapshot DeviceIntegrationControllerTest::snapshot(const DeviceProfile& profile,
                                                                 DeviceSessionState state)
{
    DeviceSessionSnapshot value;
    value.profile = profile;
    value.state = state;
    return value;
}

void DeviceIntegrationControllerTest::rapidSelectionsCoalesceAndStopCancelsPendingOpen()
{
    FakeDiscoveryService discovery;
    FakeFleetService fleet;
    FakeSecretStore secrets;
    FakeRtspPlayer player;
    DeviceIntegrationController controller({&discovery, &fleet, &secrets, &player});
    const auto scan = controller.startScan();
    discovery.sendDevice(scan, discovered());
    QVERIFY(controller.connectDiscoveredDevice(QStringLiteral("device-a"), SecretValue(QByteArrayLiteral("token"))));
    const auto online = snapshot(fleet.lastProfile, DeviceSessionState::Online);
    fleet.sendSession(online);
    QTRY_COMPARE(player.openCount, 1);
    for (int i = 0; i < 1000; ++i) {
        controller.setStreamRole(i % 2 ? RtspStreamRole::Main : RtspStreamRole::Sub);
        fleet.sendSession(online); // Repeated profile/status notifications.
    }
    QCOMPARE(player.openCount, 1);
    QTRY_COMPARE(player.openCount, 2);
    QCOMPARE(player.lastStream.role, RtspStreamRole::Main);
    fleet.sendSession(online);
    QTest::qWait(250);
    QCOMPARE(player.openCount, 2);
    controller.setStreamRole(RtspStreamRole::Sub);
    controller.setPlaybackSuspended(true);
    QCOMPARE(player.stopCount, 1);
    QTest::qWait(250);
    QCOMPARE(player.openCount, 2);
}

QTEST_MAIN(DeviceIntegrationControllerTest)

#include "DeviceIntegrationControllerTest.moc"

