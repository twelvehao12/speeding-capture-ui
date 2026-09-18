#include "EmbeddedFtpReceiveWorker.h"
#include "CacheFileLease.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>

#include <utility>

namespace rv1126b {
namespace {

QString cleanFtpPath(const QString& cwd, QString path)
{
    path = path.trimmed();
    if (path.startsWith(QLatin1Char('"')) && path.endsWith(QLatin1Char('"')) && path.size() >= 2) {
        path = path.mid(1, path.size() - 2);
    }
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (path.isEmpty()) path = QStringLiteral(".");
    if (!path.startsWith(QLatin1Char('/'))) {
        path = cwd.endsWith(QLatin1Char('/')) ? cwd + path : cwd + QLatin1Char('/') + path;
    }
    return QDir::cleanPath(path);
}

bool safeRelativePath(const QString& path, QString* relativePath)
{
    QString clean = path;
    while (clean.startsWith(QLatin1Char('/'))) clean.remove(0, 1);
    if (clean.isEmpty() || clean == QStringLiteral(".")) {
        *relativePath = QString();
        return true;
    }
    const QStringList parts = clean.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        if (part == QStringLiteral("..") || part.contains(QLatin1Char(':'))) {
            return false;
        }
    }
    *relativePath = parts.join(QLatin1Char('/'));
    return true;
}

QByteArray line(const QString& text)
{
    return text.toUtf8() + "\r\n";
}

} // namespace

class EmbeddedFtpReceiveWorker::Session final : public QObject
{
    Q_OBJECT

public:
    Session(EmbeddedFtpReceiveWorker* owner, QTcpSocket* socket)
        : QObject(owner)
        , owner_(owner)
        , control_(socket)
    {
        control_->setParent(this);
        connect(control_, &QTcpSocket::readyRead, this, &Session::readCommands);
        connect(control_, &QTcpSocket::disconnected, this, &Session::deleteLater);
        connect(this, &QObject::destroyed, owner_, [owner, this]() { owner->removeSession(this); });
        reply(220, QStringLiteral("RV1126B embedded FTP receiver ready"));
    }

private:
    void readCommands()
    {
        while (control_->canReadLine()) {
            const QString raw = QString::fromUtf8(control_->readLine()).trimmed();
            if (raw.isEmpty()) continue;
            const int split = raw.indexOf(QLatin1Char(' '));
            const QString command = (split < 0 ? raw : raw.left(split)).toUpper();
            const QString argument = split < 0 ? QString() : raw.mid(split + 1);
            handle(command, argument);
        }
    }

    void handle(const QString& command, const QString& argument)
    {
        if (command == QStringLiteral("USER")) return handleUser(argument);
        if (command == QStringLiteral("PASS")) return handlePass(argument);
        if (command == QStringLiteral("QUIT")) {
            reply(221, QStringLiteral("Goodbye"));
            control_->disconnectFromHost();
            return;
        }
        if (!authenticated_) {
            reply(530, QStringLiteral("Please login with USER and PASS"));
            return;
        }

        if (command == QStringLiteral("SYST")) return reply(215, QStringLiteral("UNIX Type: L8"));
        if (command == QStringLiteral("FEAT")) return writeRaw("211-Features\r\n EPSV\r\n PASV\r\n SIZE\r\n UTF8\r\n211 End\r\n");
        if (command == QStringLiteral("OPTS")) return reply(200, QStringLiteral("Options accepted"));
        if (command == QStringLiteral("TYPE")) return reply(200, QStringLiteral("Type set"));
        if (command == QStringLiteral("NOOP")) return reply(200, QStringLiteral("OK"));
        if (command == QStringLiteral("PWD")) return reply(257, QStringLiteral("\"%1\" is current directory").arg(cwd_));
        if (command == QStringLiteral("CWD")) return changeDirectory(argument);
        if (command == QStringLiteral("CDUP")) return changeDirectory(QStringLiteral(".."));
        if (command == QStringLiteral("MKD")) return makeDirectory(argument);
        if (command == QStringLiteral("PASV")) return enterPassive(false);
        if (command == QStringLiteral("EPSV")) return enterPassive(true);
        if (command == QStringLiteral("SIZE")) return size(argument);
        if (command == QStringLiteral("DELE")) return removeFile(argument);
        if (command == QStringLiteral("RNFR")) return renameFrom(argument);
        if (command == QStringLiteral("RNTO")) return renameTo(argument);
        if (command == QStringLiteral("STOR")) return store(argument);
        if (command == QStringLiteral("LIST") || command == QStringLiteral("NLST")) return list(argument);

        reply(502, QStringLiteral("Command not implemented"));
    }

    void handleUser(const QString& user)
    {
        userAccepted_ = user == owner_->config_.userName;
        reply(331, QStringLiteral("Password required"));
    }

