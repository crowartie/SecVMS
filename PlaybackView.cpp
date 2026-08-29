#include "PlaybackView.h"
#include "VideoCell.h"      // Decoder (архив Dahua по RTSP playback)
#include "StreamUrl.h"
#include "Theme.h"
#include <QComboBox>
#include <QDateEdit>
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QPainter>
#include <QMouseEvent>
#include <QUrl>
#include <QtConcurrent>
#include <QFutureWatcher>

// ---------------------------------------------------------------- таймлайн

// 24-часовая шкала: зоны записи + курсор; клик/протяжка = переход.
class TimelineBar : public QWidget {
    Q_OBJECT
public:
    explicit TimelineBar(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedHeight(58);
        setMouseTracking(true);
    }
    void setDay(const QDate& d)                  { day_ = d; update(); }
    void setSegments(const QVector<ArchSeg>& s)  { segs_ = s; update(); }
    void setCursorTime(const QDateTime& t)       { cur_ = t; update(); }
signals:
    void seekRequested(const QDateTime& t);
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        const QRect bar(8, 8, width() - 16, 26);
        p.fillRect(rect(), Theme::dark ? QColor("#1b1f24") : QColor("#e9edf0"));
        p.fillRect(bar, Theme::dark ? QColor("#14171b") : QColor("#ffffff"));
        p.setPen(Theme::dark ? QColor("#333a42") : QColor("#c9cfd7"));
        p.drawRect(bar.adjusted(0, 0, -1, -1));
        // зоны записи
        if (day_.isValid()) {
            const QDateTime d0(day_, QTime(0, 0, 0));
            for (const auto& s : segs_) {
                const double a = qBound<qint64>(qint64(0), d0.secsTo(s.b), qint64(86400)) / 86400.0;
                const double b = qBound<qint64>(qint64(0), d0.secsTo(s.e), qint64(86400)) / 86400.0;
                const int x0 = bar.x() + int(a * bar.width());
                const int x1 = bar.x() + qMax(int(b * bar.width()), x0 + 1);
                p.fillRect(QRect(QPoint(x0, bar.y() + 1), QPoint(x1, bar.bottom() - 1)),
                           QColor("#3ca35a"));
            }
        }
        // часовые деления + подписи каждые 3 часа
        QFont f = font(); f.setPixelSize(10); p.setFont(f);
        for (int h = 0; h <= 24; ++h) {
            const int x = bar.x() + int(bar.width() * h / 24.0);
            const bool big = (h % 3 == 0);
            p.setPen(Theme::dark ? QColor("#3a424b") : QColor("#c9cfd7"));
            p.drawLine(x, bar.bottom() + 1, x, bar.bottom() + (big ? 6 : 3));
            if (big && h < 24) {
                p.setPen(Theme::dark ? QColor("#9aa3ae") : QColor("#5a6270"));
                p.drawText(QRect(x - 14, bar.bottom() + 6, 28, 14), Qt::AlignCenter,
                           QString("%1:00").arg(h, 2, 10, QChar('0')));
            }
        }
        // курсор
        if (cur_.isValid() && day_.isValid() && cur_.date() == day_) {
            const double a = QDateTime(day_, QTime(0,0,0)).secsTo(cur_) / 86400.0;
            const int x = bar.x() + int(a * bar.width());
            p.setPen(QPen(QColor("#e2574c"), 2));
            p.drawLine(x, bar.y() - 3, x, bar.bottom() + 3);
        }
    }
    void mousePressEvent(QMouseEvent* e) override { emitSeek(e->position().x()); }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (e->buttons() & Qt::LeftButton) emitSeek(e->position().x());
    }
private:
    void emitSeek(double px) {
        if (!day_.isValid()) return;
        const QRect bar(8, 8, width() - 16, 26);
        const double frac = qBound(0.0, (px - bar.x()) / (double)bar.width(), 1.0);
        emit seekRequested(QDateTime(day_, QTime(0, 0, 0)).addSecs(qint64(frac * 86400)));
    }
    QDate day_;
    QVector<ArchSeg> segs_;
    QDateTime cur_;
};

// ---------------------------------------------------------------- экран

