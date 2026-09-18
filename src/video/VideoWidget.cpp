#include "VideoWidget.h"

#include <QDateTime>
#include <QFont>
#include <QLinearGradient>
#include <QImageReader>
#include <QPainter>
#include <QPen>
#include <QTimer>
#include <QCache>
#include <QFileInfo>
#include <QMutex>
#include <QPromise>
#include <QThreadPool>
#include <QResizeEvent>
#include <QMouseEvent>
#include <QDesktopServices>
#include <QUrl>
#include <memory>

namespace {
struct ImageLoaderState {
    QMutex mutex;
    QCache<QString, QImage> images{64 * 1024}; // KiB, shared by all previews.
    QThreadPool pool;
    ImageLoaderState() { pool.setMaxThreadCount(2); }
    ~ImageLoaderState() { pool.waitForDone(); }
};

ImageLoaderState& imageState()
{
    static ImageLoaderState state;
    return state;
}

QThreadPool& imagePool()
{
    return imageState().pool;
}

QImage readPreviewImage(const QString& path, QSize bounds)
{
    auto& mutex = imageState().mutex;
    auto& images = imageState().images;
    const QFileInfo info(path);
    const QString key = QStringLiteral("%1|%2|%3|%4x%5")
        .arg(info.absoluteFilePath()).arg(info.size()).arg(info.lastModified().toMSecsSinceEpoch())
        .arg(bounds.width()).arg(bounds.height());
    {
        QMutexLocker lock(&mutex);
        if (const auto* image = images.object(key)) return *image;
    }
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QSize source = reader.size();
    if (!source.isValid()) return {};
    if (reader.transformation() & QImageIOHandler::TransformationRotate90) bounds.transpose();
    reader.setScaledSize(source.scaled(bounds, Qt::KeepAspectRatio));
    QImage image = reader.read();
    if (!image.isNull()) {
        QMutexLocker lock(&mutex);
        const int cost = int((image.sizeInBytes() + 1023) / 1024);
        images.insert(key, new QImage(image), cost);
    }
    return image;
}
}

VideoWidget::VideoWidget(Mode mode, QWidget* parent)
    : QWidget(parent)
    , mode_(mode)
    , timer_(new QTimer(this))
{
    setMinimumSize(320, 220);
    setAutoFillBackground(false);
    if (mode_ == Mode::Snapshot) setToolTip(QStringLiteral("双击查看原图"));

    timer_->setInterval(mode_ == Mode::Live ? 1000 / displayOptions_.previewFrameRate : 500);
    connect(timer_, &QTimer::timeout, this, &VideoWidget::advanceFrame);
    timer_->start();
    evidenceLoadTimer_ = new QTimer(this);
    evidenceLoadTimer_->setSingleShot(true);
    evidenceLoadTimer_->setInterval(100);
    connect(evidenceLoadTimer_, &QTimer::timeout, this, &VideoWidget::loadEvidenceImage);
}

VideoWidget::~VideoWidget()
{
    if (imageWatcher_) imageWatcher_->cancel();
}

void VideoWidget::setDevice(const Device* device)
{
    hasDevice_ = device != nullptr;
    if (device) {
        device_ = *device;
    }
    update();
}

void VideoWidget::setLatestRecord(const CaptureRecord* record)
{
    if (!timer_->isActive()) timer_->start();
    hasVehicleEvent_ = false;
    hasLatestRecord_ = record != nullptr;
    if (record) {
        latestRecord_ = *record;
    }
    update();
}

void VideoWidget::setVehicleEvent(const rv1126b::VehicleEvent* event)
{
    timer_->stop();
    if (event && hasVehicleEvent_ && vehicleEvent_.identity == event->identity
        && vehicleEvent_.evidenceRelativeUrl == event->evidenceRelativeUrl) {
        vehicleEvent_ = *event;
        update();
        return;
    }
    ++imageGeneration_;
    evidenceLoadTimer_->stop();
    imageLease_.reset();
    hasVehicleEvent_ = event != nullptr;
    evidenceEntry_.reset();
    evidenceImage_ = QImage();
    evidenceMessage_ = QStringLiteral("正在读取本地图片状态…");
    if (event) vehicleEvent_ = *event;
    update();
}

