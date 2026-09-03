#pragma once
#include <QWidget>
#include <QVector>
#include <QList>
#include <QSet>
#include <QString>

class VideoCell;
class QGridLayout;
class QTimer;
class QDragEnterEvent;
class QDropEvent;

struct CamInfo { QString name, sub, main; int status = -1;    // status: 1 онлайн, 0 офлайн, -1 неизв.
                 bool udp = false;                             // транспорт RTSP устройства
                 bool direct = false;                          // sub/main — прямые URL камеры
                 QString fbSub, fbMain;                        // запас: через регистратор (для отката)
                 bool usingFallback = false; };                // прямой не открылся — играем через рег

// Видеостена: ЖЁСТКАЯ сетка слотов (1/4/9/16), камеры раскладываются по слотам.
// Стартует пустой: камеры появляются после перетаскивания регистратора/камеры
// из дерева «Организация» (как в Smart PSS) либо populate().
class VideoWall : public QWidget {
    Q_OBJECT
public:
    explicit VideoWall(QWidget* parent = nullptr);

    void setCameras(const QVector<CamInfo>& cams);   // камеры выбранного устройства (полная пересборка)
    void refreshMeta(const QVector<CamInfo>& cams);  // обновить имена/статус БЕЗ сброса показа
    void showWall();                 // страница открыта: запустить только видимые ячейки
    void hideWall();                 // страница закрыта: остановить все потоки
    void setLayout(int cells);       // 1,4,9,16 или 0 = все (квадратная сетка)
    void setLayoutRC(int rows, int cols);   // произвольная сетка, напр. 5×4
    int  layoutRows() const { return rowsOv_; }   // 0 = авто (квадрат)
    int  layoutCols() const { return colsOv_; }
    void setStretch(bool on);        // отрисовка: полноэкранное заполнение ячейки vs пропорции
    void setHwDecode(bool on);       // аппаратный декод для всех ячеек (флаг настроек)
    void setBuffer(int ms);          // буфер сглаживания видео для всех ячеек, мс
    void setConnTimeout(int ms);     // таймаут подключения к потоку для всех ячеек, мс
    void setShowTitles(bool on);     // подписи камер в ячейках
    QString layoutKey() const;       // текущая раскладка: "1"/"4"/"9"/"16" или "RxC"
    void populate();                 // разложить все камеры от первой клетки
    void openAll(bool onlineOnly);   // вывести все камеры (или только те, что в сети)
    void setOpenAllMode(bool autoFit, int maxCells);  // авто-подбор или макс. сетка
    void closeAll();                 // убрать все камеры со стены (потоки стоп)
    bool hasDisplayed() const;       // есть ли хоть одна выведенная камера
    bool isPopulated() const { return populated_; }
    QVector<int> slotCams() const { return slotCam_; }   // выведенный набор (для сессии)
    void setSlotCams(const QVector<int>& s);             // восстановить набор (сессия)
    void setPage(int p);
    int  currentPage() const { return page_; }
    int  pageCount() const;
    int  layout() const { return pageCells_; }
    void restartAll();               // переподключить все видимые (кнопка «обновить»)
    QStringList cameraNames() const;

    void setCameraTitles(const QStringList& titles);  // живые имена каналов с регистратора
    int  camStatus(int i) const;                      // VideoCell::Status камеры i
    bool isCameraVisible(int i) const { return visibleSet().contains(i); }

signals:
    void layoutChanged();            // сменилась раскладка/страница/зум — обновить панель
    void cameraSelected(int cam);    // клик по ячейке — подсветить в дереве
    void cameraStatusChanged(int cam, int status);    // для иконок дерева

public slots:
    void focusCamera(int index);     // показать одну камеру крупно (из дерева)

protected:
    void dragEnterEvent(QDragEnterEvent*) override;   // приём перетаскивания из дерева
    void dropEvent(QDropEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;  // ПКМ по полю: «Закрыть все видео»

private slots:
    void onCellDouble(VideoCell* c);
    void onCellClicked(VideoCell* c);
    void onCellStatus(VideoCell* c);
    void onCellClose(VideoCell* c);   // крестик шапки: убрать камеру со стены
    void reconnectTick();
    void kickStart();                // запуск отложенных ячеек по одной (без штурма регистратора)

private:
    void relayout();
    void applyStreams();             // синхронизировать потоки с видимым набором
    QSet<int> visibleSet() const;    // индексы ВИДИМЫХ КАМЕР (через слоты)
    QString   streamUrlFor(int i, bool big) const;
    QWidget*  emptyCell(int n);      // заглушка пустого слота сетки

    QGridLayout*         grid_ = nullptr;
    QVector<VideoCell*>  cells_;
    QVector<QWidget*>    empties_;    // пул заглушек
    QVector<CamInfo>     cams_;
    QVector<int>         slotCam_;    // слот сетки -> индекс камеры (-1 = пусто)
    QVector<QWidget*>    slotWidgets_;// виджеты слотов текущей страницы (для drop-позиции)
    QTimer*              startTimer_ = nullptr;
    QList<int>           pending_;    // очередь на запуск (индексы камер)
    bool  active_ = false;           // страница «Просмотр» открыта
    bool  populated_ = false;        // камеры разложены по стене
    bool  hwAll_ = false;            // аппаратный декод и для сетки
    bool  useMain_ = false;          // основной поток вместо суб (сейчас всегда суб для сетки)
    bool  stretch_ = false;          // заполнять ячейку целиком
    int   bufMs_ = 300;              // буфер сглаживания видео, мс (для новых ячеек)
    int   connMs_ = 5000;            // таймаут подключения к потоку, мс (для новых ячеек)
    bool  titles_ = true;            // подписи камер в ячейках
    bool  openAllAuto_ = true;       // «Открыть все»: подбирать сетку под число камер
    int   openAllMaxCells_ = 16;     // иначе — фиксированная максимальная сетка
    int   pageCells_ = 4;            // раскладка по умолчанию 2×2, камеры не запущены
    int   rowsOv_ = 0, colsOv_ = 0;  // явная сетка rows×cols (0 = авто-квадрат)
    int   page_ = 0;
    int   zoomed_ = -1;
    int   selectedCam_ = -1;         // выбранная кликом ячейка (белая рамка)
};