class ArchScreen : public QWidget {
public:
    explicit ArchScreen(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumSize(320, 180);
        setAttribute(Qt::WA_OpaquePaintEvent);
    }
    void setImage(const QImage& img) { img_ = img; update(); }
    void setNote(const QString& n)   { note_ = n; img_ = QImage(); update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor("#0a0d10"));
        if (!img_.isNull()) {
            p.setRenderHint(QPainter::SmoothPixmapTransform, true);
            const QSize s = img_.size().scaled(size(), Qt::KeepAspectRatio);
            p.drawImage(QRect(QPoint((width()-s.width())/2, (height()-s.height())/2), s), img_);
        } else if (!note_.isEmpty()) {
            p.setPen(QColor("#8a92a0"));
            p.drawText(rect(), Qt::AlignCenter, note_);
        }
    }
private:
    QImage  img_;
    QString note_ = QStringLiteral("Выберите регистратор, камеру и дату,\nзатем нажмите «Показать записи»");
};

// ---------------------------------------------------------------- страница

PlaybackView::PlaybackView(QWidget* parent) : QWidget(parent) { buildUi(); }

PlaybackView::~PlaybackView() { stopStream(); }

void PlaybackView::buildUi() {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(12, 10, 12, 10);
    v->setSpacing(8);

    auto* top = new QHBoxLayout; top->setSpacing(8);
    top->addWidget(new QLabel(QStringLiteral("Регистратор:")));
    devCb_ = new QComboBox; devCb_->setMinimumWidth(190);
    top->addWidget(devCb_);
    top->addWidget(new QLabel(QStringLiteral("Камера:")));
    camCb_ = new QComboBox; camCb_->setMinimumWidth(150);
    top->addWidget(camCb_);
    top->addWidget(new QLabel(QStringLiteral("Дата:")));
    dateEd_ = new QDateEdit(QDate::currentDate());
    dateEd_->setCalendarPopup(true);
    dateEd_->setMaximumDate(QDate::currentDate());
    top->addWidget(dateEd_);
    loadBtn_ = new QPushButton(QStringLiteral("Показать записи"));
    loadBtn_->setObjectName("primary");
    top->addWidget(loadBtn_);
    status_ = new QLabel; status_->setObjectName("fieldLbl");
    top->addWidget(status_, 1);
    v->addLayout(top);

    screen_ = new ArchScreen;
    v->addWidget(screen_, 1);

    tl_ = new TimelineBar;
    v->addWidget(tl_);

    auto* bot = new QHBoxLayout; bot->setSpacing(10);
    playBtn_ = new QPushButton(QStringLiteral("▶"));
    playBtn_->setObjectName("tool");
    playBtn_->setFixedWidth(44);
    playBtn_->setToolTip(QStringLiteral("Воспроизведение / пауза"));
    bot->addWidget(playBtn_);
    timeLbl_ = new QLabel(QStringLiteral("—"));
    bot->addWidget(timeLbl_);
    bot->addStretch();
    v->addLayout(bot);

    connect(devCb_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int){
        // камеры выбранного регистратора
        camCb_->clear();
        if (const Device* d = curDevice())
            for (const auto& c : d->cams)
                camCb_->addItem(c.name.isEmpty() ? QString("Камера %1").arg(c.channel) : c.name,
                                c.channel);
    });
    connect(loadBtn_, &QPushButton::clicked, this, [this]{ loadSegments(); });
    connect(tl_, &TimelineBar::seekRequested, this, [this](const QDateTime& t){
        if (!segs_.isEmpty()) seekTo(t, true);
    });
    connect(playBtn_, &QPushButton::clicked, this, [this]{
        if (playing_) {                      // пауза: остановить поток, курсор остаётся
            stopStream();
            playing_ = false;
            playBtn_->setText(QStringLiteral("▶"));
        } else if (!segs_.isEmpty()) {
            seekTo(cursor_.isValid() ? cursor_ : segs_.first().b, true);
        }
    });
}

