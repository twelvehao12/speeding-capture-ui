#pragma once

#include "../rv1126b/application/DeviceOperationsController.h"

#include <QDialog>
#include <QVector>

#include <functional>
#include <optional>

class QCheckBox;
class QComboBox;
class QDateTimeEdit;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QJsonObject;
class QSpinBox;
class QTabWidget;
class QTableWidget;
class QTimer;

namespace rv1126b {
class EmbeddedFtpReceiveServer;
class EventSyncService;
class IBoardApiClient;
}

class Rv1126bDeviceManagementDialog final : public QDialog
{
    Q_OBJECT

public:
    enum class InitialPage { Evidence, Time, EventSync, Isp, FtpConfig, FtpTasks };

    explicit Rv1126bDeviceManagementDialog(
        const QString& deviceId,
        rv1126b::DeviceOperationsController* controller,
        InitialPage initialPage = InitialPage::Evidence,
        QWidget* parent = nullptr,
        bool deviceOnline = true,
        rv1126b::EmbeddedFtpReceiveServer* ftpReceiveServer = nullptr,
        const QString& localFtpRootPath = QString(),
        rv1126b::EventSyncService* eventSyncService = nullptr,
        const QString& evidenceRootPath = QString(),
        const QString& deviceEndpointText = QString(),
        rv1126b::IBoardApiClient* boardApi = nullptr,
        std::function<rv1126b::IBoardApiClient*(const QString&)> boardApiForHost = {},
        std::function<QString(const QString&)> deviceIdForHost = {},
        const QString& storageRootPath = QString(),
        std::function<bool(const QString&)> storageRootChangeHandler = {});
    ~Rv1126bDeviceManagementDialog() override;

protected:
    void reject() override;

private:
    QWidget* createEvidencePage();
    QWidget* createTimePage();
    QWidget* createEventSyncPage();
    QWidget* createIspPage();
    QWidget* createFtpConfigPage();
    QWidget* createFtpTasksPage();
    void connectController();
    void showError(const QString& code, const QString& message);
    void applyEvidence(const rv1126b::EvidenceConfigDto& config);
    void applyTime(const rv1126b::TimeStatusDto& time);
    void applyFtpConfig(const rv1126b::FtpConfigSnapshotDto& config);
    void addFtpTargetRow(const std::optional<rv1126b::FtpTargetSnapshotDto>& target = std::nullopt);
    rv1126b::FtpConfigUpdate collectFtpConfig() const;
    bool validateFtpRowsInline();
    void clearPasswordEditors();
    void applyFtpControl(const rv1126b::FtpControlDto& control);
    void applyTaskPage(const rv1126b::FtpTaskPageDto& page, const QString& requestedCursor);
    void applyTaskDetail(const rv1126b::FtpTaskDetailDto& detail);
    void applyLocalTaskSnapshots(const QVector<rv1126b::StoredFtpTask>& tasks);
    void applyLocalTaskDetail(const rv1126b::StoredFtpTask& task);
    void createTask();
    void refreshTasks();
    void updateTaskRefreshState();
    void startLocalFtpReceiver();
    void stopLocalFtpReceiver();
    void applyLocalFtpTarget(bool saveAndEnable);
    void updateLocalFtpReceiverState();
    void refreshIspConfig();
    void saveCurrentIspConfig();
    void clearIspConfig();
    void applyIspConfigJson(const QJsonObject& config);
    QString defaultLocalFtpAddress() const;
    QString defaultLocalFtpTargetId(const QString& host) const;
    int localFtpTargetRow() const;
    void syncLocalFtpTargetIdFromHost();
    void writeLocalFtpTargetRow();
    QString defaultEventStorageRoot() const;
    QString currentEventStorageRoot() const;
    QString currentEventExportRoot() const;
    QStringList eventExportHosts() const;
    void browseEventStorageRoot();
    void saveEventStorageRoot();
    void startEventExport();
    void requestEventExportPage(const std::optional<QString>& cursor = std::nullopt);
    void handleEventExportPage(rv1126b::ApiResult<rv1126b::EventPageDto> result);
    void startNextEventExportTarget();
    void exportNextEvent();
    void handleExportEventDetail(
        const rv1126b::EventSummaryDto& summary,
        rv1126b::ApiResult<rv1126b::EventDetailDto> result);
    void downloadNextExportFile();
    void handleExportFileDownloaded(rv1126b::ApiResult<rv1126b::EvidenceDownloadResult> result);
    void finishEventExport();
    void showRevisionConflict(const rv1126b::FtpConfigSnapshotDto& remote,
                              const rv1126b::FtpConfigUpdate& local,
                              const QStringList& passwordTargetIds);
    QString conflictSummary(const rv1126b::FtpConfigSnapshotDto& remote,
                            const rv1126b::FtpConfigUpdate& local) const;

