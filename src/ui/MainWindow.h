#pragma once

#include "../models/CaptureRecord.h"
#include "../models/Device.h"
#include "../models/DeviceStatus.h"
#include "../models/SystemSettings.h"
#include "../services/CaptureStorageService.h"
#include "../rv1126b/application/DeviceIntegrationController.h"
#include "../rv1126b/application/DeviceOperationsController.h"
#include "../rv1126b/application/EventViewController.h"

#include <QMainWindow>

#include <functional>

class QAction;
class QCheckBox;
class QCloseEvent;
class QComboBox;
class QDateTimeEdit;
class QLabel;
class QLineEdit;
class QMenu;
class QPoint;
class QSortFilterProxyModel;
class QSpinBox;
class QTableView;
class QTabWidget;
class QTimer;
class QProgressDialog;
class QToolButton;
class QWidget;

class CaptureRecordTableModel;
class CaptureRecordService;
enum class CaptureRecordFilter;
class DeviceManager;
class DevicePropertyModel;
class DeviceTableModel;
class MaintenanceController;
class QEvent;
class QSplitter;
class SystemSettingsService;
class VideoWidget;
class LivePreviewPanel;
namespace rv1126b {
class EmbeddedFtpReceiveServer;
class EvidenceCacheMaintenanceService;
}

struct MainWindowDependencies {
    rv1126b::DeviceDiscoveryService* discovery = nullptr;
    rv1126b::DeviceFleetService* fleet = nullptr;
    rv1126b::ISecretStore* secretStore = nullptr;
    rv1126b::IRtspPlayer* player = nullptr;
    rv1126b::DirectDeviceProbeService* directProbe = nullptr;
    rv1126b::ForgetDeviceHandler forgetDevice;
    rv1126b::IEventRepository* eventRepository = nullptr;
    rv1126b::EvidenceCache* evidenceCache = nullptr;
    QVector<rv1126b::EventSyncService*> eventSyncServices;
    std::function<rv1126b::EventSyncService*(const QString&)> eventSyncForDevice;
    rv1126b::BoardApiResolver boardApiForDevice;
    rv1126b::FtpServiceResolver ftpServiceForDevice;
    rv1126b::FtpTaskSnapshotResolver ftpTaskSnapshotForDevice;
    rv1126b::EmbeddedFtpReceiveServer* ftpReceiveServer = nullptr;
    rv1126b::EvidenceCacheMaintenanceService* evidenceMaintenance = nullptr;
    QString evidenceRootPath;
    std::function<bool(const QString&)> switchEvidenceRoot;
    bool mockMode = false;
};

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    explicit MainWindow(MainWindowDependencies dependencies, QWidget* parent = nullptr);
    void attachEventSyncService(rv1126b::EventSyncService* service);

protected:
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private slots:
    void addDevice();
    void connectSelectedDevice();
    void disconnectSelectedDevice();
    void openDeviceConfig();
    void triggerCapture();
    void rebootSelectedDevice();
    void syncSelectedDeviceTime();
    void openGlobalSettings();
    void refreshCaptureRecords();
    void toggleCapturePause(bool paused);
    void finishCapturePause();
    void exportCaptureRecords();
    void deleteSelectedCapture();
    void clearCaptureRecords();
    void showActionMessage();
    void showSelectedNetworkInfo();
    void openSelectedLocalFolder();
    void updateDeviceProperties();
    void showDeviceContextMenu(const QPoint& position);
    void handleDeviceAdded(const Device& device);
    void handleDeviceStatusChanged(int row, const DeviceStatus& status);
    void handleDeviceConfigChanged(int row, const DeviceConfig& config);
    void handleCaptureGenerated(int row, const CaptureRecord& record);
    void handleDeviceError(int row, const QString& message);
    void handleDiscoveredDevice(const rv1126b::DiscoveredDeviceDto& device);
    void handleSessionChanged(const rv1126b::DeviceSessionSnapshot& snapshot);
    void handleSelectedVideoDeviceChanged(const QString& deviceId);
    void handleIntegrationError(const QString& code, const QString& message);
    void handleDeviceForgotten(const QString& deviceId);
    void updateSelectedEvidence();
    void changeEventViewMode();
    void previousHistoryPage();
    void nextHistoryPage();

