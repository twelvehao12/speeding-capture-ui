#include "MainWindow.h"

#include "../models/table_models/CaptureRecordTableModel.h"
#include "../models/table_models/DevicePropertyModel.h"
#include "../models/table_models/DeviceTableModel.h"
#include "../services/CaptureRecordService.h"
#include "../services/DeviceManager.h"
#include "../services/MaintenanceController.h"
#include "../services/SystemSettingsService.h"
#include "../rv1126b/services/EvidenceCacheMaintenanceService.h"
#include "../video/VideoWidget.h"
#include "DeviceConfigDialog.h"
#include "DeviceDiscoveryDialog.h"
#include "LivePreviewPanel.h"
#include "Rv1126bDeviceManagementDialog.h"
#include "SystemSettingsDialog.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QCloseEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDateTimeEdit>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPoint>
#include <QProcess>
#include <QProgressDialog>
#include <QSize>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QSplitter>
#include <QSettings>
#include <QStatusBar>
#include <QStorageInfo>
#include <QStyle>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QUrl>

#include <optional>
#include <algorithm>

namespace
{

    DeviceConnectionState uiConnectionState(rv1126b::DeviceSessionState state)
    {
        using rv1126b::DeviceSessionState;
        switch (state)
        {
        case DeviceSessionState::Connecting:
            return DeviceConnectionState::Connecting;
        case DeviceSessionState::Online:
            return DeviceConnectionState::Online;
        case DeviceSessionState::Degraded:
            return DeviceConnectionState::Degraded;
        case DeviceSessionState::AuthenticationFailed:
            return DeviceConnectionState::AuthenticationFailed;
        case DeviceSessionState::Disconnecting:
            return DeviceConnectionState::Disconnecting;
        case DeviceSessionState::Disconnected:
        default:
            return DeviceConnectionState::Offline;
        }
    }

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : MainWindow(MainWindowDependencies{}, parent)
{
}

MainWindow::MainWindow(MainWindowDependencies dependencies, QWidget *parent)
    : QMainWindow(parent), deviceManager_(new DeviceManager(this)), captureService_(dependencies.mockMode ? new CaptureRecordService(this) : nullptr), systemSettingsService_(new SystemSettingsService(this)), maintenanceController_(new MaintenanceController(this)), integrationController_(dependencies.mockMode
                                                                                                                                                                                                                                                                                                    ? nullptr
                                                                                                                                                                                                                                                                                                    : new rv1126b::DeviceIntegrationController(
                                                                                                                                                                                                                                                                                                          {dependencies.discovery,
                                                                                                                                                                                                                                                                                                           dependencies.fleet,
                                                                                                                                                                                                                                                                                                           dependencies.secretStore,
                                                                                                                                                                                                                                                                                                           dependencies.player,
                                                                                                                                                                                                                                                                                                           dependencies.directProbe,
                                                                                                                                                                                                                                                                                                           dependencies.forgetDevice},
                                                                                                                                                                                                                                                                                                          this)),
      operationsController_(dependencies.mockMode
                                ? nullptr
                                : new rv1126b::DeviceOperationsController(
                                      {dependencies.boardApiForDevice,
                                       dependencies.ftpServiceForDevice,
                                       dependencies.ftpTaskSnapshotForDevice},
                                      this)),
      eventController_(dependencies.mockMode
                           ? nullptr
                           : new rv1126b::EventViewController(
                                 {dependencies.eventRepository, dependencies.evidenceCache}, this)),
      rtspPlayer_(dependencies.player), ftpReceiveServer_(dependencies.ftpReceiveServer), eventSyncForDevice_(std::move(dependencies.eventSyncForDevice)), boardApiForDevice_(dependencies.boardApiForDevice), evidenceMaintenance_(dependencies.evidenceMaintenance), evidenceRootPath_(std::move(dependencies.evidenceRootPath)), switchEvidenceRoot_(std::move(dependencies.switchEvidenceRoot)), deviceModel_(new DeviceTableModel(this)), propertyModel_(new DevicePropertyModel(this)), captureModel_(new CaptureRecordTableModel(this)), currentSystemSettings_(systemSettingsService_->load()), storageService_(currentSystemSettings_.storage), mockMode_(dependencies.mockMode)
{
    deviceModel_->setRealMode(!mockMode_);
    setWindowTitle(QStringLiteral("车牌识别雷达测速摄像机管理软件"));
    resize(1360, 820);
    setMinimumSize(1024, 680);

    createActions();
    createToolBar();
    createCentralLayout();
    createDeviceContextMenu();
    createStatusBar();
    if (evidenceMaintenance_) connect(evidenceMaintenance_, &rv1126b::EvidenceCacheMaintenanceService::cleanupFinished,
        this, [this](const rv1126b::EvidenceCacheCleanupResult& result) {
            statusBar()->showMessage(QStringLiteral("图片缓存维护完成：删除 %1 个文件，释放 %2 MB")
                .arg(result.deletedFiles).arg(result.deletedBytes / 1024 / 1024), 4000);
        });
    applySystemSettings(true);

    if (captureService_)
    {
        if (!captureService_->initialize())
        {
            statusBar()->showMessage(QStringLiteral("抓拍记录库初始化失败：%1").arg(captureService_->lastError()), 5000);
        }
        else
        {
            refreshCaptureRecords();
        }
    }
    else
    {
        captureModel_->setVehicleEvents({});
    }

    connectDeviceManager();
    connectIntegrationController();
    connectEventController();
    for (rv1126b::EventSyncService *service : dependencies.eventSyncServices)
    {
        attachEventSyncService(service);
    }
    populateInitialData();

    if (mockMode_ && currentSystemSettings_.ui.autoConnectOnStart)
    {
        const int connected = deviceManager_->connectAllDevices();
        statusBar()->showMessage(QStringLiteral("已自动连接 %1 台设备").arg(connected), 2500);
    }

    configureMaintenanceController();
    maintenanceController_->start();

    if (!mockMode_ && integrationController_ && !integrationController_->networkServicesAvailable())
    {
        statusBar()->showMessage(QStringLiteral("真实设备网络服务尚未装配；可使用 --mock 启动开发模式"), 8000);
    }
    if (!mockMode_ && eventController_ && !eventController_->servicesAvailable())
    {
        statusBar()->showMessage(QStringLiteral("真实事件仓储和图片缓存服务尚未装配；视频预览仍可使用"), 8000);
    }
    updateCaptureControls();
}

void MainWindow::attachEventSyncService(rv1126b::EventSyncService *service)
{
    if (eventController_)
        eventController_->attachSyncService(service);
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() != QEvent::WindowStateChange || !snapshotPreview_)
    {
        return;
    }

    const bool stopVideo = currentSystemSettings_.ui.stopVideoQueryWhenMinimized && isMinimized();
    if (!mockMode_ && integrationController_)
    {
        integrationController_->setPlaybackSuspended(stopVideo);
    }
    if (livePreview_)
    {
        livePreview_->setUpdatesEnabled(!stopVideo);
    }
    if (livePreviewPanel_)
    {
        livePreviewPanel_->setUpdatesEnabled(!stopVideo);
    }
    snapshotPreview_->setUpdatesEnabled(!stopVideo);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!shutdownStarted_)
    {
        shutdownStarted_ = true;
        if (eventController_)
        {
            eventController_->shutdown();
        }
        if (operationsController_)
        {
            operationsController_->shutdown();
        }
        if (integrationController_)
        {
            integrationController_->shutdown();
        }
        if (mockMode_)
        {
            deviceManager_->disconnectAllDevices();
        }
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::createActions()
{
    const auto icon = [this](QStyle::StandardPixmap pixmap)
    {
        return style()->standardIcon(pixmap);
    };

    addDeviceAction_ = new QAction(icon(QStyle::SP_FileDialogNewFolder),
                                   mockMode_ ? QStringLiteral("添加模拟设备")
                                             : QStringLiteral("搜索设备"),
                                   this);
    connect(addDeviceAction_, &QAction::triggered, this, &MainWindow::addDevice);

    connectAction_ = new QAction(icon(QStyle::SP_DialogApplyButton), QStringLiteral("连接"), this);
    connect(connectAction_, &QAction::triggered, this, &MainWindow::connectSelectedDevice);

    disconnectAction_ = new QAction(icon(QStyle::SP_DialogCancelButton), QStringLiteral("断开"), this);
    connect(disconnectAction_, &QAction::triggered, this, &MainWindow::disconnectSelectedDevice);

    configAction_ = new QAction(icon(QStyle::SP_FileDialogDetailedView), QStringLiteral("设备配置"), this);
    connect(configAction_, &QAction::triggered, this, &MainWindow::openDeviceConfig);

    captureAction_ = new QAction(icon(QStyle::SP_ComputerIcon), QStringLiteral("手动抓拍"), this);
    connect(captureAction_, &QAction::triggered, this, &MainWindow::triggerCapture);

    rebootAction_ = new QAction(icon(QStyle::SP_BrowserReload), QStringLiteral("重启相机"), this);
    connect(rebootAction_, &QAction::triggered, this, &MainWindow::rebootSelectedDevice);

    syncTimeAction_ = new QAction(icon(QStyle::SP_DialogApplyButton), QStringLiteral("同步时间"), this);
    connect(syncTimeAction_, &QAction::triggered, this, &MainWindow::syncSelectedDeviceTime);

    if (!mockMode_)
    {
        configAction_->setEnabled(false);
        captureAction_->setEnabled(false);
        rebootAction_->setEnabled(false);
        syncTimeAction_->setEnabled(false);
        captureAction_->setVisible(false);
        rebootAction_->setVisible(false);
    }

    deleteCaptureAction_ = new QAction(icon(QStyle::SP_TrashIcon), QStringLiteral("删除记录"), this);
    connect(deleteCaptureAction_, &QAction::triggered, this, &MainWindow::deleteSelectedCapture);

    clearCaptureAction_ = new QAction(icon(QStyle::SP_DialogResetButton), QStringLiteral("清空记录"), this);
    clearCaptureAction_->setEnabled(!mockMode_);
    connect(clearCaptureAction_, &QAction::triggered, this, &MainWindow::clearCaptureRecords);

    globalSettingsAction_ = new QAction(icon(QStyle::SP_FileDialogContentsView), QStringLiteral("全局设置"), this);
    connect(globalSettingsAction_, &QAction::triggered, this, &MainWindow::openGlobalSettings);

    refreshAction_ = new QAction(icon(QStyle::SP_BrowserReload), QStringLiteral("刷新记录"), this);
    connect(refreshAction_, &QAction::triggered, this, &MainWindow::refreshCaptureRecords);

    exportAction_ = new QAction(icon(QStyle::SP_DriveFDIcon), QStringLiteral("导出记录"), this);
    connect(exportAction_, &QAction::triggered, this, &MainWindow::exportCaptureRecords);
}

void MainWindow::createToolBar()
{
    auto *toolbar = addToolBar(QStringLiteral("主工具栏"));
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(20, 20));
    toolbar->addAction(addDeviceAction_);
    toolbar->addAction(connectAction_);
    toolbar->addAction(disconnectAction_);
    toolbar->addSeparator();
    toolbar->addAction(configAction_);
    toolbar->addAction(captureAction_);
    toolbar->addAction(rebootAction_);
    toolbar->addAction(syncTimeAction_);
    toolbar->addSeparator();
    toolbar->addAction(deleteCaptureAction_);
    toolbar->addAction(clearCaptureAction_);
    toolbar->addSeparator();
    toolbar->addAction(refreshAction_);
    toolbar->addAction(exportAction_);
    toolbar->addSeparator();
    toolbar->addAction(globalSettingsAction_);
}