    QString deviceId_;
    rv1126b::DeviceOperationsController* controller_ = nullptr;
    rv1126b::EmbeddedFtpReceiveServer* ftpReceiveServer_ = nullptr;
    rv1126b::EventSyncService* eventSyncService_ = nullptr;
    rv1126b::IBoardApiClient* boardApi_ = nullptr;
    rv1126b::IBoardApiClient* currentExportApi_ = nullptr;
    std::function<rv1126b::IBoardApiClient*(const QString&)> boardApiForHost_;
    std::function<QString(const QString&)> deviceIdForHost_;
    std::function<bool(const QString&)> storageRootChangeHandler_;
    QString localFtpRootPath_;
    QString evidenceRootPath_;
    QString storageRootPath_;
    QString deviceEndpointText_;
    QTabWidget* tabs_ = nullptr;
    QLabel* globalMessage_ = nullptr;

    QLineEdit* siteNameEdit_ = nullptr;
    QLineEdit* roadDirectionEdit_ = nullptr;
    QSpinBox* speedLimitSpin_ = nullptr;
    QLineEdit* statusTextEdit_ = nullptr;
    QLineEdit* codeTextEdit_ = nullptr;
    QLabel* evidenceStatus_ = nullptr;

    QLabel* utcTimeLabel_ = nullptr;
    QLabel* localTimeLabel_ = nullptr;
    QLabel* sourceEpochLabel_ = nullptr;
    QLabel* offsetLabel_ = nullptr;
    QLabel* timeQualityLabel_ = nullptr;
    QLabel* ntpStatusLabel_ = nullptr;
    QLabel* timeWriteLabel_ = nullptr;
    QPushButton* syncTimeButton_ = nullptr;
    bool timeSetEnabled_ = false;

    QLabel* eventSyncStatus_ = nullptr;
    QLabel* eventSyncModeLabel_ = nullptr;
    QLabel* eventSyncRootLabel_ = nullptr;
    QPushButton* eventSyncStartButton_ = nullptr;
    QPushButton* eventSyncStopButton_ = nullptr;
    QPushButton* eventSyncPollButton_ = nullptr;
    QPushButton* eventSyncAllButton_ = nullptr;
    QPushButton* eventSyncFromNowButton_ = nullptr;
    QLineEdit* eventSyncHostsEdit_ = nullptr;
    QSpinBox* eventExportDaysSpin_ = nullptr;
    QLineEdit* eventStorageRootEdit_ = nullptr;
    QPushButton* eventStorageBrowseButton_ = nullptr;
    QPushButton* eventStorageSaveButton_ = nullptr;
    QComboBox* eventExportRangeCombo_ = nullptr;
    QCheckBox* exportEvidenceCheck_ = nullptr;
    QCheckBox* exportSnapshotCheck_ = nullptr;
    QCheckBox* exportFormalCheck_ = nullptr;
    QCheckBox* exportOcrCheck_ = nullptr;
    QCheckBox* exportDetailCheck_ = nullptr;
    QCheckBox* exportSummaryCheck_ = nullptr;
    QCheckBox* exportTrackMetaCheck_ = nullptr;
    QLabel* eventExportStatus_ = nullptr;
    QPushButton* eventExportButton_ = nullptr;
    struct ExportFile {
        QString url;
        QString finalPath;
        QString partPath;
        bool optional = true;
    };
    QVector<rv1126b::EventSummaryDto> exportEvents_;
    QVector<ExportFile> exportFiles_;
    QStringList exportTargetHosts_;
    QString exportRunRoot_;
    QString exportTargetRoot_;
    QString exportTargetDeviceId_;
    QString exportTargetHost_;
    int exportEventIndex_ = 0;
    int exportFileIndex_ = 0;
    int exportTargetIndex_ = 0;
    int exportSucceeded_ = 0;
    int exportFailed_ = 0;
    bool exportInFlight_ = false;