void VideoWidget::setEvidenceState(
    const std::optional<rv1126b::EvidenceCacheEntry>& entry,
    bool deviceOnline)
{
    if (entry && (!hasVehicleEvent_ || entry->identity != vehicleEvent_.identity)) return;
    if (entry && evidenceEntry_ && entry->identity == evidenceEntry_->identity
        && entry->status == rv1126b::EvidenceCacheStatus::Available
        && entry->status == evidenceEntry_->status && entry->localFilePath == evidenceEntry_->localFilePath
        && entry->updatedEpochMs == evidenceEntry_->updatedEpochMs
        && entry->contentLength == evidenceEntry_->contentLength
        && (!evidenceImage_.isNull() || evidenceLoadTimer_->isActive() || imageWatcher_)) return;
    ++imageGeneration_;
    evidenceLoadTimer_->stop();
    imageLease_.reset();
    evidenceEntry_ = entry;
    evidenceDeviceOnline_ = deviceOnline;
    evidenceImage_ = QImage();
    if (!hasVehicleEvent_) return;

    if (!entry.has_value()) {
        evidenceMessage_ = deviceOnline ? QStringLiteral("图片尚未缓存，正在请求…")
                                        : QStringLiteral("设备离线，图片未缓存");
        update();
        return;
    }

    using rv1126b::EvidenceCacheStatus;
    switch (entry->status) {
    case EvidenceCacheStatus::Available: {
        evidenceMessage_ = QStringLiteral("正在读取图片…");
        evidenceLoadTimer_->start();
        break;
    }
    case EvidenceCacheStatus::Queued:
    case EvidenceCacheStatus::Downloading:
        evidenceMessage_ = QStringLiteral("图片下载中…");
        break;
    case EvidenceCacheStatus::RetryWait:
        evidenceMessage_ = QStringLiteral("图片处理中，等待重试…");
        break;
    case EvidenceCacheStatus::Missing:
        evidenceMessage_ = entry->failureCode == QStringLiteral("offline_not_cached")
            ? QStringLiteral("设备离线，图片未缓存")
            : QStringLiteral("图片未缓存");
        break;
    case EvidenceCacheStatus::Failed:
        evidenceMessage_ = QStringLiteral("图片缓存失败");
        break;
    case EvidenceCacheStatus::NotRequested:
    default:
        evidenceMessage_ = deviceOnline ? QStringLiteral("图片尚未缓存，正在请求…")
                                        : QStringLiteral("设备离线，图片未缓存");
        break;
    }
    update();
}

void VideoWidget::clearVehicleEvent()
{
    ++imageGeneration_;
    evidenceLoadTimer_->stop();
    imageLease_.reset();
    hasVehicleEvent_ = false;
    evidenceEntry_.reset();
    evidenceImage_ = QImage();
    evidenceMessage_.clear();
    update();
}

QString VideoWidget::evidenceMessage() const { return evidenceMessage_; }
bool VideoWidget::hasDecodedEvidence() const { return !evidenceImage_.isNull(); }

void VideoWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && evidenceEntry_
        && evidenceEntry_->status == rv1126b::EvidenceCacheStatus::Available) {
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(evidenceEntry_->localFilePath))) {
            evidenceMessage_ = QStringLiteral("无法打开原图");
            update();
        }
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void VideoWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (hasVehicleEvent_ && evidenceEntry_
        && evidenceEntry_->status == rv1126b::EvidenceCacheStatus::Available) {
        ++imageGeneration_;
        evidenceLoadTimer_->start();
    }
}

