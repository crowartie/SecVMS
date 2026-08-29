#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QPalette>
#include <QColor>
#include "MainWindow.h"
#include "CheckStyle.h"
#ifdef _WIN32
#  include <windows.h>
#  include <mmsystem.h>
#endif

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/log.h>
}

int main(int argc, char** argv) {
#ifdef _WIN32
    timeBeginPeriod(1);   // точность sleep 1 мс — иначе пейсинг кадров дрожит на ±15 мс
#endif
    QApplication app(argc, argv);
    app.setApplicationName("SecVMS");
    app.setOrganizationName("SecVMS");
    app.setWindowIcon(QIcon(
        QDir(QCoreApplication::applicationDirPath()).filePath("assets/app.ico")));
    app.setStyle(new CheckStyle("Fusion"));   // кастомный индикатор чекбоксов (галочка)

    // Принудительно светлая палитра (иначе Fusion берёт тёмную тему Windows)
    QPalette pal;
    pal.setColor(QPalette::Window,      QColor("#eef1f5"));
    pal.setColor(QPalette::Base,        QColor("#ffffff"));
    pal.setColor(QPalette::AlternateBase,QColor("#f6f8fa"));
    pal.setColor(QPalette::WindowText,  QColor("#2b2f36"));
    pal.setColor(QPalette::Text,        QColor("#2b2f36"));
    pal.setColor(QPalette::Button,      QColor("#ffffff"));
    pal.setColor(QPalette::ButtonText,  QColor("#2b2f36"));
    pal.setColor(QPalette::Highlight,   QColor("#1f6fd6"));
    pal.setColor(QPalette::HighlightedText, QColor("#ffffff"));
    pal.setColor(QPalette::ToolTipBase, QColor("#ffffff"));
    pal.setColor(QPalette::ToolTipText, QColor("#2b2f36"));
    app.setPalette(pal);

    av_log_set_level(AV_LOG_ERROR);
    avformat_network_init();

    MainWindow w;
    w.show();
    int rc = app.exec();

    avformat_network_deinit();
    return rc;
}
