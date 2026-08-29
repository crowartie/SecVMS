#pragma once
#include <QWidget>
#include <QDateTime>
#include <QVector>
#include "Types.h"
#include "Archive.h"

class QComboBox;
class QDateEdit;
class QLabel;
class QPushButton;
class QProgressBar;
class TimelineBar;
class ArchScreen;
class Decoder;

// Страница «Воспроизведение»: архив регистратора как ОДНО непрерывное видео за сутки.
// Мини-файлы записи «сшиваются» в общий 24-часовой ползунок: зоны записи подсвечены,
// клик = переход, дыры проматываются автоматически. Скорость x1..x8, суб/осн поток,
// выделение интервала мышью и скачивание в MP4.
class PlaybackView : public QWidget {
    Q_OBJECT
public:
    explicit PlaybackView(QWidget* parent = nullptr);
    ~PlaybackView() override;

    void setDevices(const QVector<Device>& devs);   // при входе на страницу (актуальный список)
    void stopPlayback();                            // уход со страницы / закрытие приложения
    bool busyDownloading() const { return dl_ != nullptr; }

protected:
    void resizeEvent(QResizeEvent*) override;       // сообщить декодеру размер экрана

private:
    void buildUi();
    void loadSegments();                            // запрос фрагментов за выбранный день
    void seekTo(QDateTime t, bool autoplay);        // переход (внутри дыры — к началу записи)
    void stopStream();                              // остановить текущий поток (без сброса курсора)
    void applySpeed();                              // разослать скорость в активный поток
    void startDownload();                           // скачать выделенный интервал в MP4
    void onFrameTs(const QImage& img, const QDateTime& ts);
    void updateTimeLabel();
    void setControlsEnabled(bool on);               // блокировка UI во время скачивания
    const Device* curDevice() const;
    int  curChannel() const;
    int  curStream() const;                         // 0 = основной, 1 = субпоток
    QDateTime mapElapsed(double sec) const;         // Dahua: прошло от seek -> абсолютное время

    QVector<Device>  devs_;
    QVector<ArchSeg> segs_;
    QDate            day_;

    QComboBox*   devCb_  = nullptr;
    QComboBox*   camCb_  = nullptr;
    QComboBox*   streamCb_ = nullptr;
    QDateEdit*   dateEd_ = nullptr;
    QPushButton* loadBtn_ = nullptr;
    QPushButton* playBtn_ = nullptr;
    QLabel*      status_  = nullptr;
    QLabel*      timeLbl_ = nullptr;
    TimelineBar* tl_      = nullptr;
    ArchScreen*  screen_  = nullptr;
    QVector<QPushButton*> speedBtns_;
    QLabel*      selLbl_  = nullptr;
    QPushButton* dlBtn_   = nullptr;
    QProgressBar* dlBar_  = nullptr;
    QPushButton* dlCancel_ = nullptr;

    XmPlayThread* xm_ = nullptr;    // поток архива Xiongmai
    Decoder*      dh_ = nullptr;    // поток архива Dahua (RTSP playback)
    ArchiveDownloader* dl_ = nullptr;   // фоновая выгрузка

    QDateTime cursor_;              // текущая позиция (абсолютное время)
    QDateTime seekBase_;            // куда сикнулись (для пересчёта Dahua)
    QDateTime selStart_, selEnd_;   // выделенный интервал для скачивания (Shift+клик)
    int       seekSegIdx_ = -1;
    double    speed_ = 1.0;
    bool      playing_ = false;
    bool      loading_ = false;
    bool      paused_  = false;
};