void VideoWidget::loadEvidenceImage()
{
    if (imageWatcher_ || !hasVehicleEvent_ || !evidenceEntry_
        || evidenceEntry_->status != rv1126b::EvidenceCacheStatus::Available) return;
    const auto generation = imageGeneration_;
    const QString path = evidenceEntry_->localFilePath;
    imageLease_ = rv1126b::CacheFileLease::acquire(path);
    if (!imageLease_) { evidenceLoadTimer_->start(); return; }
    const QSize bounds = (size() * devicePixelRatioF()).expandedTo(QSize(1, 1));
    auto promise = std::make_shared<QPromise<QImage>>();
    auto* watcher = new QFutureWatcher<QImage>(this);
    imageWatcher_ = watcher;
    connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, generation] {
        imageWatcher_ = nullptr;
        if (generation == imageGeneration_ && !watcher->isCanceled()) {
            evidenceImage_ = watcher->result();
            evidenceMessage_ = evidenceImage_.isNull() ? QStringLiteral("本地图片损坏或无法解码") : QString();
            update();
        }
        watcher->deleteLater();
        if (generation != imageGeneration_ && !evidenceLoadTimer_->isActive()) loadEvidenceImage();
    });
    promise->start();
    watcher->setFuture(promise->future());
    imagePool().start([promise, path, bounds, lease = imageLease_] {
        if (!promise->isCanceled()) promise->addResult(readPreviewImage(path, bounds));
        promise->finish();
    });
}

void VideoWidget::setDisplayOptions(const VideoDisplayOptions& options)
{
    displayOptions_ = options;
    const int frameRate = qBound(1, displayOptions_.previewFrameRate, 60);
    timer_->setInterval(mode_ == Mode::Live ? 1000 / frameRate : qMax(250, 1000 / frameRate));
    update();
}

void VideoWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRect rect = this->rect().adjusted(1, 1, -1, -1);
    drawBackground(painter, rect);

    if (mode_ == Mode::Snapshot && hasVehicleEvent_) {
        drawEvidence(painter, rect);
        drawVehicleEventOverlay(painter, rect);
        return;
    }

    if (!hasDevice_) {
        drawEmptyState(painter, rect, QStringLiteral("请选择设备"));
        return;
    }

    if (mode_ == Mode::Snapshot && !hasLatestRecord_) {
        drawRoad(painter, rect);
        drawEmptyState(painter, rect, QStringLiteral("暂无抓拍图片"));
        return;
    }

    drawRoad(painter, rect);
    if (displayOptions_.showOnlyVehicleFrames && !hasLatestRecord_) {
        drawEmptyState(painter, rect, QStringLiteral("暂无车辆画面"));
        return;
    }
    drawVehicleOverlay(painter, rect);
    drawPlateCloseup(painter, rect);
    drawTextOverlay(painter, rect);
}

void VideoWidget::drawEvidence(QPainter& painter, const QRect& rect) const
{
    if (evidenceImage_.isNull()) {
        drawEmptyState(painter, rect, evidenceMessage_.isEmpty()
                                         ? QStringLiteral("暂无融合图片")
                                         : evidenceMessage_);
        return;
    }
    const QSize targetSize = evidenceImage_.size().scaled(rect.size(), Qt::KeepAspectRatio);
    const QRect target(QPoint(rect.center().x() - targetSize.width() / 2,
                              rect.center().y() - targetSize.height() / 2), targetSize);
    painter.drawImage(target, evidenceImage_);
}

void VideoWidget::drawVehicleEventOverlay(QPainter& painter, const QRect& rect) const
{
    const QString time = QDateTime::fromMSecsSinceEpoch(vehicleEvent_.eventTime.epochMs)
                             .toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    const QString plate = vehicleEvent_.plateText.isEmpty() ? QStringLiteral("-")
                                                            : vehicleEvent_.plateText;
    const QString speed = vehicleEvent_.speedValid
        ? QStringLiteral("%1 km/h").arg(vehicleEvent_.speedKmh)
        : QStringLiteral("速度无效");
    const QString warning = vehicleEvent_.eventTime.quality.value
            == rv1126b::TimeQuality::BoardEpochUnverified
        ? QStringLiteral("  ⚠ 板端时间未校验") : QString();

    const QRect overlay(rect.left() + 8, rect.bottom() - 68, rect.width() - 16, 60);
    painter.fillRect(overlay, QColor(0, 0, 0, 170));
    painter.setPen(Qt::white);
    painter.setFont(QFont(QStringLiteral("Microsoft YaHei"), 9));
    painter.drawText(overlay.adjusted(8, 5, -8, -5), Qt::AlignLeft | Qt::AlignTop,
                     QStringLiteral("%1  %2  %3\n设备 %4  事件 %5/%6%7")
                         .arg(time, plate, speed, vehicleEvent_.identity.deviceId)
                         .arg(vehicleEvent_.identity.eventId)
                         .arg(vehicleEvent_.identity.trackId)
                         .arg(warning));
}