void MainWindow::createCentralLayout()
{
    deviceTable_ = createDeviceTable();
    propertyTable_ = createPropertyTable();
    QWidget *capturePanel = createCaptureRecordPanel();
    if (mockMode_)
    {
        livePreview_ = new VideoWidget(VideoWidget::Mode::Live, this);
    }
    else
    {
        livePreviewPanel_ = new LivePreviewPanel(rtspPlayer_, this);
    }
    snapshotPreview_ = new VideoWidget(VideoWidget::Mode::Snapshot, this);

    auto *leftSplitter = new QSplitter(Qt::Vertical, this);
    leftSplitter->addWidget(deviceTable_);
    leftSplitter->addWidget(propertyTable_);
    leftSplitter->setStretchFactor(0, 3);
    leftSplitter->setStretchFactor(1, 2);

    previewSplitter_ = new QSplitter(Qt::Horizontal, this);
    previewSplitter_->addWidget(mockMode_ ? static_cast<QWidget *>(livePreview_)
                                          : static_cast<QWidget *>(livePreviewPanel_));
    previewSplitter_->addWidget(snapshotPreview_);
    previewSplitter_->setStretchFactor(0, 3);
    previewSplitter_->setStretchFactor(1, 2);

    auto *rightSplitter = new QSplitter(Qt::Vertical, this);
    rightSplitter->addWidget(previewSplitter_);
    rightSplitter->addWidget(capturePanel);
    rightSplitter->setStretchFactor(0, 5);
    rightSplitter->setStretchFactor(1, 2);

    auto *mainSplitter = new QSplitter(Qt::Horizontal, this);
    mainSplitter->addWidget(leftSplitter);
    mainSplitter->addWidget(rightSplitter);
    mainSplitter->setStretchFactor(0, 1);
    mainSplitter->setStretchFactor(1, 4);
    mainSplitter->setCollapsible(0, false);
    mainSplitter->setCollapsible(1, false);

    setCentralWidget(mainSplitter);
}

void MainWindow::createStatusBar()
{
    statusLabel_ = new QLabel(this);
    statusBar()->addWidget(statusLabel_, 1);
    persistentStatusLabel_ = new QLabel(this);
    persistentStatusLabel_->setObjectName(QStringLiteral("persistentRuntimeStatus"));
    persistentStatusLabel_->setStyleSheet(QStringLiteral("color: #b42318; font-weight: bold;"));
    persistentStatusLabel_->setVisible(false);
    statusBar()->addPermanentWidget(persistentStatusLabel_);
    // statusBar()->showMessage(QStringLiteral("视频预览系统已启动"));
    updateStatusText();
}

void MainWindow::createDeviceContextMenu()
{
    deviceContextMenu_ = new QMenu(this);
    deviceContextMenu_->addAction(connectAction_);
    deviceContextMenu_->addAction(disconnectAction_);
    deviceContextMenu_->addSeparator();
    deviceContextMenu_->addAction(configAction_);
    deviceContextMenu_->addAction(captureAction_);
    deviceContextMenu_->addSeparator();
    deviceContextMenu_->addAction(rebootAction_);
    deviceContextMenu_->addAction(syncTimeAction_);
    deviceContextMenu_->addAction(QStringLiteral("网络信息"), this, &MainWindow::showSelectedNetworkInfo);
    deviceContextMenu_->addAction(QStringLiteral("打开本地文件夹"), this, &MainWindow::openSelectedLocalFolder);
}

void MainWindow::connectDeviceManager()
{
    connect(deviceManager_, &DeviceManager::deviceAdded, this, &MainWindow::handleDeviceAdded);
    connect(deviceManager_, &DeviceManager::deviceStatusChanged, this, &MainWindow::handleDeviceStatusChanged);
    connect(deviceManager_, &DeviceManager::deviceConfigChanged, this, &MainWindow::handleDeviceConfigChanged);
    connect(deviceManager_, &DeviceManager::captureGenerated, this, &MainWindow::handleCaptureGenerated);
    connect(deviceManager_, &DeviceManager::errorOccurred, this, &MainWindow::handleDeviceError);
}

void MainWindow::connectIntegrationController()
{
    if (!integrationController_)
    {
        return;
    }
    connect(integrationController_, &rv1126b::DeviceIntegrationController::discoveredDeviceUpserted,
            this, &MainWindow::handleDiscoveredDevice);
    connect(integrationController_, &rv1126b::DeviceIntegrationController::sessionChanged,
            this, &MainWindow::handleSessionChanged);
    connect(integrationController_, &rv1126b::DeviceIntegrationController::selectedVideoDeviceChanged,
            this, &MainWindow::handleSelectedVideoDeviceChanged);
    connect(integrationController_, &rv1126b::DeviceIntegrationController::userError,
            this, &MainWindow::handleIntegrationError);
    connect(integrationController_, &rv1126b::DeviceIntegrationController::deviceForgotten,
            this, &MainWindow::handleDeviceForgotten);
    if (livePreviewPanel_)
    {
        connect(integrationController_, &rv1126b::DeviceIntegrationController::playbackStateChanged,
                livePreviewPanel_, &LivePreviewPanel::setPlaybackState);
        connect(integrationController_, &rv1126b::DeviceIntegrationController::playbackError,
                livePreviewPanel_, &LivePreviewPanel::setPlaybackError);
        connect(livePreviewPanel_, &LivePreviewPanel::streamRoleChanged,
                integrationController_, &rv1126b::DeviceIntegrationController::setStreamRole);
        connect(livePreviewPanel_, &LivePreviewPanel::previewRestartRequested,
                integrationController_, &rv1126b::DeviceIntegrationController::restartSelectedStream);
        if (rtspPlayer_)
        {
            livePreviewPanel_->setPlaybackState(rtspPlayer_->state());
        }
    }
    for (const rv1126b::DeviceSessionSnapshot &snapshot : integrationController_->sessionSnapshots())
    {
        handleSessionChanged(snapshot);
    }
}

void MainWindow::connectEventController()
{
    if (!eventController_)
        return;

    connect(eventController_, &rv1126b::EventViewController::eventsReset,
            this, [this](const QVector<rv1126b::VehicleEvent> &events)
            {
                captureModel_->setVehicleEvents(events);
                pendingCaptureCount_ = 0;
                if (snapshotPreview_) snapshotPreview_->clearVehicleEvent();
                updateCaptureControls(); });
    connect(eventController_, &rv1126b::EventViewController::eventsUpserted,
            this, [this](const QVector<rv1126b::VehicleEvent>& events)
            {
                captureModel_->upsertVehicleEvents(events, currentSystemSettings_.ui.captureListMaxRows);
                updateCaptureControls(); });
    connect(eventController_, &rv1126b::EventViewController::eventDeleted,
            this, [this](const rv1126b::EventIdentity &identity)
            {
                captureModel_->removeVehicleEvent(identity);
                if (snapshotPreview_) snapshotPreview_->clearVehicleEvent();
                updateCaptureControls(); });
    connect(eventController_, &rv1126b::EventViewController::evidenceChanged,
            this, [this](const rv1126b::EvidenceCacheEntry &entry)
            {
                captureModel_->setEvidenceState(entry);
                const rv1126b::VehicleEvent* event = currentVehicleEvent();
                if (event && event->identity == entry.identity && snapshotPreview_) {
                    snapshotPreview_->setEvidenceState(entry, selectedEventDeviceOnline());
                } });
    connect(eventController_, &rv1126b::EventViewController::pendingChangeCountChanged,
            this, [this](int count)
            {
                pendingCaptureCount_ = count;
                updateCaptureControls(); });
    connect(eventController_, &rv1126b::EventViewController::queryFinished,
            this, [this](int rowCount)
            {
                lastHistoryRowCount_ = rowCount;
                if (historyPreviousButton_) historyPreviousButton_->setEnabled(historyPage_ > 0);
                if (historyNextButton_) historyNextButton_->setEnabled(
                    eventModeCombo_ && eventModeCombo_->currentIndex() == 1
                    && rowCount == rv1126b::EventViewController::HistoryPageSize);
                if (historyPageLabel_) historyPageLabel_->setText(QStringLiteral("第 %1 页").arg(historyPage_ + 1)); });
    connect(eventController_, &rv1126b::EventViewController::deleteFinished,
            this, [this](int deleted, int failed)
            {
                statusBar()->showMessage(QStringLiteral("本地删除完成：成功 %1 条，失败 %2 条；板端保留事件可能再次同步")
                                             .arg(deleted).arg(failed), 6000);
                refreshCaptureRecords(); });
    connect(eventController_, &rv1126b::EventViewController::exportFinished,
            this, [this](const QString &path, int count)
            { statusBar()->showMessage(QStringLiteral("已导出 %1 条真实事件：%2").arg(count).arg(path), 5000); });
    connect(eventController_, &rv1126b::EventViewController::bulkProgress, this,
        [this](const QString& operation, int completed) {
            auto dialog = bulkDialogs_.value(operation);
            if (!dialog) {
                dialog = new QProgressDialog(this);
                dialog->setWindowTitle(operation == QLatin1String("export") ? QStringLiteral("导出记录") : QStringLiteral("清空记录"));
                dialog->setCancelButtonText(QStringLiteral("取消"));
                dialog->setRange(0, 0);
                dialog->setMinimumDuration(500);
                dialog->setValue(0);
                bulkDialogs_.insert(operation, dialog);
                connect(dialog, &QProgressDialog::canceled, this, [this, operation] {
                    if (operation == QLatin1String("export")) eventController_->cancelExport();
                    else eventController_->cancelClear();
                });
            }
            if (completed % 100 == 0) dialog->setLabelText(QStringLiteral("已处理 %1 条记录").arg(completed));
        });
    connect(eventController_, &rv1126b::EventViewController::bulkFinished, this,
        [this](const QString& operation, bool cancelled) {
            if (auto dialog = bulkDialogs_.take(operation)) { dialog->hide(); dialog->deleteLater(); }
            if (cancelled) statusBar()->showMessage(QStringLiteral("操作已取消，已完成的处理保留"), 4000);
        });
    connect(eventController_, &rv1126b::EventViewController::userError,
            this, &MainWindow::handleIntegrationError);
    connect(eventController_, &rv1126b::EventViewController::syncHealthy,
            this, [this](const QString&) {
                if (!persistentStatusLabel_ || !persistentStatusLabel_->isVisible()) {
                    return;
                }
                const QString text = persistentStatusLabel_->text();
                if (text.contains(QStringLiteral("事件")) || text.contains(QStringLiteral("event"))) {
                    persistentStatusLabel_->clear();
                    persistentStatusLabel_->setVisible(false);
                }
            });
}

