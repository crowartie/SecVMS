#include "Archive.h"
#include "XmClient.h"
#include <QTcpSocket>
#include <QtEndian>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QAuthenticator>
#include <QEventLoop>
#include <QRegularExpression>
#include <QUrl>
#include <QFile>
#include <QDir>
#include <QtMath>
#include <algorithm>
#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
}

// ---------------------------------------------------------------- общее

QVector<ArchSeg> archMerge(QVector<ArchSeg> in, int gapSec) {
    std::sort(in.begin(), in.end(),
              [](const ArchSeg& a, const ArchSeg& b){ return a.b < b.b; });
    QVector<ArchSeg> out;
    for (const auto& s : in) {
        if (!out.isEmpty() && out.last().e.secsTo(s.b) <= gapSec) {
            if (s.e > out.last().e) out.last().e = s.e;   // слить встык
        } else out.append(s);
    }
    return out;
}

// упакованное время из заголовка I-кадра XM-контейнера
static QDateTime xmUnpackTime(quint32 v) {
    const int sec  =  v        & 0x3F;
    const int min  = (v >> 6)  & 0x3F;
    const int hour = (v >> 12) & 0x1F;
    const int day  = (v >> 17) & 0x1F;
    const int mon  = (v >> 22) & 0x0F;
    const int year = ((v >> 26) & 0x3F) + 2000;
    QDate d(year, mon, day);
    QTime t(hour, min, sec);
    return (d.isValid() && t.isValid()) ? QDateTime(d, t) : QDateTime();
}

// определить HEVC по первому NAL полезной нагрузки
static bool payloadIsHevc(const QByteArray& pay) {
    for (int i = 0; i + 4 < pay.size(); ++i) {
        const uchar* q = (const uchar*)pay.constData() + i;
        int off = -1;
        if (q[0] == 0 && q[1] == 0 && q[2] == 1) off = i + 3;
        else if (i + 5 < pay.size() && q[0] == 0 && q[1] == 0 && q[2] == 0 && q[3] == 1) off = i + 4;
        if (off < 0 || off >= pay.size()) continue;
        const uchar n = (uchar)pay[off];
        return (n == 0x40 || n == 0x42 || n == 0x44 || n == 0x26 || n == 0x02);
    }
    return false;
}

// ---------------------------------------------------------------- Xiongmai query

QVector<ArchSeg> xmQuerySegments(const Device& d, int channel, const QDate& day, QString* err,
                                 int stream) {
    QVector<ArchSeg> out;
    XmClient c;
    if (!c.login(d.ip, d.port, d.user, d.pass, 6000)) {
        if (err) *err = c.error.isEmpty() ? QStringLiteral("нет связи") : c.error;
        return out;
    }
    const QDateTime from(day, QTime(0, 0, 0));
    const QDateTime to(day, QTime(23, 59, 59));
    auto files = c.fileQuery(channel, from, to, stream);
    if (files.isEmpty() && stream == 1) files = c.fileQuery(channel, from, to, 0);  // нет суб — берём main
    for (const auto& f : files) out.append({ f.b, f.e });
    c.logout();
    return out;
}

// ---------------------------------------------------------------- Dahua query

static QString archDahuaGet(const Device& d, const QString& pathQuery, int timeoutMs = 8000) {
    QNetworkAccessManager nam;
    QObject::connect(&nam, &QNetworkAccessManager::authenticationRequired,
                     [&](QNetworkReply*, QAuthenticator* a){
                         a->setUser(d.user); a->setPassword(d.pass);
                     });
    QNetworkRequest req(QUrl(QString("http://%1/cgi-bin/%2").arg(d.ip, pathQuery)));
    req.setTransferTimeout(timeoutMs);
    QNetworkReply* rep = nam.get(req);
    QEventLoop loop;
    QObject::connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    QString body = (rep->error() == QNetworkReply::NoError)
                       ? QString::fromUtf8(rep->readAll()) : QString();
    rep->deleteLater();
    return body;
}

