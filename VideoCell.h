#pragma once
#include <QWidget>
#include <QImage>
#include <QMutex>
#include <QString>
#include <QThread>
#include <atomic>

// Поток декодирования одной камеры на FFmpeg (libavformat/libavcodec).
// Собственный RTSP-клиент FFmpeg корректно работает с регистраторами Xiongmai,
// в отличие от сборки VLC без плагина live555. Прерывание через interrupt_callback
// даёт мгновенную неблокирующую остановку и таймауты.
class Decoder : public QThread {
    Q_OBJECT
public:
    explicit Decoder(QObject* parent = nullptr) : QThread(parent) {}
    void begin(const QString& url, bool hw, bool udp = false);
    void stopAndWait();
    void setTarget(int w, int h) { tw_ = w; th_ = h; }   // размер ячейки: скейлим в декодере
    void setBuffer(int ms) { bufferUs_ = (long long)ms * 1000; }  // буфер сглаживания (джиттер), мс
    void setConnTimeout(int ms) { connMs_ = ms; }        // таймаут открытия потока, мс
    void setOneShot(bool on) { once_ = on; }   // архив: один проход, без переподключений
    void setSpeed(double s) { speed_ = s; }    // скорость воспроизведения архива (1/2/4/8)
signals:
    void frame(const QImage& img);
    void openFailed();               // не удалось открыть поток (камера недоступна?)
    void stats(int kbps);            // скорость потока, раз в секунду
    void eof();                      // конец потока (только в режиме oneShot)
    void progress(double sec);       // позиция от начала потока, с (для архива)
protected:
    void run() override;
private:
    bool openAndDecode();
    static int interruptCb(void* ctx);

    QString url_;
    bool    hw_ = false;                    // GPU-декод (только для крупного вида)
    bool    udp_ = false;                   // транспорт RTSP: UDP вместо TCP
    std::atomic<bool>   once_{false};       // архив: один проход
    std::atomic<double> speed_{1.0};        // скорость воспроизведения (архив)
    std::atomic<bool>   stop_{false};
    std::atomic<long long> deadline_{0};   // микросекунды av_gettime()
    std::atomic<int>    tw_{0}, th_{0};    // целевой размер (размер ячейки)
    std::atomic<long long> bufferUs_{300000};  // буфер сглаживания: задержка старта показа, мкс
    std::atomic<int>    connMs_{5000};      // таймаут открытия потока, мс
    int hwPixFmt_ = -1;                     // AV_PIX_FMT_* для HW-кадра (-1 = только софт)
};

// Ячейка видеостены: рисует последний кадр сама (без нативных окон),
// поэтому корректно прячется в QStackedWidget.
class VideoCell : public QWidget {
    Q_OBJECT
public:
    enum Status { Connecting = 0, Ok = 1, Failed = 2 };

    explicit VideoCell(QWidget* parent = nullptr);
    ~VideoCell() override;

    void setTitle(const QString& name) { title_ = name; update(); }
    void setShowTitle(bool on) { showTitle_ = on; update(); }  // подпись камеры в ячейке
    void setSource(const QString& s) { source_ = s; update(); } // «напрямую» / «через регистратор» (в hover-шапке)
    void setStretch(bool on) { stretch_ = on; update(); }
    void setBuffer(int ms);   // буфер сглаживания (мс): в текущие и новые декодеры
    void setConnTimeout(int ms);   // таймаут подключения к потоку (мс)
    void setUdp(bool u) { udp_ = u; }      // транспорт RTSP для новых декодеров
    void setOffline(bool o);   // офлайн по данным регистратора: не стримить, показать «недоступна»
    bool isOffline() const { return offline_; }
    void setSelected(bool on) { if (selected_ != on) { selected_ = on; update(); } }
    bool isSelected() const { return selected_; }
    int  status() const { return status_; }
    void play(const QString& url, bool hw = false);
    void stop();
    bool isPlaying() const { return playing_; }
    void ensureAlive();
    QString url() const { return url_; }

signals:
    void doubleClicked(VideoCell* self);
    void clicked(VideoCell* self);            // выбор ячейки (рамка + подсветка в дереве)
    void statusChanged(VideoCell* self);      // Connecting/Ok/Failed — для дерева
    void closeRequested(VideoCell* self);     // крестик в hover-шапке: убрать с экрана

private slots:
    void onFrame(const QImage& img);
    void onOpenFailed();
    void onStats(int kbps);
    void onPendFrame(const QImage& img);   // первый кадр нового потока — бесшовная подмена

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override; // сообщить декодеру новый размер ячейки
    void mousePressEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void enterEvent(QEnterEvent*) override;   // hover-шапка с типом/скоростью потока
    void leaveEvent(QEvent*) override;

private:
    void setStatus(int st);
    QRect closeRect() const { return QRect(width() - 24, 0, 24, 22); }   // зона крестика

    Decoder* dec_  = nullptr;
    Decoder* pend_ = nullptr;    // открывающийся новый поток (переключение суб<->основной)
    QString  pendUrl_;
    bool     pendHw_ = false;
    QImage  frame_;
    QMutex  mtx_;
    QString url_, title_, source_;
    bool    playing_ = false;
    bool    hw_ = false;
    bool    udp_ = false;       // транспорт RTSP (для новых декодеров)
    bool    showTitle_ = true;  // рисовать подпись камеры
    bool    stretch_ = false;   // false = сохранять пропорции, true = растянуть на ячейку
    int     bufMs_ = 300;       // буфер сглаживания для новых декодеров
    int     connMs_ = 5000;     // таймаут подключения к потоку, мс
    bool    selected_ = false;  // белая рамка выбора
    bool    offline_ = false;   // офлайн по данным регистратора
    bool    hovered_ = false;   // курсор над ячейкой — показать шапку
    int     kbps_ = 0;          // текущая скорость потока
    int     status_ = Connecting;
    int     failCount_ = 0;     // подряд неудачных открытий потока
};
