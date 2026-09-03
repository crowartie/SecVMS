#include "CamResolver.h"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTcpSocket>
#include <QNetworkRequest>
#include <QEventLoop>
#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QDateTime>
#include <QRegularExpression>
#include <QUrl>
#include <QList>
#include <QPair>
#include <QSet>
#include <QStringList>
#include <algorithm>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/time.h>
}

// ---------------------------------------------------------------- ONVIF

// SOAP-вызов с WS-Security UsernameToken (digest = base64(SHA1(nonce+created+pass)))
static QString onvifSoap(const QString& ip, int port, const QString& path,
                         const QString& user, const QString& pass,
                         const QString& body, int timeoutMs) {
    QByteArray nonce;
    for (int i = 0; i < 16; ++i) nonce.append(char(QRandomGenerator::global()->bounded(256)));
    const QString created = QDateTime::currentDateTimeUtc().toString("yyyy-MM-ddThh:mm:ssZ");
    const QByteArray digest = QCryptographicHash::hash(
        nonce + created.toUtf8() + pass.toUtf8(), QCryptographicHash::Sha1);
    const QString soap = QString(
        "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
        "<s:Header><Security s:mustUnderstand=\"1\" xmlns=\"http://docs.oasis-open.org/wss/2004/01/"
        "oasis-200401-wss-wssecurity-secext-1.0.xsd\"><UsernameToken><Username>%1</Username>"
        "<Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0"
        "#PasswordDigest\">%2</Password><Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/"
        "oasis-200401-wss-soap-message-security-1.0#Base64Binary\">%3</Nonce>"
        "<Created xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\">"
        "%4</Created></UsernameToken></Security></s:Header><s:Body>%5</s:Body></s:Envelope>")
        .arg(user, QString::fromLatin1(digest.toBase64()),
             QString::fromLatin1(nonce.toBase64()), created, body);
    QNetworkAccessManager nam;
    QNetworkRequest req(QUrl(QString("http://%1:%2%3").arg(ip).arg(port).arg(path)));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/soap+xml");
    req.setTransferTimeout(timeoutMs);
    QNetworkReply* rep = nam.post(req, soap.toUtf8());
    QEventLoop loop;
    QObject::connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    QString out = (rep->error() == QNetworkReply::NoError) ? QString::fromUtf8(rep->readAll())
                                                            : QString();
    rep->deleteLater();
    return out;
}

struct OnvifProfile { QString token; qint64 area = 0; };

// GetProfiles: токены профилей + площадь кадра (чтобы отличить основной от суб)
static QList<OnvifProfile> onvifProfiles(const QString& ip, int port, const QString& user,
                                         const QString& pass, int timeoutMs) {
    QList<OnvifProfile> out;
    const QString xml = onvifSoap(ip, port, "/onvif/media_service", user, pass,
        "<GetProfiles xmlns=\"http://www.onvif.org/ver10/media/wsdl\"/>", timeoutMs);
    if (xml.isEmpty() || !xml.contains("Profiles", Qt::CaseInsensitive)) return out;
    static const QRegularExpression reP(
        "<(?:\\w+:)?Profiles\\b[^>]*\\btoken=\"([^\"]+)\"[^>]*>([\\s\\S]*?)</(?:\\w+:)?Profiles>");
    static const QRegularExpression reW("<(?:\\w+:)?Width>(\\d+)<");
    static const QRegularExpression reH("<(?:\\w+:)?Height>(\\d+)<");
    auto it = reP.globalMatch(xml);
    while (it.hasNext()) {
        auto m = it.next();
        OnvifProfile p; p.token = m.captured(1);
        const QString inner = m.captured(2);
        auto mw = reW.match(inner), mh = reH.match(inner);
        if (mw.hasMatch() && mh.hasMatch())
            p.area = mw.captured(1).toLongLong() * mh.captured(1).toLongLong();
        out.append(p);
    }
    return out;
}

static QString onvifStreamUri(const QString& ip, int port, const QString& user,
                              const QString& pass, const QString& token, int timeoutMs) {
    const QString body = QString(
        "<GetStreamUri xmlns=\"http://www.onvif.org/ver10/media/wsdl\">"
        "<StreamSetup><Stream xmlns=\"http://www.onvif.org/ver10/schema\">RTP-Unicast</Stream>"
        "<Transport xmlns=\"http://www.onvif.org/ver10/schema\"><Protocol>RTSP</Protocol></Transport>"
        "</StreamSetup><ProfileToken>%1</ProfileToken></GetStreamUri>").arg(token);
    const QString xml = onvifSoap(ip, port, "/onvif/media_service", user, pass, body, timeoutMs);
    static const QRegularExpression reU("<(?:\\w+:)?Uri>([^<]+)<");
    auto m = reU.match(xml);
    if (!m.hasMatch()) return QString();
    QString uri = m.captured(1).trimmed();
    uri.replace("&amp;", "&");
    return uri;
}