void MainWindow::populateInitialData()
{
    if (mockMode_)
    {
        deviceManager_->seedMockDevices(3);
        selectDeviceRow(0);
    }
    else if (integrationController_)
    {
        const QVector<rv1126b::DeviceSessionSnapshot> known = integrationController_->sessionSnapshots();
        for (const auto &snapshot : known)
            handleSessionChanged(snapshot);
        if (!known.isEmpty())
            selectDeviceRow(0);
        QString preferredVideo = currentSystemSettings_.ui.lastSelectedVideoDeviceId;
        const bool preferredKnown = std::any_of(known.cbegin(), known.cend(),
                                                [&preferredVideo](const auto &snapshot)
                                                {
                                                    return snapshot.profile.deviceId == preferredVideo;
                                                });
        if (!preferredKnown && !known.isEmpty())
            preferredVideo = known.first().profile.deviceId;
        if (currentSystemSettings_.ui.autoConnectOnStart)
        {
            int started = 0;
            for (const auto &snapshot : known)
            {
                if (started >= rv1126b::DeviceFleetService::MaxConcurrentDataSessions)
                    break;
                if (integrationController_->connectKnownDevice(snapshot.profile.deviceId))
                    ++started;
            }
            statusBar()->showMessage(QStringLiteral("已启动 %1 台历史设备的自动连接").arg(started), 3500);
        }
        if (!preferredVideo.isEmpty())
            integrationController_->selectVideoDevice(preferredVideo);
    }
    updateDeviceProperties();
    updateStatusText();
}

void MainWindow::updateStatusText()
{
    if (!mockMode_ && deviceModel_->deviceCount() == 0)
    {
        statusLabel_->setText(QStringLiteral("尚无已知设备 | 点击“搜索设备”进行广播发现或手工输入 IPv4"));
        return;
    }
    else
    {
        const int visibleOrPendingCaptureCount = captureModel_->recordCount() + pendingCaptureCount_;
        statusLabel_->setText(QStringLiteral("就绪 | 设备 %1 台 | 在线 %2 台 | %3 %4 条")
                                  .arg(deviceModel_->deviceCount())
                                  .arg(deviceModel_->onlineCount())
                                  .arg(mockMode_ ? QStringLiteral("抓拍记录") : QStringLiteral("事件记录"))
                                  .arg(visibleOrPendingCaptureCount));
    }
}

void MainWindow::selectDeviceRow(int row)
{
    if (row < 0 || row >= deviceModel_->rowCount())
    {
        return;
    }

    deviceTable_->selectRow(row);
}

void MainWindow::applySystemSettings(bool initialApply)
{
    QApplication::setFont(QFont(currentSystemSettings_.ui.fontFamily, currentSystemSettings_.ui.fontPointSize));
    storageService_.setSettings(currentSystemSettings_.storage);

    VideoDisplayOptions videoOptions;
    videoOptions.previewFrameRate = currentSystemSettings_.ui.previewFrameRate;
    videoOptions.showOnlyVehicleFrames = currentSystemSettings_.ui.showOnlyVehicleFrames;
    videoOptions.overlaySpeed = currentSystemSettings_.ui.overlaySpeed;
    videoOptions.showCalibrationLines = currentSystemSettings_.ui.showCalibrationLines;
    videoOptions.plateImagePosition = currentSystemSettings_.ui.plateImagePosition;
    if (livePreview_)
    {
        livePreview_->setDisplayOptions(videoOptions);
    }
    if (snapshotPreview_)
    {
        snapshotPreview_->setDisplayOptions(videoOptions);
    }
    if (eventController_)
    {
        eventController_->setSyncEnabled(currentSystemSettings_.ui.autoListenDeviceData);
    }

    applyCaptureColumnVisibility();
    updatePreviewLayout();
    configureMaintenanceController();
    updateStartupRegistration(currentSystemSettings_.maintenance.startWithSystem);

    if (captureService_ || eventController_)
    {
        refreshCaptureRecords();
    }

    if (currentSystemSettings_.ui.startMaximized && initialApply)
    {
        setWindowState(windowState() | Qt::WindowMaximized);
    }
}

void MainWindow::applyCaptureColumnVisibility()
{
    const QHash<int, QString> columns = {
        {CaptureRecordTableModel::TimeColumn, QStringLiteral("time")},
        {CaptureRecordTableModel::PlateColumn, QStringLiteral("plate")},
        {CaptureRecordTableModel::PlateColorColumn, QStringLiteral("plateColor")},
        {CaptureRecordTableModel::EventTypeColumn, QStringLiteral("eventType")},
        {CaptureRecordTableModel::DeviceIdColumn, QStringLiteral("deviceId")},
        {CaptureRecordTableModel::DirectionColumn, QStringLiteral("direction")},
        {CaptureRecordTableModel::CoordinateColumn, QStringLiteral("coordinate")},
        {CaptureRecordTableModel::RemarkColumn, QStringLiteral("remark")},
        {CaptureRecordTableModel::SpeedColumn, QStringLiteral("speed")},
        {CaptureRecordTableModel::TimeQualityColumn, QStringLiteral("timeQuality")},
        {CaptureRecordTableModel::EvidenceStatusColumn, QStringLiteral("evidenceStatus")},
    };
    const QList<QTableView *> tables = {allCaptureTable_, validCaptureTable_, unknownCaptureTable_};
    for (QTableView *table : tables)
    {
        if (!table)
        {
            continue;
        }
        for (auto it = columns.begin(); it != columns.end(); ++it)
        {
            const bool realOnlyColumn = it.key() >= CaptureRecordTableModel::SpeedColumn;
            table->setColumnHidden(
                it.key(), !(realOnlyColumn && !mockMode_) && !currentSystemSettings_.ui.captureListFields.contains(it.value()));
        }
    }
}

void MainWindow::configureMaintenanceController()
{
    if (!maintenanceController_)
    {
        return;
    }

    maintenanceController_->setSettings(currentSystemSettings_.maintenance);
    maintenanceController_->setSyncDeviceTimesCallback([this]()
                                                       {
        if (mockMode_) {
            const int synced = deviceManager_->syncAllDeviceTimes();
            statusBar()->showMessage(QStringLiteral("已定时同步 %1 台设备时间").arg(synced), 2500);
            return;
        }
        int synced = 0;
        if (integrationController_ && boardApiForDevice_) {
            for (const auto& snapshot : integrationController_->sessionSnapshots()) {
                if (snapshot.state != rv1126b::DeviceSessionState::Online) continue;
                if (auto* api = boardApiForDevice_(snapshot.profile.deviceId)) {
                    rv1126b::TimeUpdate update;
                    update.utcEpochMs = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
                    api->putTime(update, this,
                                 [](rv1126b::ApiResult<rv1126b::TimeStatusDto>) {});
                    ++synced;
                }
            }
        }
        statusBar()->showMessage(QStringLiteral("已定时同步 %1 台设备时间").arg(synced), 2500); });
    maintenanceController_->setDiskMaintenanceCallback([this]()
                                                       { performDiskMaintenance(); });
    maintenanceController_->setShutdownCallback([this]()
                                                { runScheduledShutdown(); });
}

