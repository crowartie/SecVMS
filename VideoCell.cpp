#include "VideoCell.h"
#include "Errors.h"
#include <QPainter>
#include <QMouseEvent>
#include <QEnterEvent>
#include <QResizeEvent>
#include <QFontMetrics>
#include <algorithm>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libavutil/time.h>
}

// выбор HW-формата для декодера (D3D11VA); opaque = int* с нужным pix_fmt
static enum AVPixelFormat sec_get_hw_format(AVCodecContext* ctx, const enum AVPixelFormat* fmts) {
    int want = *reinterpret_cast<int*>(ctx->opaque);
    for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if ((int)*p == want) return *p;
    return fmts[0];   // откат на программный формат
}

// одно общее D3D11-устройство на всё приложение (не плодим по устройству на камеру)
static AVBufferRef* sharedD3D11Device() {
    static QMutex m;
    QMutexLocker lk(&m);
    static AVBufferRef* dev = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        if (av_hwdevice_ctx_create(&dev, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) < 0)
            dev = nullptr;
    }
    return dev;
}

// ----------------------------- Decoder -----------------------------

void Decoder::begin(const QString& url, bool hw, bool udp) {
    url_ = url;
    hw_ = hw;
    udp_ = udp;
    stop_ = false;
    start();   // QThread::run()
}

void Decoder::stopAndWait() {
    stop_ = true;
    if (!wait(3000)) { terminate(); wait(); }
}

int Decoder::interruptCb(void* ctx) {
    auto* self = static_cast<Decoder*>(ctx);
    if (self->stop_.load()) return 1;
    long long dl = self->deadline_.load();
    if (dl && av_gettime() > dl) return 1;   // таймаут блокирующей операции
    return 0;
}

void Decoder::run() {
    while (!stop_) {
        bool ok = openAndDecode();
        if (once_) {                            // архив: один проход и конец
            if (!ok && !stop_) emit openFailed();
            if (!stop_) emit eof();
            return;
        }
        if (!ok && !stop_) emit openFailed();   // поток не открылся — камера недоступна?
        // короткая пауза и новая попытка: молчащий канал (камера перезагружается)
        // надо подхватить сразу, как он оживёт
        for (int i = 0; i < 6 && !stop_; ++i) msleep(130);
    }
}

