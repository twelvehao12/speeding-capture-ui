#pragma once

#include "../ports/IRtspPlayer.h"
#include "../ports/ISecretStore.h"
#include "../protocol/ApiDtos.h"
#include "../services/DeviceDiscoveryService.h"
#include "../services/DeviceFleetService.h"

#include <QHash>
#include <QObject>
#include <QVector>
#include <QTimer>

#include <optional>
#include <functional>

namespace rv1126b {

class DirectDeviceProbeService;
using ForgetDeviceHandler = std::function<void(const QString&, QObject*, ApiCompletion<void>)>;

struct DeviceIntegrationDependencies {
    DeviceDiscoveryService* discovery = nullptr;
    DeviceFleetService* fleet = nullptr;
    ISecretStore* secretStore = nullptr;
    IRtspPlayer* player = nullptr;
    DirectDeviceProbeService* directProbe = nullptr;
    ForgetDeviceHandler forgetDevice;
};

class DeviceIntegrationController final : public QObject
{
    Q_OBJECT

public:
    explicit DeviceIntegrationController(DeviceIntegrationDependencies dependencies,
                                         QObject* parent = nullptr);
    ~DeviceIntegrationController() override;

    bool networkServicesAvailable() const;
    bool isScanning() const;
    QVector<DiscoveredDeviceDto> discoveredDevices() const;
    QVector<DeviceSessionSnapshot> sessionSnapshots() const;
    std::optional<DiscoveredDeviceDto> discoveredDevice(const QString& deviceId) const;
    bool hasCredentialForDevice(const QString& deviceId) const;
    bool manualProbeAvailable() const;
    RtspStreamRole streamRole() const;
    QString selectedVideoDeviceId() const;

    RequestId startScan(int scanWindowMs = DeviceDiscoveryService::DefaultScanWindowMs);
    void cancelScan();
    bool connectDiscoveredDevice(const QString& deviceId, SecretValue token);
    bool connectKnownDevice(const QString& deviceId, SecretValue replacementToken = {});
    bool connectManualEndpoint(const QString& ipv4, quint16 port, SecretValue token,
                               const QString& expectedDeviceId = {});
    void forgetKnownDevice(const QString& deviceId);
    bool selectVideoDevice(const QString& deviceId);
    void disconnectDevice(const QString& deviceId);
    void setStreamRole(RtspStreamRole role);
    void restartSelectedStream(const QString& deviceId);
    void setPlaybackSuspended(bool suspended);
    void shutdown();

signals:
    void discoveredDevicesReset();
    void discoveredDeviceUpserted(const rv1126b::DiscoveredDeviceDto& device);
    void scanStateChanged(bool scanning);
    void sessionChanged(const rv1126b::DeviceSessionSnapshot& snapshot);
    void selectedVideoDeviceChanged(const QString& deviceId);
    void deviceForgotten(const QString& deviceId);
    void playbackStateChanged(rv1126b::RtspPlayerState state);
    void playbackError(const rv1126b::ApiError& error);
    void userError(const QString& code, const QString& message);

private:
    void handleDeviceFound(const RequestId& scanId, const DiscoveredDeviceDto& device);
    void handleScanFinished(const RequestId& scanId);
    void handleScanFailed(const RequestId& scanId, const ApiError& error);
    void handleSessionChanged(const DeviceSessionSnapshot& snapshot);
    void handleSelectedVideoDeviceChanged(const QString& deviceId);
    void handleFleetError(const ApiError& error);
    void handlePlaybackError(const ApiError& error);

    DeviceProfile profileFor(const DiscoveredDeviceDto& device,
                             const QString& credentialRef) const;
    std::optional<DeviceSessionSnapshot> sessionFor(const QString& deviceId) const;
    void openSelectedStream(bool force = false);
    void stopPlayback();
    void emitSafeError(const ApiError& error);
    void emitUnavailable();

    DeviceIntegrationDependencies dependencies_;
    QHash<QString, DiscoveredDeviceDto> discoveredById_;
    QHash<QString, DeviceSessionSnapshot> sessionsById_;
    RequestId activeScanId_;
    RequestId activeProbeId_;
    QString selectedVideoDeviceId_;
    QString playbackDeviceId_;
    std::optional<RtspStreamSpec> playbackSpec_;
    QTimer streamSwitchTimer_;
    RtspStreamRole streamRole_ = RtspStreamRole::Sub;
    bool scanning_ = false;
    bool shutdown_ = false;
    bool playbackSuspended_ = false;
};

} // namespace rv1126b
