#include "SpikeWindow.h"
#include "../../src/services/UiPerformanceMonitor.h"

#include "../../src/rv1126b/infrastructure/video/QtMultimediaRtspPlayer.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStringConverter>
#include <QTextStream>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#endif

namespace {

QString csvCell(QString value)
{
    value.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QStringLiteral("\"") + value + QStringLiteral("\"");
}

template<typename T>
double percentile(QList<T> values, double fraction)
{
    if (values.isEmpty()) {
        return -1.0;
    }
    std::sort(values.begin(), values.end());
    const int index = qBound(0,
                             static_cast<int>(std::ceil(fraction * values.size())) - 1,
                             values.size() - 1);
    return static_cast<double>(values.at(index));
}

double average(const QList<double>& values)
{
    if (values.isEmpty()) {
        return -1.0;
    }
    return std::accumulate(values.cbegin(), values.cend(), 0.0) / values.size();
}

#ifdef Q_OS_WIN
quint64 fileTimeValue(const FILETIME& value)
{
    ULARGE_INTEGER converted;
    converted.LowPart = value.dwLowDateTime;
    converted.HighPart = value.dwHighDateTime;
    return converted.QuadPart;
}
#endif

} // namespace

SpikeWindow::SpikeWindow(RtspSpikeOptions options, QWidget* parent)
    : QMainWindow(parent)
    , options_(std::move(options))
    , outputFile_(options_.outputPath)
    , bearerToken_(qgetenv("RV1126B_BEARER_TOKEN"))
    , currentRole_(options_.initialRole)
{
    const QFileInfo outputInfo(outputFile_);
    if (!QDir().mkpath(outputInfo.absolutePath())
        || !outputFile_.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        initializationError_ = QStringLiteral("无法创建结果文件：%1").arg(outputFile_.errorString());
        return;
    }

    QTextStream stream(&outputFile_);
    stream.setEncoding(QStringConverter::Utf8);
    stream << "timestamp,event,elapsed_ms,role,state,value1,value2,cpu_percent,working_set_mb,health_status,detail\n";
    stream.flush();

    player_ = new rv1126b::QtMultimediaRtspPlayer(this);
    new UiPerformanceMonitor(this);
    setupUi();
    connectPlayer();

    metricsTimer_ = new QTimer(this);
    metricsTimer_->setInterval(5000);
    connect(metricsTimer_, &QTimer::timeout, this, &SpikeWindow::collectMetrics);

    healthTimer_ = new QTimer(this);
    healthTimer_->setInterval(qMax(1000, options_.healthIntervalMs));
    connect(healthTimer_, &QTimer::timeout, this, &SpikeWindow::pollHealth);

    durationTimer_ = new QTimer(this);
    durationTimer_->setSingleShot(true);
    connect(durationTimer_, &QTimer::timeout, this, &SpikeWindow::finishTimedRun);

    runElapsed_.start();
    metricElapsed_.start();
    sampleProcess();
    metricsTimer_->start();
    if (options_.healthUrl.isValid()) {
        healthTimer_->start();
        QTimer::singleShot(0, this, &SpikeWindow::pollHealth);
    }
    if (options_.durationMs > 0) {
        durationTimer_->start(options_.durationMs);
    }

    writeEvent(QStringLiteral("run_started"),
               QStringLiteral("backend=ffmpeg"),
               QStringLiteral("duration_ms=%1").arg(options_.durationMs),
               QStringLiteral("main=%1; sub=%2; health=%3")
                   .arg(sanitizedUrl(options_.mainUrl),
                        sanitizedUrl(options_.subUrl),
                        sanitizedUrl(options_.healthUrl)));
    ready_ = true;
    QTimer::singleShot(0, this, [this]() { play(currentRole_); });
}

SpikeWindow::~SpikeWindow()
{
    if (healthReply_) {
        healthReply_->abort();
    }
    if (player_) {
        player_->stop();
    }
    writeSummary();
    bearerToken_.fill('\0');
}

bool SpikeWindow::isReady() const
{
    return ready_;
}

QString SpikeWindow::initializationError() const
{
    return initializationError_;
}

void SpikeWindow::closeEvent(QCloseEvent* event)
{
    stopPlayback();
    writeSummary();
    QMainWindow::closeEvent(event);
}