    void handlePass(const QString& password)
    {
        authenticated_ = userAccepted_ && password == owner_->config_.password;
        reply(authenticated_ ? 230 : 530, authenticated_ ? QStringLiteral("Login successful")
                                                         : QStringLiteral("Login incorrect"));
    }

    void changeDirectory(const QString& path)
    {
        bool ok = false;
        QString relative;
        const QString target = owner_->resolvePath(cwd_, path, &ok, &relative);
        if (!ok || !QFileInfo(target).isDir()) return reply(550, QStringLiteral("Directory unavailable"));
        cwd_ = cleanFtpPath(cwd_, path);
        if (!cwd_.startsWith(QLatin1Char('/'))) cwd_.prepend(QLatin1Char('/'));
        reply(250, QStringLiteral("Directory changed"));
    }

    void makeDirectory(const QString& path)
    {
        bool ok = false;
        QString relative;
        const QString target = owner_->resolvePath(cwd_, path, &ok, &relative);
        if (!ok || !QDir().mkpath(target)) return reply(550, QStringLiteral("Cannot create directory"));
        reply(257, QStringLiteral("\"%1\" created").arg(path));
    }

    void enterPassive(bool extended)
    {
        closePassiveServer();
        QString error;
        passiveServer_ = owner_->createPassiveServer(&error);
        if (!passiveServer_) return reply(425, error.isEmpty() ? QStringLiteral("Cannot open passive socket") : error);
        passiveServer_->setParent(this);

        const quint16 port = passiveServer_->serverPort();
        if (extended) {
            reply(229, QStringLiteral("Entering Extended Passive Mode (|||%1|)").arg(port));
            return;
        }

        QHostAddress address = owner_->passiveReplyAddress();
        if (address.isNull() || address.protocol() != QAbstractSocket::IPv4Protocol) {
            address = QHostAddress::LocalHost;
        }
        const QStringList octets = address.toString().split(QLatin1Char('.'));
        if (octets.size() != 4) return reply(425, QStringLiteral("No IPv4 passive address"));
        reply(227, QStringLiteral("Entering Passive Mode (%1,%2,%3)")
                       .arg(octets.join(QLatin1Char(',')))
                       .arg(port / 256)
                       .arg(port % 256));
    }

    void size(const QString& path)
    {
        bool ok = false;
        QString relative;
        const QString target = owner_->resolvePath(cwd_, path, &ok, &relative);
        const QFileInfo info(target);
        if (!ok || !info.isFile()) return reply(550, QStringLiteral("File unavailable"));
        reply(213, QString::number(info.size()));
    }

    void removeFile(const QString& path)
    {
        bool ok = false;
        QString relative;
        const QString target = owner_->resolvePath(cwd_, path, &ok, &relative);
        if (!ok) return reply(550, QStringLiteral("Invalid path"));
        if (QFileInfo::exists(target)) QFile::remove(target);
        reply(250, QStringLiteral("File deleted"));
    }

    void renameFrom(const QString& path)
    {
        bool ok = false;
        QString relative;
        const QString target = owner_->resolvePath(cwd_, path, &ok, &relative);
        if (!ok || !QFileInfo::exists(target)) return reply(550, QStringLiteral("Rename source unavailable"));
        renameSource_ = target;
        reply(350, QStringLiteral("Ready for RNTO"));
    }

    void renameTo(const QString& path)
    {
        if (renameSource_.isEmpty()) return reply(503, QStringLiteral("RNFR required first"));
        bool ok = false;
        QString relative;
        const QString target = owner_->resolvePath(cwd_, path, &ok, &relative);
        if (!ok || !QDir().mkpath(QFileInfo(target).absolutePath())) return reply(550, QStringLiteral("Invalid rename target"));
        QFile::remove(target);
        const bool renamed = QFile::rename(renameSource_, target);
        renameSource_.clear();
        reply(renamed ? 250 : 550, renamed ? QStringLiteral("Rename successful")
                                           : QStringLiteral("Rename failed"));
    }

    void store(const QString& path)
    {
        if (!passiveServer_) return reply(425, QStringLiteral("Use PASV or EPSV first"));
        bool ok = false;
        QString relative;
        const QString target = owner_->resolvePath(cwd_, path, &ok, &relative);
        if (!ok || !QDir().mkpath(QFileInfo(target).absolutePath())) return reply(550, QStringLiteral("Invalid upload path"));

        const auto lease = CacheFileLease::acquire(target);
        if (!lease) return reply(450, QStringLiteral("Upload path is busy"));
        auto file = QSharedPointer<QFile>::create(target);
        auto failed = QSharedPointer<bool>::create(false);
        if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) return reply(550, QStringLiteral("Cannot open upload path"));

