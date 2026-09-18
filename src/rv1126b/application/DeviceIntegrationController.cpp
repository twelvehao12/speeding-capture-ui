#include "DeviceIntegrationController.h"
#include "../network/DirectDeviceProbeService.h"

#include <QAbstractSocket>
#include <QHostAddress>
#include <QUrl>

#include <algorithm>
#include <utility>

namespace rv1126b {

DeviceIntegrationController::DeviceIntegrationController(
    DeviceIntegrationDependencies dependencies,
    QObject* parent)
    : QObject(parent)
    , dependencies_(dependencies)
{
    streamSwitchTimer_.setSingleShot(true);
    streamSwitchTimer_.setInterval(200);
    connect(&streamSwitchTimer_, &QTimer::timeout, this, [this] { openSelectedStream(); });
    if (dependencies_.discovery) {
        connect(dependencies_.discovery, &DeviceDiscoveryService::deviceFound,
                this, &DeviceIntegrationController::handleDeviceFound);
        connect(dependencies_.discovery, &DeviceDiscoveryService::scanFinished,
                this, &DeviceIntegrationController::handleScanFinished);
        connect(dependencies_.discovery, &DeviceDiscoveryService::scanFailed,
                this, &DeviceIntegrationController::handleScanFailed);
    }

    if (dependencies_.fleet) {
        const QVector<DeviceSessionSnapshot> initialSessions = dependencies_.fleet->sessions();
        for (const DeviceSessionSnapshot& snapshot : initialSessions) {
            sessionsById_.insert(snapshot.profile.deviceId, snapshot);
        }
        selectedVideoDeviceId_ = dependencies_.fleet->selectedVideoDeviceId();

        connect(dependencies_.fleet, &DeviceFleetService::sessionChanged,
                this, &DeviceIntegrationController::handleSessionChanged);
        connect(dependencies_.fleet, &DeviceFleetService::selectedVideoDeviceChanged,
                this, &DeviceIntegrationController::handleSelectedVideoDeviceChanged);
        connect(dependencies_.fleet, &DeviceFleetService::fleetError,
                this, &DeviceIntegrationController::handleFleetError);
    }

    if (dependencies_.player) {
        connect(dependencies_.player, &IRtspPlayer::stateChanged,
                this, &DeviceIntegrationController::playbackStateChanged);
        connect(dependencies_.player, &IRtspPlayer::errorOccurred,
                this, &DeviceIntegrationController::handlePlaybackError);
    }

    openSelectedStream();
}

DeviceIntegrationController::~DeviceIntegrationController()
{
    shutdown();
}

bool DeviceIntegrationController::networkServicesAvailable() const
{
    return dependencies_.discovery && dependencies_.fleet && dependencies_.secretStore;
}

bool DeviceIntegrationController::isScanning() const
{
    return scanning_;
}

QVector<DiscoveredDeviceDto> DeviceIntegrationController::discoveredDevices() const
{
    QVector<DiscoveredDeviceDto> devices = discoveredById_.values();
    std::sort(devices.begin(), devices.end(), [](const auto& left, const auto& right) {
        return left.deviceId < right.deviceId;
    });
    return devices;
}

QVector<DeviceSessionSnapshot> DeviceIntegrationController::sessionSnapshots() const
{
    QVector<DeviceSessionSnapshot> snapshots = sessionsById_.values();
    std::sort(snapshots.begin(), snapshots.end(), [](const auto& left, const auto& right) {
        return left.profile.deviceId < right.profile.deviceId;
    });
    return snapshots;
}

std::optional<DiscoveredDeviceDto> DeviceIntegrationController::discoveredDevice(
    const QString& deviceId) const
{
    const auto it = discoveredById_.constFind(deviceId);
    if (it == discoveredById_.cend()) {
        return std::nullopt;
    }
    return *it;
}

bool DeviceIntegrationController::hasCredentialForDevice(const QString& deviceId) const
{
    const auto snapshot = sessionFor(deviceId);
    return snapshot && !snapshot->profile.credentialRef.isEmpty();
}

bool DeviceIntegrationController::manualProbeAvailable() const
{
    return dependencies_.directProbe && dependencies_.fleet && dependencies_.secretStore;
}

RtspStreamRole DeviceIntegrationController::streamRole() const
{
    return streamRole_;
}

QString DeviceIntegrationController::selectedVideoDeviceId() const
{
    return selectedVideoDeviceId_;
}

RequestId DeviceIntegrationController::startScan(int scanWindowMs)
{
    if (shutdown_ || !networkServicesAvailable()) {
        emitUnavailable();
        return {};
    }

    cancelScan();
    discoveredById_.clear();
    emit discoveredDevicesReset();

    activeScanId_ = dependencies_.discovery->startScan(scanWindowMs);
    if (activeScanId_.isNull()) {
        ApiError error;
        error.code = QStringLiteral("discovery_start_failed");
        error.category = ApiErrorCategory::Network;
        emitSafeError(error);
        return {};
    }

    scanning_ = true;
    emit scanStateChanged(true);
    return activeScanId_;
}

void DeviceIntegrationController::cancelScan()
{
    if (!scanning_) {
        return;
    }

    const RequestId scanId = activeScanId_;
    activeScanId_ = RequestId();
    scanning_ = false;
    if (dependencies_.discovery && !scanId.isNull()) {
        dependencies_.discovery->cancelScan(scanId);
    }
    emit scanStateChanged(false);
}

bool DeviceIntegrationController::connectDiscoveredDevice(const QString& deviceId,
                                                           SecretValue token)
{
    if (shutdown_ || !networkServicesAvailable()) {
        emitUnavailable();
        return false;
    }

    const auto discovered = discoveredDevice(deviceId);
    if (!discovered) {
        emit userError(QStringLiteral("device_not_discovered"),
                       QStringLiteral("设备不在当前搜索结果中，请重新搜索"));
        return false;
    }

    QString credentialRef;
    if (const auto existing = sessionFor(deviceId)) {
        credentialRef = existing->profile.credentialRef;
    }

    if (!token.isEmpty()) {
        ApiResult<QString> stored = dependencies_.secretStore->storeToken(deviceId, std::move(token));
        if (!stored) {
            emitSafeError(stored.error());
            return false;
        }
        credentialRef = stored.value();
    } else if (discovered->authRequired && credentialRef.isEmpty()) {
        emit userError(QStringLiteral("token_required"),
                       QStringLiteral("该设备需要 Bearer Token"));
        return false;
    }

    const DeviceProfile profile = profileFor(*discovered, credentialRef);
    if (!dependencies_.fleet->upsertDevice(profile)) {
        emit userError(QStringLiteral("device_upsert_failed"),
                       QStringLiteral("无法保存设备连接信息"));
        return false;
    }
    if (!selectVideoDevice(deviceId)) {
        return false;
    }
    if (!dependencies_.fleet->connectDevice(deviceId)) {
        emit userError(QStringLiteral("device_connect_failed"),
                       QStringLiteral("无法启动设备连接"));
        return false;
    }
    return true;
}

bool DeviceIntegrationController::connectKnownDevice(
    const QString& deviceId, SecretValue replacementToken)
{
    if (shutdown_ || !networkServicesAvailable()) {
        emitUnavailable();
        return false;
    }
    const auto snapshot = sessionFor(deviceId);
    if (!snapshot) {
        emit userError(QStringLiteral("device_not_known"),
                       QStringLiteral("设备档案不存在，请重新搜索"));
        return false;
    }
    DeviceProfile profile = snapshot->profile;
    if (!replacementToken.isEmpty()) {
        ApiResult<QString> stored = dependencies_.secretStore->storeToken(
            deviceId, std::move(replacementToken));
        if (!stored) {
            emitSafeError(stored.error());
            return false;
        }
        profile.credentialRef = stored.value();
        if (!dependencies_.fleet->upsertDevice(profile)) return false;
    } else if (profile.credentialRef.isEmpty()) {
        emit userError(QStringLiteral("token_required"),
                       QStringLiteral("该历史设备没有安全 Token，请重新输入"));
        return false;
    }
    if (!selectVideoDevice(deviceId)) return false;
    return dependencies_.fleet->connectDevice(deviceId);
}

bool DeviceIntegrationController::connectManualEndpoint(
    const QString& ipv4, quint16 port, SecretValue token, const QString& expectedDeviceId)
{
    if (shutdown_ || !manualProbeAvailable()) {
        emit userError(QStringLiteral("manual_probe_unavailable"),
                       QStringLiteral("手工 IP 探测服务尚未装配"));
        return false;
    }
    if (!activeProbeId_.isNull()) dependencies_.directProbe->cancel(activeProbeId_);
    activeProbeId_ = dependencies_.directProbe->probe(
        ipv4.trimmed(), port, std::move(token), expectedDeviceId, this,
        [this](ApiResult<DirectProbeResult> result) {
            activeProbeId_ = RequestId();
            if (!result) {
                emitSafeError(result.error());
                return;
            }
            DirectProbeResult verified = std::move(result.value());
            QString credentialRef;
            if (!verified.token.isEmpty()) {
                ApiResult<QString> stored = dependencies_.secretStore->storeToken(
                    verified.device.deviceId, std::move(verified.token));
                if (!stored) {
                    emitSafeError(stored.error());
                    return;
                }
                credentialRef = stored.value();
            } else if (const auto known = sessionFor(verified.device.deviceId)) {
                credentialRef = known->profile.credentialRef;
            }
            if (verified.device.authRequired && credentialRef.isEmpty()) {
                emit userError(QStringLiteral("token_required"),
                               QStringLiteral("该 endpoint 需要 Bearer Token"));
                return;
            }
            discoveredById_.insert(verified.device.deviceId, verified.device);
            emit discoveredDeviceUpserted(verified.device);
            const DeviceProfile profile = profileFor(verified.device, credentialRef);
            if (!dependencies_.fleet->upsertDevice(profile)
                || !selectVideoDevice(profile.deviceId)
                || !dependencies_.fleet->connectDevice(profile.deviceId)) {
                emit userError(QStringLiteral("manual_connect_failed"),
                               QStringLiteral("设备身份已验证，但无法启动连接"));
            }
        });
    return !activeProbeId_.isNull();
}

void DeviceIntegrationController::forgetKnownDevice(const QString& deviceId)
{
    if (shutdown_ || deviceId.trimmed().isEmpty() || !dependencies_.forgetDevice) {
        emit userError(QStringLiteral("forget_device_unavailable"),
                       QStringLiteral("无法删除该设备档案"));
        return;
    }
    if (deviceId == selectedVideoDeviceId_) stopPlayback();
    dependencies_.forgetDevice(deviceId, this, [this, deviceId](ApiResult<void> result) {
        if (!result) {
            emitSafeError(result.error());
            return;
        }
        sessionsById_.remove(deviceId);
        discoveredById_.remove(deviceId);
        if (selectedVideoDeviceId_ == deviceId) {
            selectedVideoDeviceId_.clear();
            emit selectedVideoDeviceChanged({});
        }
        emit deviceForgotten(deviceId);
    });
}

bool DeviceIntegrationController::selectVideoDevice(const QString& deviceId)
{
    if (shutdown_ || !dependencies_.fleet || deviceId.trimmed().isEmpty()) {
        return false;
    }
    if (!dependencies_.fleet->selectVideoDevice(deviceId)) {
        emit userError(QStringLiteral("video_device_select_failed"),
                       QStringLiteral("无法选择视频设备"));
        return false;
    }
    handleSelectedVideoDeviceChanged(deviceId);
    return true;
}

void DeviceIntegrationController::disconnectDevice(const QString& deviceId)
{
    if (deviceId.trimmed().isEmpty() || !dependencies_.fleet) {
        return;
    }
    if (deviceId == selectedVideoDeviceId_) {
        stopPlayback();
    }
    dependencies_.fleet->disconnectDevice(deviceId);
}

void DeviceIntegrationController::setStreamRole(RtspStreamRole role)
{
    if (streamRole_ == role) {
        return;
    }
    streamRole_ = role;
    streamSwitchTimer_.start();
}

void DeviceIntegrationController::restartSelectedStream(const QString& deviceId)
{
    if (!deviceId.isEmpty() && deviceId == selectedVideoDeviceId_ && deviceId == playbackDeviceId_
        && !playbackSuspended_ && !shutdown_)
        openSelectedStream(true);
}

void DeviceIntegrationController::setPlaybackSuspended(bool suspended)
{
    if (playbackSuspended_ == suspended) return;
    playbackSuspended_ = suspended;
    if (suspended) stopPlayback();
    else openSelectedStream();
}

void DeviceIntegrationController::shutdown()
{
    if (shutdown_) {
        return;
    }
    shutdown_ = true;
    cancelScan();
    if (dependencies_.directProbe && !activeProbeId_.isNull()) {
        dependencies_.directProbe->cancel(activeProbeId_);
        activeProbeId_ = RequestId();
    }
    stopPlayback();
    if (dependencies_.discovery) {
        dependencies_.discovery->cancelAll();
    }
    if (dependencies_.fleet) {
        dependencies_.fleet->disconnectAll();
    }
}

void DeviceIntegrationController::handleDeviceFound(const RequestId& scanId,
                                                     const DiscoveredDeviceDto& device)
{
    if (!scanning_ || scanId != activeScanId_ || device.deviceId.trimmed().isEmpty()) {
        return;
    }
    discoveredById_.insert(device.deviceId, device);
    emit discoveredDeviceUpserted(device);
}

void DeviceIntegrationController::handleScanFinished(const RequestId& scanId)
{
    if (!scanning_ || scanId != activeScanId_) {
        return;
    }
    activeScanId_ = RequestId();
    scanning_ = false;
    emit scanStateChanged(false);
}

void DeviceIntegrationController::handleScanFailed(const RequestId& scanId,
                                                    const ApiError& error)
{
    if (!scanning_ || scanId != activeScanId_) {
        return;
    }
    activeScanId_ = RequestId();
    scanning_ = false;
    emit scanStateChanged(false);
    emitSafeError(error);
}

void DeviceIntegrationController::handleSessionChanged(const DeviceSessionSnapshot& snapshot)
{
    const QString deviceId = snapshot.profile.deviceId;
    const auto previous = sessionsById_.constFind(deviceId);
    const DeviceSessionState previousState = previous == sessionsById_.cend()
        ? DeviceSessionState::Disconnected
        : previous->state;
    sessionsById_.insert(deviceId, snapshot);
    emit sessionChanged(snapshot);

    if (deviceId != selectedVideoDeviceId_) {
        return;
    }

    if (snapshot.state == DeviceSessionState::Online) {
        if (!streamSwitchTimer_.isActive()) openSelectedStream();
    } else if (snapshot.state == DeviceSessionState::AuthenticationFailed) {
        stopPlayback();
        if (previousState != DeviceSessionState::AuthenticationFailed) {
            emit userError(QStringLiteral("unauthorized"),
                           QStringLiteral("认证失败，请重新配置 Token"));
        }
    } else if (snapshot.state == DeviceSessionState::Disconnected) {
        stopPlayback();
    }
}

void DeviceIntegrationController::handleSelectedVideoDeviceChanged(const QString& deviceId)
{
    if (selectedVideoDeviceId_ == deviceId) {
        return;
    }
    stopPlayback();
    selectedVideoDeviceId_ = deviceId;
    emit selectedVideoDeviceChanged(deviceId);
    if (!deviceId.isEmpty()) streamSwitchTimer_.start();
}

void DeviceIntegrationController::handleFleetError(const ApiError& error)
{
    emitSafeError(error);
}

void DeviceIntegrationController::handlePlaybackError(const ApiError& error)
{
    ApiError reported = error;
    const bool transientPlaybackError = error.retryable
        && (error.category == ApiErrorCategory::Network
            || error.category == ApiErrorCategory::Temporary);
    bool shouldEmitGlobalError = !transientPlaybackError;
    const QString deviceId = playbackDeviceId_.isEmpty() ? selectedVideoDeviceId_ : playbackDeviceId_;
    const auto snapshot = sessionFor(deviceId);
    if (snapshot) {
        if (snapshot->state != DeviceSessionState::Online) {
            reported.message = QStringLiteral("板端当前未在线，视频服务可能尚未启动");
        } else if (snapshot->lastHealth) {
            const HealthDto& health = *snapshot->lastHealth;
            if (!health.pipelineHealthAvailable) {
                reported.message = QStringLiteral("板端健康信息不可用，请检查 app_api 和生产服务状态");
            } else {
                const QJsonObject pipeline = health.pipeline;
                const QJsonObject storage = pipeline.value(QStringLiteral("storage")).toObject();
                const QString storageStatus = storage.value(QStringLiteral("status")).toString();
                const bool storageWritable = storage.value(QStringLiteral("writable")).toBool(true);
                if (storageStatus == QStringLiteral("read_only") || !storageWritable) {
                    reported.message = QStringLiteral("板端事件存储只读或不可写，生产服务可能无法正常写事件，请先处理存储状态");
                    shouldEmitGlobalError = true;
                } else {
                    const QJsonObject rkipc = pipeline.value(QStringLiteral("rkipc")).toObject();
                    if (!rkipc.value(QStringLiteral("alive")).toBool(true)) {
                        reported.message = QStringLiteral("板端 rkipc 未运行，RTSP 554 不可用，请启动生产服务");
                        shouldEmitGlobalError = true;
                    }
                }
            }
        }
    }
    if (reported.message.isEmpty()
        && (reported.category == ApiErrorCategory::Network
            || reported.category == ApiErrorCategory::Temporary)) {
        reported.message = QStringLiteral("RTSP 首帧超时，应用会继续重连；请检查板端服务、网络和电脑解码压力");
    }
    emit playbackError(reported);
    if (shouldEmitGlobalError) {
        emitSafeError(reported);
    }
}

DeviceProfile DeviceIntegrationController::profileFor(const DiscoveredDeviceDto& device,
                                                       const QString& credentialRef) const
{
    DeviceProfile profile;
    profile.deviceId = device.deviceId;
    profile.deviceModel = device.deviceModel;
    profile.releaseVersion = device.releaseVersion;
    profile.endpoint.ipv4 = device.ipv4;
    profile.endpoint.apiBaseUrl = device.apiUrl;
    profile.endpoint.httpPort = static_cast<quint16>(device.apiUrl.port(18080));
    profile.endpoint.discoveryPort = DeviceDiscoveryService::DefaultDiscoveryPort;
    profile.credentialRef = credentialRef;
    profile.advertisedCapabilities = device.capabilities;
    return profile;
}

std::optional<DeviceSessionSnapshot> DeviceIntegrationController::sessionFor(
    const QString& deviceId) const
{
    const auto it = sessionsById_.constFind(deviceId);
    if (it == sessionsById_.cend()) {
        return std::nullopt;
    }
    return *it;
}

void DeviceIntegrationController::openSelectedStream(bool force)
{
    if (shutdown_ || playbackSuspended_ || !dependencies_.player || selectedVideoDeviceId_.isEmpty()) {
        return;
    }
    const auto snapshot = sessionFor(selectedVideoDeviceId_);
    if (!snapshot || snapshot->state != DeviceSessionState::Online) {
        return;
    }

    QHostAddress address;
    if (!address.setAddress(snapshot->profile.endpoint.ipv4)
        || address.protocol() != QAbstractSocket::IPv4Protocol) {
        emit userError(QStringLiteral("invalid_rtsp_endpoint"),
                       QStringLiteral("设备 IPv4 地址无效，无法打开实时视频"));
        return;
    }

    QUrl url;
    url.setScheme(QStringLiteral("rtsp"));
    url.setHost(snapshot->profile.endpoint.ipv4);
    url.setPath(streamRole_ == RtspStreamRole::Main
                    ? QStringLiteral("/live/0")
                    : QStringLiteral("/live/1"));

    RtspStreamSpec stream;
    stream.deviceId = selectedVideoDeviceId_;
    stream.url = url;
    stream.role = streamRole_;
    const auto state = dependencies_.player->state();
    if (!force && playbackSpec_ && playbackSpec_->deviceId == stream.deviceId
        && playbackSpec_->url == stream.url && playbackSpec_->role == stream.role
        && (state == RtspPlayerState::Opening || state == RtspPlayerState::Playing
            || state == RtspPlayerState::Reconnecting)) return;
    streamSwitchTimer_.stop();
    playbackSpec_ = stream;
    playbackDeviceId_ = selectedVideoDeviceId_;
    dependencies_.player->open(stream);
}

void DeviceIntegrationController::stopPlayback()
{
    streamSwitchTimer_.stop();
    playbackSpec_.reset();
    if (!dependencies_.player || playbackDeviceId_.isEmpty()) {
        return;
    }
    dependencies_.player->stop();
    playbackDeviceId_.clear();
}

void DeviceIntegrationController::emitSafeError(const ApiError& error)
{
    if (error.code == QStringLiteral("unauthorized")
        || error.category == ApiErrorCategory::Authentication) {
        emit userError(QStringLiteral("unauthorized"),
                       QStringLiteral("认证失败，请重新配置 Token"));
        return;
    }

    QString message;
    switch (error.category) {
    case ApiErrorCategory::Network:
    case ApiErrorCategory::Temporary:
        message = QStringLiteral("设备网络暂时不可用，请稍后重试");
        break;
    case ApiErrorCategory::Cancelled:
        message = QStringLiteral("操作已取消");
        break;
    case ApiErrorCategory::Validation:
        message = QStringLiteral("设备请求参数无效");
        break;
    case ApiErrorCategory::Storage:
        message = QStringLiteral("无法安全保存设备凭据");
        break;
    case ApiErrorCategory::Protocol:
        message = QStringLiteral("设备返回了不兼容的数据");
        break;
    default:
        message = QStringLiteral("设备操作失败");
        break;
    }
    emit userError(error.code.isEmpty() ? QStringLiteral("device_operation_failed") : error.code,
                   message);
}

void DeviceIntegrationController::emitUnavailable()
{
    emit userError(QStringLiteral("services_unavailable"),
                   QStringLiteral("真实设备网络服务尚未装配"));
}

} // namespace rv1126b