void SpikeWindow::setupUi()
{
    setWindowTitle(QStringLiteral("RV1126B RTSP 技术验证（Qt Multimedia / FFmpeg）"));
    resize(1000, 760);

    auto* root = new QWidget(this);
    auto* layout = new QVBoxLayout(root);
    layout->addWidget(player_->outputWidget(), 1);

    stateLabel_ = new QLabel(root);
    metricsLabel_ = new QLabel(root);
    healthLabel_ = new QLabel(root);
    layout->addWidget(stateLabel_);
    layout->addWidget(metricsLabel_);
    layout->addWidget(healthLabel_);

    auto* controls = new QHBoxLayout;
    mainButton_ = new QPushButton(QStringLiteral("播放主码流 /live/0"), root);
    subButton_ = new QPushButton(QStringLiteral("播放辅码流 /live/1"), root);
    auto* reopenButton = new QPushButton(QStringLiteral("冷启动/重新打开"), root);
    auto* stopButton = new QPushButton(QStringLiteral("停止"), root);
    controls->addWidget(mainButton_);
    controls->addWidget(subButton_);
    controls->addWidget(reopenButton);
    controls->addWidget(stopButton);
    layout->addLayout(controls);

    auto* evidenceControls = new QHBoxLayout;
    latencySpin_ = new QSpinBox(root);
    latencySpin_->setRange(0, 60000);
    latencySpin_->setSuffix(QStringLiteral(" ms"));
    auto* latencyButton = new QPushButton(QStringLiteral("记录延迟样本"), root);
    auto* downButton = new QPushButton(QStringLiteral("标记断网开始"), root);
    auto* restoredButton = new QPushButton(QStringLiteral("标记网络恢复"), root);
    evidenceControls->addWidget(new QLabel(QStringLiteral("人工测得端到端延迟："), root));
    evidenceControls->addWidget(latencySpin_);
    evidenceControls->addWidget(latencyButton);
    evidenceControls->addStretch();
    evidenceControls->addWidget(downButton);
    evidenceControls->addWidget(restoredButton);
    layout->addLayout(evidenceControls);

    eventLog_ = new QPlainTextEdit(root);
    eventLog_->setReadOnly(true);
    eventLog_->setMaximumBlockCount(500);
    layout->addWidget(eventLog_, 0);
    setCentralWidget(root);

    mainButton_->setEnabled(options_.mainUrl.isValid());
    subButton_->setEnabled(options_.subUrl.isValid());
    connect(mainButton_, &QPushButton::clicked, this, &SpikeWindow::playMain);
    connect(subButton_, &QPushButton::clicked, this, &SpikeWindow::playSub);
    connect(reopenButton, &QPushButton::clicked, this, &SpikeWindow::reopenCurrent);
    connect(stopButton, &QPushButton::clicked, this, &SpikeWindow::stopPlayback);
    connect(latencyButton, &QPushButton::clicked, this, &SpikeWindow::recordLatencySample);
    connect(downButton, &QPushButton::clicked, this, &SpikeWindow::markNetworkDown);
    connect(restoredButton, &QPushButton::clicked, this, &SpikeWindow::markNetworkRestored);
    updateStatusLabels();
}

