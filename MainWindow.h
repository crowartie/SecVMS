#pragma once
#include <QMainWindow>
#include <QPoint>
#include <QMap>
#include <QVector>
#include "Types.h"
#include "Journal.h"

class QTimer;
class LiveView;
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
    void closeTab(int page);
    void setActiveTab(int page);
    void toggleMax();

    // выбор регистратора / вьюхи просмотра
    void rebuildDeviceTiles();      // плитки со статусами (В сети / Не в сети / Проверка)
    void openDeviceView(int devId); // выбор регистратора: опрос + окно загрузки + вкладка
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
    void checkAllDevices();

    QStackedWidget* stack_ = nullptr;
    QWidget*     topBar_ = nullptr;        // прячем в полноэкранном
    QGridLayout* tilesGrid_ = nullptr;     // сетка плиток выбора регистратора

    QMap<int, LiveView*> liveViews_;       // deviceId -> вьюха
    QMap<int, int>       livePage_;        // deviceId -> индекс страницы в stack_

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
    bool     cfgHwDecode_ = false;  // settings.hwdecode из config.json
    QStringList cfgLayouts_;        // settings.layouts ("4x5", ...)
    int      cfgDefaultLayout_ = 4; // settings.defaultLayout: сетка при открытии (4/9/16)
    int      cfgHeartbeatSec_ = 15; // settings.heartbeatSec: период проверки устройств
    // --- зарезервировано (контролы на странице серые, значения хранятся в конфиге) ---
    bool     cfgDefaultStretch_  = false; // масштаб по умолчанию: полноэкранное заполнение
    int      cfgBufferMs_        = 300;   // буфер сглаживания видео, мс
    bool     cfgShowTitles_      = true;  // подписи камер в ячейках
    bool     cfgRestoreSession_  = false; // восстанавливать сессию при запуске
    int      cfgAutoOpenDev_     = -1;    // id устройства для автооткрытия просмотра (-1 = нет)
    bool     cfgStartFullscreen_ = false; // полноэкранный режим при запуске
    bool     cfgHideCursor_      = false; // скрывать курсор при бездействии
    QString  cfgAdminPass_;               // пароль на Устройства/Настройки (пусто = выкл)
    int      cfgConnTimeoutSec_  = 5;     // таймаут подключения к потоку, с
    QString  cfgSnapshotDir_;             // папка снимков/записей
    bool     cfgAudioEnabled_    = false; // звук на развёрнутой камере
    int      cfgAudioVolume_     = 80;    // громкость, %
    bool     cfgLogEnabled_      = false; // журнал работы в файл
    // самообучающаяся база: модель -> {протокол, порт} (пополняется кнопкой «Определить»)
    QMap<QString, QPair<QString,int>> cfgKnownModels_;
    int      nextId_ = 1;
    int      editingId_ = -1;       // -1 = режим добавления, иначе id редактируемого
    QTimer*  heartbeat_ = nullptr;

    QPoint dragPos_;
    bool   dragging_ = false;
};
