#include "LivePreviewPanel.h"

#include <QComboBox>
#include <QEvent>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPushButton>
#include <QStackedLayout>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>

namespace {

QString playbackStateText(rv1126b::RtspPlayerState state)
{
    using rv1126b::RtspPlayerState;
    switch (state) {
    case RtspPlayerState::Opening:
        return QStringLiteral("正在打开视频…");
    case RtspPlayerState::Playing:
        return QStringLiteral("正在播放");
    case RtspPlayerState::Reconnecting:
        return QStringLiteral("视频重连中…");
    case RtspPlayerState::Stopped:
        return QStringLiteral("视频已停止");
    case RtspPlayerState::Error:
        return QStringLiteral("视频播放失败");
    case RtspPlayerState::Idle:
    default:
        return QStringLiteral("请选择并连接设备");
    }
}

int clampPermille(int value)
{
    return std::clamp(value, 0, 1000);
}

QRectF contentRectFor(const QRect& bounds, const QSize& frameSize)
{
    if (!frameSize.isValid() || bounds.width() <= 0 || bounds.height() <= 0) {
        return QRectF(bounds);
    }
    QSizeF frame(frameSize);
    frame.scale(bounds.size(), Qt::KeepAspectRatio);
    const QPointF topLeft(bounds.x() + (bounds.width() - frame.width()) / 2.0,
                          bounds.y() + (bounds.height() - frame.height()) / 2.0);
    return QRectF(topLeft, frame);
}

bool isTransientNetworkError(const rv1126b::ApiError& error)
{
    return error.retryable
        && (error.category == rv1126b::ApiErrorCategory::Network
            || error.category == rv1126b::ApiErrorCategory::Temporary);
}

bool usesPreLine(const QString& triggerMode)
{
    return triggerMode != QLatin1String("trigger_only");
}

bool usesLightLine(const QString& triggerMode)
{
    return triggerMode != QLatin1String("trigger_only");
}

void normalizeTriggerOnlyDirection(int* triggerPermille, int* prePermille, int* lightPermille, int direction)
{
    if (!triggerPermille || !prePermille || !lightPermille) return;
    if (direction < 0) {
        *triggerPermille = std::min(clampPermille(*triggerPermille), 980);
        *prePermille = std::min(990, *triggerPermille + 80);
        if (*prePermille <= *triggerPermille) *prePermille = *triggerPermille + 10;
        *lightPermille = std::min(1000, *prePermille + 40);
        if (*lightPermille <= *prePermille) *lightPermille = *prePermille + 10;
        return;
    }
    *triggerPermille = std::max(clampPermille(*triggerPermille), 20);
    *prePermille = std::max(10, *triggerPermille - 80);
    if (*prePermille >= *triggerPermille) *prePermille = *triggerPermille - 10;
    *lightPermille = std::max(0, *prePermille - 40);
    if (*lightPermille >= *prePermille) *lightPermille = *prePermille - 10;
}

} // namespace

class LineRegionOverlayWidget final : public QWidget
{
public:
    explicit LineRegionOverlayWidget(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("lineRegionOverlay"));
        setAttribute(Qt::WA_TranslucentBackground);
        setMouseTracking(true);
        setCursor(Qt::CrossCursor);
    }

    void setFrameSize(const QSize& frameSize)
    {
        if (frameSize_ == frameSize) return;
        frameSize_ = frameSize;
        update();
    }

    void setLineRegion(const rv1126b::LineRegionSettings& lineRegion)
    {
        lineRegion_ = lineRegion;
        update();
    }

    void setTriggerMode(const QString& triggerMode)
    {
        if (triggerMode_ == triggerMode) return;
        triggerMode_ = triggerMode;
        activeHandle_ = Handle::None;
        update();
    }

    void setVisibleLineTypes(bool trigger, bool pre, bool light)
    {
        showTriggerLine_ = trigger;
        showPreLine_ = pre;
        showLightLine_ = light;
        activeHandle_ = Handle::None;
        update();
    }

    void clearLineRegion()
    {
        lineRegion_.reset();
        activeHandle_ = Handle::None;
        update();
    }

    std::optional<rv1126b::LineRegionSettings> lineRegion() const { return lineRegion_; }

    std::function<void(const rv1126b::LineRegionSettings&)> onEdited;

