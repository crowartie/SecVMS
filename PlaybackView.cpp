#include "PlaybackView.h"
#include "VideoCell.h"      // Decoder (архив Dahua по RTSP playback)
#include "StreamUrl.h"
#include "Theme.h"
#include <QComboBox>
#include <QDateEdit>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QPainter>
#include <QMouseEvent>
#include <QFileDialog>
#include <QMessageBox>
#include <QStandardPaths>
#include <QUrl>
#include <QtConcurrent>
#include <QFutureWatcher>

// ---------------------------------------------------------------- таймлайн

// 24-часовая шкала: зоны записи + курсор + выделенный интервал.
// Обычный клик/протяжка = переход; Shift+клик = отметить начало/конец интервала.
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
    void setSelection(const QDateTime& a, const QDateTime& b) { selA_ = a; selB_ = b; update(); }
signals:
    void seekRequested(const QDateTime& t);
    void selectRequested(const QDateTime& t);
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        const QRect bar(8, 8, width() - 16, 26);
        p.fillRect(rect(), Theme::dark ? QColor("#1b1f24") : QColor("#e9edf0"));
        p.fillRect(bar, Theme::dark ? QColor("#14171b") : QColor("#ffffff"));
        p.setPen(Theme::dark ? QColor("#333a42") : QColor("#c9cfd7"));
        p.drawRect(bar.adjusted(0, 0, -1, -1));
        auto xOf = [&](const QDateTime& t){
            const double a = qBound<qint64>(qint64(0), QDateTime(day_,QTime(0,0,0)).secsTo(t),
                                            qint64(86400)) / 86400.0;
            return bar.x() + int(a * bar.width());
        };
        if (day_.isValid())
            for (const auto& s : segs_) {
                const int x0 = xOf(s.b), x1 = qMax(xOf(s.e), x0 + 1);
                p.fillRect(QRect(QPoint(x0, bar.y() + 1), QPoint(x1, bar.bottom() - 1)),
                           QColor("#3ca35a"));
            }
        // выделенный для скачивания интервал
        if (selA_.isValid() && selB_.isValid() && day_.isValid()) {
            int x0 = xOf(selA_ < selB_ ? selA_ : selB_);
            int x1 = xOf(selA_ < selB_ ? selB_ : selA_);
            p.fillRect(QRect(QPoint(x0, bar.y()), QPoint(qMax(x1, x0+1), bar.bottom())),
                       QColor(31, 111, 214, 90));
            p.setPen(QColor("#1f6fd6"));
            p.drawLine(x0, bar.y(), x0, bar.bottom());
            p.drawLine(x1, bar.y(), x1, bar.bottom());
        }
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
        if (cur_.isValid() && day_.isValid() && cur_.date() == day_) {
            const int x = xOf(cur_);
            p.setPen(QPen(QColor("#e2574c"), 2));
            p.drawLine(x, bar.y() - 3, x, bar.bottom() + 3);
        }
    }
    void mousePressEvent(QMouseEvent* e) override { emitAt(e); }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (e->buttons() & Qt::LeftButton) emitAt(e);
    }
private:
    void emitAt(QMouseEvent* e) {
        if (!day_.isValid()) return;
        const QRect bar(8, 8, width() - 16, 26);
        const double frac = qBound(0.0, (e->position().x() - bar.x()) / (double)bar.width(), 1.0);
        const QDateTime t = QDateTime(day_, QTime(0, 0, 0)).addSecs(qint64(frac * 86400));
        if (e->modifiers() & Qt::ShiftModifier) emit selectRequested(t);
        else                                    emit seekRequested(t);
    }
    QDate day_;
    QVector<ArchSeg> segs_;
    QDateTime cur_, selA_, selB_;
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

PlaybackView::~PlaybackView() { stopStream(); if (dl_) dl_->cancel(); }