    QLabel* ispStatus_ = nullptr;
    QLabel* ispCurrentLabel_ = nullptr;
    QLabel* ispPersistedLabel_ = nullptr;
    QPushButton* ispRefreshButton_ = nullptr;
    QPushButton* ispSaveCurrentButton_ = nullptr;
    QPushButton* ispClearButton_ = nullptr;

    QLabel* ftpRevisionLabel_ = nullptr;
    QSpinBox* retryMaxSpin_ = nullptr;
    QSpinBox* retryIntervalSpin_ = nullptr;
    QSpinBox* connectTimeoutSpin_ = nullptr;
    QSpinBox* transferTimeoutSpin_ = nullptr;
    QSpinBox* scanIntervalSpin_ = nullptr;
    QTableWidget* ftpTargetsTable_ = nullptr;
    QCheckBox* autoEnabledCheck_ = nullptr;
    QComboBox* autoScopeCombo_ = nullptr;
    QLabel* ftpStatus_ = nullptr;
    QPushButton* saveFtpButton_ = nullptr;
    QString ftpRevision_;
    QLineEdit* localFtpRootEdit_ = nullptr;
    QLineEdit* localFtpHostEdit_ = nullptr;
    QLineEdit* localFtpTargetIdEdit_ = nullptr;
    QSpinBox* localFtpPortSpin_ = nullptr;
    QSpinBox* localFtpPassiveStartSpin_ = nullptr;
    QSpinBox* localFtpPassiveEndSpin_ = nullptr;
    QLineEdit* localFtpUserEdit_ = nullptr;
    QLineEdit* localFtpPasswordEdit_ = nullptr;
    QLabel* localFtpStatus_ = nullptr;
    QPushButton* localFtpStartButton_ = nullptr;
    QPushButton* localFtpStopButton_ = nullptr;
    bool localFtpTargetIdAuto_ = true;
    std::optional<bool> pendingLocalFtpApply_;

    QDateTimeEdit* taskStartEdit_ = nullptr;
    QDateTimeEdit* taskEndEdit_ = nullptr;
    QListWidget* taskTargets_ = nullptr;
    QTableWidget* taskTable_ = nullptr;
    QTableWidget* taskDetailTable_ = nullptr;
    QPushButton* previousTasksButton_ = nullptr;
    QPushButton* nextTasksButton_ = nullptr;
    QPushButton* retryTaskButton_ = nullptr;
    QPushButton* createTaskButton_ = nullptr;
    QLabel* taskPageLabel_ = nullptr;
    QTimer* taskRefreshTimer_ = nullptr;
    QStringList taskPageCursors_ {QString()};
    int taskPageIndex_ = 0;
    std::optional<QString> nextTaskCursor_;
    QString selectedTaskId_;
    rv1126b::FtpTaskState selectedTaskState_ = rv1126b::FtpTaskState::Unknown;
    QVector<rv1126b::StoredFtpTask> localTaskSnapshots_;
    bool deviceOnline_ = true;
};