QVector<ArchSeg> dahuaQuerySegments(const Device& d, int channel, const QDate& day, QString* err) {
    QVector<ArchSeg> out;
    QString r = archDahuaGet(d, "mediaFileFind.cgi?action=factory.create");
    const qint64 token = r.section('=', 1).trimmed().toLongLong();
    if (token == 0) { if (err) *err = QStringLiteral("регистратор не ответил (mediaFileFind)"); return out; }
    const QString t0 = QDateTime(day, QTime(0,0,0)).toString("yyyy-M-d HH:mm:ss");
    const QString t1 = QDateTime(day, QTime(23,59,59)).toString("yyyy-M-d HH:mm:ss");
    archDahuaGet(d, QString("mediaFileFind.cgi?action=findFile&object=%1&condition.Channel=%2"
                            "&condition.StartTime=%3&condition.EndTime=%4&condition.Types[0]=dav")
                        .arg(token).arg(channel)
                        .arg(QString::fromLatin1(QUrl::toPercentEncoding(t0)),
                             QString::fromLatin1(QUrl::toPercentEncoding(t1))));
    static const QRegularExpression re(
        "items\\[(\\d+)\\]\\.(StartTime|EndTime)=([0-9: -]+)");
    for (int page = 0; page < 200; ++page) {
        QString body = archDahuaGet(d,
            QString("mediaFileFind.cgi?action=findNextFile&object=%1&count=100").arg(token));
        QMap<int, ArchSeg> items;
        auto it = re.globalMatch(body);
        while (it.hasNext()) {
            auto m = it.next();
            int i = m.captured(1).toInt();
            QDateTime t = QDateTime::fromString(m.captured(3).trimmed(), "yyyy-MM-dd HH:mm:ss");
            if (!t.isValid()) t = QDateTime::fromString(m.captured(3).trimmed(), "yyyy-M-d HH:mm:ss");
            if (m.captured(2) == "StartTime") items[i].b = t; else items[i].e = t;
        }
        int got = 0;
        for (const auto& s : items)
            if (s.b.isValid() && s.e.isValid() && s.e > s.b) { out.append(s); ++got; }
        if (got < 100) break;
    }
    archDahuaGet(d, QString("mediaFileFind.cgi?action=close&object=%1").arg(token), 3000);
    archDahuaGet(d, QString("mediaFileFind.cgi?action=destroy&object=%1").arg(token), 3000);
    return out;
}

// ---------------------------------------------------------------- XM depacketize

// Достаёт из TCP-буфера sofia-пакеты, склеивает медиа (пропуская JSON-статусы).
// Возвращает false при явном сбое протокола.
static bool xmDepacketize(QByteArray& buf, QByteArray& media) {
    while (buf.size() >= 20) {
        const quint32 dlen = qFromLittleEndian<quint32>((const uchar*)buf.constData() + 16);
        if (dlen > 4u * 1024 * 1024) return false;
        if ((quint32)buf.size() < 20 + dlen) break;
        const char first = dlen ? buf.at(20) : '\0';
        if (first != '{') media += buf.mid(20, (int)dlen);
        buf.remove(0, 20 + (int)dlen);
    }
    return true;
}

