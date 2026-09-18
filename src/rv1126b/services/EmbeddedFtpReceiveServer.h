#pragma once

#include <QHostAddress>
#include <QObject>
#include <QThread>

class QTcpServer;

namespace rv1126b {

struct EmbeddedFtpReceiveServerConfig {
    QString rootPath;
    QString userName = QStringLiteral("upload");
    QString password;
    QHostAddress listenAddress = QHostAddress::AnyIPv4;
    QHostAddress advertisedAddress;
    quint16 controlPort = 0;
    quint16 passivePortStart = 0;
    quint16 passivePortEnd = 0;
};

class EmbeddedFtpReceiveWorker;

class EmbeddedFtpReceiveServer final : public QObject
{
    Q_OBJECT

public:
    explicit EmbeddedFtpReceiveServer(QObject* parent = nullptr);
    ~EmbeddedFtpReceiveServer() override;

    bool start(const EmbeddedFtpReceiveServerConfig& config);
    void startAsync(const EmbeddedFtpReceiveServerConfig& config);
    bool isStarting() const { return starting_; }
    void stop();

    bool isListening() const;
    quint16 controlPort() const;
    QString lastError() const;
    EmbeddedFtpReceiveServerConfig config() const;

signals:
    void started(bool success);
    void fileStored(const QString& relativePath, qint64 bytes);

private:
    QThread ioThread_;
    EmbeddedFtpReceiveWorker* worker_ = nullptr;
    EmbeddedFtpReceiveServerConfig config_;
    QString lastError_;
    quint16 port_ = 0;
    quint64 generation_ = 0;
    bool starting_ = false;
};
} // namespace rv1126b