protected:
    void paintEvent(QPaintEvent*) override
    {
        if (!lineRegion_) return;
        const QRectF content = videoContentRect();
        if (content.width() <= 1 || content.height() <= 1) return;

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), QColor(0, 0, 0, 12));

        const auto x = [&](int permille) {
            return content.left() + clampPermille(permille) * content.width() / 1000.0;
        };
        const auto y = [&](int permille) {
            return content.top() + clampPermille(permille) * content.height() / 1000.0;
        };

        QPen boundaryPen(QColor(255, 255, 255, 210), 2.0);
        painter.setPen(boundaryPen);
        painter.drawLine(QPointF(x(lineRegion_->leftPermille), content.top()),
                         QPointF(x(lineRegion_->leftPermille), content.bottom()));
        painter.drawLine(QPointF(x(lineRegion_->rightPermille), content.top()),
                         QPointF(x(lineRegion_->rightPermille), content.bottom()));

        auto drawHandle = [&](const QPointF& center, const QColor& color) {
            painter.setPen(QPen(QColor(12, 18, 28, 220), 3.0));
            painter.setBrush(color);
            painter.drawEllipse(center, 6.0, 6.0);
        };

        auto drawHorizontal = [&](int permille, const QColor& color, const QString& label, Qt::PenStyle style = Qt::SolidLine) {
            QPen pen(color, 2.5);
            pen.setStyle(style);
            painter.setPen(pen);
            const double lineY = y(permille);
            painter.drawLine(QPointF(content.left(), lineY), QPointF(content.right(), lineY));
            painter.setPen(QPen(QColor(12, 18, 28, 220), 4.0));
            painter.drawText(QPointF(content.left() + 8.0, lineY - 6.0), label);
            painter.setPen(QPen(color, 1.0));
            painter.drawText(QPointF(content.left() + 8.0, lineY - 6.0), label);
            drawHandle(QPointF(content.left() + 12.0, lineY), color);
            drawHandle(QPointF(content.right() - 12.0, lineY), color);
        };

        if (showLightLine_ && usesLightLine(triggerMode_) && lineRegion_->lightLineEnabled == 1) {
            if (lineRegion_->bidirectional == 1) {
                drawHorizontal(lineRegion_->downLightLinePermille, QColor(34, 197, 94), QStringLiteral("LIGHT D"));
                drawHorizontal(lineRegion_->upLightLinePermille, QColor(74, 222, 128), QStringLiteral("LIGHT U"), Qt::DashLine);
            } else {
                drawHorizontal(lineRegion_->lightLinePermille, QColor(34, 197, 94), QStringLiteral("LIGHT"));
            }
        }
        if (lineRegion_->bidirectional == 1) {
            if (showPreLine_ && usesPreLine(triggerMode_)) {
                drawHorizontal(lineRegion_->downPreLinePermille, QColor(250, 204, 21), QStringLiteral("PRE D"));
            }
            if (showTriggerLine_) {
                drawHorizontal(lineRegion_->downTriggerLinePermille, QColor(34, 211, 238), QStringLiteral("TRIGGER D"));
            }
            if (showPreLine_ && usesPreLine(triggerMode_)) {
                drawHorizontal(lineRegion_->upPreLinePermille, QColor(253, 224, 71), QStringLiteral("PRE U"), Qt::DashLine);
            }
            if (showTriggerLine_) {
                drawHorizontal(lineRegion_->upTriggerLinePermille, QColor(103, 232, 249), QStringLiteral("TRIGGER U"), Qt::DashLine);
            }
        } else {
            if (showPreLine_ && usesPreLine(triggerMode_)) {
                drawHorizontal(lineRegion_->preLinePermille, QColor(250, 204, 21), QStringLiteral("PRE"));
            }
            if (showTriggerLine_) {
                drawHorizontal(lineRegion_->triggerLinePermille, QColor(34, 211, 238), QStringLiteral("TRIGGER"));
            }
        }
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        activeHandle_ = hitTest(event->position());
        if (activeHandle_ != Handle::None) {
            event->accept();
        }
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!lineRegion_ || activeHandle_ == Handle::None) {
            setCursor(hitTest(event->position()) == Handle::None ? Qt::CrossCursor : Qt::SizeAllCursor);
            return;
        }
        updateHandle(event->position());
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        Q_UNUSED(event)
        activeHandle_ = Handle::None;
    }