// вписать учётку в URI и зафиксировать хост = IP камеры (камеры иногда отдают 0.0.0.0/чужой)
static QString withCreds(const QString& uri, const QString& ip, const QString& user, const QString& pass) {
    QUrl u(uri);
    if (!u.isValid() || u.scheme().compare("rtsp", Qt::CaseInsensitive) != 0) return QString();
    u.setHost(ip);
    if (!user.isEmpty()) { u.setUserName(user); u.setPassword(pass); }
    return u.toString(QUrl::FullyEncoded);
}

// ---------------------------------------------------------------- шаблоны + проверка

static QString enc(const QString& s) { return QString::fromLatin1(QUrl::toPercentEncoding(s)); }

// Известные RTSP-пути IP-камер: {имя, основной, суб}. Учётка подставляется как user:pass@.
// Xiongmai — особый (учётка в пути), см. tplXm.
struct Tpl { const char* name; const char* main; const char* sub; };
static const Tpl kTpls[] = {
    { "dahua",   "/cam/realmonitor?channel=1&subtype=0", "/cam/realmonitor?channel=1&subtype=1" },
    { "hik",     "/Streaming/Channels/101",               "/Streaming/Channels/102" },
    { "hik-old", "/h264/ch1/main/av_stream",              "/h264/ch1/sub/av_stream" },
    { "tvt",     "/profile1",                             "/profile2" },          // TVT / Provision-ISR
    { "uniview", "/media/video1",                         "/media/video2" },
    { "generic", "/live/ch00_0",                          "/live/ch00_1" },
    { "stream",  "/stream1",                              "/stream2" },
    { "tplink",  "/11",                                   "/12" },
    { "onvifN",  "/onvif1",                               "/onvif2" },
    { "foscam",  "/videoMain",                            "/videoSub" },
    { "axis",    "/axis-media/media.amp",                 "/axis-media/media.amp?streamprofile=Quality" },
    { "h264q",   "/H264?ch=1&subtype=0",                  "/H264?ch=1&subtype=1" },
};

static QString tplUrl(const QString& ip, int port, const QString& u, const QString& p, const char* path) {
    return QString("rtsp://%1:%2@%3:%4%5").arg(enc(u), enc(p), ip).arg(port).arg(QString::fromLatin1(path));
}
static QString tplXm(const QString& ip, int port, const QString& u, const QString& p, bool sub) {
    return QString("rtsp://%1:%2/user=%3&password=%4&channel=1&stream=%5.sdp?real_stream")
        .arg(ip).arg(port).arg(enc(u), enc(p)).arg(sub ? 1 : 0);
}

// Реальное открытие RTSP (DESCRIBE/SETUP) — единственная надёжная проверка пути и учётки
struct ProbeCtx { long long deadline; };
static int probeInterrupt(void* p) { return av_gettime() > static_cast<ProbeCtx*>(p)->deadline; }

static bool rtspProbe(const QString& url, int timeoutMs) {
    AVFormatContext* fmt = avformat_alloc_context();
    if (!fmt) return false;
    ProbeCtx ctx{ av_gettime() + (long long)timeoutMs * 1000 };
    fmt->interrupt_callback.callback = &probeInterrupt;
    fmt->interrupt_callback.opaque   = &ctx;
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&opts, "timeout", QByteArray::number((long long)timeoutMs * 1000).constData(), 0);
    int r = avformat_open_input(&fmt, url.toUtf8().constData(), nullptr, &opts);
    av_dict_free(&opts);
    if (r < 0) { if (fmt) avformat_free_context(fmt); return false; }
    avformat_close_input(&fmt);
    return true;
}

// ---------------------------------------------------------------- резолвер

static bool tcpAlive(const QString& ip, quint16 port, int ms) {
    QTcpSocket s;
    s.connectToHost(ip, port);
    const bool ok = s.waitForConnected(ms);
    s.abort();
    return ok;
}

