#pragma once

#include "IMediaPlaybackBackend.h"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QPointer>
#include <QVector>

class QMediaPlayer;
class QVideoFrame;
class QVideoSink;
class QWidget;

namespace rv1126b {

class QtMediaPlaybackBackend final : public IMediaPlaybackBackend
{
    Q_OBJECT

public:
    explicit QtMediaPlaybackBackend(QObject* parent = nullptr);
    ~QtMediaPlaybackBackend() override;

    void play(const QUrl& url, quint64 attemptToken) override;
    void stop() override;
    QWidget* outputWidget() const override;

private:
    void disconnectAttemptSignals();
    void handleVideoFrame(const QVideoFrame& frame, quint64 attemptToken);

    QMediaPlayer* mediaPlayer_ = nullptr;
    QPointer<QVideoSink> videoSink_;
    QPointer<QWidget> videoWidget_;
    QElapsedTimer renderThrottle_;
    QVector<QMetaObject::Connection> attemptConnections_;
    quint64 activeAttemptToken_ = 0;
    bool softwarePreview_ = false;
};

} // namespace rv1126b