private:
    enum class Handle {
        None,
        Left,
        Right,
        Light,
        Pre,
        Trigger,
        DownLight,
        DownPre,
        DownTrigger,
        UpLight,
        UpPre,
        UpTrigger,
    };

    QRectF videoContentRect() const { return contentRectFor(rect(), frameSize_); }

    Handle hitTest(const QPointF& point) const
    {
        if (!lineRegion_) return Handle::None;
        const QRectF content = videoContentRect();
        if (!content.adjusted(-8, -8, 8, 8).contains(point)) return Handle::None;
        const auto x = [&](int permille) { return content.left() + permille * content.width() / 1000.0; };
        const auto y = [&](int permille) { return content.top() + permille * content.height() / 1000.0; };
        const double threshold = 9.0;
        auto nearVertical = [&](int permille) { return std::abs(point.x() - x(permille)) <= threshold; };
        auto nearHorizontal = [&](int permille) { return std::abs(point.y() - y(permille)) <= threshold; };
        if (nearVertical(lineRegion_->leftPermille)) return Handle::Left;
        if (nearVertical(lineRegion_->rightPermille)) return Handle::Right;
        if (showLightLine_ && usesLightLine(triggerMode_) && lineRegion_->lightLineEnabled == 1) {
            if (lineRegion_->bidirectional == 1) {
                if (nearHorizontal(lineRegion_->downLightLinePermille)) return Handle::DownLight;
                if (nearHorizontal(lineRegion_->upLightLinePermille)) return Handle::UpLight;
            } else if (nearHorizontal(lineRegion_->lightLinePermille)) {
                return Handle::Light;
            }
        }
        if (lineRegion_->bidirectional == 1) {
            if (showPreLine_ && usesPreLine(triggerMode_) && nearHorizontal(lineRegion_->downPreLinePermille)) return Handle::DownPre;
            if (showTriggerLine_ && nearHorizontal(lineRegion_->downTriggerLinePermille)) return Handle::DownTrigger;
            if (showPreLine_ && usesPreLine(triggerMode_) && nearHorizontal(lineRegion_->upPreLinePermille)) return Handle::UpPre;
            if (showTriggerLine_ && nearHorizontal(lineRegion_->upTriggerLinePermille)) return Handle::UpTrigger;
        } else {
            if (showPreLine_ && usesPreLine(triggerMode_) && nearHorizontal(lineRegion_->preLinePermille)) return Handle::Pre;
            if (showTriggerLine_ && nearHorizontal(lineRegion_->triggerLinePermille)) return Handle::Trigger;
        }
        return Handle::None;
    }

    void updateHandle(const QPointF& point)
    {
        QRectF content = videoContentRect();
        if (content.width() <= 1 || content.height() <= 1) return;
        const int px = clampPermille(static_cast<int>(std::lround((point.x() - content.left()) * 1000.0 / content.width())));
        const int py = clampPermille(static_cast<int>(std::lround((point.y() - content.top()) * 1000.0 / content.height())));
        switch (activeHandle_) {
        case Handle::Left: lineRegion_->leftPermille = px; break;
        case Handle::Right: lineRegion_->rightPermille = px; break;
        case Handle::Light: lineRegion_->lightLinePermille = py; break;
        case Handle::Pre: lineRegion_->preLinePermille = py; break;
        case Handle::Trigger: lineRegion_->triggerLinePermille = py; break;
        case Handle::DownLight: lineRegion_->downLightLinePermille = py; break;
        case Handle::DownPre: lineRegion_->downPreLinePermille = py; break;
        case Handle::DownTrigger: lineRegion_->downTriggerLinePermille = py; break;
        case Handle::UpLight: lineRegion_->upLightLinePermille = py; break;
        case Handle::UpPre: lineRegion_->upPreLinePermille = py; break;
        case Handle::UpTrigger: lineRegion_->upTriggerLinePermille = py; break;
        case Handle::None: break;
        }
        update();
        if (onEdited) onEdited(*lineRegion_);
    }

    QSize frameSize_;
    std::optional<rv1126b::LineRegionSettings> lineRegion_;
    QString triggerMode_ = QStringLiteral("dual_line");
    bool showTriggerLine_ = true;
    bool showPreLine_ = true;
    bool showLightLine_ = true;
    Handle activeHandle_ = Handle::None;
};

namespace {
} // namespace