DirectResult resolveCameraDirect(const CamRef& cam, const QString& fbUser,
                                 const QString& fbPass, int timeoutMs) {
    DirectResult r; r.channel = cam.channel;
    if (cam.ip.isEmpty()) { r.why = QStringLiteral("регистратор не отдал IP камеры"); return r; }
    const QString hint = cam.camProto.toLower();

    // 1) какие порты вообще открыты (быстро): RTSP-кандидаты и HTTP/ONVIF-кандидаты
    QList<int> rtspPorts, httpPorts;
    for (int p : { 554, 8554, 10554 }) if (tcpAlive(cam.ip, (quint16)p, 1200)) rtspPorts << p;
    QList<int> httpTry = { 80, 8899, 8080, 8000, 2020, 10080, 5000, 8001 };
    if (cam.camPort > 0 && !httpTry.contains(cam.camPort) && cam.camPort != 554 &&
        cam.camPort != 34567 && cam.camPort != 37777) httpTry.prepend(cam.camPort);
    for (int p : httpTry) if (tcpAlive(cam.ip, (quint16)p, 700)) httpPorts << p;
    if (rtspPorts.isEmpty() && httpPorts.isEmpty()) {
        r.why = QStringLiteral("недосягаема: закрыты порты RTSP 554/8554/10554 и HTTP");
        return r;
    }

    // 2) кандидаты учёток. Dahua-NVR отдаёт логин, а пароль маскирует — сочетаем логин
    //    регистратора с запасным паролем; пустой пароль не пробуем.
    QList<QPair<QString,QString>> creds;
    auto addCred = [&](const QString& u, const QString& p){
        if (u.isEmpty() || p.isEmpty()) return;
        for (const auto& c : creds) if (c.first == u && c.second == p) return;
        creds.append({ u, p });
    };
    addCred(cam.camUser, cam.camPass);
    addCred(cam.camUser, fbPass);
    addCred(fbUser, fbPass);
    if (creds.isEmpty()) {
        r.why = QStringLiteral("нет учётки: регистратор пароль не отдал, запасная не задана");
        return r;
    }

    // 3) ONVIF: камера сама отдаёт точные URL
    bool onvifSeen = false;
    for (const auto& cr : creds) {
        const QString& user = cr.first; const QString& pass = cr.second;
        for (int port : httpPorts) {
            auto profs = onvifProfiles(cam.ip, port, user, pass, timeoutMs);
            if (profs.isEmpty()) continue;
            onvifSeen = true;
            std::sort(profs.begin(), profs.end(),
                      [](const OnvifProfile& a, const OnvifProfile& b){ return a.area > b.area; });
            const QString mainUri = withCreds(onvifStreamUri(cam.ip, port, user, pass, profs.first().token, timeoutMs),
                                              cam.ip, user, pass);
            if (mainUri.isEmpty()) continue;
            QString subUri = mainUri;
            if (profs.size() >= 2 && profs.last().token != profs.first().token) {
                const QString s = withCreds(onvifStreamUri(cam.ip, port, user, pass, profs.last().token, timeoutMs),
                                            cam.ip, user, pass);
                if (!s.isEmpty()) subUri = s;
            }
            r.main = mainUri; r.sub = subUri; r.how = QString("onvif:%1").arg(port);
            return r;
        }
    }

    // 4) шаблоны — по подсказке протокола регистратора вперёд, потом все остальные;
    //    каждый на каждом живом RTSP-порту с реальной проверкой
    QStringList order;
    auto pushFirst = [&](std::initializer_list<const char*> names){ for (auto n : names) order << n; };
    if      (hint.contains("dahua") || hint.contains("private"))                 pushFirst({ "dahua" });
    else if (hint.contains("hik") || hint.contains("isapi"))                     pushFirst({ "hik", "hik-old" });
    else if (hint.contains("tvt") || hint.contains("ipc") || hint.contains("nvms")) pushFirst({ "tvt" });
    else if (hint.contains("uni"))                                               pushFirst({ "uniview" });
    for (const auto& t : kTpls) if (!order.contains(t.name)) order << t.name;
    const bool xmHint = hint.contains("netip") || hint.contains("xm") || hint.contains("xiongmai");
    QList<int> ports = rtspPorts.isEmpty() ? QList<int>{ 554 } : rtspPorts;
    int tried = 0;
    for (const auto& cr : creds) {
        const QString& user = cr.first; const QString& pass = cr.second;
        for (int port : ports) {
            if (xmHint) {   // XM IPC — сначала его формат
                ++tried;
                if (rtspProbe(tplXm(cam.ip, port, user, pass, true), timeoutMs + 1500)) {
                    r.main = tplXm(cam.ip, port, user, pass, false); r.sub = tplXm(cam.ip, port, user, pass, true);
                    r.how = "xm"; return r;
                }
            }
            for (const QString& name : order) {
                const Tpl* t = nullptr;
                for (const auto& k : kTpls) if (name == k.name) { t = &k; break; }
                if (!t) continue;
                ++tried;
                if (rtspProbe(tplUrl(cam.ip, port, user, pass, t->sub), timeoutMs + 1500)) {
                    r.main = tplUrl(cam.ip, port, user, pass, t->main);
                    r.sub  = tplUrl(cam.ip, port, user, pass, t->sub);
                    r.how  = QString::fromLatin1(t->name); return r;
                }
            }
            if (!xmHint) {  // XM-формат — в конце, если подсказки не было
                ++tried;
                if (rtspProbe(tplXm(cam.ip, port, user, pass, true), timeoutMs + 1500)) {
                    r.main = tplXm(cam.ip, port, user, pass, false); r.sub = tplXm(cam.ip, port, user, pass, true);
                    r.how = "xm"; return r;
                }
            }
        }
    }
    QStringList rp; for (int p : rtspPorts) rp << QString::number(p);
    QStringList hp; for (int p : httpPorts) hp << QString::number(p);
    r.why = QStringLiteral("RTSP-порты: %1; HTTP: %2; ONVIF: %3; шаблонов проверено: %4; учёток: %5; протокол на реге: «%6»")
                .arg(rp.isEmpty() ? QStringLiteral("нет") : rp.join('/'),
                     hp.isEmpty() ? QStringLiteral("нет") : hp.join('/'),
                     onvifSeen ? QStringLiteral("отвечает, но URL не дал/учётка не подошла")
                               : QStringLiteral("не отвечает"))
                .arg(tried).arg(creds.size()).arg(cam.camProto);
    return r;
}