void VideoWidget::advanceFrame()
{
    ++frameIndex_;
    if (mode_ == Mode::Live || hasLatestRecord_) {
        update();
    }
}

void VideoWidget::drawBackground(QPainter& painter, const QRect& rect) const
{
    QLinearGradient gradient(rect.topLeft(), rect.bottomLeft());
    gradient.setColorAt(0.0, QColor(18, 24, 32));
    gradient.setColorAt(0.55, QColor(30, 34, 38));
    gradient.setColorAt(1.0, QColor(12, 16, 20));
    painter.fillRect(rect, gradient);

    painter.setPen(QPen(QColor(75, 86, 98), 1));
    painter.drawRect(rect);
}

void VideoWidget::drawRoad(QPainter& painter, const QRect& rect) const
{
    const int horizon = rect.top() + rect.height() / 4;
    const QPoint leftNear(rect.left() + rect.width() / 10, rect.bottom());
    const QPoint rightNear(rect.right() - rect.width() / 10, rect.bottom());
    const QPoint leftFar(rect.center().x() - rect.width() / 8, horizon);
    const QPoint rightFar(rect.center().x() + rect.width() / 8, horizon);

    QPolygon road;
    road << leftNear << leftFar << rightFar << rightNear;
    painter.setBrush(QColor(45, 48, 50));
    painter.setPen(Qt::NoPen);
    painter.drawPolygon(road);

    if (displayOptions_.showCalibrationLines) {
        painter.setPen(QPen(QColor(210, 210, 190), 2, Qt::DashLine));
        for (int i = -1; i <= 1; ++i) {
            const int nearX = rect.center().x() + i * rect.width() / 7;
            const int farX = rect.center().x() + i * rect.width() / 24;
            painter.drawLine(QPoint(nearX, rect.bottom()), QPoint(farX, horizon));
        }
    }

    painter.setPen(QPen(QColor(110, 118, 125), 2));
    painter.drawLine(leftNear, leftFar);
    painter.drawLine(rightNear, rightFar);

    painter.setPen(QPen(QColor(55, 75, 72), 1));
    for (int y = horizon; y < rect.bottom(); y += 28) {
        painter.drawLine(rect.left() + 16, y, rect.right() - 16, y + 12);
    }
}

void VideoWidget::drawVehicleOverlay(QPainter& painter, const QRect& rect) const
{
    const bool snapshot = mode_ == Mode::Snapshot && hasLatestRecord_;
    const int progress = snapshot ? 55 : (frameIndex_ * 3) % 100;
    const int vehicleWidth = rect.width() / 5;
    const int vehicleHeight = rect.height() / 5;
    const int x = rect.center().x() - vehicleWidth / 2 + (snapshot ? 0 : (progress - 50) * rect.width() / 420);
    const int y = rect.top() + rect.height() / 2 + (snapshot ? rect.height() / 12 : progress * rect.height() / 520);

    QRect vehicleRect(x, y, vehicleWidth, vehicleHeight);
    vehicleRect = vehicleRect.intersected(rect.adjusted(24, 36, -24, -28));

    painter.setPen(QPen(QColor(255, 60, 45), 3));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(vehicleRect);

    QRect plateRect(
        vehicleRect.center().x() - vehicleRect.width() / 4,
        vehicleRect.bottom() - vehicleRect.height() / 4,
        vehicleRect.width() / 2,
        qMax(18, vehicleRect.height() / 5));

    painter.setPen(QPen(QColor(60, 150, 255), 2));
    painter.drawRect(plateRect);

    painter.fillRect(plateRect.adjusted(1, 1, -1, -1), QColor(35, 85, 155, 190));
    painter.setPen(Qt::white);
    painter.setFont(QFont(QStringLiteral("Microsoft YaHei"), 9, QFont::Bold));
    painter.drawText(plateRect, Qt::AlignCenter, hasLatestRecord_ ? latestRecord_.plateNumber : QStringLiteral("粤B12345"));
}

