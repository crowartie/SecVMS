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
#include <algorithm>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
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

// ---------------------------------------------------------------- Xiongmai

QVector<ArchSeg> xmQuerySegments(const Device& d, int channel, const QDate& day, QString* err) {
    QVector<ArchSeg> out;
    XmClient c;
    if (!c.login(d.ip, d.port, d.user, d.pass, 6000)) {
        if (err) *err = c.error.isEmpty() ? QStringLiteral("нет связи") : c.error;
        return out;
    }
    const QDateTime from(day, QTime(0, 0, 0));
    const QDateTime to(day, QTime(23, 59, 59));
    for (const auto& p : c.fileQuery(channel - 1, from, to))
        out.append({ p.first, p.second });
    c.logout();
    return out;
}

// ---------------------------------------------------------------- Dahua

// HTTP GET с digest (как dahuaHttpGet в MainWindow, локальная копия для архива)
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
    // 1) фабрика поиска
    QString r = archDahuaGet(d, "mediaFileFind.cgi?action=factory.create");
    const qint64 token = r.section('=', 1).trimmed().toLongLong();
    if (token == 0) { if (err) *err = QStringLiteral("регистратор не ответил (mediaFileFind)"); return out; }
    // 2) условие: канал + сутки, тип dav
    const QString t0 = QDateTime(day, QTime(0,0,0)).toString("yyyy-M-d HH:mm:ss");
    const QString t1 = QDateTime(day, QTime(23,59,59)).toString("yyyy-M-d HH:mm:ss");
    archDahuaGet(d, QString("mediaFileFind.cgi?action=findFile&object=%1&condition.Channel=%2"
                            "&condition.StartTime=%3&condition.EndTime=%4&condition.Types[0]=dav")
                        .arg(token).arg(channel)
                        .arg(QString::fromLatin1(QUrl::toPercentEncoding(t0)),
                             QString::fromLatin1(QUrl::toPercentEncoding(t1))));
    // 3) страницы результатов
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
        if (got < 100) break;                       // последняя страница
    }
    archDahuaGet(d, QString("mediaFileFind.cgi?action=close&object=%1").arg(token), 3000);
    archDahuaGet(d, QString("mediaFileFind.cgi?action=destroy&object=%1").arg(token), 3000);
    return out;
}

// ---------------------------------------------------------------- XM playback

// упакованное время из заголовка I-кадра XM-контейнера
static QDateTime xmUnpackTime(quint32 v) {
    const int sec  =  v        & 0x3F;
    const int min  = (v >> 6)  & 0x3F;
    const int hour = (v >> 12) & 0x1F;
    const int day  = (v >> 17) & 0x1F;
    const int mon  = (v >> 22) & 0x0F;
    const int year = ((v >> 26) & 0x3F) + 2000;
    return QDateTime(QDate(year, mon, day), QTime(hour, min, sec));
}

void XmPlayThread::begin(const Device& d, int channel, const QDateTime& from, const QDateTime& to) {
    dev_ = d; channel_ = channel; from_ = from; to_ = to;
    stop_ = false;
    start();
}

void XmPlayThread::stopAndWait() {
    stop_ = true;
    if (!wait(4000)) { terminate(); wait(); }
}

