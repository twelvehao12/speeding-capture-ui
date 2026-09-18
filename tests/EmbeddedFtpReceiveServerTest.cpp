#include "../src/rv1126b/services/EmbeddedFtpReceiveServer.h"

#include <QDir>
#include <QFile>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QtTest>

using namespace rv1126b;

class EmbeddedFtpReceiveServerTest final : public QObject
{
    Q_OBJECT

private slots:
    void storesUploadedFilesThroughPassiveFtp();
    void abortedPassiveSessionReleasesListener();
};

namespace {

QString readReply(QTcpSocket& socket)
{
    QElapsedTimer timer;
    timer.start();
    while (!socket.canReadLine() && timer.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        socket.waitForReadyRead(20);
    }
    if (!socket.canReadLine()) {
        QTest::qFail("Timed out waiting for FTP reply.", __FILE__, __LINE__);
        return {};
    }
    return QString::fromUtf8(socket.readLine()).trimmed();
}

void sendCommand(QTcpSocket& socket, const QString& command)
{
    socket.write(command.toUtf8() + "\r\n");
    QVERIFY(socket.waitForBytesWritten(1000));
}

QString commandReply(QTcpSocket& socket, const QString& command)
{
    sendCommand(socket, command);
    return readReply(socket);
}

quint16 enterExtendedPassive(QTcpSocket& socket)
{
    const QString reply = commandReply(socket, QStringLiteral("EPSV"));
    if (!reply.startsWith(QStringLiteral("229 "))) {
        QTest::qFail(qPrintable(reply), __FILE__, __LINE__);
        return 0;
    }
    const QRegularExpression pattern(QStringLiteral("\\(\\|\\|\\|(\\d+)\\|\\)"));
    const QRegularExpressionMatch match = pattern.match(reply);
    if (!match.hasMatch()) {
        QTest::qFail(qPrintable(reply), __FILE__, __LINE__);
        return 0;
    }
    return static_cast<quint16>(match.captured(1).toUShort());
}

} // namespace

void EmbeddedFtpReceiveServerTest::storesUploadedFilesThroughPassiveFtp()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());

    EmbeddedFtpReceiveServer server;
    EmbeddedFtpReceiveServerConfig config;
    config.rootPath = root.path();
    config.userName = QStringLiteral("upload");
    config.password = QStringLiteral("secret");
    config.listenAddress = QHostAddress::LocalHost;

    QVERIFY2(server.start(config), qPrintable(server.lastError()));
    QVERIFY(server.controlPort() > 0);

    QTcpSocket control;
    control.connectToHost(QHostAddress::LocalHost, server.controlPort());
    QVERIFY(control.waitForConnected(1000));
    QVERIFY(readReply(control).startsWith(QStringLiteral("220 ")));
    QVERIFY(commandReply(control, QStringLiteral("USER upload")).startsWith(QStringLiteral("331 ")));
    QVERIFY(commandReply(control, QStringLiteral("PASS secret")).startsWith(QStringLiteral("230 ")));
    QVERIFY(commandReply(control, QStringLiteral("TYPE I")).startsWith(QStringLiteral("200 ")));

    const quint16 passivePort = enterExtendedPassive(control);
    sendCommand(control, QStringLiteral("STOR /vehicle_events/20260722/event.json.uploading"));
    QVERIFY(readReply(control).startsWith(QStringLiteral("150 ")));

    QTcpSocket data;
    data.connectToHost(QHostAddress::LocalHost, passivePort);
    QVERIFY(data.waitForConnected(1000));
    data.write("payload");
    QVERIFY(data.waitForBytesWritten(1000));
    data.disconnectFromHost();
    if (data.state() != QAbstractSocket::UnconnectedState) {
        QVERIFY(data.waitForDisconnected(1000));
    }
    QVERIFY(readReply(control).startsWith(QStringLiteral("226 ")));

    QVERIFY(commandReply(control, QStringLiteral("RNFR /vehicle_events/20260722/event.json.uploading")).startsWith(QStringLiteral("350 ")));
    QVERIFY(commandReply(control, QStringLiteral("RNTO /vehicle_events/20260722/event.json")).startsWith(QStringLiteral("250 ")));
    QCOMPARE(commandReply(control, QStringLiteral("SIZE /vehicle_events/20260722/event.json")), QStringLiteral("213 7"));
    QVERIFY(commandReply(control, QStringLiteral("QUIT")).startsWith(QStringLiteral("221 ")));

    QFile stored(QDir(root.path()).filePath(QStringLiteral("vehicle_events/20260722/event.json")));
    QVERIFY(stored.open(QIODevice::ReadOnly));
    QCOMPARE(stored.readAll(), QByteArrayLiteral("payload"));
}

void EmbeddedFtpReceiveServerTest::abortedPassiveSessionReleasesListener()
{
    QTemporaryDir dir;
    EmbeddedFtpReceiveServer server;
    EmbeddedFtpReceiveServerConfig config;
    config.rootPath = dir.path();
    config.password = QStringLiteral("secret");
    config.listenAddress = QHostAddress::LocalHost;
    QVERIFY(server.start(config));
    QTcpSocket control;
    control.connectToHost(QHostAddress::LocalHost, server.controlPort());
    QVERIFY(control.waitForConnected(1000));
    QVERIFY(readReply(control).startsWith(QStringLiteral("220 ")));
    QVERIFY(commandReply(control, QStringLiteral("USER upload")).startsWith(QStringLiteral("331 ")));
    QVERIFY(commandReply(control, QStringLiteral("PASS secret")).startsWith(QStringLiteral("230 ")));
    const quint16 passivePort = enterExtendedPassive(control);
    QVERIFY(passivePort > 0);
    sendCommand(control, QStringLiteral("STOR interrupted.jpg"));
    QVERIFY(readReply(control).startsWith(QStringLiteral("150 ")));
    control.abort();
    QTcpServer probe;
    QTRY_VERIFY_WITH_TIMEOUT(probe.isListening() || probe.listen(QHostAddress::LocalHost, passivePort), 2000);
}

QTEST_MAIN(EmbeddedFtpReceiveServerTest)

#include "EmbeddedFtpReceiveServerTest.moc"