void MainWindow::performDiskMaintenance()
{
    if (!currentSystemSettings_.maintenance.enableDiskMaintenance)
    {
        return;
    }

    if (!mockMode_ && evidenceMaintenance_)
    {
        evidenceMaintenance_->cleanupAsync(
            rv1126b::EvidenceCacheCleanupPolicy::fromMaintenanceSettings(
                currentSystemSettings_.maintenance));
        return;
    }

    const int expiredDays = qMin(
        currentSystemSettings_.maintenance.expireCaptureDays,
        currentSystemSettings_.maintenance.expireVideoDays);
    const int expiredRemoved = storageService_.deleteFilesOlderThan(expiredDays);

    const qint64 minFreeBytes = qint64(currentSystemSettings_.maintenance.minFreeSpaceGb) * 1024 * 1024 * 1024;
    const QStorageInfo storage(currentSystemSettings_.storage.rootPath);
    if (currentSystemSettings_.maintenance.deleteOldestWhenLowSpace && storage.isValid() && storage.bytesAvailable() < minFreeBytes)
    {
        const int removed = storageService_.deleteOldestFiles(100, minFreeBytes - storage.bytesAvailable());
        statusBar()->showMessage(QStringLiteral("磁盘维护已删除过期素材 %1 个、最早素材 %2 个").arg(expiredRemoved).arg(removed), 3500);
    }
    else if (expiredRemoved > 0)
    {
        statusBar()->showMessage(QStringLiteral("磁盘维护已删除过期素材 %1 个").arg(expiredRemoved), 3500);
    }
}

void MainWindow::runScheduledShutdown()
{
#ifdef Q_OS_WIN
    QProcess::startDetached(QStringLiteral("shutdown"), {QStringLiteral("/s"), QStringLiteral("/t"), QStringLiteral("60")});
#else
    statusBar()->showMessage(QStringLiteral("当前平台不支持自动关机命令"), 3500);
#endif
}

void MainWindow::updateStartupRegistration(bool enabled)
{
#ifdef Q_OS_WIN
    QSettings runKey(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"), QSettings::NativeFormat);
    const QString appName = QCoreApplication::applicationName();
    if (enabled)
    {
        runKey.setValue(appName, QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));
    }
    else
    {
        runKey.remove(appName);
    }
#else
    Q_UNUSED(enabled)
#endif
}

void MainWindow::updatePreviewLayout()
{
    if (snapshotPreview_)
    {
        snapshotPreview_->setVisible(currentSystemSettings_.ui.multiVideoSplit);
    }
}

int MainWindow::currentDeviceRow() const
{
    const QModelIndex index = deviceTable_->currentIndex();
    return index.isValid() ? index.row() : -1;
}

int MainWindow::currentCaptureRow() const
{
    QSortFilterProxyModel *proxy = currentCaptureProxy();
    QTableView *table = currentCaptureTable();
    if (!proxy || !table || !table->currentIndex().isValid())
    {
        return -1;
    }
    return proxy->mapToSource(table->currentIndex()).row();
}

QTableView *MainWindow::createDeviceTable()
{
    auto *table = new QTableView(this);
    table->setModel(deviceModel_);
    table->setContextMenuPolicy(Qt::CustomContextMenu);
    configureTableView(table);

    connect(table->selectionModel(), &QItemSelectionModel::currentRowChanged, this, &MainWindow::updateDeviceProperties);
    connect(table->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this]()
            {
        if (!mockMode_ && eventDeviceScopeCombo_ && eventDeviceScopeCombo_->currentIndex() == 0) {
            historyPage_ = 0;
            refreshCaptureRecords();
        } });
    connect(table, &QTableView::customContextMenuRequested, this, &MainWindow::showDeviceContextMenu);

    return table;
}

QTableView *MainWindow::createPropertyTable()
{
    auto *table = new QTableView(this);
    table->setModel(propertyModel_);
    configureTableView(table);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    return table;
}

QWidget *MainWindow::createCaptureRecordPanel()
{
    auto *panel = new QWidget(this);
    auto *rootLayout = new QVBoxLayout(panel);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(6);

    auto *controls = new QHBoxLayout();
    controls->setContentsMargins(0, 0, 0, 0);

    if (!mockMode_)
    {
        eventModeCombo_ = new QComboBox(panel);
        eventModeCombo_->setObjectName(QStringLiteral("eventViewModeCombo"));
        eventModeCombo_->addItem(QStringLiteral("实时事件"), QStringLiteral("realtime"));
        eventModeCombo_->addItem(QStringLiteral("本地历史"), QStringLiteral("history"));
        connect(eventModeCombo_, &QComboBox::currentIndexChanged,
                this, &MainWindow::changeEventViewMode);
        controls->addWidget(eventModeCombo_);

        eventDeviceScopeCombo_ = new QComboBox(panel);
        eventDeviceScopeCombo_->setObjectName(QStringLiteral("eventDeviceScopeCombo"));
        eventDeviceScopeCombo_->addItem(QStringLiteral("当前设备"), QStringLiteral("current"));
        eventDeviceScopeCombo_->addItem(QStringLiteral("全部设备"), QStringLiteral("all"));
        connect(eventDeviceScopeCombo_, &QComboBox::currentIndexChanged,
                this, [this]()
                { historyPage_ = 0; refreshCaptureRecords(); });
        controls->addWidget(eventDeviceScopeCombo_);

        eventTimeRangeCombo_ = new QComboBox(panel);
        eventTimeRangeCombo_->setObjectName(QStringLiteral("eventTimeRangeCombo"));
        eventTimeRangeCombo_->addItem(QStringLiteral("最近 1 天"), 1);
        eventTimeRangeCombo_->addItem(QStringLiteral("最近 2 天"), 2);
        eventTimeRangeCombo_->addItem(QStringLiteral("最近 7 天"), 7);
        eventTimeRangeCombo_->addItem(QStringLiteral("全部"), 0);
        eventTimeRangeCombo_->setToolTip(QStringLiteral("只查看最近指定天数内的本地缓存事件；选择“全部”则不做时间过滤。"));
        connect(eventTimeRangeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this]()
                { historyPage_ = 0; refreshCaptureRecords(); });
        controls->addWidget(eventTimeRangeCombo_);
    }

    auto *refreshButton = new QToolButton(panel);
    refreshButton->setDefaultAction(refreshAction_);
    controls->addWidget(refreshButton);

    pauseCaptureButton_ = new QToolButton(panel);
    pauseCaptureButton_->setText(QStringLiteral("暂停刷新"));
    pauseCaptureButton_->setCheckable(true);
    connect(pauseCaptureButton_, &QToolButton::toggled, this, &MainWindow::toggleCapturePause);
    controls->addWidget(pauseCaptureButton_);

    controls->addWidget(new QLabel(QStringLiteral("暂停秒数"), panel));
    pauseSecondsSpin_ = new QSpinBox(panel);
    pauseSecondsSpin_->setRange(5, 3600);
    pauseSecondsSpin_->setValue(30);
    pauseSecondsSpin_->setSuffix(QStringLiteral(" 秒"));
    controls->addWidget(pauseSecondsSpin_);

    pendingCaptureLabel_ = new QLabel(QStringLiteral("待刷新 0 条"), panel);
    controls->addWidget(pendingCaptureLabel_);
    controls->addStretch(1);

    auto *deleteButton = new QToolButton(panel);
    deleteButton->setDefaultAction(deleteCaptureAction_);
    controls->addWidget(deleteButton);

    auto *exportButton = new QToolButton(panel);
    exportButton->setDefaultAction(exportAction_);
    controls->addWidget(exportButton);

    rootLayout->addLayout(controls);

    if (!mockMode_)
    {
        historyFilterWidget_ = new QWidget(panel);
        historyFilterWidget_->setObjectName(QStringLiteral("historyFilterWidget"));
        auto *historyControls = new QHBoxLayout(historyFilterWidget_);
        historyControls->setContentsMargins(0, 0, 0, 0);
        historyControls->addWidget(new QLabel(QStringLiteral("车牌"), historyFilterWidget_));
        historyPlateEdit_ = new QLineEdit(historyFilterWidget_);
        historyPlateEdit_->setObjectName(QStringLiteral("historyPlateEdit"));
        historyPlateEdit_->setPlaceholderText(QStringLiteral("包含文本"));
        historyPlateEdit_->setMaximumWidth(130);
        historyControls->addWidget(historyPlateEdit_);
        historyTimeRangeCheck_ = new QCheckBox(QStringLiteral("时间范围"), historyFilterWidget_);
        historyControls->addWidget(historyTimeRangeCheck_);
        historyStartEdit_ = new QDateTimeEdit(QDateTime::currentDateTime().addDays(-1), historyFilterWidget_);
        historyStartEdit_->setObjectName(QStringLiteral("historyStartEdit"));
        historyStartEdit_->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        historyStartEdit_->setCalendarPopup(true);
        historyEndEdit_ = new QDateTimeEdit(QDateTime::currentDateTime(), historyFilterWidget_);
        historyEndEdit_->setObjectName(QStringLiteral("historyEndEdit"));
        historyEndEdit_->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        historyEndEdit_->setCalendarPopup(true);
        historyControls->addWidget(historyStartEdit_);
        historyControls->addWidget(new QLabel(QStringLiteral("至"), historyFilterWidget_));
        historyControls->addWidget(historyEndEdit_);
        auto *applyHistoryButton = new QToolButton(historyFilterWidget_);
        applyHistoryButton->setObjectName(QStringLiteral("historyApplyButton"));
        applyHistoryButton->setText(QStringLiteral("查询"));
        connect(applyHistoryButton, &QToolButton::clicked, this, [this]()
                {
            historyPage_ = 0;
            refreshCaptureRecords(); });
        historyControls->addWidget(applyHistoryButton);
        historyControls->addStretch(1);
        historyPreviousButton_ = new QToolButton(historyFilterWidget_);
        historyPreviousButton_->setObjectName(QStringLiteral("historyPreviousButton"));
        historyPreviousButton_->setText(QStringLiteral("上一页"));
        connect(historyPreviousButton_, &QToolButton::clicked, this, &MainWindow::previousHistoryPage);
        historyControls->addWidget(historyPreviousButton_);
        historyPageLabel_ = new QLabel(QStringLiteral("第 1 页"), historyFilterWidget_);
        historyPageLabel_->setObjectName(QStringLiteral("historyPageLabel"));
        historyControls->addWidget(historyPageLabel_);
        historyNextButton_ = new QToolButton(historyFilterWidget_);
        historyNextButton_->setObjectName(QStringLiteral("historyNextButton"));
        historyNextButton_->setText(QStringLiteral("下一页"));
        connect(historyNextButton_, &QToolButton::clicked, this, &MainWindow::nextHistoryPage);
        historyControls->addWidget(historyNextButton_);
        historyFilterWidget_->setVisible(false);
        rootLayout->addWidget(historyFilterWidget_);
    }

    allCaptureProxy_ = createCaptureProxy(QString());
    validCaptureProxy_ = createCaptureProxy(QStringLiteral("valid"));
    unknownCaptureProxy_ = createCaptureProxy(QStringLiteral("unknown"));

    allCaptureTable_ = createCaptureTable(allCaptureProxy_);
    validCaptureTable_ = createCaptureTable(validCaptureProxy_);
    unknownCaptureTable_ = createCaptureTable(unknownCaptureProxy_);

    captureTabs_ = new QTabWidget(panel);
    captureTabs_->addTab(allCaptureTable_, QStringLiteral("全部"));
    captureTabs_->addTab(validCaptureTable_, QStringLiteral("有效车牌"));
    captureTabs_->addTab(unknownCaptureTable_, QStringLiteral("无牌未知"));
    connect(captureTabs_, &QTabWidget::currentChanged, this, [this]()
            { updateCaptureControls(); });

    rootLayout->addWidget(captureTabs_, 1);

    capturePauseTimer_ = new QTimer(this);
    capturePauseTimer_->setSingleShot(true);
    connect(capturePauseTimer_, &QTimer::timeout, this, &MainWindow::finishCapturePause);

    return panel;
}