void SpikeWindow::connectPlayer()
{
    connect(player_, &rv1126b::IRtspPlayer::stateChanged, this,
            [this](rv1126b::RtspPlayerState state) {
                writeEvent(QStringLiteral("state_changed"), stateName(state));
                updateStatusLabels();
            });
    connect(player_, &rv1126b::IRtspPlayer::errorOccurred, this,
            [this](const rv1126b::ApiError& error) {
                const QString safeMessage = redact(error.message);
                writeEvent(QStringLiteral("player_error"), error.code,
                           error.retryable ? QStringLiteral("retryable") : QStringLiteral("terminal"),
                           safeMessage);
                appendUiLog(QStringLiteral("播放器错误 [%1] %2").arg(error.code, safeMessage));
            });
    connect(player_, &rv1126b::QtMultimediaRtspPlayer::playbackAttempted, this,
            [this](int attempt, bool reconnecting) {
                RoleStats& stats = statsForRole(currentRole_);
                ++stats.attempts;
                currentAttemptIsReconnect_ = reconnecting;
                if (!reconnecting) {
                    ++stats.coldStarts;
                }
                writeEvent(QStringLiteral("playback_attempt"), QString::number(attempt),
                           reconnecting ? QStringLiteral("reconnect") : QStringLiteral("open"));
                updateStatusLabels();
            });
    connect(player_, &rv1126b::QtMultimediaRtspPlayer::firstFrameReceived, this,
            [this](qint64 elapsedMs, const QSize& size, qreal declaredFps) {
                RoleStats& stats = statsForRole(currentRole_);
                ++stats.successes;
                if (!currentAttemptIsReconnect_) {
                    ++stats.coldStartSuccesses;
                }
                stats.firstFrameTimes.append(elapsedMs);
                writeEvent(QStringLiteral("first_frame"), QString::number(elapsedMs),
                           QStringLiteral("%1x%2").arg(size.width()).arg(size.height()),
                           QStringLiteral("declared_fps=%1").arg(declaredFps, 0, 'f', 2));
                if (networkRecoveryPending_) {
                    networkRecoveryPending_ = false;
                    writeEvent(QStringLiteral("network_recovered_to_frame"),
                               QString::number(networkRecoveryElapsed_.elapsed()));
                }
                updateStatusLabels();
            });
    connect(player_, &rv1126b::QtMultimediaRtspPlayer::frameReceived, this, [this]() {
        ++statsForRole(currentRole_).frames;
        if (frameIntervalTimer_.isValid()) {
            if (frameIntervals_.size() < 2000) frameIntervals_.append(frameIntervalTimer_.restart());
            else frameIntervalTimer_.restart();
        } else frameIntervalTimer_.start();
    });
    connect(player_, &rv1126b::QtMultimediaRtspPlayer::streamMetadataReceived, this,
            [this](const QString& codec, const QSize& resolution, qreal frameRate) {
                if (codec.isEmpty() && !resolution.isValid() && frameRate <= 0) {
                    return;
                }
                writeEvent(QStringLiteral("stream_metadata"),
                           codec.isEmpty() ? QStringLiteral("unknown") : codec,
                           QStringLiteral("%1x%2").arg(resolution.width()).arg(resolution.height()),
                           QStringLiteral("fps=%1").arg(frameRate, 0, 'f', 2));
            });
    connect(player_, &rv1126b::QtMultimediaRtspPlayer::reconnectScheduled, this,
            [this](int attempt, int delayMs) {
                ++statsForRole(currentRole_).reconnects;
                writeEvent(QStringLiteral("reconnect_scheduled"), QString::number(attempt),
                           QString::number(delayMs));
                updateStatusLabels();
            });
}

void SpikeWindow::playMain()
{
    play(rv1126b::RtspStreamRole::Main);
}

void SpikeWindow::playSub()
{
    play(rv1126b::RtspStreamRole::Sub);
}

void SpikeWindow::reopenCurrent()
{
    play(currentRole_);
}

void SpikeWindow::play(rv1126b::RtspStreamRole role)
{
    const QUrl url = urlForRole(role);
    if (!url.isValid()) {
        appendUiLog(QStringLiteral("该码流 URL 未配置"));
        return;
    }

    currentRole_ = role;
    previousMetricFrames_ = statsForRole(role).frames;
    frameIntervalTimer_.invalidate();
    frameIntervals_.clear();
    metricElapsed_.restart();
    rv1126b::RtspStreamSpec stream;
    stream.deviceId = options_.deviceId;
    stream.url = url;
    stream.role = role;
    stream.openTimeoutMs = options_.openTimeoutMs;
    writeEvent(QStringLiteral("open_requested"), sanitizedUrl(url));
    player_->open(stream);
    updateStatusLabels();
}

void SpikeWindow::stopPlayback()
{
    if (!player_) {
        return;
    }
    QElapsedTimer timer;
    timer.start();
    player_->stop();
    writeEvent(QStringLiteral("stop_completed"), QString::number(timer.elapsed()));
    updateStatusLabels();
}

void SpikeWindow::recordLatencySample()
{
    const int latencyMs = latencySpin_->value();
    statsForRole(currentRole_).latencySamples.append(latencyMs);
    writeEvent(QStringLiteral("latency_sample"), QString::number(latencyMs));
    appendUiLog(QStringLiteral("已记录 %1 延迟样本：%2 ms").arg(roleName(currentRole_)).arg(latencyMs));
}

