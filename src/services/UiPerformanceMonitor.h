#pragma once

#include <QCoreApplication>
#include <QApplication>
#include <QWidget>
#include <QSet>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QVariant>
#include <algorithm>

// Opt-in diagnostics. One bounded sample is written at a time on a separate
// thread so that a slow diagnostic disk cannot build an unbounded queue.
class UiPerformanceMonitor final : public QObject
{
public:
    explicit UiPerformanceMonitor(QObject* parent) : QObject(parent)
    {
        const QString path = qEnvironmentVariable("CAMERA_PERF_LOG");
        if (path.isEmpty()) return;
        writer_ = new QObject;
        writer_->moveToThread(&writerThread_);
        connect(&writerThread_, &QThread::finished, writer_, &QObject::deleteLater);
        writerThread_.setObjectName(QStringLiteral("performance-log"));
        writerThread_.start();
        QMetaObject::invokeMethod(writer_, [this, path] {
            QDir().mkpath(QFileInfo(path).absolutePath());
            file_ = new QFile(path, writer_);
            if (file_->open(QIODevice::WriteOnly | QIODevice::Truncate))
                file_->write("utc,heartbeat_p95_ms,heartbeat_max_ms,qobjects,pending_requests,queued_downloads,active_downloads,evidence_states,received_frames,software_paints,play_max_ms,stop_max_ms\n");
        }, Qt::QueuedConnection);
        elapsed_.start();
        timer_.setInterval(50);
        timer_.setTimerType(Qt::PreciseTimer);
        connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
        timer_.start();
    }
    ~UiPerformanceMonitor() override
    {
        timer_.stop();
        writerThread_.quit();
        writerThread_.wait();
    }

private:
    void tick()
    {
        const qint64 now = elapsed_.elapsed();
        delays_.append(qMax<qint64>(0, now - previousTick_ - 50));
        previousTick_ = now;
        if (now - previousSample_ < 5000) return;
        previousSample_ = now;
        std::sort(delays_.begin(), delays_.end());
        const qint64 p95 = delays_.at(qMax(0, int(delays_.size() * .95) - 1));
        const qint64 maximum = delays_.last();
        delays_.clear();
        if (writing_) return;
        writing_ = true;
        const auto appObjects = QCoreApplication::instance()->findChildren<QObject*>();
        QSet<QObject*> objects(appObjects.begin(), appObjects.end());
        for (auto* window : QApplication::topLevelWidgets()) {
            objects.insert(window);
            const auto children = window->findChildren<QObject*>();
            for (auto* child : children) objects.insert(child);
        }
        const char* counters[] = {"pendingRequestCount", "queuedDownloadCount", "activeDownloadCount",
            "evidenceStateCount", "receivedFrameCount", "softwarePaintCount", "playMaxMs", "stopMaxMs"};
        QList<qint64> totals(8, 0);
        for (const auto* object : objects) {
            for (int i = 0; i < 8; ++i) {
                const qint64 value = object->property(counters[i]).toLongLong();
                if (i >= 6) totals[i] = qMax(totals[i], value);
                else totals[i] += value;
            }
        }
        QByteArray line = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toUtf8()
            + ',' + QByteArray::number(p95) + ',' + QByteArray::number(maximum)
            + ',' + QByteArray::number(objects.size());
        for (qint64 value : totals) line += ',' + QByteArray::number(value);
        line += '\n';
        QMetaObject::invokeMethod(writer_, [this, line] {
            // Rotate at 16 MiB; retain the immediately preceding file.
            if (file_ && file_->isOpen()) {
                if (file_->size() > 16 * 1024 * 1024) {
                    const QString path = file_->fileName();
                    file_->close();
                    QFile::remove(path + QStringLiteral(".previous"));
                    QFile::rename(path, path + QStringLiteral(".previous"));
                    file_->open(QIODevice::WriteOnly | QIODevice::Truncate);
                }
                file_->write(line);
                file_->flush();
            }
            QMetaObject::invokeMethod(this, [this] { writing_ = false; }, Qt::QueuedConnection);
        }, Qt::QueuedConnection);
    }
    QThread writerThread_;
    QObject* writer_ = nullptr;
    QFile* file_ = nullptr; // Accessed only by writerThread_.
    QTimer timer_;
    QElapsedTimer elapsed_;
    QList<qint64> delays_;
    qint64 previousTick_ = 0;
    qint64 previousSample_ = 0;
    bool writing_ = false;
};