QTableView *MainWindow::createCaptureTable(QSortFilterProxyModel *proxyModel)
{
    auto *table = new QTableView(this);
    table->setModel(proxyModel);
    configureTableView(table);
    table->horizontalHeader()->setSectionResizeMode(CaptureRecordTableModel::TimeColumn, QHeaderView::Interactive);
    table->horizontalHeader()->setSectionResizeMode(CaptureRecordTableModel::CoordinateColumn, QHeaderView::Interactive);
    table->setColumnWidth(CaptureRecordTableModel::TimeColumn, 185);
    table->setColumnWidth(CaptureRecordTableModel::CoordinateColumn, 130);

    connect(table->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this]()
            {
        updateCaptureControls();
        updateSelectedEvidence(); });

    return table;
}

QSortFilterProxyModel *MainWindow::createCaptureProxy(const QString &plateStateFilter)
{
    auto *proxy = new QSortFilterProxyModel(this);
    proxy->setSourceModel(captureModel_);
    proxy->setDynamicSortFilter(true);
    if (!plateStateFilter.isEmpty())
    {
        proxy->setFilterRole(CaptureRecordTableModel::PlateStateRole);
        proxy->setFilterFixedString(plateStateFilter);
        proxy->setFilterCaseSensitivity(Qt::CaseSensitive);
    }
    return proxy;
}

void MainWindow::configureTableView(QTableView *table) const
{
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->setVisible(false);
    table->setAlternatingRowColors(true);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
}

void MainWindow::updateVideoWidgets()
{
    const int row = currentDeviceRow();
    const Device *device = deviceModel_->deviceAt(row);
    const std::optional<CaptureRecord> latestRecord = device && captureService_
                                                          ? captureService_->latestForDevice(device->id)
                                                          : std::nullopt;
    const CaptureRecord *latestRecordPtr = latestRecord ? &(*latestRecord) : nullptr;

    if (livePreview_)
    {
        livePreview_->setDevice(device);
        livePreview_->setLatestRecord(latestRecordPtr);
    }
    if (livePreviewPanel_)
    {
        livePreviewPanel_->setCurrentDevice(device ? device->id : QString());
        livePreviewPanel_->setBoardApiClient(device && boardApiForDevice_ ? boardApiForDevice_(device->id) : nullptr,
            device && (device->status.connectionState == DeviceConnectionState::Online
                       || device->status.connectionState == DeviceConnectionState::Degraded));
    }
    snapshotPreview_->setDevice(device);
    if (mockMode_)
        snapshotPreview_->setLatestRecord(latestRecordPtr);
}

void MainWindow::updateCaptureControls()
{
    if (pendingCaptureLabel_)
    {
        pendingCaptureLabel_->setText(QStringLiteral("待刷新 %1 条").arg(pendingCaptureCount_));
    }

    if (captureTabs_)
    {
        captureTabs_->setTabText(0, QStringLiteral("全部 (%1)").arg(allCaptureProxy_->rowCount()));
        captureTabs_->setTabText(1, QStringLiteral("有效车牌 (%1)").arg(validCaptureProxy_->rowCount()));
        captureTabs_->setTabText(2, QStringLiteral("无牌未知 (%1)").arg(unknownCaptureProxy_->rowCount()));
    }

    if (deleteCaptureAction_)
    {
        const QTableView *table = currentCaptureTable();
        deleteCaptureAction_->setEnabled(
            table && table->currentIndex().isValid() && (mockMode_ || (eventController_ && eventController_->servicesAvailable())));
    }
    if (clearCaptureAction_)
    {
        clearCaptureAction_->setEnabled(!mockMode_ && eventController_ && eventController_->servicesAvailable() && eventModeCombo_ && eventModeCombo_->currentIndex() == 1);
    }
    if (exportAction_)
    {
        exportAction_->setEnabled(mockMode_ || (eventController_ && eventController_->servicesAvailable()));
    }

    updateStatusText();
}

QTableView *MainWindow::currentCaptureTable() const
{
    if (!captureTabs_)
    {
        return nullptr;
    }

    switch (captureTabs_->currentIndex())
    {
    case 1:
        return validCaptureTable_;
    case 2:
        return unknownCaptureTable_;
    case 0:
    default:
        return allCaptureTable_;
    }
}

QSortFilterProxyModel *MainWindow::currentCaptureProxy() const
{
    if (!captureTabs_)
    {
        return nullptr;
    }

    switch (captureTabs_->currentIndex())
    {
    case 1:
        return validCaptureProxy_;
    case 2:
        return unknownCaptureProxy_;
    case 0:
    default:
        return allCaptureProxy_;
    }
}

CaptureRecordFilter MainWindow::currentCaptureFilter() const
{
    if (!captureTabs_)
    {
        return CaptureRecordFilter::All;
    }

    switch (captureTabs_->currentIndex())
    {
    case 1:
        return CaptureRecordFilter::ValidPlate;
    case 2:
        return CaptureRecordFilter::UnknownPlate;
    case 0:
    default:
        return CaptureRecordFilter::All;
    }
}

CaptureAssetKind MainWindow::storageKindForRecord(const CaptureRecord &record) const
{
    switch (record.type)
    {
    case CaptureType::Overspeed:
        return CaptureAssetKind::OverspeedImage;
    case CaptureType::Blacklist:
        return CaptureAssetKind::WatchedVehicleImage;
    case CaptureType::UnknownPlate:
    case CaptureType::Normal:
    default:
        return CaptureAssetKind::NormalImage;
    }
}

void MainWindow::addDevice()
{
    if (mockMode_)
    {
        deviceManager_->addMockDevice();
        selectDeviceRow(deviceModel_->deviceCount() - 1);
        updateDeviceProperties();
        updateStatusText();
        statusBar()->showMessage(QStringLiteral("已添加模拟设备"), 2500);
        return;
    }

    DeviceDiscoveryDialog dialog(integrationController_, {}, this);
    dialog.exec();
}

rv1126b::EventQuery MainWindow::currentEventQuery() const
{
    rv1126b::EventQuery query;
    const bool allDevices = eventDeviceScopeCombo_ && eventDeviceScopeCombo_->currentIndex() == 1;
    if (!allDevices)
    {
        if (const Device *device = deviceModel_->deviceAt(currentDeviceRow()))
            query.deviceId = device->id;
    }
    const bool historyMode = eventModeCombo_ && eventModeCombo_->currentIndex() == 1;
    if (historyMode)
    {
        const QString plate = historyPlateEdit_ ? historyPlateEdit_->text().trimmed() : QString();
        if (!plate.isEmpty())
            query.plateText = plate;
        if (historyTimeRangeCheck_ && historyTimeRangeCheck_->isChecked())
        {
            query.startEpochMs = historyStartEdit_->dateTime().toMSecsSinceEpoch();
            query.endEpochMs = historyEndEdit_->dateTime().toMSecsSinceEpoch();
        }
        query.limit = rv1126b::EventViewController::HistoryPageSize;
        query.offset = historyPage_ * query.limit;
    }
    else
    {
        query.limit = qMax(1, currentSystemSettings_.ui.captureListMaxRows);
        query.offset = 0;
    }

    // 时间范围：默认“最近 1 天”，避免把本地 SQLite 缓存里的旧事件（如换板前的历史数据）
    // 一起显示出来。app_api /api/v1/events 只接受 limit/cursor，无法按时间服务端过滤，
    // 因此这里按本地 event_epoch_ms（eventTime.epochMs）做下限过滤。
    const qint64 recentStartMs = recentRangeStartEpochMs();
    if (recentStartMs > 0)
    {
        if (!query.startEpochMs || *query.startEpochMs < recentStartMs)
            query.startEpochMs = recentStartMs;
    }

    query.newestFirst = true;
    return query;
}

qint64 MainWindow::recentRangeStartEpochMs() const
{
    if (!eventTimeRangeCombo_)
        return 0;
    const int days = eventTimeRangeCombo_->currentData().toInt();
    if (days <= 0)
        return 0;
    return QDateTime::currentDateTime().addDays(-days).toMSecsSinceEpoch();
}

const rv1126b::VehicleEvent *MainWindow::currentVehicleEvent() const
{
    return captureModel_ ? captureModel_->vehicleEventAt(currentCaptureRow()) : nullptr;
}

