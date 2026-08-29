#include <QApplication>
#include <QDir>
#include <QIcon>
#include "MainWindow.h"
#include "CheckStyle.h"
#include "Theme.h"
#ifdef _WIN32
#  include <windows.h>
#  include <mmsystem.h>
#endif

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/log.h>
}

// ---- watchdog: перезапуск при аварийном завершении (галка в настройках) ----
bool g_secWatchdog = false;
#ifdef _WIN32
static ULONGLONG g_secStartTick = 0;
static LONG WINAPI secCrashFilter(EXCEPTION_POINTERS*) {
    // защита от цикла перезапусков: если упали в первую минуту — не перезапускаем
    if (g_secWatchdog && GetTickCount64() - g_secStartTick > 60000) {
        wchar_t path[MAX_PATH];
        if (GetModuleFileNameW(nullptr, path, MAX_PATH)) {
            STARTUPINFOW si{}; si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(path, nullptr, nullptr, nullptr, FALSE, 0,
                               nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
            }
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;   // завершиться тихо, без окна WerFault
}
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    // один экземпляр: второй запуск активирует окно первого и выходит
    CreateMutexW(nullptr, TRUE, L"Local\\SecVMS_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND w = FindWindowW(nullptr, L"SecVMS");
        if (w) {
            if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
            SetForegroundWindow(w);
        }
        MessageBoxW(nullptr, L"SecVMS уже запущено — второй экземпляр не нужен.",
                    L"SecVMS", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    g_secStartTick = GetTickCount64();
    SetUnhandledExceptionFilter(secCrashFilter);
    timeBeginPeriod(1);   // точность sleep 1 мс — иначе пейсинг кадров дрожит на ±15 мс
#endif
    QApplication app(argc, argv);
    app.setApplicationName("SecVMS");
    app.setOrganizationName("SecVMS");
    app.setWindowIcon(QIcon(
        QDir(QCoreApplication::applicationDirPath()).filePath("assets/app.ico")));
    app.setStyle(new CheckStyle("Fusion"));   // кастомный индикатор чекбоксов (галочка)
    app.setPalette(Theme::palette());         // светлая по умолчанию; MainWindow применит тему из конфига

    av_log_set_level(AV_LOG_ERROR);
    avformat_network_init();

    MainWindow w;
    w.showMaximized();   // окно при запуске всегда во весь экран

    int rc = app.exec();

    avformat_network_deinit();
    return rc;
}
