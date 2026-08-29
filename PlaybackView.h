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
class TimelineBar;
class ArchScreen;
class Decoder;

// Страница «Воспроизведение»: архив регистратора как ОДНО непрерывное видео за сутки.
// Мини-файлы записи «сшиваются» в общий 24-часовой ползунок: зоны записи подсвечены,
// клик по ползунку = переход; дыры между записями проматываются автоматически.
class PlaybackView : public QWidget {
    Q_OBJECT
public:
    explicit PlaybackView(QWidget* parent = nullptr);
    ~PlaybackView() override;

    void setDevices(const QVector<Device>& devs);   // при входе на страницу (актуальный список)
    void stopPlayback();                            // уход со страницы / закрытие приложения

protected:
    void resizeEvent(QResizeEvent*) override;       // сообщить декодеру размер экрана

private:
    void buildUi();
    void loadSegments();                            // запрос фрагментов за выбранный день
    void seekTo(QDateTime t, bool autoplay);        // переход (внутри дыры — к началу записи)
    void stopStream();                              // остановить текущий поток (без сброса курсора)
    void onFrameTs(const QImage& img, const QDateTime& ts);
    void updateTimeLabel();
    const Device* curDevice() const;
    int  curChannel() const;
    QDateTime mapElapsed(double sec) const;         // Dahua: прошло от seek -> абсолютное время

    QVector<Device>  devs_;
    QVector<ArchSeg> segs_;
    QDate            day_;

    QComboBox*   devCb_  = nullptr;
    QComboBox*   camCb_  = nullptr;
    QDateEdit*   dateEd_ = nullptr;
    QPushButton* loadBtn_ = nullptr;
    QPushButton* playBtn_ = nullptr;
    QLabel*      status_  = nullptr;
    QLabel*      timeLbl_ = nullptr;
    TimelineBar* tl_      = nullptr;
    ArchScreen*  screen_  = nullptr;

    XmPlayThread* xm_ = nullptr;    // поток архива Xiongmai
    Decoder*      dh_ = nullptr;    // поток архива Dahua (RTSP playback)

    QDateTime cursor_;              // текущая позиция (абсолютное время)
    QDateTime seekBase_;            // куда сикнулись (для пересчёта Dahua)
    int       seekSegIdx_ = -1;
    bool      playing_ = false;
    bool      loading_ = false;
};