bool MainWindow::selectedEventDeviceOnline() const
{
    const rv1126b::VehicleEvent *event = currentVehicleEvent();
    if (!event)
        return false;
    const Device *device = deviceModel_->deviceAt(deviceModel_->rowForDeviceId(event->identity.deviceId));
    if (!device)
        return false;
    return device->status.connectionState == DeviceConnectionState::Online;
}

void MainWindow::connectSelectedDevice()
{
    const int row = currentDeviceRow();
    if (row < 0)
    {
        statusBar()->showMessage(QStringLiteral("请先选择一个设备"), 2500);
        return;
    }

    if (!mockMode_)
    {
        const Device *device = deviceModel_->deviceAt(row);
        DeviceDiscoveryDialog dialog(integrationController_, device ? device->id : QString(), this);
        dialog.exec();
        return;
    }

    if (deviceManager_->connectDevice(row))
    {
        statusBar()->showMessage(QStringLiteral("设备已连接，模拟状态和抓拍开始自动刷新"), 2500);
    }
}

void MainWindow::disconnectSelectedDevice()
{
    const int row = currentDeviceRow();
    if (row < 0)
    {
        statusBar()->showMessage(QStringLiteral("请先选择一个设备"), 2500);
        return;
    }

    if (!mockMode_)
    {
        const Device *device = deviceModel_->deviceAt(row);
        if (device && integrationController_)
        {
            if (eventController_)
                eventController_->stopDevice(device->id);
            integrationController_->disconnectDevice(device->id);
        }
    }
    else
    {
        deviceManager_->disconnectDevice(row);
    }
    statusBar()->showMessage(QStringLiteral("设备已断开"), 2500);
}

void MainWindow::openDeviceConfig()
{
    const int row = currentDeviceRow();
    const Device *device = mockMode_ ? deviceManager_->deviceAt(row) : deviceModel_->deviceAt(row);
    if (!device)
    {
        statusBar()->showMessage(QStringLiteral("请先选择一个设备"), 2500);
        return;
    }

    if (!mockMode_)
    {
        if (!operationsController_)
            return;
        const bool online = device->status.connectionState == DeviceConnectionState::Online;
        operationsController_->selectDevice(device->id);
        if (!operationsController_->boardApiAvailable() && !operationsController_->ftpServiceAvailable() && !operationsController_->ftpTaskSnapshotAvailable())
        {
            statusBar()->showMessage(QStringLiteral("当前设备的配置与 FTP 服务尚未装配"), 5000);
            return;
        }
        auto boardApiForHost = [this](const QString& host) -> rv1126b::IBoardApiClient* {
            if (!deviceModel_ || !boardApiForDevice_) return nullptr;
            const QString wanted = host.trimmed();
            for (int i = 0; i < deviceModel_->deviceCount(); ++i) {
                const Device* candidate = deviceModel_->deviceAt(i);
                if (!candidate) continue;
                const QUrl apiUrl(candidate->apiUrl);
                const bool match = candidate->ipAddress.compare(wanted, Qt::CaseInsensitive) == 0
                    || apiUrl.host().compare(wanted, Qt::CaseInsensitive) == 0
                    || candidate->id.compare(wanted, Qt::CaseInsensitive) == 0;
                if (match) return boardApiForDevice_(candidate->id);
            }
            return nullptr;
        };
        auto deviceIdForHost = [this](const QString& host) -> QString {
            if (!deviceModel_) return {};
            const QString wanted = host.trimmed();
            for (int i = 0; i < deviceModel_->deviceCount(); ++i) {
                const Device* candidate = deviceModel_->deviceAt(i);
                if (!candidate) continue;
                const QUrl apiUrl(candidate->apiUrl);
                const bool match = candidate->ipAddress.compare(wanted, Qt::CaseInsensitive) == 0
                    || apiUrl.host().compare(wanted, Qt::CaseInsensitive) == 0
                    || candidate->id.compare(wanted, Qt::CaseInsensitive) == 0;
                if (match) return candidate->id;
            }
            return {};
        };
        auto saveStorageRoot = [this](const QString& root) -> bool {
            SystemSettings next = currentSystemSettings_;
            next.storage.rootPath = QDir::cleanPath(root);
            if (!systemSettingsService_->save(next)) return false;
            currentSystemSettings_ = systemSettingsService_->settings();
            if (switchEvidenceRoot_) {
                const QString newEvidenceRoot = QDir(currentSystemSettings_.storage.rootPath)
                                                    .filePath(QStringLiteral("rv1126b/events"));
                if (switchEvidenceRoot_(newEvidenceRoot)) evidenceRootPath_ = newEvidenceRoot;
            }
            applySystemSettings();
            return true;
        };        Rv1126bDeviceManagementDialog dialog(
            device->id, operationsController_,
            online ? Rv1126bDeviceManagementDialog::InitialPage::Evidence
                   : Rv1126bDeviceManagementDialog::InitialPage::FtpTasks,
            this, online, ftpReceiveServer_,
            QDir(currentSystemSettings_.storage.rootPath).filePath(QStringLiteral("rv1126b/ftp-inbox")),
            eventSyncForDevice_ ? eventSyncForDevice_(device->id) : nullptr,
            evidenceRootPath_,
            QStringLiteral("%1:%2").arg(device->ipAddress).arg(device->port),
            boardApiForDevice_ ? boardApiForDevice_(device->id) : nullptr,
            boardApiForHost,
            deviceIdForHost,
            currentSystemSettings_.storage.rootPath,
            saveStorageRoot);
        dialog.exec();
        return;
    }

    DeviceConfigDialog dialog(*device, this);
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }

    if (deviceManager_->writeDeviceConfig(row, dialog.config()))
    {
        statusBar()->showMessage(QStringLiteral("设备配置已保存并下发"), 2500);
    }
    else
    {
        QMessageBox::warning(this, QStringLiteral("设备配置"), QStringLiteral("配置保存失败，请确认设备在线后重试。"));
    }
}

void MainWindow::triggerCapture()
{
    const int row = currentDeviceRow();
    if (row < 0)
    {
        statusBar()->showMessage(QStringLiteral("请先选择一个设备"), 2500);
        return;
    }

    deviceManager_->triggerCapture(row);
}

void MainWindow::rebootSelectedDevice()
{
    const int row = currentDeviceRow();
    if (row < 0)
    {
        statusBar()->showMessage(QStringLiteral("请先选择一个设备"), 2500);
        return;
    }

    if (deviceManager_->rebootDevice(row))
    {
        statusBar()->showMessage(QStringLiteral("已发送模拟重启命令"), 2500);
    }
}

void MainWindow::syncSelectedDeviceTime()
{
    const int row = currentDeviceRow();
    const Device *device = mockMode_ ? deviceManager_->deviceAt(row) : deviceModel_->deviceAt(row);
    if (!device)
    {
        statusBar()->showMessage(QStringLiteral("请先选择一个设备"), 2500);
        return;
    }

    if (!mockMode_)
    {
        if (!operationsController_ || device->status.connectionState != DeviceConnectionState::Online)
        {
            statusBar()->showMessage(QStringLiteral("设备在线后才能校时"), 3500);
            return;
        }
        operationsController_->selectDevice(device->id);
        if (!operationsController_->boardApiAvailable())
        {
            statusBar()->showMessage(QStringLiteral("当前设备的时间服务尚未装配"), 5000);
            return;
        }
        auto boardApiForHost = [this](const QString& host) -> rv1126b::IBoardApiClient* {
            if (!deviceModel_ || !boardApiForDevice_) return nullptr;
            const QString wanted = host.trimmed();
            for (int i = 0; i < deviceModel_->deviceCount(); ++i) {
                const Device* candidate = deviceModel_->deviceAt(i);
                if (!candidate) continue;
                const QUrl apiUrl(candidate->apiUrl);
                const bool match = candidate->ipAddress.compare(wanted, Qt::CaseInsensitive) == 0
                    || apiUrl.host().compare(wanted, Qt::CaseInsensitive) == 0
                    || candidate->id.compare(wanted, Qt::CaseInsensitive) == 0;
                if (match) return boardApiForDevice_(candidate->id);
            }
            return nullptr;
        };
        auto deviceIdForHost = [this](const QString& host) -> QString {
            if (!deviceModel_) return {};
            const QString wanted = host.trimmed();
            for (int i = 0; i < deviceModel_->deviceCount(); ++i) {
                const Device* candidate = deviceModel_->deviceAt(i);
                if (!candidate) continue;
                const QUrl apiUrl(candidate->apiUrl);
                const bool match = candidate->ipAddress.compare(wanted, Qt::CaseInsensitive) == 0
                    || apiUrl.host().compare(wanted, Qt::CaseInsensitive) == 0
                    || candidate->id.compare(wanted, Qt::CaseInsensitive) == 0;
                if (match) return candidate->id;
            }
            return {};
        };
        auto saveStorageRoot = [this](const QString& root) -> bool {
            SystemSettings next = currentSystemSettings_;
            next.storage.rootPath = QDir::cleanPath(root);
            if (!systemSettingsService_->save(next)) return false;
            currentSystemSettings_ = systemSettingsService_->settings();
            if (switchEvidenceRoot_) {
                const QString newEvidenceRoot = QDir(currentSystemSettings_.storage.rootPath)
                                                    .filePath(QStringLiteral("rv1126b/events"));
                if (switchEvidenceRoot_(newEvidenceRoot)) evidenceRootPath_ = newEvidenceRoot;
            }
            applySystemSettings();
            return true;
        };        Rv1126bDeviceManagementDialog dialog(
            device->id, operationsController_,
            Rv1126bDeviceManagementDialog::InitialPage::Time, this, true, ftpReceiveServer_,
            QDir(currentSystemSettings_.storage.rootPath).filePath(QStringLiteral("rv1126b/ftp-inbox")),
            eventSyncForDevice_ ? eventSyncForDevice_(device->id) : nullptr,
            evidenceRootPath_,
            QStringLiteral("%1:%2").arg(device->ipAddress).arg(device->port),
            boardApiForDevice_ ? boardApiForDevice_(device->id) : nullptr,
            boardApiForHost,
            deviceIdForHost,
            currentSystemSettings_.storage.rootPath,
            saveStorageRoot);
        dialog.exec();
        return;
    }

    if (deviceManager_->syncDeviceTime(row))
    {
        statusBar()->showMessage(QStringLiteral("已同步设备时间"), 2500);
    }
}

