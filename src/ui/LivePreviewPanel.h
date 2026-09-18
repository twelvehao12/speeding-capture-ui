#pragma once

#include "../rv1126b/ports/IBoardApiClient.h"
#include "../rv1126b/ports/IRtspPlayer.h"
#include "../rv1126b/application/VideoStreamsController.h"

#include <QWidget>
#include <QPointer>
#include <QTimer>

#include <optional>

class QComboBox;
class QEvent;
class QCheckBox;
class QLabel;
class QPushButton;

class LineRegionOverlayWidget;

class LivePreviewPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit LivePreviewPanel(rv1126b::IRtspPlayer* player, QWidget* parent = nullptr);

    void setCurrentDevice(const QString& deviceId);
    void setBoardApiClient(rv1126b::IBoardApiClient* boardApi, bool videoConfigAvailable = true);

public slots:
    void setPlaybackState(rv1126b::RtspPlayerState state);
    void setPlaybackError(const rv1126b::ApiError& error);

signals:
    void streamRoleChanged(rv1126b::RtspStreamRole role);
    void previewRestartRequested(const QString& deviceId);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void updateVideoControls();
    void clearActualResolution();
    void invalidateDetectionRequests();
    void finishDetectionRead(quint64 generation);
    void loadDetectionConfig();
    void saveDetectionConfig();
    void saveLineRegionConfig(const QString& runtimeRevision, bool restartRequired);
    void applyRuntimeConfigIfNeeded(const QString& runtimeRevision, bool restartRequired);
    void scheduleDetectionRefresh(int delayMs, const QString& message);
    void setDetectionBusy(bool busy);
    void setDetectionStatus(const QString& message, bool error = false);
    void applyTriggerModeConfig(const rv1126b::TriggerModeConfigDto& config);
    void applyLineRegionConfig(const rv1126b::LineRegionConfigDto& config);
    void updateLaneDirectionControls();
    void applyLaneDirectionSelection();
    void syncSingleDirectionAliases(rv1126b::LineRegionSettings* lineRegion) const;
    void normalizeHiddenLinesForTriggerOnly(rv1126b::LineRegionSettings* lineRegion) const;
    void markLineRegionDirty(const rv1126b::LineRegionSettings& lineRegion);

    QLabel* deviceLabel_ = nullptr;
    QLabel* stateLabel_ = nullptr;
    QLabel* detectionStatus_ = nullptr;
    QComboBox* streamCombo_ = nullptr;
    QComboBox* videoCodecCombo_ = nullptr;
    QLabel* videoConfigStatus_ = nullptr;
    QLabel* actualResolutionLabel_ = nullptr;
    QPushButton* applyVideoButton_ = nullptr;
    QPushButton* refreshVideoButton_ = nullptr;
    rv1126b::VideoStreamsController* videoController_ = nullptr;
    QComboBox* triggerModeCombo_ = nullptr;
    QComboBox* laneDirectionCombo_ = nullptr;
    QCheckBox* showTriggerLineCheck_ = nullptr;
    QCheckBox* showPreLineCheck_ = nullptr;
    QCheckBox* showLightLineCheck_ = nullptr;
    QPushButton* refreshDetectionButton_ = nullptr;
    QPushButton* saveDetectionButton_ = nullptr;
    LineRegionOverlayWidget* lineOverlay_ = nullptr;
    QWidget* videoOutput_ = nullptr;
    QWidget* videoHost_ = nullptr;
    QPointer<rv1126b::IBoardApiClient> boardApi_;
    QPointer<QObject> detectionContext_;
    QMetaObject::Connection detectionApiDestroyed_;
    QTimer detectionRefreshTimer_;
    quint64 detectionGeneration_ = 0;
    int detectionReadsPending_ = 0;
    bool detectionReadFailed_ = false;
    QSize actualFrameSize_;
    QString currentDeviceId_;
    std::optional<rv1126b::TriggerModeConfigDto> triggerModeConfig_;
    std::optional<rv1126b::LineRegionConfigDto> lineRegionConfig_;
    bool triggerModeDirty_ = false;
    bool lineRegionDirty_ = false;
    bool loadingDetection_ = false;
    bool savingDetection_ = false;
    bool updatingTriggerModeCombo_ = false;
    bool updatingLaneDirectionCombo_ = false;
};