LivePreviewPanel::LivePreviewPanel(rv1126b::IRtspPlayer* player, QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("livePreviewPanel"));
    setMinimumSize(320, 220);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(4);

    auto* header = new QHBoxLayout;
    deviceLabel_ = new QLabel(QStringLiteral("实时预览"), this);
    deviceLabel_->setObjectName(QStringLiteral("liveDeviceLabel"));
    stateLabel_ = new QLabel(playbackStateText(rv1126b::RtspPlayerState::Idle), this);
    stateLabel_->setObjectName(QStringLiteral("livePlaybackStateLabel"));
    streamCombo_ = new QComboBox(this);
    streamCombo_->setObjectName(QStringLiteral("rtspStreamRoleCombo"));
    streamCombo_->addItem(QStringLiteral("辅码流 1080p（1920×1080）"),
                          static_cast<int>(rv1126b::RtspStreamRole::Sub));
    streamCombo_->addItem(QStringLiteral("主码流 2K（2560×1440）"),
                          static_cast<int>(rv1126b::RtspStreamRole::Main));

    triggerModeCombo_ = new QComboBox(this);
    triggerModeCombo_->setObjectName(QStringLiteral("triggerModeCombo"));
    triggerModeCombo_->setMinimumWidth(150);
    laneDirectionCombo_ = new QComboBox(this);
    laneDirectionCombo_->setObjectName(QStringLiteral("laneDirectionCombo"));
    laneDirectionCombo_->addItem(QStringLiteral("单向下行"), QStringLiteral("down"));
    laneDirectionCombo_->addItem(QStringLiteral("单向上行"), QStringLiteral("up"));
    laneDirectionCombo_->addItem(QStringLiteral("双向"), QStringLiteral("bidirectional"));
    showTriggerLineCheck_ = new QCheckBox(QStringLiteral("触发线"), this);
    showTriggerLineCheck_->setObjectName(QStringLiteral("showTriggerLineCheck"));
    showTriggerLineCheck_->setChecked(true);
    showPreLineCheck_ = new QCheckBox(QStringLiteral("预触发"), this);
    showPreLineCheck_->setObjectName(QStringLiteral("showPreLineCheck"));
    showPreLineCheck_->setChecked(true);
    showLightLineCheck_ = new QCheckBox(QStringLiteral("补光线"), this);
    showLightLineCheck_->setObjectName(QStringLiteral("showLightLineCheck"));
    showLightLineCheck_->setChecked(true);
    refreshDetectionButton_ = new QPushButton(QStringLiteral("刷新线位"), this);
    refreshDetectionButton_->setObjectName(QStringLiteral("refreshLineRegionButton"));
    saveDetectionButton_ = new QPushButton(QStringLiteral("保存并应用"), this);
    saveDetectionButton_->setObjectName(QStringLiteral("saveLineRegionButton"));
    detectionStatus_ = new QLabel(this);
    detectionStatus_->setObjectName(QStringLiteral("lineRegionStatusLabel"));

    header->addWidget(deviceLabel_);
    header->addStretch(1);
    header->addWidget(detectionStatus_);
    header->addWidget(triggerModeCombo_);
    header->addWidget(laneDirectionCombo_);
    header->addWidget(showTriggerLineCheck_);
    header->addWidget(showPreLineCheck_);
    header->addWidget(showLightLineCheck_);
    header->addWidget(refreshDetectionButton_);
    header->addWidget(saveDetectionButton_);
    header->addWidget(stateLabel_);
    root->addLayout(header);

    videoController_ = new rv1126b::VideoStreamsController(this);
    auto* streamControls = new QHBoxLayout;
    streamControls->addWidget(new QLabel(QStringLiteral("目标码流"), this));
    streamControls->addWidget(streamCombo_);
    videoCodecCombo_ = new QComboBox(this);
    videoCodecCombo_->setObjectName(QStringLiteral("rtspVideoCodecCombo"));
    videoCodecCombo_->addItem(QStringLiteral("H.264"), QStringLiteral("h264"));
    videoCodecCombo_->addItem(QStringLiteral("H.265"), QStringLiteral("h265"));
    streamControls->addWidget(new QLabel(QStringLiteral("编码"), this));
    streamControls->addWidget(videoCodecCombo_);
    applyVideoButton_ = new QPushButton(QStringLiteral("应用码流设置"), this);
    applyVideoButton_->setObjectName(QStringLiteral("applyVideoStreamsButton"));
    refreshVideoButton_ = new QPushButton(QStringLiteral("刷新"), this);
    refreshVideoButton_->setObjectName(QStringLiteral("refreshVideoStreamsButton"));
    streamControls->addWidget(applyVideoButton_);
    streamControls->addWidget(refreshVideoButton_);
    actualResolutionLabel_ = new QLabel(this);
    actualResolutionLabel_->setObjectName(QStringLiteral("rtspActualResolutionLabel"));
    clearActualResolution();
    streamControls->addWidget(actualResolutionLabel_);
    streamControls->addStretch();
    root->addLayout(streamControls);
    videoConfigStatus_ = new QLabel(this);
    videoConfigStatus_->setObjectName(QStringLiteral("videoStreamsStatusLabel"));
    videoConfigStatus_->setWordWrap(true);
    root->addWidget(videoConfigStatus_);
    connect(videoController_, &rv1126b::VideoStreamsController::changed,
            this, &LivePreviewPanel::updateVideoControls);
    connect(videoController_, &rv1126b::VideoStreamsController::previewRestartRequested,
            this, [this](const QString& deviceId) {
                clearActualResolution();
                emit previewRestartRequested(deviceId);
            });
    connect(videoCodecCombo_, &QComboBox::currentIndexChanged, this, [this] {
        videoController_->setCodec(static_cast<rv1126b::RtspStreamRole>(streamCombo_->currentData().toInt()),
                                   videoCodecCombo_->currentData().toString());
    });
    connect(applyVideoButton_, &QPushButton::clicked, videoController_, &rv1126b::VideoStreamsController::apply);
    connect(refreshVideoButton_, &QPushButton::clicked, videoController_, &rv1126b::VideoStreamsController::refresh);
    updateVideoControls();

    QWidget* output = player ? player->outputWidget() : nullptr;
    if (output) {
        videoOutput_ = output;
        output->setObjectName(QStringLiteral("rtspOutputWidget"));
        videoHost_ = new QWidget(this);
        videoHost_->setObjectName(QStringLiteral("rtspOverlayHost"));
        auto* videoLayout = new QStackedLayout(videoHost_);
        videoLayout->setStackingMode(QStackedLayout::StackAll);
        videoLayout->setContentsMargins(0, 0, 0, 0);
        videoLayout->setSpacing(0);
        videoLayout->addWidget(output);
        lineOverlay_ = new LineRegionOverlayWidget(videoHost_);
        lineOverlay_->onEdited = [this](const rv1126b::LineRegionSettings& lineRegion) {
            markLineRegionDirty(lineRegion);
        };
        videoLayout->addWidget(lineOverlay_);
        lineOverlay_->show();
        lineOverlay_->raise();
        videoHost_->installEventFilter(this);
        root->addWidget(videoHost_, 1);
    } else {
        auto* unavailable = new QLabel(QStringLiteral("实时视频播放器尚未装配"), this);
        unavailable->setObjectName(QStringLiteral("rtspUnavailableLabel"));
        unavailable->setAlignment(Qt::AlignCenter);
        unavailable->setStyleSheet(QStringLiteral("background: #121820; color: #96a5b4;"));
        root->addWidget(unavailable, 1);
    }

    connect(streamCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        clearActualResolution();
        updateVideoControls();
        emit streamRoleChanged(static_cast<rv1126b::RtspStreamRole>(
            streamCombo_->currentData().toInt()));
    });
    connect(triggerModeCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (updatingTriggerModeCombo_) return;
        triggerModeDirty_ = triggerModeConfig_ && triggerModeCombo_->currentData().toString() != triggerModeConfig_->triggerMode;
        saveDetectionButton_->setEnabled(triggerModeDirty_ || lineRegionDirty_);
        if (lineOverlay_) lineOverlay_->setTriggerMode(triggerModeCombo_->currentData().toString());
        updateLaneDirectionControls();
    });
    connect(laneDirectionCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (updatingLaneDirectionCombo_) return;
        applyLaneDirectionSelection();
    });
    auto updateVisibleLines = [this] {
        if (!lineOverlay_) return;
        lineOverlay_->setVisibleLineTypes(
            !showTriggerLineCheck_ || showTriggerLineCheck_->isChecked(),
            !showPreLineCheck_ || showPreLineCheck_->isChecked(),
            !showLightLineCheck_ || showLightLineCheck_->isChecked());
    };
    connect(showTriggerLineCheck_, &QCheckBox::toggled, this, updateVisibleLines);
    connect(showPreLineCheck_, &QCheckBox::toggled, this, updateVisibleLines);
    connect(showLightLineCheck_, &QCheckBox::toggled, this, updateVisibleLines);
    connect(refreshDetectionButton_, &QPushButton::clicked, this, [this] { loadDetectionConfig(); });
    connect(saveDetectionButton_, &QPushButton::clicked, this, [this] { saveDetectionConfig(); });
    if (player) {
        connect(player, &rv1126b::IRtspPlayer::videoFrameReceived, this, [this](const QSize& frameSize) {
            if (frameSize == actualFrameSize_) return;
            actualFrameSize_ = frameSize;
            if (lineOverlay_) lineOverlay_->setFrameSize(frameSize);
            if (!frameSize.isValid()) return;
            const auto target = streamCombo_->currentData().toInt() == static_cast<int>(rv1126b::RtspStreamRole::Main)
                ? rv1126b::mainVideoStreamDefaults() : rv1126b::subVideoStreamDefaults();
            const bool matches = frameSize == QSize(target.width, target.height);
            actualResolutionLabel_->setText(QStringLiteral("实际：%1×%2%3")
                .arg(frameSize.width()).arg(frameSize.height())
                .arg(matches ? QString() : QStringLiteral("（与目标不符）")));
            actualResolutionLabel_->setStyleSheet(matches ? QString() : QStringLiteral("color: #b42318;"));
        });
    }
    setDetectionBusy(false);
    setDetectionStatus(QString());
    detectionRefreshTimer_.setSingleShot(true);
    connect(&detectionRefreshTimer_, &QTimer::timeout, this, &LivePreviewPanel::loadDetectionConfig);
}