void SpikeWindow::markNetworkDown()
{
    networkOutageElapsed_.restart();
    networkRecoveryPending_ = false;
    writeEvent(QStringLiteral("network_down_marked"));
    appendUiLog(QStringLiteral("已标记断网开始；请实际断开测试网络"));
}

void SpikeWindow::markNetworkRestored()
{
    if (!networkOutageElapsed_.isValid()) {
        appendUiLog(QStringLiteral("请先标记断网开始"));
        return;
    }
    networkRecoveryPending_ = true;
    writeEvent(QStringLiteral("network_restored_marked"), QString::number(networkOutageElapsed_.elapsed()));
    networkRecoveryElapsed_.restart();
    appendUiLog(QStringLiteral("已标记网络恢复，等待下一有效视频帧"));
}

void SpikeWindow::collectMetrics()
{
    RoleStats& stats = statsForRole(currentRole_);
    const qint64 elapsedMs = qMax<qint64>(1, metricElapsed_.restart());
    const quint64 frameDelta = stats.frames - previousMetricFrames_;
    previousMetricFrames_ = stats.frames;
    const double observedFps = frameDelta * 1000.0 / elapsedMs;
    latestMetricSample_ = sampleProcess();
    if (latestMetricSample_.cpuPercent >= 0) {
        stats.cpuSamples.append(latestMetricSample_.cpuPercent);
    }
    if (latestMetricSample_.workingSetMb >= 0) {
        stats.workingSetSamples.append(latestMetricSample_.workingSetMb);
    }
    writeEvent(QStringLiteral("metrics"),
               QStringLiteral("received_fps=%1").arg(observedFps, 0, 'f', 2),
               QStringLiteral("received_frames=%1").arg(stats.frames),
               QStringLiteral("attempts=%1; successes=%2; reconnects=%3;received_interval_p95_ms=%4;received_interval_max_ms=%5;renderer=%6;native_presentation_count=unavailable")
                   .arg(stats.attempts)
                   .arg(stats.successes)
                   .arg(stats.reconnects)
                   .arg(percentile(frameIntervals_, .95))
                   .arg(frameIntervals_.isEmpty() ? -1 : *std::max_element(frameIntervals_.cbegin(), frameIntervals_.cend()))
                   .arg(player_->outputWidget()->property("previewRenderer").toString()));
    frameIntervals_.clear();
    updateStatusLabels();
}

void SpikeWindow::pollHealth()
{
    if (!options_.healthUrl.isValid() || healthReply_) {
        return;
    }

    QNetworkRequest request(options_.healthUrl);
    request.setTransferTimeout(10000);
    request.setRawHeader("Accept", "application/json");
    if (!bearerToken_.isEmpty()) {
        request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + bearerToken_);
    }

    healthElapsed_.restart();
    healthReply_ = networkManager_.get(request);
    connect(healthReply_, &QNetworkReply::finished, this, &SpikeWindow::finishHealthRequest);
}

void SpikeWindow::finishHealthRequest()
{
    if (!healthReply_) {
        return;
    }

    QNetworkReply* reply = healthReply_;
    healthReply_.clear();
    lastHealthStatus_ = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool success = reply->error() == QNetworkReply::NoError
        && lastHealthStatus_ >= 200 && lastHealthStatus_ < 300;
    if (success) {
        ++healthSuccesses_;
    } else {
        ++healthFailures_;
    }
    writeEvent(QStringLiteral("health_result"), QString::number(lastHealthStatus_),
               QString::number(healthElapsed_.elapsed()),
               success ? QStringLiteral("ok") : redact(reply->errorString()));
    reply->deleteLater();
    updateStatusLabels();
}

void SpikeWindow::finishTimedRun()
{
    writeEvent(QStringLiteral("duration_reached"));
    stopPlayback();
    writeSummary();
    QApplication::quit();
}

QUrl SpikeWindow::urlForRole(rv1126b::RtspStreamRole role) const
{
    return role == rv1126b::RtspStreamRole::Main ? options_.mainUrl : options_.subUrl;
}

SpikeWindow::RoleStats& SpikeWindow::statsForRole(rv1126b::RtspStreamRole role)
{
    return role == rv1126b::RtspStreamRole::Main ? mainStats_ : subStats_;
}

const SpikeWindow::RoleStats& SpikeWindow::statsForRole(rv1126b::RtspStreamRole role) const
{
    return role == rv1126b::RtspStreamRole::Main ? mainStats_ : subStats_;
}