void PlaybackView::buildUi() {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(12, 10, 12, 10);
    v->setSpacing(8);

    auto* top = new QHBoxLayout; top->setSpacing(8);
    top->addWidget(new QLabel(QStringLiteral("Регистратор:")));
    devCb_ = new QComboBox; devCb_->setMinimumWidth(180);
    top->addWidget(devCb_);
    top->addWidget(new QLabel(QStringLiteral("Камера:")));
    camCb_ = new QComboBox; camCb_->setMinimumWidth(140);
    top->addWidget(camCb_);
    streamCb_ = new QComboBox;
    streamCb_->addItem(QStringLiteral("Осн. поток"), 0);
    streamCb_->addItem(QStringLiteral("Субпоток"),   1);
    streamCb_->setToolTip(QStringLiteral("Субпоток меньше грузит сеть и регистратор"));
    top->addWidget(streamCb_);
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

    auto* bot = new QHBoxLayout; bot->setSpacing(8);
    playBtn_ = new QPushButton(QStringLiteral("▶"));
    playBtn_->setObjectName("tool"); playBtn_->setFixedWidth(44);
    playBtn_->setToolTip(QStringLiteral("Воспроизведение / пауза"));
    bot->addWidget(playBtn_);
    timeLbl_ = new QLabel(QStringLiteral("—")); timeLbl_->setMinimumWidth(150);
    bot->addWidget(timeLbl_);
    bot->addSpacing(6);
    // скорость
    for (double sp : { 1.0, 2.0, 4.0, 8.0 }) {
        auto* b = new QPushButton(QString("x%1").arg(sp, 0, 'g', 2));
        b->setObjectName("tool"); b->setCheckable(true); b->setFixedWidth(38);
        b->setProperty("spd", sp);
        connect(b, &QPushButton::clicked, this, [this, sp]{ speed_ = sp; applySpeed();
            for (auto* x : speedBtns_) x->setChecked(x->property("spd").toDouble() == sp); });
        speedBtns_ << b;
        bot->addWidget(b);
    }
    speedBtns_.first()->setChecked(true);
    bot->addStretch();
    // выделение + скачивание
    selLbl_ = new QLabel(QStringLiteral("Интервал: Shift+клик по шкале (начало и конец)"));
    selLbl_->setObjectName("fieldLbl");
    bot->addWidget(selLbl_);
    dlBar_ = new QProgressBar; dlBar_->setFixedWidth(140); dlBar_->setVisible(false);
    bot->addWidget(dlBar_);
    dlCancel_ = new QPushButton(QStringLiteral("Отмена")); dlCancel_->setObjectName("ghost");
    dlCancel_->setVisible(false);
    bot->addWidget(dlCancel_);
    dlBtn_ = new QPushButton(QStringLiteral("Скачать интервал")); dlBtn_->setObjectName("tool");
    dlBtn_->setEnabled(false);
    bot->addWidget(dlBtn_);
    v->addLayout(bot);

    connect(devCb_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int){
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
    connect(tl_, &TimelineBar::selectRequested, this, [this](const QDateTime& t){
        // Shift-клики задают начало и конец интервала (третий клик начинает заново)
        if (!selStart_.isValid() || (selStart_.isValid() && selEnd_.isValid())) {
            selStart_ = t; selEnd_ = QDateTime();
        } else {
            selEnd_ = t;
        }
        tl_->setSelection(selStart_, selEnd_);
        const bool ready = selStart_.isValid() && selEnd_.isValid() && selStart_ != selEnd_;
        dlBtn_->setEnabled(ready && !dl_);
        if (ready) {
            QDateTime a = qMin(selStart_, selEnd_), b = qMax(selStart_, selEnd_);
            selLbl_->setText(QStringLiteral("Интервал: %1 – %2")
                                 .arg(a.toString("HH:mm:ss"), b.toString("HH:mm:ss")));
        } else selLbl_->setText(QStringLiteral("Интервал: отметьте конец (Shift+клик)"));
    });
    connect(playBtn_, &QPushButton::clicked, this, [this]{
        if (playing_ && !paused_) {                 // пауза
            paused_ = true;
            if (xm_) xm_->setPaused(true);
            else stopStream();                        // Dahua: пауза = стоп потока
            playBtn_->setText(QStringLiteral("▶"));
        } else if (playing_ && paused_) {           // продолжить
            paused_ = false;
            if (xm_) { xm_->setPaused(false); playBtn_->setText(QStringLiteral("⏸")); }
            else seekTo(cursor_.isValid() ? cursor_ : segs_.first().b, true);
        } else if (!segs_.isEmpty()) {
            seekTo(cursor_.isValid() ? cursor_ : segs_.first().b, true);
        }
    });
    connect(dlBtn_, &QPushButton::clicked, this, [this]{ startDownload(); });
    connect(dlCancel_, &QPushButton::clicked, this, [this]{ if (dl_) dl_->cancel(); });
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
int PlaybackView::curChannel() const { return camCb_->currentData().toInt(); }
int PlaybackView::curStream()  const { return streamCb_->currentData().toInt(); }

void PlaybackView::stopStream() {
    if (xm_) { xm_->stopAndWait(); xm_->deleteLater(); xm_ = nullptr; }
    if (dh_) { dh_->stopAndWait(); dh_->deleteLater(); dh_ = nullptr; }
}

void PlaybackView::stopPlayback() {
    stopStream();
    playing_ = false; paused_ = false;
    if (playBtn_) playBtn_->setText(QStringLiteral("▶"));
}

void PlaybackView::applySpeed() {
    if (xm_) xm_->setSpeed(speed_);
    if (dh_) dh_->setSpeed(speed_);
}

void PlaybackView::loadSegments() {
    const Device* d = curDevice();
    if (!d || curChannel() <= 0 || loading_ || dl_) return;
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
    selStart_ = selEnd_ = QDateTime(); tl_->setSelection({}, {}); dlBtn_->setEnabled(false);

    const Device dev = *d;
    const int ch = curChannel();
    const int st = curStream();
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
                             .arg(segs_.size()).arg(totalSec / 3600).arg((totalSec % 3600) / 60));
        seekTo(segs_.first().b, true);
    });
    w->setFuture(QtConcurrent::run([dev, ch, day, st]{
        QString err;
        return (dev.proto == "dahua") ? dahuaQuerySegments(dev, ch, day, &err)
                                      : xmQuerySegments(dev, ch, day, &err, st);
    }));
}