void PlaybackView::setDevices(const QVector<Device>& devs) {
    devs_ = devs;
    const QString keep = devCb_->currentData().toString();
    QSignalBlocker b(devCb_);
    devCb_->clear();
    for (const auto& d : devs_)
        devCb_->addItem(d.name.isEmpty() ? d.ip : d.name, d.ip);
    int i = devCb_->findData(keep);
    devCb_->setCurrentIndex(i < 0 ? 0 : i);
    // перезаполнить камеры вручную (сигнал заблокирован)
    camCb_->clear();
    if (const Device* d = curDevice())
        for (const auto& c : d->cams)
            camCb_->addItem(c.name.isEmpty() ? QString("Камера %1").arg(c.channel) : c.name,
                            c.channel);
}

const Device* PlaybackView::curDevice() const {
    const QString ip = devCb_->currentData().toString();
    for (const auto& d : devs_) if (d.ip == ip) return &d;
    return nullptr;
}

int PlaybackView::curChannel() const {
    return camCb_->currentData().toInt();
}

void PlaybackView::stopStream() {
    if (xm_) { xm_->stopAndWait(); xm_->deleteLater(); xm_ = nullptr; }
    if (dh_) { dh_->stopAndWait(); dh_->deleteLater(); dh_ = nullptr; }
}

void PlaybackView::stopPlayback() {
    stopStream();
    playing_ = false;
    if (playBtn_) playBtn_->setText(QStringLiteral("▶"));
}

void PlaybackView::loadSegments() {
    const Device* d = curDevice();
    if (!d || curChannel() <= 0 || loading_) return;
    if (d->proto == "tvt") {
        status_->setText(QStringLiteral("Архив TVT/ONVIF пока не поддерживается"));
        segs_.clear(); tl_->setSegments({});
        screen_->setNote(QStringLiteral("Архив TVT/ONVIF — в следующей версии"));
        return;
    }
    stopPlayback();
    loading_ = true;
    loadBtn_->setEnabled(false);
    status_->setText(QStringLiteral("Запрос списка записей..."));
    day_ = dateEd_->date();
    tl_->setDay(day_);

    const Device dev = *d;
    const int ch = curChannel();
    const QDate day = day_;
    auto* w = new QFutureWatcher<QVector<ArchSeg>>(this);
    connect(w, &QFutureWatcher<QVector<ArchSeg>>::finished, this, [this, w]{
        loading_ = false;
        loadBtn_->setEnabled(true);
        segs_ = archMerge(w->result());
        w->deleteLater();
        tl_->setSegments(segs_);
        if (segs_.isEmpty()) {
            status_->setText(QStringLiteral("Записей за эту дату нет"));
            screen_->setNote(QStringLiteral("Нет записей за выбранную дату"));
            return;
        }
        qint64 totalSec = 0;
        for (const auto& s : segs_) totalSec += s.b.secsTo(s.e);
        status_->setText(QStringLiteral("Фрагментов: %1, записано: %2 ч %3 мин")
                             .arg(segs_.size())
                             .arg(totalSec / 3600).arg((totalSec % 3600) / 60));
        seekTo(segs_.first().b, true);   // сразу играть с первой записи дня
    });
    w->setFuture(QtConcurrent::run([dev, ch, day]{
        QString err;
        QVector<ArchSeg> out = (dev.proto == "dahua")
            ? dahuaQuerySegments(dev, ch, day, &err)
            : xmQuerySegments(dev, ch, day, &err);
        return out;
    }));
}