// Разбирает XM-контейнер из media: на каждый видеокадр вызывает cb(payload, isKey, ts).
// Возвращает через ts абсолютное время I-кадров (P-кадры несут ts прошлого I).
template <typename CB>
static void xmParseFrames(QByteArray& media, QDateTime& lastI, CB&& cb) {
    for (;;) {
        if (media.size() < 8) break;
        const uchar* p = (const uchar*)media.constData();
        if (!(p[0] == 0 && p[1] == 0 && p[2] == 1 && p[3] >= 0xF8)) {
            int idx = -1;                          // ресинхронизация по следующему тегу
            for (int i = 1; i + 3 < media.size(); ++i) {
                const uchar* q = (const uchar*)media.constData() + i;
                if (q[0] == 0 && q[1] == 0 && q[2] == 1 && q[3] >= 0xF8) { idx = i; break; }
            }
            if (idx < 0) { media.clear(); break; }
            media.remove(0, idx);
            continue;
        }
        const uchar tag = p[3];
        if (tag == 0xFC || tag == 0xFE) {          // I-кадр: 16 Б заголовок
            if (media.size() < 16) break;
            const quint32 ts  = qFromLittleEndian<quint32>(p + 8);
            const quint32 len = qFromLittleEndian<quint32>(p + 12);
            if (len > 8u * 1024 * 1024) { media.remove(0, 4); continue; }
            if ((quint32)media.size() < 16 + len) break;
            const QByteArray pay = media.mid(16, (int)len);
            media.remove(0, 16 + (int)len);
            const QDateTime t = xmUnpackTime(ts);
            if (t.isValid()) lastI = t;
            cb(pay, true, lastI);
        } else if (tag == 0xFD) {                  // P-кадр: 8 Б заголовок
            if (media.size() < 8) break;
            const quint32 len = qFromLittleEndian<quint32>(p + 4);
            if (len > 8u * 1024 * 1024) { media.remove(0, 4); continue; }
            if ((quint32)media.size() < 8 + len) break;
            const QByteArray pay = media.mid(8, (int)len);
            media.remove(0, 8 + (int)len);
            cb(pay, false, lastI);
        } else if (tag == 0xFA) {                  // аудио: len16 по смещению 6
            if (media.size() < 8) break;
            const quint16 len = qFromLittleEndian<quint16>(p + 6);
            if (media.size() < 8 + (int)len) break;
            media.remove(0, 8 + (int)len);
        } else {                                    // прочее: len32 по смещению 4
            if (media.size() < 8) break;
            const quint32 len = qFromLittleEndian<quint32>(p + 4);
            if (len > 1u * 1024 * 1024) { media.remove(0, 4); continue; }
            if ((quint32)media.size() < 8 + len) break;
            media.remove(0, 8 + (int)len);
        }
    }
}

// ---------------------------------------------------------------- XM playback

void XmPlayThread::begin(const Device& d, int channel, const QDateTime& from, const QDateTime& to,
                         int stream, double speed) {
    dev_ = d; channel_ = channel; from_ = from; to_ = to; stream_ = stream; speed_ = speed;
    stop_ = false; paused_ = false;
    start();
}

void XmPlayThread::stopAndWait() {
    stop_ = true;
    if (!wait(5000)) { terminate(); wait(); }
}

