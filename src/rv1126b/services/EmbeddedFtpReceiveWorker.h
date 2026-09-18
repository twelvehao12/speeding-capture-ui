#pragma once
#include "EmbeddedFtpReceiveServer.h"

#include <QHostAddress>
#include <QObject>

class QTcpServer;

namespace rv1126b {

class EmbeddedFtpReceiveWorker final : public QObject
{
    Q_OBJECT

public:
    explicit EmbeddedFtpReceiveWorker(QObject* parent = nullptr);
    ~EmbeddedFtpReceiveWorker() override;

    bool start(const EmbeddedFtpReceiveServerConfig& config);
    void stop();

    bool isListening() const;
    quint16 controlPort() const;
    QString lastError() const;
    EmbeddedFtpReceiveServerConfig config() const;

signals:
    void fileStored(const QString& relativePath, qint64 bytes);

private:
    class Session;

    friend class Session;

    QString resolvePath(const QString& cwd, const QString& ftpPath, bool* ok, QString* relativePath) const;
    QTcpServer* createPassiveServer(QString* errorMessage) const;
    QHostAddress passiveReplyAddress() const;
    void removeSession(Session* session);

    EmbeddedFtpReceiveServerConfig config_;
    QTcpServer* controlServer_ = nullptr;
    QList<Session*> sessions_;
    QString lastError_;
};

} // namespace rv1126b