void SpikeWindow::updateStatusLabels()
{
    if (!stateLabel_) {
        return;
    }
    const RoleStats& stats = statsForRole(currentRole_);
    stateLabel_->setText(QStringLiteral("码流：%1    状态：%2    运行：%3 秒")
                             .arg(roleName(currentRole_), stateName(player_->state()))
                             .arg(runElapsed_.isValid() ? runElapsed_.elapsed() / 1000 : 0));
    metricsLabel_->setText(QStringLiteral("冷启动：%1/%2    总尝试/成功：%3/%4    重连：%5    收到帧：%6")
                               .arg(stats.coldStartSuccesses)
                               .arg(stats.coldStarts)
                               .arg(stats.successes)
                               .arg(stats.attempts)
                               .arg(stats.reconnects)
                               .arg(stats.frames));
    healthLabel_->setText(options_.healthUrl.isValid()
                              ? QStringLiteral("Health：HTTP %1    成功：%2    失败：%3")
                                    .arg(lastHealthStatus_)
                                    .arg(healthSuccesses_)
                                    .arg(healthFailures_)
                              : QStringLiteral("Health：未配置（使用 --health-url 启用并行检查）"));
}

void SpikeWindow::appendUiLog(const QString& message)
{
    eventLog_->appendPlainText(QStringLiteral("[%1] %2")
                                   .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), redact(message)));
}

void SpikeWindow::writeEvent(const QString& event,
                             const QString& value1,
                             const QString& value2,
                             const QString& detail)
{
    if (!outputFile_.isOpen()) {
        return;
    }

    const ProcessSample process = event == QStringLiteral("metrics") ? latestMetricSample_ : ProcessSample {};
    QTextStream stream(&outputFile_);
    stream.setEncoding(QStringConverter::Utf8);
    stream << csvCell(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)) << ','
           << csvCell(event) << ','
           << (runElapsed_.isValid() ? runElapsed_.elapsed() : 0) << ','
           << csvCell(roleName(currentRole_)) << ','
           << csvCell(player_ ? stateName(player_->state()) : QStringLiteral("not_ready")) << ','
           << csvCell(redact(value1)) << ','
           << csvCell(redact(value2)) << ','
           << (process.cpuPercent >= 0 ? QString::number(process.cpuPercent, 'f', 2) : QString()) << ','
           << (process.workingSetMb >= 0 ? QString::number(process.workingSetMb, 'f', 2) : QString()) << ','
           << lastHealthStatus_ << ','
           << csvCell(redact(detail)) << '\n';
    stream.flush();
}

void SpikeWindow::writeSummary()
{
    if (summaryWritten_ || !outputFile_.isOpen()) {
        return;
    }
    summaryWritten_ = true;
    const auto summarize = [this](rv1126b::RtspStreamRole role, const RoleStats& stats) {
        const double firstFrameP50 = percentile(stats.firstFrameTimes, 0.50);
        const double firstFrameP95 = percentile(stats.firstFrameTimes, 0.95);
        const double latencyP50 = percentile(stats.latencySamples, 0.50);
        const double latencyP95 = percentile(stats.latencySamples, 0.95);
        const double cpuAverage = average(stats.cpuSamples);
        const double cpuPeak = stats.cpuSamples.isEmpty()
            ? -1.0 : *std::max_element(stats.cpuSamples.cbegin(), stats.cpuSamples.cend());
        const double memoryAverage = average(stats.workingSetSamples);
        const double memoryPeak = stats.workingSetSamples.isEmpty()
            ? -1.0 : *std::max_element(stats.workingSetSamples.cbegin(), stats.workingSetSamples.cend());
        const rv1126b::RtspStreamRole previousRole = currentRole_;
        currentRole_ = role;
        writeEvent(QStringLiteral("role_summary"),
                   QStringLiteral("attempts=%1;successes=%2;reconnects=%3;frames=%4")
                       .arg(stats.attempts).arg(stats.successes).arg(stats.reconnects).arg(stats.frames),
                   QStringLiteral("cold_starts=%1;cold_successes=%2;first_frame_p50_ms=%3;first_frame_p95_ms=%4")
                       .arg(stats.coldStarts).arg(stats.coldStartSuccesses)
                       .arg(firstFrameP50, 0, 'f', 1).arg(firstFrameP95, 0, 'f', 1),
                   QStringLiteral("latency_samples=%1;latency_p50_ms=%2;latency_p95_ms=%3;cpu_avg=%4;cpu_peak=%5;memory_avg_mb=%6;memory_peak_mb=%7")
                       .arg(stats.latencySamples.size())
                       .arg(latencyP50, 0, 'f', 1).arg(latencyP95, 0, 'f', 1)
                       .arg(cpuAverage, 0, 'f', 2).arg(cpuPeak, 0, 'f', 2)
                       .arg(memoryAverage, 0, 'f', 2).arg(memoryPeak, 0, 'f', 2));
        currentRole_ = previousRole;
    };
    summarize(rv1126b::RtspStreamRole::Main, mainStats_);
    summarize(rv1126b::RtspStreamRole::Sub, subStats_);
    writeEvent(QStringLiteral("run_summary"),
               QStringLiteral("health_successes=%1").arg(healthSuccesses_),
               QStringLiteral("health_failures=%1").arg(healthFailures_));
    outputFile_.close();
}