void XmPlayThread::run() {
    XmClient c;
    if (!c.login(dev_.ip, dev_.port, dev_.user, dev_.pass, 6000)) {
        if (!stop_) emit failed(c.error.isEmpty() ? QStringLiteral("нет связи") : c.error);
        return;
    }
    auto files = c.fileQuery(channel_, from_, to_, stream_);
    if (files.isEmpty() && stream_ == 1) files = c.fileQuery(channel_, from_, to_, 0);
    std::sort(files.begin(), files.end(),
              [](const XmClient::ArcFile& a, const XmClient::ArcFile& b){ return a.b < b.b; });
    if (files.isEmpty()) { if (!stop_) emit ended(); c.logout(); return; }

    int startIdx = 0;                              // первый файл, что заканчивается позже from_
    for (int i = 0; i < files.size(); ++i)
        if (files[i].e > from_) { startIdx = i; break; }

    // --- декодер (кодек по первому I-кадру) ---
    AVCodecContext* ctx = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVFrame*  frm = av_frame_alloc();
    SwsContext* sws = nullptr;
    int sw = 0, sh = 0, sfmt = -1, dw = 0, dh = 0;
    double frameDurMs = 40.0;
    qint64 lastShow = 0;

    auto ensureCodec = [&](const QByteArray& pay) -> bool {
        if (ctx) return true;
        const AVCodec* dec = avcodec_find_decoder(payloadIsHevc(pay) ? AV_CODEC_ID_HEVC
                                                                     : AV_CODEC_ID_H264);
        if (!dec) return false;
        ctx = avcodec_alloc_context3(dec);
        if (!ctx) return false;
        ctx->flags2 |= AV_CODEC_FLAG2_FAST;
        return avcodec_open2(ctx, dec, nullptr) == 0;
    };
    auto decodeShow = [&](const QByteArray& pay, const QDateTime& ts) {
        if (!ensureCodec(pay)) return;
        if (av_new_packet(pkt, pay.size()) < 0) return;
        memcpy(pkt->data, pay.constData(), pay.size());
        if (avcodec_send_packet(ctx, pkt) == 0) {
            while (!stop_ && avcodec_receive_frame(ctx, frm) == 0) {
                int wantW = frm->width, wantH = frm->height;
                int tw = tw_.load(), th = th_.load();
                if (tw > 15 && th > 15) {
                    double k = std::min((double)tw / frm->width, (double)th / frm->height);
                    wantW = std::max(16, ((int)(frm->width  * k)) & ~1);
                    wantH = std::max(16, ((int)(frm->height * k)) & ~1);
                }
                if (!sws || sw != frm->width || sh != frm->height ||
                    sfmt != frm->format || dw != wantW || dh != wantH) {
                    if (sws) sws_freeContext(sws);
                    sw = frm->width; sh = frm->height; sfmt = frm->format; dw = wantW; dh = wantH;
                    sws = sws_getContext(sw, sh, (AVPixelFormat)frm->format,
                                         dw, dh, AV_PIX_FMT_BGRA,
                                         (dw < sw) ? SWS_AREA : SWS_BILINEAR, nullptr, nullptr, nullptr);
                }
                if (sws) {
                    QImage img(dw, dh, QImage::Format_RGB32);
                    uint8_t* dst[4] = { img.bits(), nullptr, nullptr, nullptr };
                    int stride[4]   = { (int)img.bytesPerLine(), 0, 0, 0 };
                    sws_scale(sws, frm->data, frm->linesize, 0, sh, dst, stride);
                    const double sp = qMax(0.25, speed_.load());
                    const qint64 now = av_gettime() / 1000;
                    if (lastShow) {
                        qint64 w = (qint64)(frameDurMs / sp) - (now - lastShow);
                        if (w > 1 && w < 400) msleep((unsigned long)w);
                    }
                    lastShow = av_gettime() / 1000;
                    emit frame(img, ts);
                }
                av_frame_unref(frm);
            }
        }
        av_packet_unref(pkt);
    };

    QByteArray buf, media;
    QDateTime  lastI;
    bool       protoFail = false;

    for (int i = startIdx; i < files.size() && !stop_; ++i) {
        const QString fname = files[i].name;
        if (!c.playClaimByName(fname)) { protoFail = true; break; }
        c.playStartByName(fname);
        QTcpSocket& s = c.sock();

        // применить скорость к новому файлу (XM Fast удваивает; x2=1, x4=2, x8=3)
        int wantLvl = (int)qRound(std::log2(qMax(1.0, speed_.load())));
        for (int k = 0; k < wantLvl; ++k) c.playControl("Fast");
        int appliedLvl = wantLvl;
        bool wasPaused = false;
        int  frameCnt = 0, gopCnt = 0;
        QDateTime prevI;
        qint64 lastData = av_gettime() / 1000;

        buf.clear(); media.clear(); lastI = QDateTime();
        while (!stop_) {
            // пауза
            if (paused_.load()) {
                if (!wasPaused) { c.playControl("Pause"); wasPaused = true; }
                msleep(60); continue;
            } else if (wasPaused) { c.playControl("Continue"); wasPaused = false; lastData = av_gettime()/1000; }

            // изменение скорости на лету
            int lvl = (int)qRound(std::log2(qMax(1.0, speed_.load())));
            while (appliedLvl < lvl) { c.playControl("Fast"); ++appliedLvl; }
            while (appliedLvl > lvl) { c.playControl("Slow"); --appliedLvl; }

            if (s.state() != QAbstractSocket::ConnectedState) { protoFail = true; break; }
            if (s.bytesAvailable() == 0 && !s.waitForReadyRead(250)) {
                if (av_gettime()/1000 - lastData > 1200 && frameCnt > 0) break;   // файл доигран
                if (av_gettime()/1000 - lastData > 6000) { protoFail = true; break; }
                continue;
            }
            buf += s.readAll();
            lastData = av_gettime() / 1000;
            if (!xmDepacketize(buf, media)) { protoFail = true; break; }

            xmParseFrames(media, lastI, [&](const QByteArray& pay, bool key, const QDateTime& ts){
                if (key) {
                    if (prevI.isValid() && gopCnt > 0) {   // оценка fps по GOP
                        const qint64 dms = prevI.msecsTo(ts.isValid() ? ts : prevI);
                        if (dms > 0 && dms < 30000)
                            frameDurMs = qBound(10.0, (double)dms / (gopCnt + 1), 200.0);
                    }
                    if (ts.isValid()) prevI = ts;
                    gopCnt = 0;
                } else ++gopCnt;
                ++frameCnt;
                decodeShow(pay, ts);
            });
        }
        c.playStop(fname);
        if (protoFail) break;
    }

    if (sws) sws_freeContext(sws);
    av_frame_free(&frm);
    av_packet_free(&pkt);
    if (ctx) avcodec_free_context(&ctx);
    c.logout();
    if (!stop_) {
        if (protoFail) emit failed(QStringLiteral("сбой протокола воспроизведения"));
        else           emit ended();
    }
}