        QTcpServer* server = std::exchange(passiveServer_, nullptr);
        connect(server, &QTcpServer::newConnection, this, [this, server, file, relative, failed, lease]() {
            QTcpSocket* data = server->nextPendingConnection();
            server->close();
            server->deleteLater();
            if (!data) return;
            data->setParent(this);
            data->setReadBufferSize(256 * 1024);
            auto drain = [data, file, failed] {
                while (data->bytesAvailable()) {
                    const QByteArray block = data->read(64 * 1024);
                    if (block.isEmpty()) break;
                    if (file->write(block) != block.size()) *failed = true;
                }
            };
            connect(data, &QTcpSocket::readyRead, this, drain);
            connect(data, &QTcpSocket::disconnected, this, [this, data, file, relative, failed, drain, lease]() {
                drain();
                if (!file->flush()) *failed = true;
                const qint64 bytes = file->size();
                file->close();
                data->deleteLater();
                reply(*failed ? 451 : 226, *failed ? QStringLiteral("File write failed") : QStringLiteral("Transfer complete"));
                if (!*failed) emit owner_->fileStored(relative, bytes);
            });
        });
        reply(150, QStringLiteral("Opening data connection"));
    }

    void list(const QString& path)
    {
        if (!passiveServer_) return reply(425, QStringLiteral("Use PASV or EPSV first"));
        bool ok = false;
        QString relative;
        const QString target = owner_->resolvePath(cwd_, path.isEmpty() ? QStringLiteral(".") : path, &ok, &relative);
        if (!ok) return reply(550, QStringLiteral("Invalid list path"));
        const QByteArray payload = listPayload(target);
        QTcpServer* server = std::exchange(passiveServer_, nullptr);
        connect(server, &QTcpServer::newConnection, this, [this, server, payload]() {
            QTcpSocket* data = server->nextPendingConnection();
            server->close();
            server->deleteLater();
            if (!data) return;
            data->write(payload);
            data->disconnectFromHost();
            connect(data, &QTcpSocket::disconnected, this, [this, data]() {
                data->deleteLater();
                reply(226, QStringLiteral("Transfer complete"));
            });
        });
        reply(150, QStringLiteral("Opening data connection"));
    }

    QByteArray listPayload(const QString& path) const
    {
        QFileInfo info(path);
        if (info.isFile()) return line(info.fileName());
        if (!info.isDir()) return {};
        QByteArray payload;
        const QDir dir(path);
        for (const QFileInfo& entry : dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
            payload += line(entry.fileName());
        }
        return payload;
    }

    void closePassiveServer()
    {
        if (!passiveServer_) return;
        passiveServer_->close();
        passiveServer_->deleteLater();
        passiveServer_ = nullptr;
    }

    void reply(int code, const QString& message)
    {
        writeRaw(QStringLiteral("%1 %2\r\n").arg(code, 3, 10, QLatin1Char('0')).arg(message).toUtf8());
    }

    void writeRaw(const QByteArray& bytes)
    {
        control_->write(bytes);
        control_->flush();
    }

    EmbeddedFtpReceiveWorker* owner_ = nullptr;
    QTcpSocket* control_ = nullptr;
    QTcpServer* passiveServer_ = nullptr;
    QString cwd_ = QStringLiteral("/");
    QString renameSource_;
    bool userAccepted_ = false;
    bool authenticated_ = false;
};

EmbeddedFtpReceiveWorker::EmbeddedFtpReceiveWorker(QObject* parent)
    : QObject(parent)
{
}

EmbeddedFtpReceiveWorker::~EmbeddedFtpReceiveWorker()
{
    stop();
}

bool EmbeddedFtpReceiveWorker::start(const EmbeddedFtpReceiveServerConfig& config)
{
    stop();
    lastError_.clear();
    if (config.rootPath.trimmed().isEmpty()) {
        lastError_ = QStringLiteral("FTP 接收目录不能为空");
        return false;
    }
    if (config.userName.trimmed().isEmpty() || config.password.isEmpty()) {
        lastError_ = QStringLiteral("FTP 用户名和密码不能为空");
        return false;
    }
    if (!QDir().mkpath(config.rootPath)) {
        lastError_ = QStringLiteral("无法创建 FTP 接收目录");
        return false;
    }

    config_ = config;
    controlServer_ = new QTcpServer(this);
    connect(controlServer_, &QTcpServer::newConnection, this, [this]() {
        while (controlServer_->hasPendingConnections()) {
            auto* session = new Session(this, controlServer_->nextPendingConnection());
            sessions_.append(session);
        }
    });
    if (!controlServer_->listen(config.listenAddress, config.controlPort)) {
        lastError_ = controlServer_->errorString();
        stop();
        return false;
    }
    return true;
}

void EmbeddedFtpReceiveWorker::stop()
{
    const QList<Session*> sessions = sessions_;
    for (Session* session : sessions) {
        delete session;
    }
    sessions_.clear();
    if (controlServer_) {
        controlServer_->close();
        delete controlServer_;
        controlServer_ = nullptr;
    }
}