void PlaybackView::seekTo(QDateTime t, bool autoplay) {
    if (segs_.isEmpty()) return;
    seekSegIdx_ = -1;
    for (int i = 0; i < segs_.size(); ++i) {
        if (t < segs_[i].b)                       { t = segs_[i].b; seekSegIdx_ = i; break; }
        if (t >= segs_[i].b && t <= segs_[i].e)   { seekSegIdx_ = i; break; }
    }
    if (seekSegIdx_ < 0) { seekSegIdx_ = segs_.size() - 1; t = segs_.last().b; }

    stopStream();
    cursor_ = t; seekBase_ = t;
    paused_ = false;
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
        const QString u = QString::fromLatin1(QUrl::toPercentEncoding(d->user));
        const QString p = QString::fromLatin1(QUrl::toPercentEncoding(d->pass));
        const QString url = QString("rtsp://%1:%2@%3:%4/cam/playback?channel=%5&subtype=%6"
                                    "&starttime=%7&endtime=%8")
            .arg(u, p, d->ip).arg(d->rtspPort).arg(curChannel()).arg(curStream())
            .arg(t.toString("yyyy_MM_dd_HH_mm_ss"), dayEnd.toString("yyyy_MM_dd_HH_mm_ss"));
        dh_ = new Decoder(this);
        dh_->setOneShot(true);
        dh_->setBuffer(150);
        dh_->setSpeed(speed_);
        dh_->setTarget(screen_->width(), screen_->height());
        connect(dh_, &Decoder::frame, this, [this](const QImage& img){ screen_->setImage(img); },
                Qt::QueuedConnection);
        connect(dh_, &Decoder::progress, this, [this](double sec){
            cursor_ = mapElapsed(sec); tl_->setCursorTime(cursor_); updateTimeLabel();
        }, Qt::QueuedConnection);
        connect(dh_, &Decoder::eof, this, [this]{
            playing_ = false; playBtn_->setText(QStringLiteral("▶"));
            status_->setText(QStringLiteral("Конец записей за день"));
        }, Qt::QueuedConnection);
        connect(dh_, &Decoder::openFailed, this, [this]{
            status_->setText(QStringLiteral("Не удалось открыть архивный поток"));
        }, Qt::QueuedConnection);
        dh_->begin(url, false, d->rtspUdp);
    } else {
        xm_ = new XmPlayThread(this);
        xm_->setTarget(screen_->width(), screen_->height());
        connect(xm_, &XmPlayThread::frame, this, &PlaybackView::onFrameTs, Qt::QueuedConnection);
        connect(xm_, &XmPlayThread::ended, this, [this]{
            playing_ = false; playBtn_->setText(QStringLiteral("▶"));
            status_->setText(QStringLiteral("Конец записей за день"));
        }, Qt::QueuedConnection);
        connect(xm_, &XmPlayThread::failed, this, [this](const QString& why){
            playing_ = false; playBtn_->setText(QStringLiteral("▶"));
            status_->setText(QStringLiteral("Архив: %1").arg(why));
        }, Qt::QueuedConnection);
        xm_->begin(*d, curChannel(), t, dayEnd, curStream(), speed_);
    }
}