SpikeWindow::ProcessSample SpikeWindow::sampleProcess()
{
    ProcessSample sample;
#ifdef Q_OS_WIN
    FILETIME creationTime;
    FILETIME exitTime;
    FILETIME kernelTime;
    FILETIME userTime;
    FILETIME idleSystemTime;
    FILETIME kernelSystemTime;
    FILETIME userSystemTime;
    if (GetProcessTimes(GetCurrentProcess(), &creationTime, &exitTime, &kernelTime, &userTime)
        && GetSystemTimes(&idleSystemTime, &kernelSystemTime, &userSystemTime)) {
        const quint64 processTime = fileTimeValue(kernelTime) + fileTimeValue(userTime);
        const quint64 systemTime = fileTimeValue(kernelSystemTime) + fileTimeValue(userSystemTime);
        if (previousProcessTime_ > 0 && systemTime > previousSystemTime_) {
            sample.cpuPercent = 100.0 * static_cast<double>(processTime - previousProcessTime_)
                / static_cast<double>(systemTime - previousSystemTime_);
        }
        previousProcessTime_ = processTime;
        previousSystemTime_ = systemTime;
    }

    PROCESS_MEMORY_COUNTERS_EX counters {};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        sample.workingSetMb = static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
    }
#endif
    return sample;
}

QString SpikeWindow::roleName(rv1126b::RtspStreamRole role) const
{
    return role == rv1126b::RtspStreamRole::Main ? QStringLiteral("main") : QStringLiteral("sub");
}

QString SpikeWindow::stateName(rv1126b::RtspPlayerState state) const
{
    switch (state) {
    case rv1126b::RtspPlayerState::Idle: return QStringLiteral("Idle");
    case rv1126b::RtspPlayerState::Opening: return QStringLiteral("Opening");
    case rv1126b::RtspPlayerState::Playing: return QStringLiteral("Playing");
    case rv1126b::RtspPlayerState::Reconnecting: return QStringLiteral("Reconnecting");
    case rv1126b::RtspPlayerState::Stopped: return QStringLiteral("Stopped");
    case rv1126b::RtspPlayerState::Error: return QStringLiteral("Error");
    }
    return QStringLiteral("Unknown");
}

QString SpikeWindow::sanitizedUrl(const QUrl& url) const
{
    if (!url.isValid() || url.isEmpty()) {
        return QStringLiteral("not_configured");
    }
    return url.toString(QUrl::RemoveUserInfo | QUrl::RemoveQuery | QUrl::RemoveFragment);
}

QString SpikeWindow::redact(const QString& value) const
{
    QString safe = value;
    const QList<QByteArray> secrets = {
        bearerToken_,
        qgetenv("RV1126B_RTSP_USER"),
        qgetenv("RV1126B_RTSP_PASSWORD")
    };
    for (const QByteArray& secret : secrets) {
        if (!secret.isEmpty()) {
            safe.replace(QString::fromUtf8(secret), QStringLiteral("***"), Qt::CaseSensitive);
            safe.replace(QString::fromUtf8(QUrl::toPercentEncoding(QString::fromUtf8(secret))),
                         QStringLiteral("***"), Qt::CaseInsensitive);
        }
    }
    return safe;
}