// ---------------------------------------------------------------- download

void ArchiveDownloader::begin(const Device& d, int channel, const QDateTime& from,
                              const QDateTime& to, bool mainStream, const QString& outPath) {
    dev_ = d; channel_ = channel; from_ = from; to_ = to; main_ = mainStream; out_ = outPath;
    stop_ = false;
    start();
}

void ArchiveDownloader::run() {
    if (dev_.proto == "dahua") runDahua();
    else                       runXm();
}

// Dahua: RTSP playback интервала -> ремукс в MP4 без перекодирования.
void ArchiveDownloader::runDahua() {
    const QString u = QString::fromLatin1(QUrl::toPercentEncoding(dev_.user));
    const QString p = QString::fromLatin1(QUrl::toPercentEncoding(dev_.pass));
    const int sub = main_ ? 0 : 1;
    const QString url = QString("rtsp://%1:%2@%3:%4/cam/playback?channel=%5&subtype=%6"
                                "&starttime=%7&endtime=%8")
        .arg(u, p, dev_.ip).arg(dev_.rtspPort).arg(channel_).arg(sub)
        .arg(from_.toString("yyyy_MM_dd_HH_mm_ss"), to_.toString("yyyy_MM_dd_HH_mm_ss"));

    AVFormatContext* in = nullptr;
    AVDictionary* opt = nullptr;
    av_dict_set(&opt, "rtsp_transport", dev_.rtspUdp ? "udp" : "tcp", 0);
    av_dict_set(&opt, "timeout", "8000000", 0);
    if (avformat_open_input(&in, url.toUtf8().constData(), nullptr, &opt) < 0) {
        av_dict_free(&opt);
        if (!stop_) emit failed(QStringLiteral("не удалось открыть архивный поток")); return;
    }
    av_dict_free(&opt);
    if (avformat_find_stream_info(in, nullptr) < 0) {
        avformat_close_input(&in);
        if (!stop_) emit failed(QStringLiteral("нет данных в потоке")); return;
    }
    int vs = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) { avformat_close_input(&in);
        if (!stop_) emit failed(QStringLiteral("видеопоток не найден")); return; }

    AVFormatContext* out = nullptr;
    avformat_alloc_output_context2(&out, nullptr, "mp4", out_.toUtf8().constData());
    if (!out) { avformat_close_input(&in);
        if (!stop_) emit failed(QStringLiteral("не создать MP4")); return; }
    AVStream* os = avformat_new_stream(out, nullptr);
    avcodec_parameters_copy(os->codecpar, in->streams[vs]->codecpar);
    os->codecpar->codec_tag = 0;
    if (!(out->oformat->flags & AVFMT_NOFILE))
        if (avio_open(&out->pb, out_.toUtf8().constData(), AVIO_FLAG_WRITE) < 0) {
            avformat_free_context(out); avformat_close_input(&in);
            if (!stop_) emit failed(QStringLiteral("не открыть файл для записи")); return;
        }
    if (avformat_write_header(out, nullptr) < 0) {
        avio_closep(&out->pb); avformat_free_context(out); avformat_close_input(&in);
        if (!stop_) emit failed(QStringLiteral("ошибка записи MP4")); return;
    }

    const double totalSec = qMax<double>(1.0, (double)from_.secsTo(to_));
    const AVRational tb = in->streams[vs]->time_base;
    AVPacket* pkt = av_packet_alloc();
    qint64 firstPts = AV_NOPTS_VALUE, lastPts = 0;
    int lastPct = -1;
    while (!stop_ && av_read_frame(in, pkt) >= 0) {
        if (pkt->stream_index == vs) {
            if (firstPts == AV_NOPTS_VALUE && pkt->pts != AV_NOPTS_VALUE) firstPts = pkt->pts;
            if (pkt->pts != AV_NOPTS_VALUE) lastPts = pkt->pts;
            av_packet_rescale_ts(pkt, tb, os->time_base);
            pkt->stream_index = 0;
            av_interleaved_write_frame(out, pkt);
            if (firstPts != AV_NOPTS_VALUE) {
                double done = (lastPts - firstPts) * av_q2d(tb);
                int pct = qBound(0, (int)(done / totalSec * 100), 99);
                if (pct != lastPct) { lastPct = pct; emit progress(pct); }
            }
        }
        av_packet_unref(pkt);
    }
    av_write_trailer(out);
    av_packet_free(&pkt);
    avio_closep(&out->pb);
    avformat_free_context(out);
    avformat_close_input(&in);

    if (stop_) { QFile::remove(out_); return; }
    emit progress(100);
    emit done(out_);
}

