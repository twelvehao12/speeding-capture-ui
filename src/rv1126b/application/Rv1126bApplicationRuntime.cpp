#include "Rv1126bApplicationRuntime.h"

#include "../infrastructure/video/QtMultimediaRtspPlayer.h"
#include "../network/UdpDeviceDiscoveryService.h"
#include "../network/DirectDeviceProbeService.h"
#include "../protocol/BoardApiCodec.h"
#include "../security/WindowsCredentialStore.h"
#include "../services/BoardDeviceFleetService.h"
#include "../services/BoardEvidenceCache.h"
#include "../services/EmbeddedFtpReceiveServer.h"
#include "../services/EvidenceCacheMaintenanceService.h"
#include "../storage/SqliteEventRepository.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDirIterator>
#include <QPointer>
#include <QStandardPaths>

#include <utility>

namespace rv1126b {
namespace {

bool copyDirectoryWithoutOverwrite(const QString& sourcePath, const QString& targetPath)
{
    if (!QFileInfo::exists(sourcePath) || QFileInfo::exists(targetPath)) return true;
    if (!QDir().mkpath(targetPath)) return false;
    QDirIterator iterator(sourcePath, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories);
    const QDir source(sourcePath);
    while (iterator.hasNext()) {
        const QString itemPath = iterator.next();
        const QString relative = source.relativeFilePath(itemPath);
        const QString target = QDir(targetPath).filePath(relative);
        const QFileInfo info(itemPath);
        if (info.isDir()) {
            if (!QDir().mkpath(target)) return false;
        } else {
            if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !QFile::copy(itemPath, target))
                return false;
        }
    }
    return true;
}

} // namespace

Rv1126bApplicationRuntime::Rv1126bApplicationRuntime(
    SystemSettings settings, QObject* parent)
    : Rv1126bApplicationRuntime(std::move(settings), QString(), parent)
{
}

Rv1126bApplicationRuntime::Rv1126bApplicationRuntime(
    SystemSettings settings, QString appDataRootPath, QObject* parent)
    : QObject(parent)
    , settings_(std::move(settings))
    , codec_(std::make_unique<BoardApiCodec>())
{
    const QDir appData(appDataRootPath.isEmpty()
                           ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                           : std::move(appDataRootPath));
    databasePath_ = appData.filePath(QStringLiteral("data/rv1126b_events.sqlite"));
    evidenceRootPath_ = QDir(settings_.storage.rootPath)
                            .filePath(QStringLiteral("rv1126b/events"));
    importLegacyDataIfNeeded();

    discovery_ = new UdpDeviceDiscoveryService(codec_.get(), {},
                                                DeviceDiscoveryService::DefaultDiscoveryPort, this);
    directProbe_ = new DirectDeviceProbeService(codec_.get(), this);
    secretStore_ = new WindowsCredentialStore(this);
    repository_ = new SqliteEventRepository(databasePath_, this);
    fleet_ = new BoardDeviceFleetService(repository_, secretStore_, codec_.get(), this);
    evidenceCache_ = new BoardEvidenceCache(
        [fleet = QPointer<BoardDeviceFleetService>(fleet_)](const QString& deviceId) {
            return fleet ? fleet->boardApiForDevice(deviceId) : nullptr;
        }, repository_, evidenceRootPath_, this);
    ftpReceiveServer_ = new EmbeddedFtpReceiveServer(this);
    evidenceMaintenance_ = new EvidenceCacheMaintenanceService(evidenceRootPath_, this);
    player_ = new QtMultimediaRtspPlayer(this);
}

Rv1126bApplicationRuntime::~Rv1126bApplicationRuntime()
{
    shutdown();
}

void Rv1126bApplicationRuntime::initialize(QObject* context, ApiCompletion<void> completion)
{
    if (shutdown_ || !repository_ || !fleet_) {
        ApiError error;
        error.code = QStringLiteral("runtime_unavailable");
        error.message = QStringLiteral("生产运行时不可用");
        error.category = ApiErrorCategory::Validation;
        completion(ApiResult<void>::failure(error));
        return;
    }
    repository_->initialize(context, [this, context, completion = std::move(completion)](
                                         ApiResult<void> result) mutable {
        if (!result) {
            completion(ApiResult<void>::failure(result.error()));
            return;
        }
        fleet_->initialize(context, [this, completion = std::move(completion)](
                                        ApiResult<void> fleetResult) mutable {
            initialized_ = fleetResult.isSuccess();
            completion(std::move(fleetResult));
        });
    });
}