bool Decoder::openAndDecode() {
    AVFormatContext* fmt = avformat_alloc_context();
    if (!fmt) return false;
    fmt->interrupt_callback.callback = &Decoder::interruptCb;
    fmt->interrupt_callback.opaque   = this;

    const long long connUs = (long long)connMs_.load() * 1000;   // таймаут из настроек
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", udp_ ? "udp" : "tcp", 0);
    if (!udp_) av_dict_set(&opts, "rtsp_flags", "prefer_tcp", 0);
    av_dict_set(&opts, "timeout", QByteArray::number(connUs).constData(), 0);
                                                          // НЕ "stimeout" — в FFmpeg 5+ переименована
    av_dict_set(&opts, "max_delay",      "500000", 0);
    av_dict_set(&opts, "fflags",         "nobuffer", 0);
    av_dict_set(&opts, "probesize",       "262144", 0);   // быстрый старт: не ждать
    av_dict_set(&opts, "analyzeduration", "500000", 0);   // долгий анализ потока

    deadline_ = av_gettime() + connUs;   // молчащий канал бросаем быстро
    int r = avformat_open_input(&fmt, url_.toUtf8().constData(), nullptr, &opts);
    av_dict_free(&opts);
    if (r < 0) { if (fmt) avformat_free_context(fmt); return false; }

    deadline_ = av_gettime() + connUs;
    if (avformat_find_stream_info(fmt, nullptr) < 0) { avformat_close_input(&fmt); return false; }

    int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) { avformat_close_input(&fmt); return false; }

    AVCodecParameters* par = fmt->streams[vs]->codecpar;
    const AVCodec* dec = avcodec_find_decoder(par->codec_id);
    if (!dec) { avformat_close_input(&fmt); return false; }
    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    if (!ctx) { avformat_close_input(&fmt); return false; }
    const bool big = url_.contains("stream=0") || url_.contains("subtype=0")
                     || url_.contains("/s0/");   // основной поток (XM/Dahua/TVT)
    avcodec_parameters_to_context(ctx, par);
    if (!big) ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;   // big: без LOW_DELAY, чтобы работал
                                                       // frame-threading по ядрам
    ctx->flags2 |= AV_CODEC_FLAG2_FAST;

    // GPU (D3D11VA, общее устройство) — только для крупного вида (один большой поток).
    // Сетка мелких суб-потоков декодируется софтом: 16 HW-сессий с обратным
    // копированием кадров дают рывки, софт для D1 дешевле и плавнее.
    hwPixFmt_ = -1;
    if (hw_) {
        for (int i = 0;; ++i) {
            const AVCodecHWConfig* cfg = avcodec_get_hw_config(dec, i);
            if (!cfg) break;
            if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                cfg->device_type == AV_HWDEVICE_TYPE_D3D11VA) {
                hwPixFmt_ = (int)cfg->pix_fmt; break;
            }
        }
        AVBufferRef* shared = (hwPixFmt_ != -1) ? sharedD3D11Device() : nullptr;
        if (shared) {
            ctx->hw_device_ctx = av_buffer_ref(shared);
            ctx->opaque = &hwPixFmt_;
            ctx->get_format = sec_get_hw_format;
        } else hwPixFmt_ = -1;
    }
    if (hwPixFmt_ == -1) {                      // софт-декод
        ctx->thread_count = big ? 4 : 1;        // большой поток — 4 потока по ядрам
        if (!big) ctx->skip_loop_filter = AVDISCARD_NONKEY;   // сетке хватает
    }

    if (avcodec_open2(ctx, dec, nullptr) < 0) {
        avcodec_free_context(&ctx); avformat_close_input(&fmt); return false;
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame*  frm = av_frame_alloc();
    AVFrame*  swf = av_frame_alloc();   // для переноса GPU->CPU
    SwsContext* sws = nullptr;
    int sw = 0, sh = 0, sfmt = -1;      // параметры источника
    int dw = 0, dh = 0;                 // размер выходной картинки (под ячейку)

    // пейсинг по PTS: показываем кадры равномерно, в ритме камеры,
    // а не пачками по мере прихода из TCP (иначе микрорывки)
    AVRational tb = fmt->streams[vs]->time_base;
    AVRational fr = fmt->streams[vs]->avg_frame_rate;
    long long frameDurUs = (fr.num > 0 && fr.den > 0)
        ? (long long)(1000000.0 * fr.den / fr.num) : 40000;
    if (frameDurUs < 10000 || frameDurUs > 200000) frameDurUs = 40000;
    long long startWallUs = 0, firstPtsUs = -1, lastEmitUs = 0;
    long long statBytes = 0, statT0 = av_gettime();   // счёт скорости потока
    long long playedUs = 0, prevPtsUs = -1, lastProgUs = 0;   // позиция для архива

    while (!stop_) {
        deadline_ = av_gettime() + connUs + 3LL * 1000000;   // чтение: таймаут + запас
        int rr = av_read_frame(fmt, pkt);
        if (rr < 0) break;                         // EOF/ошибка -> переподключение
        if (pkt->stream_index == vs) {
            statBytes += pkt->size;                // скорость потока (кбит/с раз в секунду)
            long long nowStat = av_gettime();
            if (nowStat - statT0 >= 1000000) {
                emit stats((int)(statBytes * 8ll * 1000000 / (nowStat - statT0) / 1000));
                statBytes = 0; statT0 = nowStat;
            }
            if (avcodec_send_packet(ctx, pkt) == 0) {
                while (!stop_ && avcodec_receive_frame(ctx, frm) == 0) {
                    AVFrame* show = frm;
                    if (hwPixFmt_ != -1 && frm->format == hwPixFmt_) {
                        if (av_hwframe_transfer_data(swf, frm, 0) < 0) { av_frame_unref(frm); continue; }
                        swf->width = frm->width; swf->height = frm->height;
                        show = swf;                // NV12 в системной памяти
                    }
                    // выход = размер ячейки (пропорционально): масштаб делаем ЗДЕСЬ,
                    // в потоке декодера качественным фильтром, а GUI рисует 1:1 —
                    // иначе GUI-поток скейлит 16 потоков сам и всё дёргается
                    int wantW = show->width, wantH = show->height;
                    int tw = tw_.load(), th = th_.load();
                    if (tw > 15 && th > 15) {
                        double s = std::min((double)tw / show->width,
                                            (double)th / show->height);
                        wantW = std::max(16, ((int)(show->width  * s)) & ~1);
                        wantH = std::max(16, ((int)(show->height * s)) & ~1);
                    }
                    if (!sws || sw != show->width || sh != show->height ||
                        sfmt != show->format || dw != wantW || dh != wantH) {
                        if (sws) sws_freeContext(sws);
                        sw = show->width; sh = show->height; sfmt = show->format;
                        dw = wantW; dh = wantH;
                        int flags = (dw < sw) ? SWS_AREA : SWS_BILINEAR;  // AREA — чистое уменьшение
                        sws = sws_getContext(sw, sh, (AVPixelFormat)show->format,
                                             dw, dh, AV_PIX_FMT_BGRA,
                                             flags, nullptr, nullptr, nullptr);
                    }
                    if (sws) {
                        QImage img(dw, dh, QImage::Format_RGB32);   // в памяти BGRA (LE)
                        uint8_t* dst[4] = { img.bits(), nullptr, nullptr, nullptr };
                        int stride[4]  = { (int)img.bytesPerLine(), 0, 0, 0 };
                        sws_scale(sws, show->data, show->linesize, 0, sh, dst, stride);

                        // подождать «своего времени» кадра (ровная подача).
                        // Буфер сглаживания: якорь старта сдвигаем в будущее на bufferUs —
                        // все кадры показываются с задержкой buffer, что гасит сетевой джиттер.
                        long long nowUs = av_gettime();
                        long long bufUs = bufferUs_.load();
                        int64_t bpts = frm->best_effort_timestamp;
                        long long targetUs;
                        if (bpts != AV_NOPTS_VALUE) {
                            long long ptsUs = (long long)(bpts * av_q2d(tb) * 1000000.0);
                            if (firstPtsUs < 0) { firstPtsUs = ptsUs; startWallUs = nowUs + bufUs; }
                            targetUs = startWallUs + (ptsUs - firstPtsUs);
                            long long drift = targetUs - nowUs;
                            // «убежали вперёд» разрешаем на величину буфера (+запас), назад — 300мс
                            if (drift > bufUs + 700000 || drift < -300000) {   // скачок PTS / отстали — ресинхрон
                                firstPtsUs = ptsUs; startWallUs = nowUs + bufUs; targetUs = nowUs + bufUs;
                            }
                        } else {
                            targetUs = lastEmitUs ? lastEmitUs + frameDurUs : nowUs + bufUs;
                        }
                        while (!stop_) {
                            long long w = targetUs - av_gettime();
                            if (w <= 2000) break;
                            QThread::usleep((unsigned long)qMin<long long>(w, 15000));
                        }
                        lastEmitUs = av_gettime();
                        emit frame(img);            // QImage COW -> живёт в очереди событий
                        // позиция воспроизведения (архив): суммируем дельты PTS,
                        // скачки (пропуск дыры регистратором) не считаем временем
                        if (once_) {
                            long long curPts = (bpts != AV_NOPTS_VALUE)
                                ? (long long)(bpts * av_q2d(tb) * 1000000.0) : -1;
                            if (curPts >= 0 && prevPtsUs >= 0 && curPts > prevPtsUs &&
                                curPts - prevPtsUs < 500000)
                                playedUs += curPts - prevPtsUs;
                            else
                                playedUs += frameDurUs;
                            if (curPts >= 0) prevPtsUs = curPts;
                            if (playedUs - lastProgUs > 200000) {   // раз в 0.2 с
                                lastProgUs = playedUs;
                                emit progress(playedUs / 1e6);
                            }
                        }
                    }
                    if (show == swf) av_frame_unref(swf);
                    av_frame_unref(frm);
                }
            }
        }
        av_packet_unref(pkt);
    }

    if (sws) sws_freeContext(sws);
    av_frame_free(&frm);
    av_frame_free(&swf);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    return true;
}

