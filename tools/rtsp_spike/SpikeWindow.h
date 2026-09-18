#pragma once

#include "../../src/rv1126b/ports/IRtspPlayer.h"

#include <QElapsedTimer>
#include <QFile>
#include <QList>
#include <QMainWindow>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QUrl>

class QLabel;
class QNetworkReply;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTimer;

namespace rv1126b {
class QtMultimediaRtspPlayer;
}

struct RtspSpikeOptions {
    QString deviceId;
    QUrl mainUrl;
    QUrl subUrl;
    QUrl healthUrl;
    QString outputPath;
    rv1126b::RtspStreamRole initialRole = rv1126b::RtspStreamRole::Main;
    int openTimeoutMs = 15000;
    int durationMs = 30 * 60 * 1000;
    int healthIntervalMs = 5000;
};

class SpikeWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit SpikeWindow(RtspSpikeOptions options, QWidget* parent = nullptr);
    ~SpikeWindow() override;

    bool isReady() const;
    QString initializationError() const;

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void playMain();
    void playSub();
    void reopenCurrent();
    void stopPlayback();
    void recordLatencySample();
    void markNetworkDown();
    void markNetworkRestored();
    void collectMetrics();
    void pollHealth();
    void finishHealthRequest();
    void finishTimedRun();

private:
    struct RoleStats {
        int attempts = 0;
        int successes = 0;
        int coldStarts = 0;
        int coldStartSuccesses = 0;
        int reconnects = 0;
        quint64 frames = 0;
        QList<qint64> firstFrameTimes;
        QList<int> latencySamples;
        QList<double> cpuSamples;
        QList<double> workingSetSamples;
    };

    struct ProcessSample {
        double cpuPercent = -1.0;
        double workingSetMb = -1.0;
    };

    void setupUi();
    void connectPlayer();
    void play(rv1126b::RtspStreamRole role);
    QUrl urlForRole(rv1126b::RtspStreamRole role) const;
    RoleStats& statsForRole(rv1126b::RtspStreamRole role);
    const RoleStats& statsForRole(rv1126b::RtspStreamRole role) const;
    void updateStatusLabels();
    void appendUiLog(const QString& message);
    void writeEvent(const QString& event,
                    const QString& value1 = QString(),
                    const QString& value2 = QString(),
                    const QString& detail = QString());
    void writeSummary();
    ProcessSample sampleProcess();
    QString roleName(rv1126b::RtspStreamRole role) const;
    QString stateName(rv1126b::RtspPlayerState state) const;
    QString sanitizedUrl(const QUrl& url) const;
    QString redact(const QString& value) const;

    RtspSpikeOptions options_;
    rv1126b::QtMultimediaRtspPlayer* player_ = nullptr;
    QLabel* stateLabel_ = nullptr;
    QLabel* metricsLabel_ = nullptr;
    QLabel* healthLabel_ = nullptr;
    QPlainTextEdit* eventLog_ = nullptr;
    QPushButton* mainButton_ = nullptr;
    QPushButton* subButton_ = nullptr;
    QSpinBox* latencySpin_ = nullptr;
    QNetworkAccessManager networkManager_;
    QPointer<QNetworkReply> healthReply_;
    QTimer* metricsTimer_ = nullptr;
    QTimer* healthTimer_ = nullptr;
    QTimer* durationTimer_ = nullptr;
    QFile outputFile_;
    QByteArray bearerToken_;
    RoleStats mainStats_;
    RoleStats subStats_;
    rv1126b::RtspStreamRole currentRole_ = rv1126b::RtspStreamRole::Main;
    QElapsedTimer runElapsed_;
    QElapsedTimer healthElapsed_;
    QElapsedTimer metricElapsed_;
    QElapsedTimer frameIntervalTimer_;
    QList<qint64> frameIntervals_;
    QElapsedTimer networkOutageElapsed_;
    QElapsedTimer networkRecoveryElapsed_;
    bool networkRecoveryPending_ = false;
    bool currentAttemptIsReconnect_ = false;
    bool summaryWritten_ = false;
    bool ready_ = false;
    QString initializationError_;
    quint64 previousMetricFrames_ = 0;
    int lastHealthStatus_ = 0;
    int healthSuccesses_ = 0;
    int healthFailures_ = 0;
    quint64 previousProcessTime_ = 0;
    quint64 previousSystemTime_ = 0;
    ProcessSample latestMetricSample_;
};