void PlaybackView::onFrameTs(const QImage& img, const QDateTime& ts) {
    screen_->setImage(img);
    if (ts.isValid()) { cursor_ = ts; tl_->setCursorTime(ts); updateTimeLabel(); }
}

void PlaybackView::updateTimeLabel() {
    timeLbl_->setText(cursor_.isValid()
        ? cursor_.toString("dd.MM.yyyy  HH:mm:ss") : QStringLiteral("—"));
}

QDateTime PlaybackView::mapElapsed(double sec) const {
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

void PlaybackView::setControlsEnabled(bool on) {
    devCb_->setEnabled(on); camCb_->setEnabled(on); streamCb_->setEnabled(on);
    dateEd_->setEnabled(on); loadBtn_->setEnabled(on); playBtn_->setEnabled(on);
    for (auto* b : speedBtns_) b->setEnabled(on);
    dlBtn_->setEnabled(on && selStart_.isValid() && selEnd_.isValid());
}

void PlaybackView::startDownload() {
    const Device* d = curDevice();
    if (!d || !selStart_.isValid() || !selEnd_.isValid() || dl_) return;
    QDateTime a = qMin(selStart_, selEnd_), b = qMax(selStart_, selEnd_);
    if (a.secsTo(b) < 2) { status_->setText(QStringLiteral("Слишком короткий интервал")); return; }
    if (a.secsTo(b) > 3 * 3600) {   // разумное ограничение — 3 часа за раз
        if (QMessageBox::question(this, QStringLiteral("Скачивание"),
                QStringLiteral("Интервал больше 3 часов — файл будет очень большим. Продолжить?"))
            != QMessageBox::Yes) return;
    }
    const QString base = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    const QString suggest = QString("%1/%2_%3_%4.mp4")
        .arg(base.isEmpty() ? QDir::homePath() : base,
             d->name.isEmpty() ? d->ip : d->name,
             a.toString("yyyyMMdd_HHmmss"), b.toString("HHmmss"));
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Сохранить видео"),
                                                      suggest, "MP4 (*.mp4)");
    if (path.isEmpty()) return;

    // на время скачивания останавливаем просмотр (не грузим регистратор дважды)
    stopPlayback();
    setControlsEnabled(false);
    dlBar_->setValue(0); dlBar_->setVisible(true); dlCancel_->setVisible(true);
    status_->setText(QStringLiteral("Скачивание..."));

    dl_ = new ArchiveDownloader(this);
    connect(dl_, &ArchiveDownloader::progress, this, [this](int pct){ dlBar_->setValue(pct); },
            Qt::QueuedConnection);
    connect(dl_, &ArchiveDownloader::done, this, [this](const QString& p){
        dlBar_->setVisible(false); dlCancel_->setVisible(false);
        setControlsEnabled(true);
        dl_->deleteLater(); dl_ = nullptr;
        status_->setText(QStringLiteral("Сохранено: %1").arg(p));
    }, Qt::QueuedConnection);
    connect(dl_, &ArchiveDownloader::failed, this, [this](const QString& why){
        dlBar_->setVisible(false); dlCancel_->setVisible(false);
        setControlsEnabled(true);
        dl_->deleteLater(); dl_ = nullptr;
        status_->setText(QStringLiteral("Скачивание не удалось: %1").arg(why));
    }, Qt::QueuedConnection);
    dl_->begin(*d, curChannel(), a, b, curStream() == 0, path);
}

void PlaybackView::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    if (xm_) xm_->setTarget(screen_->width(), screen_->height());
    if (dh_) dh_->setTarget(screen_->width(), screen_->height());
}

#include "PlaybackView.moc"