// ----------------------------- VideoCell -----------------------------

VideoCell::VideoCell(QWidget* parent) : QWidget(parent) {
    setMinimumSize(160, 90);
    setAttribute(Qt::WA_OpaquePaintEvent);   // рисуем всю ячейку сами — без двойной заливки
}

VideoCell::~VideoCell() { stop(); }

void VideoCell::setOffline(bool o) {
    offline_ = o;
    if (o) { stop(); setStatus(Failed); }
    update();
}

void VideoCell::setStatus(int st) {
    if (status_ == st) return;
    status_ = st;
    emit statusChanged(this);
    update();
}

void VideoCell::play(const QString& url, bool hw) {
    if (playing_ && url == url_) {                     // уже играем этот поток
        if (pend_) { pend_->stopAndWait(); pend_->deleteLater(); pend_ = nullptr; }
        return;
    }
    if (playing_ && dec_) {
        // БЕСШОВНОЕ переключение (суб <-> основной): старый поток продолжает
        // играть, новый открывается фоном; подмена — по первому кадру нового.
        // Иначе зум ждёт кейфрейма основного потока (DVR шлёт с середины GOP).
        if (pend_ && pendUrl_ == url) return;          // уже открываем его
        if (pend_) { pend_->stopAndWait(); pend_->deleteLater(); }
        pendUrl_ = url; pendHw_ = hw;
        pend_ = new Decoder(this);
        connect(pend_, &Decoder::frame, this, &VideoCell::onPendFrame, Qt::QueuedConnection);
        pend_->setTarget(width(), height());
        pend_->setBuffer(bufMs_);
        pend_->setConnTimeout(connMs_);
        pend_->begin(url, hw, udp_);
        return;
    }
    // холодный старт
    url_ = url;
    hw_ = hw;
    failCount_ = 0;
    setStatus(Connecting);
    dec_ = new Decoder(this);
    connect(dec_, &Decoder::frame, this, &VideoCell::onFrame, Qt::QueuedConnection);
    connect(dec_, &Decoder::openFailed, this, &VideoCell::onOpenFailed, Qt::QueuedConnection);
    connect(dec_, &Decoder::stats, this, &VideoCell::onStats, Qt::QueuedConnection);
    playing_ = true;
    kbps_ = 0;
    dec_->setTarget(width(), height());   // скейл под фактический размер ячейки
    dec_->setBuffer(bufMs_);
    dec_->setConnTimeout(connMs_);
    dec_->begin(url, hw, udp_);
}

