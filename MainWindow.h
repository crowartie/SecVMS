#pragma once
#include <QMainWindow>
#include <QPoint>
#include <QMap>
#include <QVector>
#include "Types.h"
#include "Journal.h"

class QTimer;
class LiveView;
class PlaybackView;
class QStackedWidget;
class QPushButton;
class QHBoxLayout;
class QGridLayout;
class QFrame;
class QTableWidget;
class QLineEdit;
class QLabel;
class QDialog;
class QTableWidget;
class QComboBox;

// Оболочка в стиле Smart PSS Lite: безрамочное окно, ДИНАМИЧЕСКИЕ вкладки
// (Домой всегда; «Просмотр» = выбор регистратора; каждый открытый регистратор —
// своя вкладка «Просмотр <имя>» со своим LiveView).
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;  // даблклик по шапке: развернуть/свернуть
    void closeEvent(QCloseEvent*) override;         // остановить потоки перед выходом
    void keyPressEvent(QKeyEvent*) override;        // ESC — выход из полноэкранного
    bool eventFilter(QObject*, QEvent*) override;   // показ крестика вкладки при наведении

private:
    QWidget* buildTopBar();
    QWidget* buildHome();
    QWidget* buildDeviceSelect();   // страница «Просмотр»: плитки регистраторов
    QWidget* buildDevices();
    QWidget* buildSettings();       // страница «Настройки»
    QWidget* buildJournal();        // страница «Журнал»
    void     journalAddRow(const LogEntry& e);
    void     journalRefilter();
    QWidget* makeBigCard(const QString& icon, const QString& title, const QString& desc,
                         int page, const QString& tabTitle);
    QWidget* makeMgmt(const QString& icon, const QString& title, int page, const QString& tabTitle);

    void gotoPage(int page, const QString& tabTitle = QString());
    void applyTheme();              // палитра + глобальный QSS по cfgTheme_
    void applyKeepAwake();          // блокировка сна/гашения экрана
    void applyHideCursor();         // автоскрытие курсора при бездействии
    void closeTab(int page);
    void setActiveTab(int page);
    void toggleMax();

    // выбор регистратора / вьюхи просмотра
    void rebuildDeviceTiles();      // плитки со статусами (В сети / Не в сети / Проверка)
    void openDeviceView(int devId); // выбор регистратора: опрос + окно загрузки + вкладка
    int  createDeviceView(const Device& d);    // создать вьюху+связи, вернуть индекс страницы
    int  effBufferMs(const Device& d) const;   // буфер для рега: глобальный или индивидуальный
    void applyBufferToViews();      // разослать актуальный буфер во все открытые вьюхи
    void applyOpenAllMode();        // разослать режим «Открыть все» во все вьюхи
    void rebuildCustomGrids();      // перезаполнить карточку «Пользовательские сетки»
    void rebuildSettingsDeviceLists();  // списки регов в настройках (буферы, автооткрытие)
    void saveSession();             // запомнить открытые вкладки/активную/раскладки
    void restoreSession();          // восстановить вкладки при запуске
    void openAutoSearch();          // модалка автопоиска устройств в сегменте сети
    void buildPerfPopup(QPushButton* anchor);   // индикатор ЦПУ/ОЗУ в шапке
    void samplePerf();              // замер загрузки ЦПУ/ОЗУ

    // страница «Устройства»
    void addDeviceRow(const Device& d, int num);
    void deleteCheckedDevices();
    void showAddPanel(bool on);

    // единый конфиг (config.json): настройки + устройства с камерами
    void loadConfig();
    void saveConfig();
    void rebuildDeviceTable();
    void removeDeviceById(int id);
    void startHeartbeat();
    void checkAllDevices(bool force = true);   // force: вне расписания per-device периодов

    QStackedWidget* stack_ = nullptr;
    QWidget*     topBar_ = nullptr;        // прячем в полноэкранном
    QGridLayout* tilesGrid_ = nullptr;     // сетка плиток выбора регистратора

    QMap<int, LiveView*> liveViews_;       // deviceId -> вьюха
    QMap<int, int>       livePage_;        // deviceId -> индекс страницы в stack_
    PlaybackView* playback_ = nullptr;     // страница «Воспроизведение» (архив), индекс 5

    QHBoxLayout* tabsRow_ = nullptr;
    QMap<int, QPushButton*> tabBtn_;   // page -> кнопка-заголовок вкладки
    QMap<int, QWidget*>     tabBox_;    // page -> контейнер вкладки (для закрытия)
    QMap<int, QPushButton*> tabX_;      // page -> крестик (виден только при наведении)

    QTableWidget* journalTable_ = nullptr;
    QComboBox*    journalLevel_ = nullptr;   // фильтр по уровню

    QFrame*  perfPopup_ = nullptr;      // всплывашка ЦПУ/ОЗУ
    QLabel*  perfCpu_ = nullptr;
    QLabel*  perfRam_ = nullptr;
    QWidget* perfBarCpu_ = nullptr;
    QWidget* perfBarRam_ = nullptr;
    quint64  perfPrevIdle_ = 0, perfPrevTotal_ = 0;

    QDialog*      addPanel_ = nullptr;   // модальное окно «Доб. устройство» по центру
    QTableWidget* devTable_ = nullptr;
    QLineEdit* fName_ = nullptr;
    QLineEdit* fIp_ = nullptr;   QLineEdit* fPort_ = nullptr;
    QLineEdit* fUser_ = nullptr; QLineEdit* fPass_ = nullptr;
    QLabel*    detLbl_ = nullptr;   // подпись «Найдено: ...» под кнопкой «Определить»

    QVector<Device> devices_;
    QWidget* customGridsHost_ = nullptr;   // контейнер списка сеток в настройках (для live-обновления)
    QWidget*   bufferPerRegHost_ = nullptr; // контейнер инд. буферов по регам (перестраиваемый)
    QWidget*   connPerRegHost_ = nullptr;   // контейнер per-device подключения (RTSP/опрос)
    QComboBox* autoOpenCombo_ = nullptr;    // «Сразу открывать просмотр» (перестраиваемый)
    bool     cfgHwDecode_ = false;  // settings.hwdecode из config.json
    QStringList cfgLayouts_;        // settings.layouts ("4x5", ...)
    int      cfgDefaultLayout_ = 4; // settings.defaultLayout: сетка при открытии (4/9/16)
    int      cfgHeartbeatSec_ = 15; // settings.heartbeatSec: период проверки устройств
    // --- зарезервировано (контролы на странице серые, значения хранятся в конфиге) ---
    bool     cfgDefaultStretch_  = false; // масштаб по умолчанию: полноэкранное заполнение
    int      cfgBufferMs_        = 300;   // буфер сглаживания видео, мс (глобальный)
    bool     cfgBufferApplyAll_  = true;  // применять глобальный буфер ко ВСЕМ регистраторам
    bool     cfgOpenAllAuto_     = true;  // «Открыть все»: авто-подбор сетки под число камер
    int      cfgOpenAllMaxCells_ = 16;    // иначе — максимальная сетка (4/9/16/25/36), с пагинацией
    bool     cfgShowTitles_      = true;  // подписи камер в ячейках
    bool     cfgRestoreSession_  = false; // восстанавливать сессию при запуске
    QString  cfgTheme_ = "light";        // тема интерфейса: "light" | "dark"
    bool     cfgKeepAwake_    = false;   // не давать ПК спать/гасить экран
    bool     cfgWatchdog_     = false;   // перезапуск приложения при сбое
    bool     cfgEncryptPass_  = false;   // шифровать пароли устройств (DPAPI)
    bool     cfgConfirmExit_  = false;   // подтверждать выход из приложения
    bool     cfgConfirmCloseAll_ = false; // подтверждать «закрыть все камеры»
    int      cfgPollTimeoutSec_  = 6;    // таймаут опроса регистратора, с
    QStringList cfgSessionOpen_;          // IP регистраторов с открытыми вкладками (для восстановления)
    QString  cfgSessionActive_;           // IP активной вкладки
    // ip -> {раскладка стены, каналы по слотам}: какие камеры были выведены
    QMap<QString, QPair<QString, QVector<int>>> cfgSessionShown_;
    bool     cfgWindowMax_       = false; // окно было развёрнуто
    QString  cfgAutoOpenIp_;              // IP устройства для автооткрытия просмотра (пусто = нет)
    bool     cfgStartFullscreen_ = false; // полноэкранный режим при запуске
    bool     cfgHideCursor_      = false; // скрывать курсор при бездействии
    QString  cfgAdminPass_;               // SHA-256 пароля на Устройства/Настройки (пусто = выкл)
    int      cfgConnTimeoutSec_  = 5;     // таймаут подключения к потоку, с
    bool     cfgLogEnabled_      = false; // журнал работы в файл
    // самообучающаяся база: модель -> {протокол, порт} (пополняется кнопкой «Определить»)
    QMap<QString, QPair<QString,int>> cfgKnownModels_;
    int      nextId_ = 1;
    int      editingId_ = -1;       // -1 = режим добавления, иначе id редактируемого
    QTimer*  heartbeat_ = nullptr;
    QMap<int, qint64> hbLast_;        // id -> время последней проверки (per-device период)
    bool     adminUnlocked_ = false;  // пароль администратора введён в этой сессии
    bool     pendingStartFullscreen_ = false;  // войти в полноэкранный при первом просмотре
    QTimer*  cursorTimer_ = nullptr;  // автоскрытие курсора
    bool     cursorHidden_ = false;
    bool     cursorWatch_ = false;    // фильтр событий активности установлен

    QPoint dragPos_;
    bool   dragging_ = false;
};
