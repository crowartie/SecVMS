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
#include <QSet>
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

static QString tplDahua(const QString& ip, const QString& u, const QString& p, bool sub) {
    return QString("rtsp://%1:%2@%3:554/cam/realmonitor?channel=1&subtype=%4")
        .arg(enc(u), enc(p), ip).arg(sub ? 1 : 0);
}
static QString tplHik(const QString& ip, const QString& u, const QString& p, bool sub) {
    return QString("rtsp://%1:%2@%3:554/Streaming/Channels/10%4")
        .arg(enc(u), enc(p), ip).arg(sub ? 2 : 1);
}
static QString tplXm(const QString& ip, const QString& u, const QString& p, bool sub) {
    return QString("rtsp://%1:554/user=%2&password=%3&channel=1&stream=%4.sdp?real_stream")
        .arg(ip, enc(u), enc(p)).arg(sub ? 1 : 0);
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

// быстрая проверка досягаемости: RTSP-порт камеры (иначе ONVIF/шаблоны ждут таймауты зря)
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
    if (cam.ip.isEmpty()) return r;
    if (!tcpAlive(cam.ip, 554, 1500) && !tcpAlive(cam.ip, 80, 1200)) return r;   // камера не досягаема с этого ПК
    // учётка: то, что знает регистратор; иначе — запасная из настроек
    const QString user = cam.camUser.isEmpty() ? fbUser : cam.camUser;
    const QString pass = cam.camUser.isEmpty() ? fbPass : cam.camPass;
    const QString hint = cam.camProto.toLower();

    // 1) ONVIF: камера сама отдаёт точные URL. Порты: сообщённый регистратором (если это
    //    не чисто RTSP/вендорный порт), затем типичные ONVIF-порты.
    QList<int> ports;
    if (cam.camPort > 0 && cam.camPort != 554 && cam.camPort != 34567 &&
        cam.camPort != 37777 && cam.camPort != 8000) ports << cam.camPort;
    for (int p : { 80, 8899, 8080, 2020 }) if (!ports.contains(p)) ports << p;
    for (int port : ports) {
        auto profs = onvifProfiles(cam.ip, port, user, pass, timeoutMs);
        if (profs.isEmpty()) continue;
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
        r.main = mainUri; r.sub = subUri; r.how = "onvif";
        return r;
    }

    // 2) шаблоны по подсказке протокола регистратора, каждый — с реальной проверкой
    QStringList order;
    if      (hint.contains("dahua") || hint.contains("dh") || hint.contains("private2")) order = { "dahua", "hik", "xm" };
    else if (hint.contains("hik") || hint.contains("isapi"))                                order = { "hik", "dahua", "xm" };
    else if (hint.contains("netip") || hint.contains("xm") || hint.contains("xiongmai"))     order = { "xm", "dahua", "hik" };
    else                                                                                     order = { "dahua", "hik", "xm" };
    for (const QString& t : order) {
        QString main, sub;
        if      (t == "dahua") { main = tplDahua(cam.ip, user, pass, false); sub = tplDahua(cam.ip, user, pass, true); }
        else if (t == "hik")   { main = tplHik(cam.ip, user, pass, false);   sub = tplHik(cam.ip, user, pass, true); }
        else                   { main = tplXm(cam.ip, user, pass, false);    sub = tplXm(cam.ip, user, pass, true); }
        if (rtspProbe(sub, timeoutMs + 1500)) { r.main = main; r.sub = sub; r.how = t; return r; }
    }
    return r;   // не удалось — останется через регистратор
}