void LivePreviewPanel::setCurrentDevice(const QString& deviceId)
{
    const bool changed = currentDeviceId_ != deviceId;
    currentDeviceId_ = deviceId;
    deviceLabel_->setText(deviceId.isEmpty()
                              ? QStringLiteral("实时预览")
                              : QStringLiteral("实时预览 · %1").arg(deviceId));
    if (!changed) return;
    invalidateDetectionRequests();
    disconnect(detectionApiDestroyed_);
    boardApi_ = nullptr;
    clearActualResolution();
    videoController_->setDevice(deviceId, nullptr);
    triggerModeConfig_.reset();
    lineRegionConfig_.reset();
    triggerModeDirty_ = false;
    lineRegionDirty_ = false;
    if (lineOverlay_) lineOverlay_->clearLineRegion();
    loadDetectionConfig();
}

void LivePreviewPanel::setBoardApiClient(rv1126b::IBoardApiClient* boardApi, bool videoConfigAvailable)
{
    videoController_->setDevice(currentDeviceId_, videoConfigAvailable ? boardApi : nullptr);
    if (boardApi_ == boardApi) return;
    invalidateDetectionRequests();
    disconnect(detectionApiDestroyed_);
    boardApi_ = boardApi;
    if (boardApi) {
        detectionApiDestroyed_ = connect(boardApi, &QObject::destroyed, this, [this] {
            invalidateDetectionRequests();
            boardApi_ = nullptr;
            triggerModeConfig_.reset();
            lineRegionConfig_.reset();
            setDetectionBusy(false);
        });
    }
    triggerModeConfig_.reset();
    lineRegionConfig_.reset();
    triggerModeDirty_ = false;
    lineRegionDirty_ = false;
    loadDetectionConfig();
}

void LivePreviewPanel::setPlaybackState(rv1126b::RtspPlayerState state)
{
    if (state != rv1126b::RtspPlayerState::Playing) clearActualResolution();
    stateLabel_->setText(playbackStateText(state));
    stateLabel_->setStyleSheet(state == rv1126b::RtspPlayerState::Error
                                   ? QStringLiteral("color: #b42318;")
                                   : QString());
}

void LivePreviewPanel::setPlaybackError(const rv1126b::ApiError& error)
{
    stateLabel_->setText(error.message.isEmpty()
                             ? QStringLiteral("视频播放失败，等待重连")
                             : error.message);
    stateLabel_->setStyleSheet(QStringLiteral("color: #b42318;"));
}

void LivePreviewPanel::clearActualResolution()
{
    actualFrameSize_ = QSize();
    actualResolutionLabel_->setText(QStringLiteral("实际分辨率：等待视频帧"));
    actualResolutionLabel_->setStyleSheet(QString());
}

