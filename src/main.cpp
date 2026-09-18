#include "ui/MainWindow.h"
#include "rv1126b/infrastructure/video/QtMultimediaRtspPlayer.h"
#include "rv1126b/application/Rv1126bApplicationRuntime.h"
#include "services/SystemSettingsService.h"
#include "services/UiPerformanceMonitor.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QMessageBox>

int main(int argc, char* argv[])
{
    qputenv("QT_MEDIA_BACKEND", "ffmpeg");
    if (qgetenv("QT_FFMPEG_DECODING_HW_DEVICE_TYPES").isEmpty()) {
        qputenv("QT_FFMPEG_DECODING_HW_DEVICE_TYPES", "d3d11va,dxva2");
    }
    QApplication app(argc, argv);
    UiPerformanceMonitor performanceMonitor(&app);

    QCoreApplication::setOrganizationName("CameraTools");
    QCoreApplication::setApplicationName("CameraManagerApp");
    QCoreApplication::setApplicationVersion("0.1.0");

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("RV1126B 车牌识别雷达测速摄像机管理软件"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption mockOption(QStringLiteral("mock"),
                                  QStringLiteral("启用旧模拟设备开发模式"));
    parser.addOption(mockOption);
    parser.process(app);

    const bool mockMode = parser.isSet(mockOption);
    if (mockMode) {
        MainWindowDependencies dependencies;
        dependencies.mockMode = true;
        auto* window = new MainWindow(dependencies);
        window->setAttribute(Qt::WA_DeleteOnClose);
        window->show();
    } else {
        SystemSettingsService settingsService;
        auto* runtime = new rv1126b::Rv1126bApplicationRuntime(settingsService.load(), &app);
        runtime->initialize(&app, [runtime, &app](rv1126b::ApiResult<void> result) {
            if (!result) {
                QMessageBox::critical(nullptr, QStringLiteral("RV1126B 初始化失败"),
                                      QStringLiteral("无法初始化设备数据库或生产服务：%1")
                                          .arg(result.error().message));
                QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
                return;
            }
            auto* window = new MainWindow(runtime->mainWindowDependencies());
            window->setAttribute(Qt::WA_DeleteOnClose);
            window->show();
        });
    }

    return app.exec();
}