void VideoCell::setBuffer(int ms) {
    bufMs_ = ms;
    if (dec_)  dec_->setBuffer(ms);
    if (pend_) pend_->setBuffer(ms);
}

void VideoCell::setConnTimeout(int ms) {
    connMs_ = ms;
    if (dec_)  dec_->setConnTimeout(ms);
    if (pend_) pend_->setConnTimeout(ms);
}

void VideoCell::onPendFrame(const QImage& img) {
    if (!pend_ || sender() != pend_) return;
    // новый поток дал кадр — мгновенно подменяем
    { QMutexLocker lk(&mtx_); frame_ = img; }
    if (dec_) { dec_->stopAndWait(); dec_->deleteLater(); }
    dec_ = pend_; pend_ = nullptr;
    url_ = pendUrl_; hw_ = pendHw_;
    kbps_ = 0; failCount_ = 0;
    disconnect(dec_, nullptr, this, nullptr);          // перевесить на боевые слоты
    connect(dec_, &Decoder::frame, this, &VideoCell::onFrame, Qt::QueuedConnection);
    connect(dec_, &Decoder::openFailed, this, &VideoCell::onOpenFailed, Qt::QueuedConnection);
    connect(dec_, &Decoder::stats, this, &VideoCell::onStats, Qt::QueuedConnection);
    if (status_ != Ok) setStatus(Ok);
    update();
}

void VideoCell::resizeEvent(QResizeEvent* e) {
    if (dec_)  dec_->setTarget(width(), height());
    if (pend_) pend_->setTarget(width(), height());
    QWidget::resizeEvent(e);
}

void VideoCell::stop() {
    if (!playing_ && !dec_ && !pend_) return;
    playing_ = false;
    if (pend_) { pend_->stopAndWait(); pend_->deleteLater(); pend_ = nullptr; }
    if (dec_)  { dec_->stopAndWait();  dec_->deleteLater();  dec_ = nullptr; }
    { QMutexLocker lk(&mtx_); frame_ = QImage(); }
    update();
}