void LivePreviewPanel::updateVideoControls()
{
    const QSignalBlocker blocker(videoCodecCombo_);
    const auto role = static_cast<rv1126b::RtspStreamRole>(streamCombo_->currentData().toInt());
    videoCodecCombo_->setCurrentIndex(videoController_->hasConfig()
        ? videoCodecCombo_->findData(videoController_->codec(role)) : -1);
    videoCodecCombo_->setEnabled(videoController_->editable());
    applyVideoButton_->setEnabled(videoController_->canApply());
    refreshVideoButton_->setEnabled(videoController_->available() && !videoController_->busy());
    videoConfigStatus_->setText(videoController_->status());
    videoConfigStatus_->setStyleSheet(videoController_->hasError() ? QStringLiteral("color: #b42318;") : QString());
}

bool LivePreviewPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == videoHost_ && lineOverlay_
        && (event->type() == QEvent::Resize
            || event->type() == QEvent::Show
            || event->type() == QEvent::LayoutRequest)) {
        lineOverlay_->raise();
    }
    return QWidget::eventFilter(watched, event);
}

void LivePreviewPanel::loadDetectionConfig()
{
    if (!boardApi_ || currentDeviceId_.isEmpty()) {
        setDetectionStatus(QString());
        if (lineOverlay_) lineOverlay_->clearLineRegion();
        return;
    }
    if (loadingDetection_ || savingDetection_) return;
    if (!detectionContext_) detectionContext_ = new QObject(this);
    const auto generation = detectionGeneration_;
    detectionReadsPending_ = 2;
    detectionReadFailed_ = false;
    loadingDetection_ = true;
    setDetectionBusy(true);
    setDetectionStatus(QStringLiteral("读取线位..."));
    const QPointer<LivePreviewPanel> guard(this);

    boardApi_->getTriggerModeConfig(detectionContext_, [this, generation, guard = QPointer<LivePreviewPanel>(this)](rv1126b::ApiResult<rv1126b::TriggerModeConfigDto> result) {
        if (!guard || generation != detectionGeneration_) return;
        if (result.isSuccess()) {
            applyTriggerModeConfig(result.value());
        } else {
            detectionReadFailed_ = true;
            setDetectionStatus(result.error().message, true);
        }
        finishDetectionRead(generation);
    });
    if (!guard || generation != detectionGeneration_ || !boardApi_) return;
    boardApi_->getLineRegionConfig(detectionContext_, [this, generation, guard = QPointer<LivePreviewPanel>(this)](rv1126b::ApiResult<rv1126b::LineRegionConfigDto> result) {
        if (!guard || generation != detectionGeneration_) return;
        if (result.isSuccess()) {
            applyLineRegionConfig(result.value());
        } else {
            detectionReadFailed_ = true;
            setDetectionStatus(result.error().message, true);
        }
        finishDetectionRead(generation);
    });
}

void LivePreviewPanel::saveDetectionConfig()
{
    if (!boardApi_) {
        setDetectionStatus(QStringLiteral("设备 API 不可用"), true);
        return;
    }
    if (savingDetection_ || loadingDetection_) return;
    if (!triggerModeDirty_ && !lineRegionDirty_) {
        setDetectionStatus(QStringLiteral("没有需要保存的修改"));
        return;
    }
    savingDetection_ = true;
    if (!detectionContext_) detectionContext_ = new QObject(this);
    const auto generation = detectionGeneration_;
    setDetectionBusy(true);
    setDetectionStatus(QStringLiteral("保存线位..."));

    if (triggerModeDirty_) {
        if (!triggerModeConfig_) {
            setDetectionBusy(false);
            savingDetection_ = false;
            setDetectionStatus(QStringLiteral("缺少检测模式 revision，请先刷新"), true);
            return;
        }
        rv1126b::TriggerModeUpdate update;
        update.expectedRevision = triggerModeConfig_->revision;
        update.triggerMode = triggerModeCombo_->currentData().toString();
        boardApi_->putTriggerModeConfig(update, detectionContext_, [this, generation, guard = QPointer<LivePreviewPanel>(this)](rv1126b::ApiResult<rv1126b::TriggerModeConfigDto> result) {
            if (!guard || generation != detectionGeneration_ || !boardApi_) return;
            if (!result.isSuccess()) {
                savingDetection_ = false;
                setDetectionBusy(false);
                setDetectionStatus(result.error().message, true);
                return;
            }
            applyTriggerModeConfig(result.value());
            saveLineRegionConfig(result.value().runtimeRevision, result.value().restartRequired);
        });
        return;
    }
    saveLineRegionConfig(lineRegionConfig_ ? lineRegionConfig_->runtimeRevision : QString(), false);
}

void LivePreviewPanel::saveLineRegionConfig(const QString& runtimeRevision, bool restartRequired)
{
    if (!boardApi_) return;
    const auto generation = detectionGeneration_;
    if (!lineRegionDirty_) {
        applyRuntimeConfigIfNeeded(runtimeRevision, restartRequired);
        return;
    }
    if (!lineRegionConfig_ || !lineOverlay_ || !lineOverlay_->lineRegion()) {
        savingDetection_ = false;
        setDetectionBusy(false);
        setDetectionStatus(QStringLiteral("缺少检测线 revision，请先刷新"), true);
        return;
    }
    rv1126b::LineRegionUpdate update;
    update.expectedRevision = lineRegionConfig_->revision;
    update.lineRegion = *lineOverlay_->lineRegion();
    syncSingleDirectionAliases(&update.lineRegion);
    normalizeHiddenLinesForTriggerOnly(&update.lineRegion);
    boardApi_->putLineRegionConfig(update, detectionContext_, [this, restartRequired, generation, guard = QPointer<LivePreviewPanel>(this)](rv1126b::ApiResult<rv1126b::LineRegionConfigDto> result) {
        if (!guard || generation != detectionGeneration_ || !boardApi_) return;
        if (!result.isSuccess()) {
            savingDetection_ = false;
            setDetectionBusy(false);
            setDetectionStatus(result.error().message, true);
            return;
        }
        applyLineRegionConfig(result.value());
        applyRuntimeConfigIfNeeded(result.value().runtimeRevision, restartRequired || result.value().restartRequired);
    });
}