void VideoWidget::drawPlateCloseup(QPainter& painter, const QRect& rect) const
{
    if (mode_ != Mode::Snapshot || !hasLatestRecord_ || displayOptions_.plateImagePosition == QStringLiteral("hidden")) {
        return;
    }

    QRect closeupRect;
    if (displayOptions_.plateImagePosition == QStringLiteral("bottom")) {
        closeupRect = QRect(rect.center().x() - rect.width() / 5, rect.bottom() - 112, rect.width() * 2 / 5, 56);
    } else {
        closeupRect = QRect(rect.right() - rect.width() / 3 - 18, rect.center().y() - 28, rect.width() / 3, 56);
    }

    painter.setPen(QPen(QColor(70, 150, 255), 2));
    painter.setBrush(QColor(15, 42, 78, 210));
    painter.drawRoundedRect(closeupRect, 4, 4);
    painter.setPen(Qt::white);
    painter.setFont(QFont(QStringLiteral("Microsoft YaHei"), 14, QFont::Bold));
    painter.drawText(closeupRect, Qt::AlignCenter, latestRecord_.plateNumber);
}

void VideoWidget::drawTextOverlay(QPainter& painter, const QRect& rect) const
{
    const CaptureRecord* record = hasLatestRecord_ ? &latestRecord_ : nullptr;
    const QString plate = record ? record->plateNumber : QStringLiteral("实时检测中");
    const QString plateColor = record ? record->plateColor : QStringLiteral("-");
    const int speed = record ? record->speedKmh : 0;
    const int speedLimit = record ? record->speedLimitKmh : device_.config.speedLimitKmh;
    const QString direction = record ? record->direction : device_.config.direction;
    const QString captureTime = record
        ? record->timestamp.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
        : QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));

    painter.setFont(QFont(QStringLiteral("Microsoft YaHei"), 10));
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 150));
    painter.drawRoundedRect(rect.adjusted(10, 10, -10, -rect.height() + 100), 4, 4);

    painter.setPen(QColor(235, 242, 250));
    const int left = rect.left() + 18;
    int top = rect.top() + 22;
    const int lineHeight = 20;

    painter.drawText(left, top, titleText());
    top += lineHeight;
    painter.drawText(left, top, QStringLiteral("地点：%1   方向：%2").arg(device_.config.location, direction));
    top += lineHeight;
    painter.drawText(left, top, QStringLiteral("时间：%1   防伪码：%2").arg(captureTime, antiFakeCode()));

    painter.setBrush(QColor(0, 0, 0, 150));
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(rect.adjusted(10, rect.height() - 76, -10, -10), 4, 4);

    painter.setPen(QColor(235, 242, 250));
    const QString vehicleLine = displayOptions_.overlaySpeed
        ? QStringLiteral("车牌：%1   颜色：%2   速度：%3 km/h   限速：%4 km/h")
              .arg(plate, plateColor)
              .arg(speed)
              .arg(speedLimit)
        : QStringLiteral("车牌：%1   颜色：%2").arg(plate, plateColor);
    painter.drawText(rect.left() + 18, rect.bottom() - 48, vehicleLine);
    painter.drawText(rect.left() + 18, rect.bottom() - 24,
                     QStringLiteral("设备：%1   状态：%2")
                         .arg(device_.name, connectionStateText(device_.status.connectionState)));
}

void VideoWidget::drawEmptyState(QPainter& painter, const QRect& rect, const QString& text) const
{
    painter.setPen(QColor(150, 165, 180));
    painter.setFont(QFont(QStringLiteral("Microsoft YaHei"), 16, QFont::Bold));
    painter.drawText(rect, Qt::AlignCenter, text);
}

QString VideoWidget::titleText() const
{
    return mode_ == Mode::Live ? QStringLiteral("实时预览") : QStringLiteral("最近抓拍");
}

QString VideoWidget::antiFakeCode() const
{
    const QString seed = hasLatestRecord_ ? latestRecord_.id : device_.id;
    return QString::number(qHash(seed) & 0xFFFFFF, 16).rightJustified(8, QLatin1Char('0')).toUpper();
}