MainWindowDependencies Rv1126bApplicationRuntime::mainWindowDependencies() const
{
    MainWindowDependencies dependencies;
    dependencies.discovery = discovery_;
    dependencies.fleet = fleet_;
    dependencies.secretStore = secretStore_;
    dependencies.player = player_;
    dependencies.directProbe = directProbe_;
    dependencies.eventRepository = repository_;
    dependencies.evidenceCache = evidenceCache_;
    if (fleet_) {
        for (const DeviceSessionSnapshot& snapshot : fleet_->sessions()) {
            if (EventSyncService* sync = fleet_->eventSyncForDevice(snapshot.profile.deviceId)) {
                dependencies.eventSyncServices.append(sync);
            }
        }
    }
    const QPointer<BoardDeviceFleetService> fleet(fleet_);
    dependencies.forgetDevice = [fleet](const QString& deviceId, QObject* context,
                                        ApiCompletion<void> completion) {
        if (fleet) {
            fleet->forgetDevice(deviceId, context, std::move(completion));
        } else {
            ApiError error;
            error.code = QStringLiteral("fleet_unavailable");
            error.message = QStringLiteral("设备运行时已关闭");
            error.category = ApiErrorCategory::Network;
            completion(ApiResult<void>::failure(error));
        }
    };
    dependencies.boardApiForDevice = [fleet](const QString& deviceId) {
        return fleet ? fleet->boardApiForDevice(deviceId) : nullptr;
    };
    dependencies.ftpServiceForDevice = [fleet](const QString& deviceId) {
        return fleet ? fleet->ftpServiceForDevice(deviceId) : nullptr;
    };
    dependencies.ftpTaskSnapshotForDevice = [fleet](const QString& deviceId) {
        return fleet ? fleet->ftpSnapshotForDevice(deviceId) : nullptr;
    };
    dependencies.ftpReceiveServer = ftpReceiveServer_;
    dependencies.eventSyncForDevice = [fleet](const QString& deviceId) {
        return fleet ? fleet->eventSyncForDevice(deviceId) : nullptr;
    };
    dependencies.evidenceMaintenance = evidenceMaintenance_;
    dependencies.evidenceRootPath = evidenceRootPath_;
    dependencies.switchEvidenceRoot = [runtime = QPointer<Rv1126bApplicationRuntime>(
                                           const_cast<Rv1126bApplicationRuntime*>(this))](
                                           const QString& path) {
        if (!runtime || !runtime->evidenceCache_ || !runtime->evidenceMaintenance_) return false;
        runtime->evidenceCache_->cancelAll();
        if (!runtime->evidenceCache_->setCacheRootPath(path)) return false;
        runtime->evidenceMaintenance_->setCacheRootPath(path);
        runtime->evidenceRootPath_ = path;
        return true;
    };
    return dependencies;
}

BoardDeviceFleetService* Rv1126bApplicationRuntime::fleet() const { return fleet_; }
SqliteEventRepository* Rv1126bApplicationRuntime::repository() const { return repository_; }
BoardEvidenceCache* Rv1126bApplicationRuntime::evidenceCache() const { return evidenceCache_; }
EvidenceCacheMaintenanceService* Rv1126bApplicationRuntime::evidenceMaintenance() const { return evidenceMaintenance_; }
QtMultimediaRtspPlayer* Rv1126bApplicationRuntime::player() const { return player_; }
QString Rv1126bApplicationRuntime::evidenceRootPath() const { return evidenceRootPath_; }

void Rv1126bApplicationRuntime::shutdown()
{
    if (shutdown_) return;
    shutdown_ = true;
    if (evidenceMaintenance_) evidenceMaintenance_->cancel();
    if (evidenceCache_) evidenceCache_->cancelAll();
    if (ftpReceiveServer_) ftpReceiveServer_->stop();
    if (directProbe_) directProbe_->cancelAll();
    if (fleet_) fleet_->shutdown();
    if (repository_) repository_->cancelAll();
    if (player_) player_->stop();
}

void Rv1126bApplicationRuntime::importLegacyDataIfNeeded()
{
#ifdef CAMERA_MANAGER_SOURCE_DIR
    const QDir legacyRoot(QString::fromUtf8(CAMERA_MANAGER_SOURCE_DIR));
    const QString legacyDatabase = legacyRoot.filePath(QStringLiteral("data/rv1126b_events.sqlite"));
    if (!QFileInfo::exists(databasePath_) && QFileInfo::exists(legacyDatabase)) {
        QDir().mkpath(QFileInfo(databasePath_).absolutePath());
        QFile::copy(legacyDatabase, databasePath_);
    }
    const QString legacyEvidence = legacyRoot.filePath(QStringLiteral("data/evidence-cache"));
    copyDirectoryWithoutOverwrite(legacyEvidence, evidenceRootPath_);
#endif
}

} // namespace rv1126b