void LivePreviewPanel::applyRuntimeConfigIfNeeded(const QString& runtimeRevision, bool restartRequired)
{
    if (!boardApi_) return;
    const auto generation = detectionGeneration_;
    if (!restartRequired) {
        savingDetection_ = false;
        setDetectionBusy(false);
        scheduleDetectionRefresh(1000, QStringLiteral("已保存，正在读回确认..."));
        return;
    }
    if (runtimeRevision.isEmpty()) {
        savingDetection_ = false;
        setDetectionBusy(false);
        setDetectionStatus(QStringLiteral("已保存，板端未返回运行 revision，请手动应用"), true);
        return;
    }
    rv1126b::RuntimeApplyUpdate update;
    update.expectedRevision = runtimeRevision;
    update.scope = QStringLiteral("rkipc");
    boardApi_->applyRuntimeConfig(update, detectionContext_, [this, generation, guard = QPointer<LivePreviewPanel>(this)](rv1126b::ApiResult<rv1126b::RuntimeApplyDto> result) {
        if (!guard || generation != detectionGeneration_ || !boardApi_) return;
        savingDetection_ = false;
        setDetectionBusy(false);
        if (!result.isSuccess()) {
            if (isTransientNetworkError(result.error())) {
                scheduleDetectionRefresh(8000, QStringLiteral("已下发，板端正在应用，稍后自动刷新..."));
                return;
            }
            setDetectionStatus(result.error().message, true);
            return;
        }
        scheduleDetectionRefresh(4000, QStringLiteral("已保存并应用，稍后自动刷新..."));
    });
}

void LivePreviewPanel::scheduleDetectionRefresh(int delayMs, const QString& message)
{
    setDetectionStatus(message);
    detectionRefreshTimer_.start(delayMs);
}

void LivePreviewPanel::invalidateDetectionRequests()
{
    ++detectionGeneration_;
    detectionRefreshTimer_.stop();
    delete detectionContext_.data();
    loadingDetection_ = savingDetection_ = false;
    detectionReadsPending_ = 0;
    setDetectionBusy(false);
}

void LivePreviewPanel::finishDetectionRead(quint64 generation)
{
    if (generation != detectionGeneration_ || --detectionReadsPending_ != 0) return;
    loadingDetection_ = false;
    if (!detectionReadFailed_) setDetectionStatus(QStringLiteral("线位已读取"));
    setDetectionBusy(false);
}

void LivePreviewPanel::setDetectionBusy(bool busy)
{
    const bool enabled = !busy && boardApi_ && !currentDeviceId_.isEmpty();
    if (refreshDetectionButton_) refreshDetectionButton_->setEnabled(enabled);
    if (saveDetectionButton_) saveDetectionButton_->setEnabled(enabled && (triggerModeDirty_ || lineRegionDirty_));
    if (triggerModeCombo_) triggerModeCombo_->setEnabled(enabled && triggerModeConfig_ && triggerModeConfig_->writeEnabled);
    if (laneDirectionCombo_) laneDirectionCombo_->setEnabled(enabled && lineRegionConfig_ && lineRegionConfig_->writeEnabled);
    if (showTriggerLineCheck_) showTriggerLineCheck_->setEnabled(true);
    updateLaneDirectionControls();
}

void LivePreviewPanel::setDetectionStatus(const QString& message, bool error)
{
    if (!detectionStatus_) return;
    detectionStatus_->setText(message);
    detectionStatus_->setStyleSheet(error ? QStringLiteral("color: #b42318;") : QStringLiteral("color: #475467;"));
}

void LivePreviewPanel::applyTriggerModeConfig(const rv1126b::TriggerModeConfigDto& config)
{
    triggerModeConfig_ = config;
    if (lineOverlay_) lineOverlay_->setTriggerMode(config.triggerMode);
    updatingTriggerModeCombo_ = true;
    triggerModeCombo_->clear();
    for (const QString& mode : config.supportedModes) {
        triggerModeCombo_->addItem(mode, mode);
    }
    const int index = triggerModeCombo_->findData(config.triggerMode);
    if (index >= 0) triggerModeCombo_->setCurrentIndex(index);
    updatingTriggerModeCombo_ = false;
    triggerModeDirty_ = false;
    updateLaneDirectionControls();
    setDetectionBusy(loadingDetection_ || savingDetection_);
}

void LivePreviewPanel::applyLineRegionConfig(const rv1126b::LineRegionConfigDto& config)
{
    lineRegionConfig_ = config;
    syncSingleDirectionAliases(&lineRegionConfig_->lineRegion);
    if (lineOverlay_) {
        lineOverlay_->setGeometry(lineOverlay_->parentWidget() ? lineOverlay_->parentWidget()->rect() : lineOverlay_->rect());
        lineOverlay_->setLineRegion(lineRegionConfig_->lineRegion);
        lineOverlay_->show();
        lineOverlay_->raise();
    }
    lineRegionDirty_ = false;
    updateLaneDirectionControls();
    setDetectionBusy(loadingDetection_ || savingDetection_);
}