void PlaybackView::seekTo(QDateTime t, bool autoplay) {
    if (segs_.isEmpty()) return;
    // внутри дыры — прыжок к началу следующей записи; после последней — к последней
    seekSegIdx_ = -1;
    for (int i = 0; i < segs_.size(); ++i) {
        if (t < segs_[i].b)                       { t = segs_[i].b; seekSegIdx_ = i; break; }
        if (t >= segs_[i].b && t <= segs_[i].e)   { seekSegIdx_ = i; break; }
    }
    if (seekSegIdx_ < 0) { seekSegIdx_ = segs_.size() - 1; t = segs_.last().b; }

    stopStream();
    cursor_ = t;
    seekBase_ = t;
    tl_->setCursorTime(t);
    updateTimeLabel();
    if (!autoplay) return;

    const Device* d = curDevice();
    if (!d) return;
    const QDateTime dayEnd(day_, QTime(23, 59, 59));
    playing_ = true;
    playBtn_->setText(QStringLiteral("⏸"));
    status_->clear();

    if (d->proto == "dahua") {
        // Dahua: RTSP playback — регистратор сам играет через все файлы до конца суток
        const QString u = QString::fromLatin1(QUrl::toPercentEncoding(d->user));
        const QString p = QString::fromLatin1(QUrl::toPercentEncoding(d->pass));
        const QString url = QString("rtsp://%1:%2@%3:%4/cam/playback?channel=%5"
                                    "&starttime=%6&endtime=%7")
            .arg(u, p, d->ip).arg(d->rtspPort).arg(curChannel())
            .arg(t.toString("yyyy_MM_dd_HH_mm_ss"), dayEnd.toString("yyyy_MM_dd_HH_mm_ss"));
        dh_ = new Decoder(this);
        dh_->setOneShot(true);
        dh_->setBuffer(200);
        dh_->setTarget(screen_->width(), screen_->height());
        connect(dh_, &Decoder::frame, this, [this](const QImage& img){ screen_->setImage(img); },
                Qt::QueuedConnection);
        connect(dh_, &Decoder::progress, this, [this](double sec){
            cursor_ = mapElapsed(sec);
            tl_->setCursorTime(cursor_);
            updateTimeLabel();
        }, Qt::QueuedConnection);
        connect(dh_, &Decoder::eof, this, [this]{
            playing_ = false;
            playBtn_->setText(QStringLiteral("▶"));
            status_->setText(QStringLiteral("Конец записей за день"));
        }, Qt::QueuedConnection);
        connect(dh_, &Decoder::openFailed, this, [this]{
            status_->setText(QStringLiteral("Не удалось открыть архивный поток"));
        }, Qt::QueuedConnection);
        dh_->begin(url, false, d->rtspUdp);
    } else {
        // Xiongmai: DVRIP ByTime — регистратор «сшивает» файлы и пропускает дыры сам
        xm_ = new XmPlayThread(this);
        xm_->setTarget(screen_->width(), screen_->height());
        connect(xm_, &XmPlayThread::frame, this, &PlaybackView::onFrameTs, Qt::QueuedConnection);
        connect(xm_, &XmPlayThread::ended, this, [this]{
            playing_ = false;
            playBtn_->setText(QStringLiteral("▶"));
            status_->setText(QStringLiteral("Конец записей за день"));
        }, Qt::QueuedConnection);
        connect(xm_, &XmPlayThread::failed, this, [this](const QString& why){
            playing_ = false;
            playBtn_->setText(QStringLiteral("▶"));
            status_->setText(QStringLiteral("Архив: %1").arg(why));
        }, Qt::QueuedConnection);
        xm_->begin(*d, curChannel(), t, dayEnd);
    }
}

void PlaybackView::onFrameTs(const QImage& img, const QDateTime& ts) {
    screen_->setImage(img);
    if (ts.isValid()) {
        cursor_ = ts;
        tl_->setCursorTime(ts);
        updateTimeLabel();
    }
}

void PlaybackView::updateTimeLabel() {
    timeLbl_->setText(cursor_.isValid()
        ? cursor_.toString("dd.MM.yyyy  HH:mm:ss") : QStringLiteral("—"));
}

QDateTime PlaybackView::mapElapsed(double sec) const {
    // Регистратор играет только записанные участки подряд: прошедшее время потока
    // раскладывается по цепочке сегментов начиная с точки перехода.
    if (seekSegIdx_ < 0 || segs_.isEmpty())
        return seekBase_.addMSecs((qint64)(sec * 1000));
    double rem = sec;
    QDateTime pos = seekBase_;
    for (int i = seekSegIdx_; i < segs_.size(); ++i) {
        const double avail = pos.msecsTo(segs_[i].e) / 1000.0;
        if (rem <= avail) return pos.addMSecs((qint64)(rem * 1000));
        rem -= avail;
        if (i + 1 < segs_.size()) pos = segs_[i + 1].b;
        else return segs_[i].e;
    }
    return pos;
}

void PlaybackView::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    if (xm_) xm_->setTarget(screen_->width(), screen_->height());
    if (dh_) dh_->setTarget(screen_->width(), screen_->height());
}

#include "PlaybackView.moc"