void MainWindow::openGlobalSettings()
{
    const QString previousStorageRoot = currentSystemSettings_.storage.rootPath;
    SystemSettingsDialog dialog(currentSystemSettings_, this);
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }

    if (!systemSettingsService_->save(dialog.settings()))
    {
        QMessageBox::warning(this, QStringLiteral("全局设置"), systemSettingsService_->lastError());
        return;
    }

    currentSystemSettings_ = systemSettingsService_->settings();
    if (!mockMode_ && previousStorageRoot != currentSystemSettings_.storage.rootPath && switchEvidenceRoot_)
    {
        const QString newEvidenceRoot = QDir(currentSystemSettings_.storage.rootPath)
                                            .filePath(QStringLiteral("rv1126b/events"));
        if (switchEvidenceRoot_(newEvidenceRoot))
        {
            evidenceRootPath_ = newEvidenceRoot;
            statusBar()->showMessage(QStringLiteral("后续 evidence 将写入新目录；旧图片保持原路径"), 6000);
        }
        else if (persistentStatusLabel_)
        {
            persistentStatusLabel_->setVisible(true);
            persistentStatusLabel_->setText(QStringLiteral("evidence 目录切换失败，仍使用原目录"));
        }
    }
    applySystemSettings();

    if (currentSystemSettings_.ui.showSuccessPopup)
    {
        QMessageBox::information(this, QStringLiteral("全局设置"), QStringLiteral("设置已保存并应用。"));
    }
    else
    {
        statusBar()->showMessage(QStringLiteral("全局设置已保存并应用"), 2500);
    }
}

void MainWindow::refreshCaptureRecords()
{
    if (!mockMode_)
    {
        if (!eventController_)
            return;
        const rv1126b::EventQuery query = currentEventQuery();
        if (eventModeCombo_ && eventModeCombo_->currentIndex() == 1)
        {
            eventController_->queryHistory(query);
        }
        else
        {
            eventController_->refreshRealtime(query);
        }
        return;
    }
    if (!captureService_)
    {
        return;
    }

    captureModel_->setRecords(captureService_->records(CaptureRecordFilter::All, currentSystemSettings_.ui.captureListMaxRows));
    pendingCaptureCount_ = 0;
    updateCaptureControls();
    updateDeviceProperties();
    statusBar()->showMessage(QStringLiteral("抓拍记录已刷新"), 1800);
}

void MainWindow::toggleCapturePause(bool paused)
{
    if (!mockMode_ && eventController_)
        eventController_->setPaused(paused);
    if (paused)
    {
        const int seconds = pauseSecondsSpin_ ? pauseSecondsSpin_->value() : 30;
        capturePauseTimer_->start(seconds * 1000);
        pauseCaptureButton_->setText(QStringLiteral("恢复刷新"));
        statusBar()->showMessage(QStringLiteral("抓拍列表已暂停刷新 %1 秒").arg(seconds), 2500);
        return;
    }

    capturePauseTimer_->stop();
    pauseCaptureButton_->setText(QStringLiteral("暂停刷新"));
    refreshCaptureRecords();
}

void MainWindow::finishCapturePause()
{
    if (pauseCaptureButton_ && pauseCaptureButton_->isChecked())
    {
        pauseCaptureButton_->setChecked(false);
    }
    else
    {
        refreshCaptureRecords();
    }
}

void MainWindow::exportCaptureRecords()
{
    const QString defaultName = QStringLiteral("capture_records_%1.csv")
                                    .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    const QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("导出抓拍记录"),
        QDir::home().filePath(defaultName),
        QStringLiteral("CSV 文件 (*.csv)"));

    if (path.isEmpty())
    {
        return;
    }

    if (!mockMode_)
    {
        if (eventController_)
            eventController_->exportHistory(currentEventQuery(), path);
        return;
    }

    if (!captureService_ || !captureService_->exportCsv(currentCaptureFilter(), path))
    {
        QMessageBox::warning(this, QStringLiteral("导出抓拍记录"), captureService_->lastError());
        return;
    }

    statusBar()->showMessage(QStringLiteral("抓拍记录已导出：%1").arg(path), 3500);
}