void LivePreviewPanel::updateLaneDirectionControls()
{
    if (!laneDirectionCombo_ || !lineRegionConfig_) return;
    const rv1126b::LineRegionSettings& line = lineRegionConfig_->lineRegion;
    const QString mode = line.bidirectional == 1
        ? QStringLiteral("bidirectional")
        : line.direction < 0 ? QStringLiteral("up") : QStringLiteral("down");
    const int index = laneDirectionCombo_->findData(mode);
    updatingLaneDirectionCombo_ = true;
    if (index >= 0) laneDirectionCombo_->setCurrentIndex(index);
    updatingLaneDirectionCombo_ = false;

    const QString triggerMode = triggerModeCombo_ ? triggerModeCombo_->currentData().toString() : QString();
    const bool preAvailable = usesPreLine(triggerMode);
    const bool lightAvailable = usesLightLine(triggerMode) && line.lightLineEnabled == 1;
    if (showPreLineCheck_) showPreLineCheck_->setEnabled(preAvailable);
    if (showLightLineCheck_) showLightLineCheck_->setEnabled(lightAvailable);
    if (lineOverlay_) {
        lineOverlay_->setVisibleLineTypes(
            !showTriggerLineCheck_ || showTriggerLineCheck_->isChecked(),
            preAvailable && (!showPreLineCheck_ || showPreLineCheck_->isChecked()),
            lightAvailable && (!showLightLineCheck_ || showLightLineCheck_->isChecked()));
    }
}

void LivePreviewPanel::applyLaneDirectionSelection()
{
    if (!lineRegionConfig_ || !laneDirectionCombo_) return;
    rv1126b::LineRegionSettings line = lineRegionConfig_->lineRegion;
    const QString mode = laneDirectionCombo_->currentData().toString();
    if (mode == QLatin1String("bidirectional")) {
        line.bidirectional = 1;
        line.direction = 1;
    } else if (mode == QLatin1String("up")) {
        line.bidirectional = 0;
        line.direction = -1;
        line.lightLinePermille = line.upLightLinePermille;
        line.preLinePermille = line.upPreLinePermille;
        line.triggerLinePermille = line.upTriggerLinePermille;
    } else {
        line.bidirectional = 0;
        line.direction = 1;
        line.lightLinePermille = line.downLightLinePermille;
        line.preLinePermille = line.downPreLinePermille;
        line.triggerLinePermille = line.downTriggerLinePermille;
    }
    syncSingleDirectionAliases(&line);
    lineRegionConfig_->lineRegion = line;
    if (lineOverlay_) lineOverlay_->setLineRegion(line);
    lineRegionDirty_ = true;
    saveDetectionButton_->setEnabled(!loadingDetection_ && !savingDetection_);
    setDetectionStatus(QStringLiteral("线位未保存"));
    updateLaneDirectionControls();
}

void LivePreviewPanel::syncSingleDirectionAliases(rv1126b::LineRegionSettings* lineRegion) const
{
    if (!lineRegion || lineRegion->bidirectional == 1) return;
    if (lineRegion->direction < 0) {
        lineRegion->upLightLinePermille = lineRegion->lightLinePermille;
        lineRegion->upPreLinePermille = lineRegion->preLinePermille;
        lineRegion->upTriggerLinePermille = lineRegion->triggerLinePermille;
    } else {
        lineRegion->downLightLinePermille = lineRegion->lightLinePermille;
        lineRegion->downPreLinePermille = lineRegion->preLinePermille;
        lineRegion->downTriggerLinePermille = lineRegion->triggerLinePermille;
    }
}

void LivePreviewPanel::normalizeHiddenLinesForTriggerOnly(rv1126b::LineRegionSettings* lineRegion) const
{
    const QString triggerMode = triggerModeCombo_ ? triggerModeCombo_->currentData().toString() : QString();
    if (!lineRegion || triggerMode != QLatin1String("trigger_only")) return;

    normalizeTriggerOnlyDirection(
        &lineRegion->triggerLinePermille,
        &lineRegion->preLinePermille,
        &lineRegion->lightLinePermille,
        lineRegion->direction);
    normalizeTriggerOnlyDirection(
        &lineRegion->downTriggerLinePermille,
        &lineRegion->downPreLinePermille,
        &lineRegion->downLightLinePermille,
        1);
    normalizeTriggerOnlyDirection(
        &lineRegion->upTriggerLinePermille,
        &lineRegion->upPreLinePermille,
        &lineRegion->upLightLinePermille,
        -1);
}

void LivePreviewPanel::markLineRegionDirty(const rv1126b::LineRegionSettings& lineRegion)
{
    if (!lineRegionConfig_) return;
    rv1126b::LineRegionSettings next = lineRegion;
    syncSingleDirectionAliases(&next);
    lineRegionConfig_->lineRegion = next;
    lineRegionDirty_ = true;
    saveDetectionButton_->setEnabled(!loadingDetection_ && !savingDetection_);
    setDetectionStatus(QStringLiteral("线位未保存"));
}