void VideoCell::ensureAlive() {
    // декодер сам переподключается в цикле run(); подстрахуемся, если поток умер
    if (playing_ && dec_ && !dec_->isRunning()) dec_->begin(url_, hw_, udp_);
}

void VideoCell::onFrame(const QImage& img) {
    { QMutexLocker lk(&mtx_); frame_ = img; }
    failCount_ = 0;
    if (status_ != Ok) setStatus(Ok);
    update();
}

void VideoCell::onOpenFailed() {
    if (!playing_) return;
    if (++failCount_ >= 2) setStatus(Failed);   // два подряд неудачных открытия
}

void VideoCell::onStats(int kbps) {
    kbps_ = kbps;
    if (hovered_) update();   // шапка с цифрами видна — обновить
}

void VideoCell::enterEvent(QEnterEvent*) { hovered_ = true;  update(); }
void VideoCell::leaveEvent(QEvent*)      { hovered_ = false; update(); }

void VideoCell::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor("#101418"));
    QImage f;
    { QMutexLocker lk(&mtx_); f = frame_; }
    if (!f.isNull()) {
        // кадр уже отмасштабирован декодером под ячейку — здесь остаточный скейл
        // (доводка при resize), сглаженный, почти всегда 1:1
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        if (stretch_) {
            p.drawImage(rect(), f);                       // заполнить всю ячейку
        } else {
            QSize s = f.size().scaled(size(), Qt::KeepAspectRatio);  // исходные пропорции
            QRect tr(QPoint((width()-s.width())/2, (height()-s.height())/2), s);
            p.drawImage(tr, f);
        }
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    }
    // состояние (пока нет картинки): офлайн / подключение / недоступна
    if ((playing_ || offline_) && f.isNull()) {
        p.setPen(QColor("#8a92a0"));
        QString msg = (offline_ || status_ == Failed) ? Err::withCode(Err::CamUnavailable)
                                                       : Err::text(Err::CamConnecting);
        p.drawText(rect(), Qt::AlignCenter, msg);
    }
    if (showTitle_ && !title_.isEmpty()) {
        QString t = " " + title_ + " ";
        QFontMetrics fm(font());
        int w = fm.horizontalAdvance(t) + 6, h = fm.height() + 4;
        int y = height() - h - 4;                    // подпись снизу-слева, как в Smart PSS
        p.fillRect(4, y, w, h, QColor(0,0,0,120));
        p.setPen(QColor("#eaeaea"));
        p.drawText(QRect(4, y, w, h), Qt::AlignCenter, t);
    }
    // hover-шапка: тип потока, скорость, разрешение + крестик (как в Smart PSS)
    if (hovered_ && playing_) {
        const int hh = 22;
        p.fillRect(0, 0, width(), hh, QColor(20, 24, 28, 205));
        QString type = (url_.contains("stream=0") || url_.contains("subtype=0")
                        || url_.contains("/s0/"))
            ? QStringLiteral("Основной поток") : QStringLiteral("Дополнительный поток");
        QString info = type;
        if (!f.isNull() && kbps_ > 0)
            info += QString("(%1kbps, %2*%3)").arg(kbps_).arg(f.width()).arg(f.height());
        p.setPen(QColor("#d8dde3"));
        QFontMetrics fm(font());
        p.drawText(QRect(8, 0, width() - 36, hh), Qt::AlignVCenter | Qt::AlignLeft,
                   fm.elidedText(info, Qt::ElideRight, width() - 36));
        // крестик — убрать камеру с экрана
        p.drawText(closeRect(), Qt::AlignCenter, QStringLiteral("✕"));
    }
    // рамка выбранной ячейки: 1px по самому краю (ложится в зазор сетки)
    if (selected_) {
        p.setPen(QPen(Qt::white, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(rect().adjusted(0, 0, -1, -1));
    }
}

void VideoCell::mousePressEvent(QMouseEvent* e) {
    if (playing_ && hovered_ && closeRect().contains(e->pos())) {
        emit closeRequested(this);   // крестик шапки: убрать с экрана
        return;
    }
    emit clicked(this);
    QWidget::mousePressEvent(e);
}

void VideoCell::mouseDoubleClickEvent(QMouseEvent*) {
    emit doubleClicked(this);
}