void MainWindow::deleteSelectedCapture()
{
    const int sourceRow = currentCaptureRow();
    if (!mockMode_)
    {
        const rv1126b::VehicleEvent *event = captureModel_->vehicleEventAt(sourceRow);
        if (!event || !eventController_)
        {
            statusBar()->showMessage(QStringLiteral("请先选择一条真实事件"), 2500);
            return;
        }
        const auto answer = QMessageBox::warning(
            this, QStringLiteral("删除本地事件"),
            QStringLiteral("将删除 PC 本地事件及融合图片。板端不支持删除，若事件仍在板端保留期内，后续同步可能重新补回。是否继续？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer == QMessageBox::Yes)
            eventController_->deleteLocalEvent(*event);
        return;
    }
    const CaptureRecord *record = captureModel_->recordAt(sourceRow);
    if (!record)
    {
        statusBar()->showMessage(QStringLiteral("请先选择一条抓拍记录"), 2500);
        return;
    }

    const QString id = record->id;
    if (!captureService_->deleteRecord(id))
    {
        QMessageBox::warning(this, QStringLiteral("删除抓拍记录"), captureService_->lastError());
        return;
    }

    refreshCaptureRecords();
    statusBar()->showMessage(QStringLiteral("已删除选中的抓拍记录"), 2500);
}

void MainWindow::clearCaptureRecords()
{
    if (mockMode_ || !eventController_ || !eventModeCombo_ || eventModeCombo_->currentIndex() != 1)
    {
        statusBar()->showMessage(QStringLiteral("请在本地历史模式下按当前筛选条件清空"), 2500);
        return;
    }
    const auto answer = QMessageBox::warning(
        this, QStringLiteral("清空本地历史"),
        QStringLiteral("将删除当前设备、车牌和时间筛选匹配的全部 PC 本地事件及图片，而不只是当前页。板端保留事件可能再次同步。是否继续？"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer == QMessageBox::Yes)
    {
        rv1126b::EventQuery query = currentEventQuery();
        query.offset = 0;
        eventController_->clearLocalHistory(query);
    }
}

void MainWindow::showActionMessage()
{
    const auto *action = qobject_cast<QAction *>(sender());
    const QString name = action ? action->text() : QStringLiteral("操作");
    statusBar()->showMessage(QStringLiteral("%1 功能将在后续阶段完善").arg(name), 2500);
}

void MainWindow::showSelectedNetworkInfo()
{
    const Device *device = deviceModel_->deviceAt(currentDeviceRow());
    if (!device)
        return;
    QMessageBox::information(
        this, QStringLiteral("设备网络信息"),
        QStringLiteral("设备 ID：%1\nIPv4：%2\nHTTP 端口：%3\nAPI URL：%4\n固件：%5\n能力：%6")
            .arg(device->id, device->ipAddress)
            .arg(device->port)
            .arg(device->apiUrl.isEmpty() ? QStringLiteral("—") : device->apiUrl,
                 device->status.firmwareVersion,
                 device->capabilities.isEmpty() ? QStringLiteral("未声明")
                                                : device->capabilities.join(QStringLiteral(", "))));
}

void MainWindow::openSelectedLocalFolder()
{
    const Device *device = deviceModel_->deviceAt(currentDeviceRow());
    if (!device || evidenceRootPath_.isEmpty())
        return;
    const QString path = QDir(evidenceRootPath_).filePath(device->id);
    QDir().mkpath(path);
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
    {
        statusBar()->showMessage(QStringLiteral("无法打开本地目录：%1").arg(path), 5000);
    }
}

void MainWindow::handleDeviceForgotten(const QString &deviceId)
{
    deviceModel_->removeDevice(deviceId);
    if (deviceModel_->rowCount() > 0)
        selectDeviceRow(0);
    else
        propertyModel_->clear();
    updateCaptureControls();
    updateDeviceProperties();
}

void MainWindow::updateDeviceProperties()
{
    const int row = currentDeviceRow();
    const Device *device = deviceModel_->deviceAt(row);
    const std::optional<CaptureRecord> latestRecord = device && captureService_
                                                          ? captureService_->latestForDevice(device->id)
                                                          : std::nullopt;
    const CaptureRecord *latestRecordPtr = latestRecord ? &(*latestRecord) : nullptr;

    propertyModel_->setDevice(device, latestRecordPtr);
    if (!mockMode_ && operationsController_)
    {
        operationsController_->selectDevice(device ? device->id : QString());
        const bool online = device && device->status.connectionState == DeviceConnectionState::Online;
        configAction_->setEnabled(device && ((online && (operationsController_->boardApiAvailable() || operationsController_->ftpServiceAvailable())) || operationsController_->ftpTaskSnapshotAvailable()));
        syncTimeAction_->setEnabled(online && operationsController_->boardApiAvailable());
        connectAction_->setEnabled(device && !online && device->status.connectionState != DeviceConnectionState::Connecting);
        connectAction_->setText(device && device->status.connectionState == DeviceConnectionState::AuthenticationFailed
                                    ? QStringLiteral("重新配置 Token")
                                    : QStringLiteral("连接"));
        disconnectAction_->setEnabled(device && (online || device->status.connectionState == DeviceConnectionState::Connecting || device->status.connectionState == DeviceConnectionState::Degraded));
    }
    if (!mockMode_ && integrationController_ && device && integrationController_->selectedVideoDeviceId() != device->id && (device->status.connectionState == DeviceConnectionState::Online || device->status.connectionState == DeviceConnectionState::Degraded))
    {
        integrationController_->selectVideoDevice(device->id);
    }
    updateVideoWidgets();
    updateStatusText();
}

void MainWindow::showDeviceContextMenu(const QPoint &position)
{
    if (!deviceTable_->indexAt(position).isValid())
    {
        return;
    }

    deviceContextMenu_->exec(deviceTable_->viewport()->mapToGlobal(position));
}

void MainWindow::handleDeviceAdded(const Device &device)
{
    deviceModel_->addDevice(device);
    updateStatusText();
}

void MainWindow::handleDeviceStatusChanged(int row, const DeviceStatus &status)
{
    deviceModel_->updateStatus(row, status);
    if (row == currentDeviceRow())
    {
        updateDeviceProperties();
    }
    updateStatusText();
}

void MainWindow::handleDeviceConfigChanged(int row, const DeviceConfig &config)
{
    deviceModel_->updateConfig(row, config);
    if (row == currentDeviceRow())
    {
        updateDeviceProperties();
    }
    updateStatusText();
}

void MainWindow::handleCaptureGenerated(int row, const CaptureRecord &record)
{
    CaptureRecord storedRecord = record;
    const CaptureStorageResult storageResult = storageService_.saveCaptureAssets(storedRecord, storageKindForRecord(storedRecord));
    if (storageResult.ok)
    {
        storedRecord.filePath = storageResult.primaryPath;
    }
    else
    {
        storedRecord.filePath = QStringLiteral("未保存：%1").arg(storageResult.errorMessage);
    }

    if (!captureService_->addRecord(storedRecord))
    {
        QMessageBox::warning(this, QStringLiteral("保存抓拍记录"), captureService_->lastError());
        return;
    }

    deviceModel_->incrementCaptureCount(row);

    if (pauseCaptureButton_ && pauseCaptureButton_->isChecked())
    {
        ++pendingCaptureCount_;
        updateCaptureControls();
    }
    else
    {
        refreshCaptureRecords();
        const QModelIndex sourceIndex = captureModel_->index(captureModel_->recordCount() - 1, 0);
        const QModelIndex proxyIndex = allCaptureProxy_->mapFromSource(sourceIndex);
        if (proxyIndex.isValid())
        {
            allCaptureTable_->selectRow(proxyIndex.row());
        }
    }

    if (row == currentDeviceRow())
    {
        updateDeviceProperties();
    }

    updateStatusText();
    statusBar()->showMessage(QStringLiteral("收到模拟抓拍：%1 %2 km/h").arg(storedRecord.plateNumber).arg(storedRecord.speedKmh), 2500);
}

void MainWindow::handleDeviceError(int row, const QString &message)
{
    const Device *device = deviceModel_->deviceAt(row);
    const QString prefix = device ? device->name : QStringLiteral("设备");
    statusBar()->showMessage(QStringLiteral("%1：%2").arg(prefix, message), 3500);
}

void MainWindow::handleDiscoveredDevice(const rv1126b::DiscoveredDeviceDto &discovered)
{
    Device device;
    const int existingRow = deviceModel_->rowForDeviceId(discovered.deviceId);
    if (const Device *existing = deviceModel_->deviceAt(existingRow))
    {
        device = *existing;
    }
    device.id = discovered.deviceId;
    device.name = discovered.deviceModel.isEmpty()
                      ? discovered.deviceId
                      : QStringLiteral("%1 · %2").arg(discovered.deviceModel, discovered.deviceId);
    device.ipAddress = discovered.ipv4;
    device.port = discovered.apiUrl.port(18080);
    device.apiUrl = discovered.apiUrl.toString();
    device.capabilities = discovered.capabilities;
    device.status.firmwareVersion = discovered.releaseVersion;
    const int row = deviceModel_->upsertDevice(device);
    if (currentDeviceRow() < 0)
    {
        selectDeviceRow(row);
    }
    updateStatusText();
}

void MainWindow::handleSessionChanged(const rv1126b::DeviceSessionSnapshot &snapshot)
{
    if (eventController_ && eventSyncForDevice_)
    {
        eventController_->attachSyncService(eventSyncForDevice_(snapshot.profile.deviceId));
    }
    if (eventController_)
        eventController_->setDeviceSession(snapshot);
    int row = deviceModel_->rowForDeviceId(snapshot.profile.deviceId);
    if (row < 0)
    {
        Device device;
        device.id = snapshot.profile.deviceId;
        device.name = snapshot.profile.deviceModel.isEmpty()
                          ? snapshot.profile.deviceId
                          : QStringLiteral("%1 · %2").arg(snapshot.profile.deviceModel,
                                                          snapshot.profile.deviceId);
        device.ipAddress = snapshot.profile.endpoint.ipv4;
        device.port = snapshot.profile.endpoint.httpPort;
        device.status.firmwareVersion = snapshot.profile.releaseVersion;
        row = deviceModel_->upsertDevice(device);
    }

    const Device *existingDevice = deviceModel_->deviceAt(row);
    if (!existingDevice)
    {
        return;
    }
    Device device = *existingDevice;
    device.ipAddress = snapshot.profile.endpoint.ipv4;
    device.port = snapshot.profile.endpoint.httpPort;
    device.apiUrl = snapshot.profile.endpoint.apiBaseUrl.toString();
    device.capabilities = snapshot.profile.advertisedCapabilities;
    device.lastOnline = snapshot.profile.lastOnlineEpochMs > 0
                            ? QDateTime::fromMSecsSinceEpoch(snapshot.profile.lastOnlineEpochMs)
                            : QDateTime();
    device.lastError = snapshot.lastError ? snapshot.lastError->code : QString();
    device.status.connectionState = uiConnectionState(snapshot.state);
    device.status.runningState = connectionStateText(device.status.connectionState);
    device.status.firmwareVersion = snapshot.profile.releaseVersion;
    device.status.lastHeartbeat = snapshot.lastHealthEpochMs > 0
                                      ? QDateTime::fromMSecsSinceEpoch(snapshot.lastHealthEpochMs)
                                      : QDateTime();
    deviceModel_->upsertDevice(device);
    if (operationsController_ && operationsController_->deviceId() == snapshot.profile.deviceId && snapshot.state != rv1126b::DeviceSessionState::Online)
    {
        operationsController_->cancelPending();
    }
    if (currentDeviceRow() < 0)
    {
        selectDeviceRow(row);
    }
    if (row == currentDeviceRow())
    {
        updateDeviceProperties();
        if (livePreviewPanel_ && boardApiForDevice_) {
            livePreviewPanel_->setBoardApiClient(boardApiForDevice_(snapshot.profile.deviceId),
                snapshot.state == rv1126b::DeviceSessionState::Online
                || snapshot.state == rv1126b::DeviceSessionState::Degraded);
        }
    }
    if (persistentStatusLabel_)
    {
        const bool unhealthy = snapshot.state == rv1126b::DeviceSessionState::AuthenticationFailed || snapshot.state == rv1126b::DeviceSessionState::Degraded;
        persistentStatusLabel_->setVisible(unhealthy);
        persistentStatusLabel_->setText(unhealthy
                                            ? QStringLiteral("%1：%2").arg(snapshot.profile.deviceId,
                                                                           snapshot.state == rv1126b::DeviceSessionState::AuthenticationFailed
                                                                               ? QStringLiteral("认证失败")
                                                                               : QStringLiteral("连接退化"))
                                            : QString());
    }
    updateStatusText();
}

void MainWindow::handleSelectedVideoDeviceChanged(const QString &deviceId)
{
    const int row = deviceModel_->rowForDeviceId(deviceId);
    if (row >= 0 && row != currentDeviceRow())
    {
        selectDeviceRow(row);
    }
    if (livePreviewPanel_)
    {
        livePreviewPanel_->setCurrentDevice(deviceId);
        const Device* device = deviceModel_->deviceAt(row);
        livePreviewPanel_->setBoardApiClient(!deviceId.isEmpty() && boardApiForDevice_ ? boardApiForDevice_(deviceId) : nullptr,
            device && (device->status.connectionState == DeviceConnectionState::Online
                       || device->status.connectionState == DeviceConnectionState::Degraded));
    }
    if (!mockMode_ && !deviceId.isEmpty() && currentSystemSettings_.ui.lastSelectedVideoDeviceId != deviceId)
    {
        currentSystemSettings_.ui.lastSelectedVideoDeviceId = deviceId;
        if (!systemSettingsService_->save(currentSystemSettings_) && persistentStatusLabel_)
        {
            persistentStatusLabel_->setVisible(true);
            persistentStatusLabel_->setText(QStringLiteral("无法保存上次视频设备：%1")
                                                .arg(systemSettingsService_->lastError()));
        }
    }
}

void MainWindow::updateSelectedEvidence()
{
    if (mockMode_ || !snapshotPreview_)
        return;
    const int row = currentCaptureRow();
    const rv1126b::VehicleEvent *event = captureModel_->vehicleEventAt(row);
    if (!event)
    {
        snapshotPreview_->clearVehicleEvent();
        return;
    }
    snapshotPreview_->setVehicleEvent(event);
    snapshotPreview_->setEvidenceState(captureModel_->evidenceStateAt(row), selectedEventDeviceOnline());
    if (eventController_)
        eventController_->requestEvidence(*event);
}

void MainWindow::changeEventViewMode()
{
    historyPage_ = 0;
    if (historyFilterWidget_)
        historyFilterWidget_->setVisible(eventModeCombo_->currentIndex() == 1);
    updateCaptureControls();
    refreshCaptureRecords();
}

void MainWindow::previousHistoryPage()
{
    if (historyPage_ <= 0)
        return;
    --historyPage_;
    refreshCaptureRecords();
}

void MainWindow::nextHistoryPage()
{
    if (lastHistoryRowCount_ < rv1126b::EventViewController::HistoryPageSize)
        return;
    ++historyPage_;
    refreshCaptureRecords();
}

void MainWindow::handleIntegrationError(const QString &, const QString &message)
{
    statusBar()->showMessage(message, 5000);
    if (persistentStatusLabel_)
    {
        persistentStatusLabel_->setText(message);
        persistentStatusLabel_->setVisible(true);
    }
}