void XmPlayThread::run() {
    XmClient c;
    if (!c.login(dev_.ip, dev_.port, dev_.user, dev_.pass, 6000)) {
        if (!stop_) emit failed(c.error.isEmpty() ? QStringLiteral("нет связи") : c.error);
        return;
    }
    if (!c.playClaim(channel_ - 1, from_, to_)) {
        if (!stop_) emit failed(c.error);
        c.logout();
        return;
    }
    c.playStart(channel_ - 1, from_, to_);
    QTcpSocket& s = c.sock();

    // FFmpeg: кодек определяется по первому I-кадру (H.264 / H.265)
    AVCodecContext* ctx = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVFrame*  frm = av_frame_alloc();
    SwsContext* sws = nullptr;
    int sw = 0, sh = 0, sfmt = -1, dw = 0, dh = 0;

    QByteArray buf, media;
    QDateTime iTime, lastITime;
    int    gopFrames = 0;           // кадров после последнего I (для fps и курсора)
    double frameDurMs = 40.0;       // оценка длительности кадра
    qint64 lastShow = 0;
    qint64 lastData = QDateTime::currentMSecsSinceEpoch();
    bool   protoFail = false;

    auto ensureCodec = [&](const QByteArray& pay) -> bool {
        if (ctx) return true;
        // первый NAL: HEVC VPS/SPS/PPS = 0x40/0x42/0x44; H264 SPS = 0x67/0x27
        bool hevc = false;
        for (int i = 0; i + 4 < pay.size(); ++i) {
            const uchar* q = (const uchar*)pay.constData() + i;
            int off = -1;
            if (q[0] == 0 && q[1] == 0 && q[2] == 1) off = i + 3;
            else if (i + 5 < pay.size() && q[0] == 0 && q[1] == 0 && q[2] == 0 && q[3] == 1) off = i + 4;
            if (off < 0 || off >= pay.size()) continue;
            const uchar n = (uchar)pay[off];
            hevc = (n == 0x40 || n == 0x42 || n == 0x44 || n == 0x26 || n == 0x02);
            break;
        }
        const AVCodec* dec = avcodec_find_decoder(hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
        if (!dec) return false;
        ctx = avcodec_alloc_context3(dec);
        if (!ctx) return false;
        ctx->flags2 |= AV_CODEC_FLAG2_FAST;
        return avcodec_open2(ctx, dec, nullptr) == 0;
    };

    auto decodeShow = [&](const QByteArray& pay) {
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
                    sw = frm->width; sh = frm->height; sfmt = frm->format;
                    dw = wantW; dh = wantH;
                    sws = sws_getContext(sw, sh, (AVPixelFormat)frm->format,
                                         dw, dh, AV_PIX_FMT_BGRA,
                                         (dw < sw) ? SWS_AREA : SWS_BILINEAR,
                                         nullptr, nullptr, nullptr);
                }
                if (sws) {
                    QImage img(dw, dh, QImage::Format_RGB32);
                    uint8_t* dst[4] = { img.bits(), nullptr, nullptr, nullptr };
                    int stride[4]   = { (int)img.bytesPerLine(), 0, 0, 0 };
                    sws_scale(sws, frm->data, frm->linesize, 0, sh, dst, stride);
                    // пейсинг: кадры равномерно, по оценённому fps
                    const qint64 now = QDateTime::currentMSecsSinceEpoch();
                    if (lastShow) {
                        qint64 w = (qint64)frameDurMs - (now - lastShow);
                        if (w > 1 && w < 300) msleep((unsigned long)w);
                    }
                    lastShow = QDateTime::currentMSecsSinceEpoch();
                    const QDateTime ts = iTime.isValid()
                        ? iTime.addMSecs((qint64)(gopFrames * frameDurMs)) : from_;
                    emit frame(img, ts);
                }
                av_frame_unref(frm);
            }
        }
        av_packet_unref(pkt);
    };

    while (!stop_) {
        if (s.state() != QAbstractSocket::ConnectedState) break;
        if (s.bytesAvailable() == 0 && !s.waitForReadyRead(300)) {
            if (QDateTime::currentMSecsSinceEpoch() - lastData > 6000) break;   // поток иссяк
            continue;
        }
        buf += s.readAll();
        lastData = QDateTime::currentMSecsSinceEpoch();

        // sofia-пакеты: 20 Б заголовок + payload (JSON-статусы пропускаем)
        while (buf.size() >= 20) {
            const quint32 dlen = qFromLittleEndian<quint32>((const uchar*)buf.constData() + 16);
            if (dlen > 4u * 1024 * 1024) { protoFail = true; break; }
            if ((quint32)buf.size() < 20 + dlen) break;
            const char first = dlen ? buf.at(20) : '\0';
            if (first != '{') media += buf.mid(20, (int)dlen);
            buf.remove(0, 20 + (int)dlen);
        }
        if (protoFail) break;

        // контейнер XM: кадры с тегами 00 00 01 FC/FE (I), FD (P), FA (аудио), F9 (мета)
        for (;;) {
            if (stop_ || media.size() < 8) break;
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
                if (t.isValid()) {
                    if (lastITime.isValid() && gopFrames > 0) {   // оценка fps по GOP
                        const qint64 dms = lastITime.msecsTo(t);
                        if (dms > 0 && dms < 30000)
                            frameDurMs = qBound(10.0, (double)dms / (gopFrames + 1), 200.0);
                    }
                    lastITime = t;
                    iTime = t;
                }
                gopFrames = 0;
                decodeShow(pay);
            } else if (tag == 0xFD) {                  // P-кадр: 8 Б заголовок
                if (media.size() < 8) break;
                const quint32 len = qFromLittleEndian<quint32>(p + 4);
                if (len > 8u * 1024 * 1024) { media.remove(0, 4); continue; }
                if ((quint32)media.size() < 8 + len) break;
                const QByteArray pay = media.mid(8, (int)len);
                media.remove(0, 8 + (int)len);
                ++gopFrames;
                decodeShow(pay);
            } else if (tag == 0xFA) {                  // аудио: len16 по смещению 6
                if (media.size() < 8) break;
                const quint16 len = qFromLittleEndian<quint16>(p + 6);
                if (media.size() < 8 + (int)len) break;
                media.remove(0, 8 + (int)len);
            } else {                                    // F9 и прочее: len32 по смещению 4
                if (media.size() < 8) break;
                const quint32 len = qFromLittleEndian<quint32>(p + 4);
                if (len > 1u * 1024 * 1024) { media.remove(0, 4); continue; }
                if ((quint32)media.size() < 8 + len) break;
                media.remove(0, 8 + (int)len);
            }
        }
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