bool EmbeddedFtpReceiveWorker::isListening() const
{
    return controlServer_ && controlServer_->isListening();
}

quint16 EmbeddedFtpReceiveWorker::controlPort() const
{
    return controlServer_ ? controlServer_->serverPort() : 0;
}

QString EmbeddedFtpReceiveWorker::lastError() const
{
    return lastError_;
}

EmbeddedFtpReceiveServerConfig EmbeddedFtpReceiveWorker::config() const
{
    return config_;
}

QString EmbeddedFtpReceiveWorker::resolvePath(
    const QString& cwd,
    const QString& ftpPath,
    bool* ok,
    QString* relativePath) const
{
    *ok = false;
    QString relative;
    if (!safeRelativePath(cleanFtpPath(cwd, ftpPath), &relative)) {
        return {};
    }
    const QString root = QFileInfo(config_.rootPath).absoluteFilePath();
    const QString target = relative.isEmpty() ? root : QDir(root).filePath(relative);
    *ok = true;
    if (relativePath) *relativePath = relative;
    return target;
}

QTcpServer* EmbeddedFtpReceiveWorker::createPassiveServer(QString* errorMessage) const
{
    auto* server = new QTcpServer(const_cast<EmbeddedFtpReceiveWorker*>(this));
    const QHostAddress address = config_.listenAddress;
    if (config_.passivePortStart == 0 && config_.passivePortEnd == 0) {
        if (server->listen(address, 0)) return server;
    } else {
        for (quint32 port = config_.passivePortStart; port <= config_.passivePortEnd; ++port) {
            if (server->listen(address, static_cast<quint16>(port))) return server;
        }
    }
    if (errorMessage) *errorMessage = server->errorString();
    server->deleteLater();
    return nullptr;
}

QHostAddress EmbeddedFtpReceiveWorker::passiveReplyAddress() const
{
    if (!config_.advertisedAddress.isNull()) return config_.advertisedAddress;
    if (controlServer_) return controlServer_->serverAddress();
    return config_.listenAddress;
}

void EmbeddedFtpReceiveWorker::removeSession(Session* session)
{
    sessions_.removeAll(session);
}

} // namespace rv1126b

#include "EmbeddedFtpReceiveServer.moc"

namespace rv1126b {
EmbeddedFtpReceiveServer::EmbeddedFtpReceiveServer(QObject* parent) : QObject(parent)
{
    worker_ = new EmbeddedFtpReceiveWorker;
    worker_->moveToThread(&ioThread_);
    ioThread_.setObjectName(QStringLiteral("ftp-receive-io"));
    connect(&ioThread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &EmbeddedFtpReceiveWorker::fileStored, this, &EmbeddedFtpReceiveServer::fileStored);
    ioThread_.start();
}
EmbeddedFtpReceiveServer::~EmbeddedFtpReceiveServer()
{
    stop();
    ioThread_.quit();
    ioThread_.wait();
}
// Compatibility entry point for non-interactive callers. The UI uses startAsync.
bool EmbeddedFtpReceiveServer::start(const EmbeddedFtpReceiveServerConfig& config)
{
    ++generation_;
    starting_ = false;
    config_ = config;
    bool ok = false;
    QMetaObject::invokeMethod(worker_, [this, config, &ok] {
        ok = worker_->start(config);
        port_ = worker_->controlPort();
        lastError_ = worker_->lastError();
    }, Qt::BlockingQueuedConnection);
    return ok;
}
void EmbeddedFtpReceiveServer::startAsync(const EmbeddedFtpReceiveServerConfig& config)
{
    const auto generation = ++generation_;
    config_ = config;
    port_ = 0;
    starting_ = true;
    QMetaObject::invokeMethod(worker_, [this, config, generation] {
        const bool ok = worker_->start(config);
        const auto port = worker_->controlPort();
        const auto error = worker_->lastError();
        QMetaObject::invokeMethod(this, [this, generation, ok, port, error] {
            if (generation != generation_) return;
            port_ = port;
            lastError_ = error;
            starting_ = false;
            emit started(ok);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}
void EmbeddedFtpReceiveServer::stop()
{
    ++generation_;
    starting_ = false;
    port_ = 0;
    QMetaObject::invokeMethod(worker_, [worker = worker_] { worker->stop(); }, Qt::QueuedConnection);
}
bool EmbeddedFtpReceiveServer::isListening() const { return port_ != 0; }
quint16 EmbeddedFtpReceiveServer::controlPort() const { return port_; }
QString EmbeddedFtpReceiveServer::lastError() const { return lastError_; }
EmbeddedFtpReceiveServerConfig EmbeddedFtpReceiveServer::config() const { return config_; }
} // namespace rv1126b