// Xiongmai: цепочка файлов ByName -> сырой annexB во временный файл -> ремукс в MP4.
void ArchiveDownloader::runXm() {
    XmClient c;
    if (!c.login(dev_.ip, dev_.port, dev_.user, dev_.pass, 6000)) {
        if (!stop_) emit failed(c.error.isEmpty() ? QStringLiteral("нет связи") : c.error); return;
    }
    auto files = c.fileQuery(channel_, from_, to_, main_ ? 0 : 1);
    if (files.isEmpty() && !main_) files = c.fileQuery(channel_, from_, to_, 0);
    std::sort(files.begin(), files.end(),
              [](const XmClient::ArcFile& a, const XmClient::ArcFile& b){ return a.b < b.b; });
    // оставить только файлы, пересекающие интервал
    QVector<XmClient::ArcFile> use;
    for (const auto& f : files) if (f.e > from_ && f.b < to_) use.append(f);
    if (use.isEmpty()) { c.logout(); if (!stop_) emit failed(QStringLiteral("нет записей в интервале")); return; }

    const QString tmp = out_ + ".es";                 // временный annexB
    QFile es(tmp);
    if (!es.open(QIODevice::WriteOnly)) { c.logout();
        if (!stop_) emit failed(QStringLiteral("не создать временный файл")); return; }

    // Файлы XM почасовые: ByName играет файл целиком, поэтому обрезаем интервал
    // ПО ВРЕМЕНИ КАДРОВ на клиенте (иначе скачается весь час вместо нужных минут).
    bool hevc = false, gotCodec = false, started = false, clipDone = false;
    qint64 frames = 0;
    const qint64 wantSec = qMax<qint64>(1, from_.secsTo(to_));
    QByteArray buf, media;
    QDateTime lastI;
    for (int i = 0; i < use.size() && !stop_ && !clipDone; ++i) {
        if (!c.playClaimByName(use[i].name)) continue;
        c.playStartByName(use[i].name);
        QTcpSocket& s = c.sock();
        int frameCnt = 0;
        qint64 lastData = av_gettime() / 1000;
        buf.clear(); media.clear(); lastI = QDateTime();
        while (!stop_ && !clipDone) {
            if (s.state() != QAbstractSocket::ConnectedState) break;
            if (s.bytesAvailable() == 0 && !s.waitForReadyRead(250)) {
                if (av_gettime()/1000 - lastData > 1200 && frameCnt > 0) break;
                if (av_gettime()/1000 - lastData > 6000) break;
                continue;
            }
            buf += s.readAll();
            lastData = av_gettime() / 1000;
            if (!xmDepacketize(buf, media)) break;
            xmParseFrames(media, lastI, [&](const QByteArray& pay, bool key, const QDateTime& ts){
                if (clipDone) return;
                if (ts.isValid()) {
                    // начинаем с первого I-кадра в интервале; кончаем, когда вышли за конец
                    if (!started && key && ts >= from_) started = true;
                    if (started && ts > to_) { clipDone = true; return; }
                }
                if (!started) return;                 // ещё до начала интервала
                if (!gotCodec) { hevc = payloadIsHevc(pay); gotCodec = true; }
                es.write(pay);
                ++frameCnt; ++frames;
            });
        }
        c.playStop(use[i].name);
        emit progress(qBound(0, (int)(frames > 0 ? 50 + (i + 1) * 40 / use.size() : 5), 99));
    }
    es.close();
    c.logout();
    if (stop_) { QFile::remove(tmp); return; }
    if (!gotCodec || frames < 2) { QFile::remove(tmp); emit failed(QStringLiteral("пустая запись")); return; }

    // реальный fps по числу кадров и длительности интервала (для корректных меток времени)
    double fps = (double)frames / wantSec;
    fps = qBound(4.0, fps, 30.0);
    const AVRational myTb = { 1, (int)qRound(fps) };

    // ремукс annexB -> MP4: демуксер парсит NAL, метки времени назначаем САМИ (монотонно)
    AVFormatContext* in = nullptr;
    AVDictionary* opt = nullptr;
    av_dict_set(&opt, "framerate", QByteArray::number(qRound(fps)).constData(), 0);
    if (avformat_open_input(&in, tmp.toUtf8().constData(),
                            av_find_input_format(hevc ? "hevc" : "h264"), &opt) < 0) {
        av_dict_free(&opt); QFile::remove(tmp);
        if (!stop_) emit failed(QStringLiteral("не разобрать поток")); return;
    }
    av_dict_free(&opt);
    avformat_find_stream_info(in, nullptr);
    AVFormatContext* out = nullptr;
    avformat_alloc_output_context2(&out, nullptr, "mp4", out_.toUtf8().constData());
    AVStream* os = avformat_new_stream(out, nullptr);
    avcodec_parameters_copy(os->codecpar, in->streams[0]->codecpar);
    os->codecpar->codec_tag = 0;
    os->time_base = myTb;
    if (avio_open(&out->pb, out_.toUtf8().constData(), AVIO_FLAG_WRITE) < 0 ||
        avformat_write_header(out, nullptr) < 0) {
        if (out->pb) avio_closep(&out->pb);
        avformat_free_context(out); avformat_close_input(&in); QFile::remove(tmp);
        if (!stop_) emit failed(QStringLiteral("ошибка записи MP4")); return;
    }
    AVPacket* pkt = av_packet_alloc();
    qint64 idx = 0;
    while (!stop_ && av_read_frame(in, pkt) >= 0) {
        pkt->pts = pkt->dts = idx;      // монотонно, 1 кадр = 1 тик myTb
        pkt->duration = 1;
        av_packet_rescale_ts(pkt, myTb, os->time_base);
        pkt->stream_index = 0;
        av_interleaved_write_frame(out, pkt);
        av_packet_unref(pkt);
        ++idx;
    }
    av_write_trailer(out);
    av_packet_free(&pkt);
    avio_closep(&out->pb);
    avformat_free_context(out);
    avformat_close_input(&in);
    QFile::remove(tmp);
    if (stop_) { QFile::remove(out_); return; }
    emit progress(100);
    emit done(out_);
}