private:
    void createActions();
    void createToolBar();
    void createCentralLayout();
    void createStatusBar();
    void createDeviceContextMenu();
    void connectDeviceManager();
    void connectIntegrationController();
    void connectEventController();
    void populateInitialData();
    void updateStatusText();
    void selectDeviceRow(int row);
    void applySystemSettings(bool initialApply = false);
    void applyCaptureColumnVisibility();
    void configureMaintenanceController();
    void performDiskMaintenance();
    void runScheduledShutdown();
    void updateStartupRegistration(bool enabled);
    void updatePreviewLayout();

    int currentDeviceRow() const;
    int currentCaptureRow() const;

    QTableView* createDeviceTable();
    QTableView* createPropertyTable();
    QWidget* createCaptureRecordPanel();
    QTableView* createCaptureTable(QSortFilterProxyModel* proxyModel);
    QSortFilterProxyModel* createCaptureProxy(const QString& plateStateFilter);
    void configureTableView(QTableView* table) const;
    void updateVideoWidgets();
    void updateCaptureControls();
    QTableView* currentCaptureTable() const;
    QSortFilterProxyModel* currentCaptureProxy() const;
    CaptureRecordFilter currentCaptureFilter() const;
    CaptureAssetKind storageKindForRecord(const CaptureRecord& record) const;
    rv1126b::EventQuery currentEventQuery() const;
    qint64 recentRangeStartEpochMs() const;
    const rv1126b::VehicleEvent* currentVehicleEvent() const;
    bool selectedEventDeviceOnline() const;

    QAction* addDeviceAction_ = nullptr;
    QAction* connectAction_ = nullptr;
    QAction* disconnectAction_ = nullptr;
    QAction* configAction_ = nullptr;
    QAction* captureAction_ = nullptr;
    QAction* rebootAction_ = nullptr;
    QAction* syncTimeAction_ = nullptr;
    QAction* deleteCaptureAction_ = nullptr;
    QAction* clearCaptureAction_ = nullptr;
    QAction* globalSettingsAction_ = nullptr;
    QAction* refreshAction_ = nullptr;
    QAction* exportAction_ = nullptr;

    QTableView* deviceTable_ = nullptr;
    QTableView* propertyTable_ = nullptr;
    QTabWidget* captureTabs_ = nullptr;
    QTableView* allCaptureTable_ = nullptr;
    QTableView* validCaptureTable_ = nullptr;
    QTableView* unknownCaptureTable_ = nullptr;
    QSortFilterProxyModel* allCaptureProxy_ = nullptr;
    QSortFilterProxyModel* validCaptureProxy_ = nullptr;
    QSortFilterProxyModel* unknownCaptureProxy_ = nullptr;
    QToolButton* pauseCaptureButton_ = nullptr;
    QSpinBox* pauseSecondsSpin_ = nullptr;
    QLabel* pendingCaptureLabel_ = nullptr;
    QComboBox* eventModeCombo_ = nullptr;
    QComboBox* eventDeviceScopeCombo_ = nullptr;
    QComboBox* eventTimeRangeCombo_ = nullptr;
    QWidget* historyFilterWidget_ = nullptr;
    QLineEdit* historyPlateEdit_ = nullptr;
    QCheckBox* historyTimeRangeCheck_ = nullptr;
    QDateTimeEdit* historyStartEdit_ = nullptr;
    QDateTimeEdit* historyEndEdit_ = nullptr;
    QToolButton* historyPreviousButton_ = nullptr;
    QToolButton* historyNextButton_ = nullptr;
    QLabel* historyPageLabel_ = nullptr;
    VideoWidget* livePreview_ = nullptr;
    LivePreviewPanel* livePreviewPanel_ = nullptr;
    VideoWidget* snapshotPreview_ = nullptr;
    QSplitter* previewSplitter_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* persistentStatusLabel_ = nullptr;
    QTimer* capturePauseTimer_ = nullptr;
    QMenu* deviceContextMenu_ = nullptr;

    DeviceManager* deviceManager_ = nullptr;
    CaptureRecordService* captureService_ = nullptr;
    SystemSettingsService* systemSettingsService_ = nullptr;
    MaintenanceController* maintenanceController_ = nullptr;
    rv1126b::DeviceIntegrationController* integrationController_ = nullptr;
    rv1126b::DeviceOperationsController* operationsController_ = nullptr;
    rv1126b::EventViewController* eventController_ = nullptr;
    rv1126b::IRtspPlayer* rtspPlayer_ = nullptr;
    rv1126b::EmbeddedFtpReceiveServer* ftpReceiveServer_ = nullptr;
    std::function<rv1126b::EventSyncService*(const QString&)> eventSyncForDevice_;
    rv1126b::BoardApiResolver boardApiForDevice_;
    rv1126b::EvidenceCacheMaintenanceService* evidenceMaintenance_ = nullptr;
    QString evidenceRootPath_;
    std::function<bool(const QString&)> switchEvidenceRoot_;
    DeviceTableModel* deviceModel_ = nullptr;
    DevicePropertyModel* propertyModel_ = nullptr;
    CaptureRecordTableModel* captureModel_ = nullptr;
    SystemSettings currentSystemSettings_;
    CaptureStorageService storageService_;
    int pendingCaptureCount_ = 0;
    int historyPage_ = 0;
    int lastHistoryRowCount_ = 0;
    bool mockMode_ = false;
    bool shutdownStarted_ = false;
    QHash<QString, QPointer<QProgressDialog>> bulkDialogs_;
};
