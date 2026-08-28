#include "MainWindow.h"
#include "LiveView.h"
#include "XmClient.h"
#include "DahuaUtil.h"
#include "Discovery.h"
#include <QApplication>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QCheckBox>
#include <QHeaderView>
#include <QKeyEvent>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QDir>
#include <QStyle>
#include <QIcon>
#include <QMouseEvent>
#include <QCloseEvent>
#include <QEvent>
#include <QCursor>
#include <QStandardPaths>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QTimer>
#include <QSaveFile>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QMimeData>
#include <QSettings>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QAuthenticator>
#include <QTcpSocket>
#include <QEventLoop>
#include <QNetworkInformation>
#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QDateTime>
#include <QRegularExpression>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QThreadPool>
#include <QSet>
#include <QtEndian>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QUuid>
#include <QScreen>
#include <QSharedPointer>
#include <QDialog>
#include <QSpinBox>
#include <QListWidget>
#include <QMessageBox>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QPainter>
#include <QFileDialog>
#include <QDesktopServices>
#include <QUrl>
#include <QScrollArea>
#include <QSlider>
#include <functional>

#ifdef _WIN32
#  include <winsock2.h>
#  include <windows.h>
#  include <wincrypt.h>
#  include <iphlpapi.h>
#endif

// ---- DPAPI: шифрование пароля ключом учётки Windows ----
static QByteArray dpapi(const QByteArray& in, bool enc) {
#ifdef _WIN32
    DATA_BLOB din, dout;
    din.pbData = (BYTE*)in.constData(); din.cbData = (DWORD)in.size();
    BOOL ok = enc ? CryptProtectData(&din, L"SecVMS", nullptr, nullptr, nullptr, 0, &dout)
                  : CryptUnprotectData(&din, nullptr, nullptr, nullptr, nullptr, 0, &dout);
    if (!ok) return {};
    QByteArray out((const char*)dout.pbData, (int)dout.cbData);
    LocalFree(dout.pbData);
    return out;
#else
    Q_UNUSED(enc); return in;
#endif
}
static QString encPass(const QString& p) { return QString::fromLatin1(dpapi(p.toUtf8(), true).toBase64()); }
static QString decPass(const QString& b) { return QString::fromUtf8(dpapi(QByteArray::fromBase64(b.toLatin1()), false)); }
static QString appDataDir() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir;
}
static QString configFile() { return appDataDir() + "/config.json"; }   // единый конфиг

// ONVIF GetVideoSources → число каналов (для NVR на TVT/ONVIF).
// WS-Security UsernameToken (digest = base64(SHA1(nonce+created+pass))).
// Возвращает 0, если сервис не ответил (значит не ONVIF/недоступно).
static int onvifChannelCount(const QString& ip, const QString& user,
                             const QString& pass, int timeoutMs = 6000) {
    QByteArray nonce;
    for (int i = 0; i < 16; ++i) nonce.append(char(QRandomGenerator::global()->bounded(256)));
    QString created = QDateTime::currentDateTimeUtc().toString("yyyy-MM-ddThh:mm:ssZ");
    QByteArray digest = QCryptographicHash::hash(
        nonce + created.toUtf8() + pass.toUtf8(), QCryptographicHash::Sha1);
    QString soap = QString(
        "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
        "<s:Header><Security s:mustUnderstand=\"1\" xmlns=\"http://docs.oasis-open.org/wss/2004/01/"
        "oasis-200401-wss-wssecurity-secext-1.0.xsd\"><UsernameToken><Username>%1</Username>"
        "<Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0"
        "#PasswordDigest\">%2</Password><Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/"
        "oasis-200401-wss-soap-message-security-1.0#Base64Binary\">%3</Nonce>"
        "<Created xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\">"
        "%4</Created></UsernameToken></Security></s:Header>"
        "<s:Body><GetVideoSources xmlns=\"http://www.onvif.org/ver10/media/wsdl\"/></s:Body></s:Envelope>")
        .arg(user, QString::fromLatin1(digest.toBase64()),
             QString::fromLatin1(nonce.toBase64()), created);

    QNetworkAccessManager nam;
    QNetworkRequest req(QUrl(QString("http://%1/onvif/media_service").arg(ip)));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/soap+xml");
    req.setTransferTimeout(timeoutMs);
    QNetworkReply* rep = nam.post(req, soap.toUtf8());
    QEventLoop loop;
    QObject::connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    int count = 0;
    if (rep->error() == QNetworkReply::NoError) {
        const QString body = QString::fromUtf8(rep->readAll());
        static const QRegularExpression re("VideoSources[^>]*token=");
        auto it = re.globalMatch(body);
        while (it.hasNext()) { it.next(); ++count; }
    }
    rep->deleteLater();
    return count;
}

// ONVIF GetDeviceInformation -> модель/серийник (паспорт устройства)
static void onvifDeviceInfo(const QString& ip, const QString& user, const QString& pass,
                            QString& model, QString& serial, int timeoutMs = 6000) {
    QByteArray nonce;
    for (int i = 0; i < 16; ++i) nonce.append(char(QRandomGenerator::global()->bounded(256)));
    QString created = QDateTime::currentDateTimeUtc().toString("yyyy-MM-ddThh:mm:ssZ");
    QByteArray digest = QCryptographicHash::hash(
        nonce + created.toUtf8() + pass.toUtf8(), QCryptographicHash::Sha1);
    QString soap = QString(
        "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
        "<s:Header><Security s:mustUnderstand=\"1\" xmlns=\"http://docs.oasis-open.org/wss/2004/01/"
        "oasis-200401-wss-wssecurity-secext-1.0.xsd\"><UsernameToken><Username>%1</Username>"
        "<Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0"
        "#PasswordDigest\">%2</Password><Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/"
        "oasis-200401-wss-soap-message-security-1.0#Base64Binary\">%3</Nonce>"
        "<Created xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\">"
        "%4</Created></UsernameToken></Security></s:Header>"
        "<s:Body><GetDeviceInformation xmlns=\"http://www.onvif.org/ver10/device/wsdl\"/></s:Body></s:Envelope>")
        .arg(user, QString::fromLatin1(digest.toBase64()),
             QString::fromLatin1(nonce.toBase64()), created);
    QNetworkAccessManager nam;
    QNetworkRequest req(QUrl(QString("http://%1/onvif/device_service").arg(ip)));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/soap+xml");
    req.setTransferTimeout(timeoutMs);
    QNetworkReply* rep = nam.post(req, soap.toUtf8());
    QEventLoop loop;
    QObject::connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    if (rep->error() == QNetworkReply::NoError) {
        const QString body = QString::fromUtf8(rep->readAll());
        auto grab = [&](const char* tag) {
            QRegularExpression re(QString("<[^>]*%1[^>]*>([^<]*)</").arg(tag));
            auto m = re.match(body);
            return m.hasMatch() ? m.captured(1).trimmed() : QString();
        };
        model  = grab("Model");
        serial = grab("SerialNumber");
    }
    rep->deleteLater();
}

// forward-декларации (определения ниже)
static QString dahuaHttpGet(const QString& ip, const QString& user, const QString& pass,
                            const QString& pathQuery, int timeoutMs);
static QVector<CamRef> tvtLapiChannels(const QString& ip, const QString& user, const QString& pass);

// Состояние камер Dahua: LogicDeviceManager getCameraState -> канал -> онлайн?
static QMap<int,bool> dahuaCameraStates(const QString& ip, const QString& user, const QString& pass) {
    QMap<int,bool> st;
    QString body = dahuaHttpGet(ip, user, pass,
        "LogicDeviceManager.cgi?action=getCameraState&uniqueChannels=-1", 6000);
    // строки вида: states[0].channel=0 ... states[0].connectionState=Connected
    static const QRegularExpression re("\\[(\\d+)\\][^\\n]*[Cc]onnectionState=(\\w+)");
    auto it = re.globalMatch(body);
    while (it.hasNext()) {
        auto m = it.next();
        st[m.captured(1).toInt() + 1] = m.captured(2).compare("Connected", Qt::CaseInsensitive) == 0;
    }
    return st;
}

// Единый ОПРОС РЕГИСТРАТОРА: имена каналов + какие онлайн/офлайн (за нас это знает
// сам регистратор — не опрашиваем камеры по отдельности).
static QVector<CamRef> fetchDeviceCameras(const Device& d) {
    if (d.proto == "tvt")
        return tvtLapiChannels(d.ip, d.user, d.pass);   // имя + Status

    if (d.proto == "dahua") {
        QString remote = dahuaHttpGet(d.ip, d.user, d.pass,
            "configManager.cgi?action=getConfig&name=RemoteDevice", 9000);
        QVector<CamRef> cams = parseDahuaRemoteDevice(remote);
        QMap<int,bool> states = dahuaCameraStates(d.ip, d.user, d.pass);
        for (auto& c : cams)
            if (states.contains(c.channel)) c.status = states[c.channel] ? 1 : 0;
        return cams;
    }

    // Xiongmai: один опрос регистратора — привязанные камеры + имена + online/offline
    XmClient c; QVector<CamRef> cams;
    if (c.login(d.ip, d.port, d.user, d.pass)) {
        c.fetchTitles();    // OSD-имена каналов
        c.fetchCameras();   // NetWork.RemoteDeviceV3: ConfName + IP
        c.fetchStatus();    // NetWork.ChnStatus: online/offline
        c.logout();
        for (const auto& cam : c.cameras) {
            QString name = cam.name.trimmed();                       // ConfName из RemoteDeviceV3
            if (name.isEmpty() && cam.channel - 1 < c.titles.size())
                name = c.titles[cam.channel - 1].trimmed();          // запасной вариант: OSD-имя
            if (name.isEmpty()) name = QString("Камера %1").arg(cam.channel);
            int status = c.chnStatus.value(cam.channel, -1);         // -1 если регистратор не отдал
            cams.append({ name, cam.channel, cam.ip, status });
        }
    }
    return cams;
}

// TVT/Uniview LAPI: логин + список каналов с РЕАЛЬНЫМИ ИМЕНАМИ (и IP камер).
// Digest realm "NVRDVR"; Qt сам считает digest через authenticationRequired.
// Возвращает камеры только тех каналов, где реально привязано устройство.
static QVector<CamRef> tvtLapiChannels(const QString& ip, const QString& user, const QString& pass) {
    QVector<CamRef> out;
    QNetworkAccessManager nam;
    QObject::connect(&nam, &QNetworkAccessManager::authenticationRequired,
                     [&](QNetworkReply*, QAuthenticator* a){ a->setUser(user); a->setPassword(pass); });
    auto req = [&](const QString& path, bool put) -> QByteArray {
        QNetworkRequest r(QUrl("http://" + ip + path));
        r.setTransferTimeout(6000);
        QNetworkReply* rep = put ? nam.put(r, QByteArray()) : nam.get(r);
        QEventLoop l; QObject::connect(rep, &QNetworkReply::finished, &l, &QEventLoop::quit); l.exec();
        QByteArray b = (rep->error() == QNetworkReply::NoError) ? rep->readAll() : QByteArray();
        rep->deleteLater(); return b;
    };
    // 1) логин (устанавливает сессию), 2) детали каналов
    req("/LAPI/V1.0/System/Security/Login", true);
    QByteArray body = req("/LAPI/V1.0/Channels/System/ChannelDetailInfos", false);
    QJsonArray arr = QJsonDocument::fromJson(body).object()
                        .value("Response").toObject().value("Data").toObject()
                        .value("DetailInfos").toArray();
    for (const auto& v : arr) {
        QJsonObject o = v.toObject();
        QString addr = o.value("AddressInfo").toObject().value("Address").toString();
        if (addr.isEmpty()) continue;         // пустой слот — камеры нет
        CamRef c;
        c.channel = o.value("ID").toInt();
        c.name    = o.value("Name").toString().trimmed();
        c.ip      = addr;
        c.status  = (o.value("Status").toInt() == 1) ? 1 : 0;   // онлайн/офлайн от регистратора
        if (c.name.isEmpty()) c.name = QString("Камера %1").arg(c.channel);
        out.append(c);
    }
    return out;
}

// ONVIF GetProfiles -> список НОМЕРОВ КАНАЛОВ, где реально есть камера.
// У ONVIF-NVR профили создаются только для подключённых каналов; номер канала
// закодирован в SourceToken (00400->4, 01300->13, 00001->1).
static QVector<int> onvifConnectedChannels(const QString& ip, const QString& user,
                                           const QString& pass, int timeoutMs = 8000) {
    QByteArray nonce;
    for (int i = 0; i < 16; ++i) nonce.append(char(QRandomGenerator::global()->bounded(256)));
    QString created = QDateTime::currentDateTimeUtc().toString("yyyy-MM-ddThh:mm:ssZ");
    QByteArray digest = QCryptographicHash::hash(
        nonce + created.toUtf8() + pass.toUtf8(), QCryptographicHash::Sha1);
    QString soap = QString(
        "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
        "<s:Header><Security s:mustUnderstand=\"1\" xmlns=\"http://docs.oasis-open.org/wss/2004/01/"
        "oasis-200401-wss-wssecurity-secext-1.0.xsd\"><UsernameToken><Username>%1</Username>"
        "<Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0"
        "#PasswordDigest\">%2</Password><Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/"
        "oasis-200401-wss-soap-message-security-1.0#Base64Binary\">%3</Nonce>"
        "<Created xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\">"
        "%4</Created></UsernameToken></Security></s:Header>"
        "<s:Body><GetProfiles xmlns=\"http://www.onvif.org/ver10/media/wsdl\"/></s:Body></s:Envelope>")
        .arg(user, QString::fromLatin1(digest.toBase64()),
             QString::fromLatin1(nonce.toBase64()), created);
    QNetworkAccessManager nam;
    QNetworkRequest req(QUrl(QString("http://%1/onvif/media_service").arg(ip)));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/soap+xml");
    req.setTransferTimeout(timeoutMs);
    QNetworkReply* rep = nam.post(req, soap.toUtf8());
    QEventLoop loop;
    QObject::connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    QVector<int> chans;
    if (rep->error() == QNetworkReply::NoError) {
        const QString body = QString::fromUtf8(rep->readAll());
        static const QRegularExpression re("SourceToken>0*(\\d+)<");
        auto it = re.globalMatch(body);
        QSet<int> seen;
        while (it.hasNext()) {
            int n = it.next().captured(1).toInt();
            int ch = (n >= 100) ? n / 100 : n;   // 400->4, 1300->13, 1->1
            if (ch > 0 && !seen.contains(ch)) { seen.insert(ch); chans.append(ch); }
        }
    }
    rep->deleteLater();
    std::sort(chans.begin(), chans.end());
    return chans;
}

// синхронный HTTP GET с digest-авторизацией (CGI регистраторов Dahua)
static QString dahuaHttpGet(const QString& ip, const QString& user, const QString& pass,
                            const QString& pathQuery, int timeoutMs = 5000) {
    QNetworkAccessManager nam;
    QObject::connect(&nam, &QNetworkAccessManager::authenticationRequired,
                     [&](QNetworkReply*, QAuthenticator* a){
                         a->setUser(user); a->setPassword(pass);
                     });
    QNetworkRequest req(QUrl(QString("http://%1/cgi-bin/%2").arg(ip, pathQuery)));
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

// Результат зонда одного IP.
struct ProbeResult { bool found = false; QString proto; int port = 0; QString model, serial, mac, vendor; };

static QString macFromArp(const QString& ip);   // определение ниже (в блоке автопоиска)

// --- credential-free вендорные unicast-запросы (модель/MAC/серийник без логина) ---

// Dahua: unicast UDP 37810 DHDiscover.search -> JSON с DeviceType/Mac/SerialNo
static bool dahuaUdpInfo(const QString& ip, QString& model, QString& mac, QString& serial) {
    QUdpSocket u;
    QByteArray probe = "{\"method\":\"DHDiscover.search\",\"params\":{\"mac\":\"\",\"uni\":1}}";
    u.writeDatagram(probe, QHostAddress(ip), 37810);
    if (!u.waitForReadyRead(1200)) return false;
    QByteArray data;
    while (u.hasPendingDatagrams()) { QNetworkDatagram d = u.receiveDatagram(); data += d.data(); }
    QJsonObject o = QJsonDocument::fromJson(data).object();
    QJsonObject info = o.value("params").toObject().value("deviceInfo").toObject();
    if (info.isEmpty()) info = o.value("params").toObject();
    if (info.isEmpty()) return false;
    model  = info.value("DeviceType").toString();
    mac    = info.value("Mac").toString();
    serial = info.value("SerialNo").toString();
    return !model.isEmpty() || !mac.isEmpty();
}

// ONVIF: unicast WS-Discovery Probe на ip:3702 -> модель/MAC из Scopes (без auth)
static bool onvifUnicastInfo(const QString& ip, QString& model, QString& mac, int& onvifPort) {
    QUdpSocket u;
    QString id = "urn:uuid:" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QByteArray probe = QString(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
        "xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
        "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\">"
        "<s:Header><a:Action s:mustUnderstand=\"1\">"
        "http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</a:Action>"
        "<a:MessageID>%1</a:MessageID>"
        "<a:To s:mustUnderstand=\"1\">urn:schemas-xmlsoap-org:ws:2005:04:discovery</a:To>"
        "</s:Header><s:Body><d:Probe/></s:Body></s:Envelope>").arg(id).toUtf8();
    u.writeDatagram(probe, QHostAddress(ip), 3702);
    if (!u.waitForReadyRead(1500)) return false;
    QString xml;
    while (u.hasPendingDatagrams()) { QNetworkDatagram d = u.receiveDatagram(); xml += QString::fromUtf8(d.data()); }
    if (!xml.contains("onvif", Qt::CaseInsensitive)) return false;
    QRegularExpression reX("<[^>]*XAddrs>([^<]+)<");
    auto mx = reX.match(xml);
    if (mx.hasMatch()) { QUrl uu(mx.captured(1).split(' ').first()); if (uu.port() > 0) onvifPort = uu.port(); }
    QString scopes = QRegularExpression("<[^>]*Scopes>([\\s\\S]*?)<").match(xml).captured(1);
    auto sv = [&](const QString& k){ QRegularExpression re("onvif://[^/]+/" + k + "/([^ \\t\\r\\n<]+)");
        auto m = re.match(scopes); return m.hasMatch() ? QUrl::fromPercentEncoding(m.captured(1).toUtf8()) : QString(); };
    QString name = sv("name").trimmed(), hw = sv("hardware").trimmed();
    if (name == hw || hw.isEmpty())      model = name;   // не дублировать
    else if (name.isEmpty())             model = hw;
    else if (hw.contains(name))          model = hw;
    else if (name.contains(hw))          model = name;
    else                                 model = name + " " + hw;
    mac = sv("MAC");
    return true;
}

// Credential-free зонд одного IP по портам, связанным с камерами (без логина/пароля).
// Подтверждаем реальный стрим-девайс через RTSP OPTIONS (отвечают 200 без авторизации).
// Возвращает {found, proto-подсказка, порт, model=Server-заголовок если есть}.
// Многопроходный credential-free зонд одного IP.
// Проход 1: живой ли хост. Проход 2: камера ли (RTSP OPTIONS). Проход 3: обогащение
// (модель/MAC/протокол) вендорными unicast-запросами БЕЗ логина/пароля.
static ProbeResult probeDeep(const QString& ip) {
    ProbeResult r;
    auto portOpen = [&](quint16 p, int ms){ QTcpSocket t; t.connectToHost(ip, p);
        bool ok = t.waitForConnected(ms); t.abort(); return ok; };

    // Проход 1: быстрая проверка «жив ли хост» — хотя бы один камерный порт
    bool alive = false;
    for (quint16 p : { 554, 80, 37777, 34567, 8000 })
        if (portOpen(p, 400)) { alive = true; break; }
    if (!alive) return r;

    // Проход 2: RTSP OPTIONS на 554 — надёжное подтверждение стрим-устройства (без auth)
    QString serverHdr;
    {
        QTcpSocket s;
        s.connectToHost(ip, 554);
        if (s.waitForConnected(600)) {
            s.write("OPTIONS rtsp://" + ip.toUtf8() + ":554/ RTSP/1.0\r\nCSeq: 1\r\n\r\n");
            s.flush();
            if (s.waitForReadyRead(1200)) {
                QByteArray resp = s.readAll();
                if (resp.startsWith("RTSP/")) {
                    r.found = true; r.port = 554;
                    auto m = QRegularExpression("Server:\\s*([^\\r\\n]+)").match(QString::fromLatin1(resp));
                    if (m.hasMatch()) serverHdr = m.captured(1).trimmed();
                }
            }
            s.abort();
        }
    }

    // Проход 3: обогащение (модель/MAC/протокол) — вендорные unicast БЕЗ учётных данных.
    // Протокол определяем ТОЛЬКО по протокольным признакам (не по «порт открыт» —
    // через VPN/файрвол порты отвечают на всё и дают ложную картину).
    QString model, mac, serial;
    // 3a) Dahua discovery UDP 37810 -> модель/MAC/серийник + вендор Dahua
    if (dahuaUdpInfo(ip, model, mac, serial)) {
        r.found = true; r.proto = "dahua"; r.port = 37777; r.vendor = "Dahua";
        r.model = model; r.mac = mac; r.serial = serial;
    }
    // 3b) ONVIF unicast WS-Discovery -> модель/MAC из scopes
    if (r.model.isEmpty() || r.mac.isEmpty()) {
        QString m2, mc2; int op = 80;
        if (onvifUnicastInfo(ip, m2, mc2, op)) {
            r.found = true;
            if (r.model.isEmpty()) r.model = m2;
            if (r.mac.isEmpty())   r.mac = mc2;
            if (r.proto.isEmpty()) { r.proto = "tvt"; r.vendor = "ONVIF"; }
        }
    }
    // 3c) Xiongmai по RTSP-сигнатуре Server: H264DVR
    if (r.proto.isEmpty() && serverHdr.contains("H264DVR", Qt::CaseInsensitive)) {
        r.proto = "xm"; r.port = 34567; r.vendor = "Xiongmai";
    }
    // модель из RTSP Server, если вендорные методы не дали
    if (r.model.isEmpty() && !serverHdr.isEmpty()) r.model = serverHdr;
    // MAC через ARP, если не пришёл (работает в своей подсети)
    if (r.mac.isEmpty()) { QString m = macFromArp(ip); if (m != "—") r.mac = m; }
    return r;
}

// Определить протокол/порт/модель по IP (XM -> Dahua -> ONVIF).
// discoverOnly=true — быстрый режим для сканирования сегмента (короче таймауты).
static ProbeResult probeOne(const QString& ip, const QString& user, const QString& pass,
                            bool discoverOnly = false) {
    ProbeResult r;
    const int t = discoverOnly ? 1200 : 2500;
    // при массовом скане — быстрый отсев мёртвых адресов:
    // живое устройство слушает хотя бы один из типичных портов
    if (discoverOnly) {
        bool alive = false;
        for (int port : { 80, 34567, 554, 37777 }) {
            QTcpSocket s;
            s.connectToHost(ip, (quint16)port);
            if (s.waitForConnected(600)) { alive = true; s.abort(); break; }
            s.abort();
        }
        if (!alive) return r;
    }
    // 1) Xiongmai DVRIP :34567
    {
        XmClient c;
        if (c.login(ip, 34567, user, pass, t)) {
            c.fetchInfo(); c.logout();
            r = { true, "xm", 34567, c.model, c.serial }; return r;
        }
        if (c.error != QStringLiteral("нет связи") &&
            c.error != QStringLiteral("нет ответа") &&
            c.error != QStringLiteral("обрыв ответа"))
            { r = { true, "xm", 34567, QString(), QString() }; return r; }
    }
    // 2) Dahua CGI :80 — Dahua ТОЛЬКО если getDeviceType вернул "type=..."
    //    (роутеры/прочий HTTP на 200 сюда не попадают — иначе ложные срабатывания)
    {
        QString dt = dahuaHttpGet(ip, user, pass,
            "magicBox.cgi?action=getDeviceType", discoverOnly ? 2000 : 5000);
        if (dt.startsWith("type=")) {
            QString serial = dahuaHttpGet(ip, user, pass,
                "magicBox.cgi?action=getSerialNo", discoverOnly ? 2000 : 5000)
                .section('=', 1).trimmed();
            r = { true, "dahua", 37777, dt.section('=', 1).trimmed(), serial }; return r;
        }
    }
    // 3) ONVIF (TVT и совместимые). ВАЖНО: у TVT управляющий порт = 80 (ONVIF/LAPI),
    //    а 554 — это RTSP (видео). В поле «Порт» должен попадать управляющий 80.
    {
        QString model, serial;
        onvifDeviceInfo(ip, user, pass, model, serial, discoverOnly ? 2000 : 4000);
        if (!model.isEmpty()) { r = { true, "tvt", 80, model, serial }; return r; }
        if (onvifChannelCount(ip, user, pass, discoverOnly ? 2000 : 4000) > 0)
            { r = { true, "tvt", 80, QString(), QString() }; return r; }
    }
    return r;
}

// Собрать Device из результата зонда (тянет камеры с устройства).
static Device buildDeviceFromProbe(const QString& ip, const QString& user, const QString& pass,
                                   const ProbeResult& pr) {
    Device d;
    d.ip = ip; d.user = user; d.pass = pass;
    d.proto = pr.proto; d.port = pr.port; d.model = pr.model; d.serial = pr.serial;
    d.online = true;
    d.name = pr.model.isEmpty() ? ip : pr.model;
    if (pr.proto == "dahua") {
        d.type = "Dahua/NVR";
        QString remote = dahuaHttpGet(ip, user, pass,
            "configManager.cgi?action=getConfig&name=RemoteDevice", 9000);
        d.cams = parseDahuaRemoteDevice(remote);
        d.channels = d.cams.size();
    } else if (pr.proto == "tvt") {
        d.type = "TVT/ONVIF"; d.port = 80; d.rtspPort = 554;   // управление 80, видео 554
        onvifDeviceInfo(ip, user, pass, d.model, d.serial);
        QVector<CamRef> lapi = tvtLapiChannels(ip, user, pass);   // имена + каналы
        if (!lapi.isEmpty()) d.cams = lapi;
        else {
            QVector<int> chans = onvifConnectedChannels(ip, user, pass);
            if (!chans.isEmpty())
                for (int c : chans) d.cams.append({ QString("Камера %1").arg(c), c, QString() });
            else {
                int ch = onvifChannelCount(ip, user, pass);
                for (int i = 1; i <= ch; ++i)
                    d.cams.append({ QString("Камера %1").arg(i), i, QString() });
            }
        }
        d.channels = d.cams.size();
    } else {
        d.type = "XM/HVR";
        XmClient c;
        if (c.login(ip, 34567, user, pass)) {
            c.fetchInfo(); c.fetchCameras(); c.logout();
            d.model = c.model; d.serial = c.serial;
            for (const auto& cam : c.cameras)
                d.cams.append({ QString("Камера %1").arg(cam.channel), cam.channel, cam.ip });
            d.channels = c.channels;
        }
    }
    return d;
}

static const int TOPBAR_H = 46;

static QPixmap iconPix(const QString& name, int px) {
    QString path = QDir(QApplication::applicationDirPath()).filePath("assets/" + name + ".svg");
    return QIcon(path).pixmap(px, px);
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
    resize(1280, 760);
    setStyleSheet(R"(
        QMainWindow, #root { background:#e9edf0; }   /* светло-серый фон: хедер, футер, поля */
        QWidget { color:#2b2f36; font-family:'Segoe UI'; font-size:13px; }
        QStackedWidget { background:#e9edf0; }
        #topbar { background:#e9edf0; }               /* шапка — тот же светло-серый */
        #logo   { color:#2b2f36; font-size:17px; font-weight:700; }
        QPushButton#tab { border:none; background:transparent; padding:0 12px; height:44px;
                          font-size:15px; color:#5a6270; }
        QPushButton#tab:hover { color:#1f6fd6; }
        QPushButton#tab[active="true"] { color:#1f6fd6; font-weight:600; }
        QPushButton#tabx { border:none; border-radius:8px; background:#c4cad3; color:#ffffff;
                           font-size:9px; min-width:16px; max-width:16px; min-height:16px; max-height:16px; padding:0; }
        QPushButton#tabx:hover { background:#e2574c; }
        QPushButton#win, QPushButton#winClose { border:none; background:transparent;
                          min-width:42px; max-width:42px; height:44px; font-size:15px; color:#5a6270; }
        QPushButton#win:hover { background:#dce0e6; }
        QPushButton#winClose:hover { background:#e81123; color:#ffffff; }
        #homePage { background:#e9edf0; }              /* фон страницы серый */
        #homeCard { background:#ffffff; border-radius:8px; }   /* центр — белая карточка */
        #card { background:transparent; border:none; border-radius:6px; }
        #card:hover { background:#f4f6f9; }
        #cardTitle { font-size:19px; font-weight:700; color:#2b2f36; }
        #cardDesc  { font-size:13px; color:#7a8290; }
        #mgmtBar   { background:#e9edf0; }             /* футер — тот же светло-серый */
        #mgmtTitle { color:#2b2f36; font-size:14px; font-weight:700; }
        /* пункт футера: иконка слева, текст справа по ЛЕВОМУ краю (как в оригинале) */
        #mgmt { background:transparent; border:none; border-radius:6px; }
        #mgmt:hover { background:#dfe4ea; }
        #mgmtLbl { font-size:14px; font-weight:600; color:#2b2f36; }
        QCheckBox::indicator { width:16px; height:16px; }
        QPushButton#opbtn { border:none; background:transparent; min-width:22px; max-width:22px; height:22px; }
        QPushButton#opbtn:hover { background:#e4e8ee; border-radius:3px; }
        QTableWidget { background:#ffffff; border:1px solid #e3e7ec; gridline-color:#eef1f4; }
        QHeaderView::section { background:#f4f6f9; border:none; border-right:1px solid #eef1f4;
                               border-bottom:1px solid #e3e7ec; padding:6px; color:#5a6270; }
        #toolbar { background:#f4f6f9; border-bottom:1px solid #e3e7ec; }
        QPushButton#lay { border:1px solid #d3d9e0; background:#ffffff; border-radius:3px;
                          min-width:34px; height:26px; }
        QPushButton#lay:hover { border-color:#1f6fd6; color:#1f6fd6; }
        QPushButton#tool { background:#ffffff; border:1px solid #d9dee4; border-radius:3px; padding:6px 14px; }
        QPushButton#tool:hover { border-color:#1f6fd6; color:#1f6fd6; }
        #addpanel { background:#ffffff; border:1px solid #c6ccd4; border-radius:8px; }
        #addTitle { font-size:14px; font-weight:600; color:#2b2f36; }
        #fieldLbl { color:#5a6270; }
        QLineEdit, QComboBox { border:1px solid #d3d9e0; border-radius:3px; padding:2px 8px;
                               min-height:20px; max-height:22px; background:#ffffff; }
        QLineEdit:focus, QComboBox:focus { border-color:#1f6fd6; }
        QPushButton#primary { background:#1f6fd6; color:#ffffff; border:none; border-radius:3px; padding:7px 18px; }
        QPushButton#primary:hover { background:#1a60ba; }
        QPushButton#ghost { background:#ffffff; border:1px solid #d3d9e0; border-radius:3px; padding:7px 18px; }

        /* --- страница «Просмотр»: левая панель — белый бокс на сером --- */
        #orgpanel { background:#e9edf0; }
        #orgBox   { background:#ffffff; border:1px solid #dfe3e8; border-radius:4px; }
        #orgSearch { border:1px solid #d3d9e0; border-radius:3px; padding:2px 8px; background:#ffffff;
                     min-height:20px; max-height:22px; }
        QTreeWidget { background:#ffffff; border:none; outline:0; font-size:12px; color:#3a414b; }
        QTreeWidget::item { height:24px; }
        QTreeWidget::item:hover { background:#eef3fa; }
        QTreeWidget::item:selected { background:#d5e3f7; color:#1f4e8f; }

        /* --- нижняя панель видеостены: светлая; подсветка — сам значок при hover --- */
        #livebar { background:#e9edf0; border-top:1px solid #dfe3e8; }
        QPushButton#lbtn { border:none; background:transparent; min-width:28px; max-width:28px; height:24px; }
        #pagelbl { color:#5a6270; font-size:12px; min-width:42px; }
        #livebar QComboBox { background:#ffffff; color:#2b2f36; border:1px solid #d3d9e0; border-radius:3px;
                             padding:0px 6px; min-height:18px; max-height:20px; min-width:96px; font-size:12px; }
        #livebar QComboBox QAbstractItemView { background:#ffffff; color:#2b2f36; selection-background-color:#1f6fd6; }

        /* --- страница выбора регистратора: плитки --- */
        #selPage  { background:#e9edf0; }
        #selTitle { font-size:14px; font-weight:700; color:#2b2f36; }
        #devTile  { background:#ffffff; border:1px solid #dfe3e8; border-radius:6px; }
        #devTile:hover { border-color:#1f6fd6; }
        #devTile[off="true"] { background:#f0f2f4; }
        #devTile[off="true"]:hover { border-color:#dfe3e8; }
        #devTileName { font-size:14px; font-weight:600; color:#2b2f36; }
        #devTileIp   { font-size:12px; color:#8a92a0; }
        #devTileOn   { font-size:12px; color:#3ca35a; }
        #devTileOff  { font-size:12px; color:#e2574c; }
        #devTileWait { font-size:12px; color:#8a92a0; }

        /* --- страница «Настройки» --- */
        #setCard    { background:#ffffff; border:1px solid #dfe3e8; border-radius:6px; }
        #setSection { font-size:14px; font-weight:700; color:#2b2f36; }
        #setHint    { font-size:12px; color:#8a92a0; }
    )");

    loadConfig();   // до построения UI: страница настроек читает значения конфига

    // журнал: файл рядом с конфигом, включённость — из настроек
    Journal::inst().setFile(appDataDir() + "/journal.log");
    Journal::inst().setEnabled(cfgLogEnabled_);
    Journal::inst().info(QStringLiteral("Система"), QStringLiteral("Запуск приложения"));

    auto* root = new QWidget; root->setObjectName("root");
    auto* v = new QVBoxLayout(root);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    topBar_ = buildTopBar();
    v->addWidget(topBar_);

    stack_ = new QStackedWidget;
    stack_->addWidget(buildHome());          // 0
    stack_->addWidget(buildDeviceSelect());  // 1 — «Просмотр»: выбор регистратора
    stack_->addWidget(buildDevices());       // 2
    stack_->addWidget(buildSettings());      // 3
    stack_->addWidget(buildJournal());       // 4
    v->addWidget(stack_, 1);
    setCentralWidget(root);

    gotoPage(0);

    rebuildDeviceTable();
    rebuildDeviceTiles();
    startHeartbeat();

    // смена сети (Wi-Fi/кабель/VPN): при появлении связи сразу перепроверить устройства,
    // не дожидаясь 15-секундного heartbeat; потоки переподключаются сами
    if (QNetworkInformation::loadDefaultBackend()) {
        connect(QNetworkInformation::instance(), &QNetworkInformation::reachabilityChanged,
                this, [this](QNetworkInformation::Reachability r){
            if (r == QNetworkInformation::Reachability::Online)
                QTimer::singleShot(1500, this, [this]{ checkAllDevices(); });
        });
    }
}

QWidget* MainWindow::buildTopBar() {
    auto* bar = new QWidget; bar->setObjectName("topbar"); bar->setFixedHeight(TOPBAR_H);
    auto* h = new QHBoxLayout(bar); h->setContentsMargins(14,0,6,0); h->setSpacing(0);

    auto* dot = new QLabel; dot->setPixmap(iconPix("logo", 18));
    auto* logo = new QLabel("SecVMS"); logo->setObjectName("logo");
    h->addWidget(dot); h->addSpacing(8); h->addWidget(logo); h->addSpacing(20);

    // ряд вкладок: «Домой» постоянная, остальные — динамически
    tabsRow_ = new QHBoxLayout; tabsRow_->setSpacing(0); tabsRow_->setContentsMargins(0,0,0,0);
    h->addLayout(tabsRow_);
    {   // постоянная вкладка «Домой»
        auto* box = new QWidget; auto* bl = new QHBoxLayout(box);
        bl->setContentsMargins(0,0,0,0); bl->setSpacing(0);
        auto* t = new QPushButton("Домой"); t->setObjectName("tab"); t->setCursor(Qt::PointingHandCursor);
        connect(t, &QPushButton::clicked, this, [this]{ gotoPage(0); });
        bl->addWidget(t);
        tabsRow_->addWidget(box);
        tabBtn_[0] = t; tabBox_[0] = box;
    }
    h->addStretch();

    auto iconBtn = [&](const QString& name, int sz = 18) {
        auto* b = new QPushButton; b->setObjectName("win");
        b->setIcon(QIcon(iconPix(name, sz))); b->setIconSize(QSize(sz, sz));
        b->setCursor(Qt::PointingHandCursor);
        return b;
    };
    // индикатор производительности (ЦПУ/ОЗУ) — попап по клику, как в оригинале
    auto* perf = iconBtn("tb_perf", 20);
    perf->setToolTip(QStringLiteral("Загрузка ЦПУ и ОЗУ"));
    buildPerfPopup(perf);
    h->addWidget(perf);
    h->addSpacing(8);
    // кнопки окна — из SVG одинакового размера (единый визуальный вес)
    auto winBtn = [&](const QString& icon, bool isClose) {
        auto* b = new QPushButton;
        b->setObjectName(isClose ? "winClose" : "win");
        b->setIcon(QIcon(iconPix(icon, 16))); b->setIconSize(QSize(16, 16));
        b->setCursor(Qt::PointingHandCursor);
        return b;
    };
    auto* mn = winBtn("win_min", false);
    connect(mn,&QPushButton::clicked,this,[this]{ showMinimized(); });
    auto* mx = winBtn("win_max", false);
    connect(mx,&QPushButton::clicked,this,[this]{ toggleMax(); });
    auto* cl = winBtn("win_close", true);
    connect(cl,&QPushButton::clicked,this,[this]{ close(); });
    h->addWidget(mn); h->addWidget(mx); h->addWidget(cl);
    return bar;
}

QWidget* MainWindow::makeBigCard(const QString& icon, const QString& title, const QString& desc,
                                 int page, const QString& tabTitle) {
    auto* card = new QFrame; card->setObjectName("card");
    card->setCursor(Qt::PointingHandCursor); card->setFixedSize(420, 150);
    auto* h = new QHBoxLayout(card); h->setContentsMargins(26,8,26,8); h->setSpacing(20);
    auto* ic = new QLabel; ic->setPixmap(iconPix(icon, 64)); ic->setFixedSize(64,64);
    ic->setAlignment(Qt::AlignCenter);
    auto* tv = new QVBoxLayout; tv->setSpacing(8);   // компактный зазор, как в оригинале
    auto* tl = new QLabel(title); tl->setObjectName("cardTitle");
    auto* dl = new QLabel(desc);  dl->setObjectName("cardDesc"); dl->setWordWrap(true);
    tv->addWidget(tl); tv->addWidget(dl); tv->addStretch();
    h->addWidget(ic, 0, Qt::AlignTop); h->addLayout(tv, 1);
    auto* click = new QPushButton(card); click->setFlat(true);
    click->setStyleSheet("background:transparent;border:none;");
    click->setGeometry(0,0,420,150); click->setCursor(Qt::PointingHandCursor);
    connect(click, &QPushButton::clicked, this, [this,page,tabTitle]{ gotoPage(page, tabTitle); });
    return card;
}

// кликабельный контейнер (QPushButton центрирует многострочный текст — не подходит)
class ClickFrame : public QFrame {
public:
    std::function<void()> onClick;
protected:
    void mousePressEvent(QMouseEvent*) override { if (onClick) onClick(); }
};

QWidget* MainWindow::makeMgmt(const QString& icon, const QString& title, int page, const QString& tabTitle) {
    // как в оригинале: иконка слева, двухстрочный жирный текст справа по левому краю
    auto* w = new ClickFrame;
    w->setObjectName("mgmt");
    w->setCursor(Qt::PointingHandCursor);
    w->setAttribute(Qt::WA_Hover);          // для :hover в QSS
    w->setAttribute(Qt::WA_StyledBackground);
    auto* h = new QHBoxLayout(w); h->setContentsMargins(6, 4, 12, 4); h->setSpacing(10);
    auto* ic = new QLabel; ic->setPixmap(iconPix(icon, 40)); ic->setFixedSize(40, 40);
    auto* lb = new QLabel(title); lb->setObjectName("mgmtLbl");
    lb->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    h->addWidget(ic); h->addWidget(lb);
    w->onClick = [this, page, tabTitle]{ gotoPage(page, tabTitle); };
    return w;
}

QWidget* MainWindow::buildHome() {
    auto* page = new QWidget;
    page->setObjectName("homePage");
    page->setAttribute(Qt::WA_StyledBackground);   // иначе QSS-фон страницы не применится
    auto* v = new QVBoxLayout(page); v->setContentsMargins(0,0,0,0); v->setSpacing(0);

    // центр — белая карточка с отступами по краям на сером фоне
    auto* wrap = new QWidget;
    auto* wl = new QVBoxLayout(wrap); wl->setContentsMargins(18, 6, 18, 12);
    auto* card = new QFrame; card->setObjectName("homeCard");
    card->setAttribute(Qt::WA_StyledBackground);
    // как в оригинале: блоки прижаты к верху, колоннами от левого поля
    auto* tg = new QHBoxLayout(card); tg->setContentsMargins(130, 60, 60, 20); tg->setSpacing(150);
    tg->setAlignment(Qt::AlignTop);
    tg->addWidget(makeBigCard("live", "Просмотр в реальном\nвремени",
        "Просмотр каналов в реальном времени: запись, стоп-кадр и операции PTZ.", 1, "Просмотр"));
    tg->addWidget(makeBigCard("playback", "Воспроизведение",
        "Поиск и воспроизведение видеозаписей каналов в дистанционном режиме, экспорт.", -1, ""));
    tg->addStretch();
    wl->addWidget(card);
    v->addWidget(wrap, 1);

    auto* bottom = new QWidget; bottom->setObjectName("mgmtBar"); bottom->setFixedHeight(170);
    auto* bh = new QHBoxLayout(bottom); bh->setContentsMargins(50,18,50,18);
    // слева: «Управление» + свои разделы
    auto* leftCol = new QVBoxLayout; leftCol->setSpacing(18);
    auto* cap = new QLabel("Управление"); cap->setObjectName("mgmtTitle");
    leftCol->addWidget(cap);
    auto* row = new QHBoxLayout; row->setSpacing(70);
    row->addWidget(makeMgmt("devices", "Диспетчер\nустройств", 2, "Устройства"));
    row->addWidget(makeMgmt("log", "Запрос\nжурнала", 4, "Журнал"));
    row->addWidget(makeMgmt("cog", "Настройки", 3, "Настройки"));
    leftCol->addLayout(row);
    leftCol->addStretch();
    bh->addLayout(leftCol);
    bh->addStretch();
    // справа: свой заголовок над «Руководством», как в оригинале
    auto* rightCol = new QVBoxLayout; rightCol->setSpacing(18);
    auto* cap2 = new QLabel(QStringLiteral("Руководство польз.")); cap2->setObjectName("mgmtTitle");
    rightCol->addWidget(cap2);
    auto* row2 = new QHBoxLayout;
    row2->addWidget(makeMgmt("manual", "Руководство\nпольз.", -1, ""));
    row2->addStretch();
    rightCol->addLayout(row2);
    rightCol->addStretch();
    bh->addLayout(rightCol);
    v->addWidget(bottom);
    return page;
}

// ---- страница «Просмотр»: выбор регистратора плитками ----

QWidget* MainWindow::buildDeviceSelect() {
    auto* page = new QWidget;
    page->setObjectName("selPage");
    page->setAttribute(Qt::WA_StyledBackground);
    auto* v = new QVBoxLayout(page); v->setContentsMargins(24,20,24,20); v->setSpacing(14);
    auto* cap = new QLabel(QStringLiteral("Выберите регистратор"));
    cap->setObjectName("selTitle");
    v->addWidget(cap);
    auto* wrap = new QWidget;
    tilesGrid_ = new QGridLayout(wrap);
    tilesGrid_->setSpacing(14);
    tilesGrid_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    v->addWidget(wrap, 1);
    return page;
}

void MainWindow::rebuildDeviceTiles() {
    if (!tilesGrid_) return;
    while (auto* it = tilesGrid_->takeAt(0)) {
        if (it->widget()) it->widget()->deleteLater();
        delete it;
    }
    int col = 0, row = 0;
    for (const auto& d : devices_) {
        auto* t = new ClickFrame; t->setObjectName("devTile");
        t->setAttribute(Qt::WA_StyledBackground);
        t->setFixedSize(230, 108);
        auto* tv = new QVBoxLayout(t); tv->setContentsMargins(14,10,14,10); tv->setSpacing(4);
        auto* nm = new QLabel(d.name.isEmpty() ? d.ip : d.name); nm->setObjectName("devTileName");
        auto* ipl = new QLabel(d.ip + ":" + QString::number(d.port)); ipl->setObjectName("devTileIp");
        auto* st = new QLabel;
        if (!d.checked)      { st->setText(QStringLiteral("Проверка...")); st->setObjectName("devTileWait"); }
        else if (d.online)   { st->setText(QStringLiteral("● В сети")); st->setObjectName("devTileOn"); }
        else                 { st->setText(QStringLiteral("● Не в сети")); st->setObjectName("devTileOff"); }
        tv->addWidget(nm); tv->addWidget(ipl); tv->addStretch(); tv->addWidget(st);
        const bool clickable = d.checked && d.online;
        if (clickable) {
            t->setCursor(Qt::PointingHandCursor);
            const int id = d.id;
            t->onClick = [this, id]{ openDeviceView(id); };
        } else {
            t->setProperty("off", true);   // не в сети/не проверен — некликабельная
        }
        tilesGrid_->addWidget(t, row, col);
        if (++col >= 4) { col = 0; ++row; }
    }
}

// ---- страница «Журнал» ----

QWidget* MainWindow::buildJournal() {
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page); v->setContentsMargins(12,12,12,12); v->setSpacing(8);

    auto* bar = new QHBoxLayout;
    journalLevel_ = new QComboBox;
    journalLevel_->addItem(QStringLiteral("Все события"), "");
    journalLevel_->addItem(QStringLiteral("Ошибки"),      "error");
    journalLevel_->addItem(QStringLiteral("Предупреждения"), "warn");
    journalLevel_->addItem(QStringLiteral("Информация"),  "info");
    journalLevel_->setFixedWidth(160);
    connect(journalLevel_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this]{ journalRefilter(); });
    bar->addWidget(new QLabel(QStringLiteral("Фильтр:")));
    bar->addWidget(journalLevel_);
    bar->addStretch();
    auto mkTool = [&](const QString& t){ auto* b=new QPushButton(t); b->setObjectName("tool"); return b; };
    auto* exp = mkTool(QStringLiteral("Экспорт..."));
    auto* clr = mkTool(QStringLiteral("Очистить"));
    connect(exp, &QPushButton::clicked, this, [this]{
        QString to = QFileDialog::getSaveFileName(this, QStringLiteral("Экспорт журнала"),
            "journal.csv", "CSV (*.csv)");
        if (to.isEmpty()) return;
        QFile f(to);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        f.write("\xEF\xBB\xBF");   // BOM для Excel
        f.write("Время;Уровень;Источник;Событие\r\n");
        const QString want = journalLevel_->currentData().toString();
        auto esc = [](QString s){ s.replace('"', "\"\""); return "\"" + s + "\""; };
        for (const auto& e : Journal::inst().entries()) {
            if (!want.isEmpty() && want != e.level) continue;
            QString lvl = e.level == "error" ? QStringLiteral("Ошибка")
                        : e.level == "warn"  ? QStringLiteral("Предупреждение")
                                             : QStringLiteral("Информация");
            f.write((esc(e.time.toString("dd.MM.yyyy HH:mm:ss")) + ";" + esc(lvl) + ";" +
                     esc(e.source) + ";" + esc(e.text) + "\r\n").toUtf8());
        }
    });
    connect(clr, &QPushButton::clicked, this, [this]{
        if (QMessageBox::question(this, QStringLiteral("Журнал"),
                QStringLiteral("Очистить журнал?")) != QMessageBox::Yes) return;
        QFile::remove(Journal::inst().filePath());
        journalTable_->setRowCount(0);
    });
    bar->addWidget(exp); bar->addWidget(clr);
    v->addLayout(bar);

    journalTable_ = new QTableWidget(0, 4);
    journalTable_->setHorizontalHeaderLabels(
        {QStringLiteral("Время"), QStringLiteral("Уровень"),
         QStringLiteral("Источник"), QStringLiteral("Событие")});
    journalTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    journalTable_->setColumnWidth(0, 150); journalTable_->setColumnWidth(1, 110);
    journalTable_->setColumnWidth(2, 200);
    journalTable_->verticalHeader()->setVisible(false);
    journalTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    journalTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    v->addWidget(journalTable_, 1);

    for (const auto& e : Journal::inst().entries()) journalAddRow(e);
    connect(&Journal::inst(), &Journal::appended, this, [this](const LogEntry& e){
        int filt = journalLevel_ ? journalLevel_->currentIndex() : 0;
        QString want = filt <= 0 ? QString() : journalLevel_->currentData().toString();
        if (want.isEmpty() || want == e.level) journalAddRow(e);
    });
    return page;
}

void MainWindow::journalAddRow(const LogEntry& e) {
    if (!journalTable_) return;
    int r = 0; journalTable_->insertRow(r);   // новые сверху
    journalTable_->setItem(r, 0, new QTableWidgetItem(e.time.toString("dd.MM.yyyy HH:mm:ss")));
    QString lvl = e.level == "error" ? QStringLiteral("Ошибка")
                : e.level == "warn"  ? QStringLiteral("Предупр.")
                                     : QStringLiteral("Инфо");
    auto* li = new QTableWidgetItem(lvl);
    li->setForeground(e.level == "error" ? QColor("#e2574c")
                    : e.level == "warn"  ? QColor("#d08a1a") : QColor("#5a6270"));
    journalTable_->setItem(r, 1, li);
    journalTable_->setItem(r, 2, new QTableWidgetItem(e.source));
    journalTable_->setItem(r, 3, new QTableWidgetItem(e.text));
}

void MainWindow::journalRefilter() {
    if (!journalTable_) return;
    journalTable_->setRowCount(0);
    QString want = journalLevel_->currentData().toString();
    for (const auto& e : Journal::inst().entries())
        if (want.isEmpty() || want == e.level) journalAddRow(e);
}

// ---- страница «Настройки»: карточки-группы, 2 колонки, со скроллом ----

QWidget* MainWindow::buildSettings() {
    auto* page = new QWidget;
    page->setObjectName("selPage");
    page->setAttribute(Qt::WA_StyledBackground);
    auto* outer = new QVBoxLayout(page); outer->setContentsMargins(0,0,0,0);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea, QScrollArea > QWidget > QWidget { background:transparent; }");

    auto* wrap = new QWidget;
    auto* grid = new QGridLayout(wrap);
    grid->setContentsMargins(24, 20, 24, 20);
    grid->setSpacing(14);
    grid->setAlignment(Qt::AlignTop | Qt::AlignLeft);

    int col = 0, row = 0;
    // карточка-группа; возвращает layout для наполнения
    auto card = [&](const QString& title) {
        auto* c = new QFrame; c->setObjectName("setCard");
        c->setAttribute(Qt::WA_StyledBackground);
        c->setFixedWidth(430);
        auto* v = new QVBoxLayout(c); v->setContentsMargins(20,14,20,16); v->setSpacing(9);
        auto* l = new QLabel(title); l->setObjectName("setSection");
        v->addWidget(l);
        grid->addWidget(c, row, col, Qt::AlignTop);
        if (++col >= 2) { col = 0; ++row; }
        return v;
    };
    // пометка для нереализованных контролов: серые, значения только отображаются
    auto soon = [&](QWidget* w) {
        w->setEnabled(false);
        w->setToolTip(QStringLiteral("В разработке"));
        return w;
    };

    // ===== Видео =====
    {
        auto* v = card(QStringLiteral("Видео"));
        auto* hw = new QCheckBox(QStringLiteral("Аппаратное декодирование (GPU) для всех камер"));
        hw->setChecked(cfgHwDecode_);
        connect(hw, &QCheckBox::toggled, this, [this](bool on){
            cfgHwDecode_ = on; saveConfig();
            for (auto* vv : liveViews_) vv->setHwDecode(on);
        });
        v->addWidget(hw);
        {
            auto* rowl = new QHBoxLayout;
            rowl->addWidget(new QLabel(QStringLiteral("Сетка при открытии просмотра:")));
            auto* cb = new QComboBox;
            cb->addItem(QStringLiteral("1×1"), 1);  cb->addItem(QStringLiteral("2×2"), 4);
            cb->addItem(QStringLiteral("3×3"), 9);  cb->addItem(QStringLiteral("4×4"), 16);
            cb->setCurrentIndex(cb->findData(cfgDefaultLayout_));
            connect(cb, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, cb](int){
                cfgDefaultLayout_ = cb->currentData().toInt(); saveConfig();
            });
            rowl->addWidget(cb); rowl->addStretch();
            v->addLayout(rowl);
        }
        {
            auto* rowl = new QHBoxLayout;
            rowl->addWidget(soon(new QLabel(QStringLiteral("Масштаб по умолчанию:"))));
            auto* cb = new QComboBox;
            cb->addItem(QStringLiteral("Оригинал"));
            cb->addItem(QStringLiteral("Полноэкранный режим"));
            cb->setCurrentIndex(cfgDefaultStretch_ ? 1 : 0);
            soon(cb);
            rowl->addWidget(cb); rowl->addStretch();
            v->addLayout(rowl);
        }
        {
            auto* rowl = new QHBoxLayout;
            rowl->addWidget(soon(new QLabel(QStringLiteral("Буфер сглаживания:"))));
            auto* sp = new QSpinBox; sp->setRange(100, 2000); sp->setSingleStep(100);
            sp->setSuffix(QStringLiteral(" мс")); sp->setValue(cfgBufferMs_);
            soon(sp);
            rowl->addWidget(sp); rowl->addStretch();
            v->addLayout(rowl);
        }
        auto* tt = new QCheckBox(QStringLiteral("Показывать подписи камер в ячейках"));
        tt->setChecked(cfgShowTitles_);
        soon(tt);
        v->addWidget(tt);
    }

    // ===== Автозапуск и режим поста =====
    {
        auto* v = card(QStringLiteral("Автозапуск и режим поста"));
        const QString runKey =
            "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
        auto* au = new QCheckBox(QStringLiteral("Запускать вместе с Windows"));
        au->setChecked(!QSettings(runKey, QSettings::NativeFormat)
                            .value("SecVMS").toString().isEmpty());
        connect(au, &QCheckBox::toggled, this, [runKey](bool on){
            QSettings rs(runKey, QSettings::NativeFormat);
            if (on) rs.setValue("SecVMS",
                QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));
            else    rs.remove("SecVMS");
        });
        v->addWidget(au);
        auto* rest = new QCheckBox(QStringLiteral("Восстанавливать сессию при запуске"));
        rest->setChecked(cfgRestoreSession_);
        soon(rest);
        v->addWidget(rest);
        {
            auto* rowl = new QHBoxLayout;
            rowl->addWidget(soon(new QLabel(QStringLiteral("Сразу открывать просмотр:"))));
            auto* cb = new QComboBox;
            cb->addItem(QStringLiteral("Нет"), -1);
            for (const auto& d : devices_)
                cb->addItem(d.name.isEmpty() ? d.ip : d.name, d.id);
            int i = cb->findData(cfgAutoOpenDev_);
            cb->setCurrentIndex(i < 0 ? 0 : i);
            soon(cb);
            rowl->addWidget(cb); rowl->addStretch();
            v->addLayout(rowl);
        }
        auto* fs = new QCheckBox(QStringLiteral("Полноэкранный режим при запуске"));
        fs->setChecked(cfgStartFullscreen_);
        soon(fs);
        v->addWidget(fs);
        auto* hc = new QCheckBox(QStringLiteral("Скрывать курсор при бездействии"));
        hc->setChecked(cfgHideCursor_);
        soon(hc);
        v->addWidget(hc);
        {
            auto* rowl = new QHBoxLayout;
            rowl->addWidget(soon(new QLabel(QStringLiteral("Пароль на настройки и устройства:"))));
            auto* pw = new QLineEdit; pw->setEchoMode(QLineEdit::Password);
            pw->setPlaceholderText(cfgAdminPass_.isEmpty()
                ? QStringLiteral("не задан") : QStringLiteral("задан"));
            pw->setFixedWidth(140);
            soon(pw);
            rowl->addWidget(pw); rowl->addStretch();
            v->addLayout(rowl);
        }
    }

    // ===== Подключение =====
    {
        auto* v = card(QStringLiteral("Подключение"));
        {
            auto* rowl = new QHBoxLayout;
            rowl->addWidget(new QLabel(QStringLiteral("Проверка устройств каждые:")));
            auto* sp = new QSpinBox; sp->setRange(5, 300); sp->setSuffix(QStringLiteral(" с"));
            sp->setValue(cfgHeartbeatSec_);
            connect(sp, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int sec){
                cfgHeartbeatSec_ = sec; saveConfig();
                if (heartbeat_) heartbeat_->setInterval(sec * 1000);
            });
            rowl->addWidget(sp); rowl->addStretch();
            v->addLayout(rowl);
        }
        {
            auto* rowl = new QHBoxLayout;
            rowl->addWidget(soon(new QLabel(QStringLiteral("Таймаут подключения к потоку:"))));
            auto* sp = new QSpinBox; sp->setRange(3, 30); sp->setSuffix(QStringLiteral(" с"));
            sp->setValue(cfgConnTimeoutSec_);
            soon(sp);
            rowl->addWidget(sp); rowl->addStretch();
            v->addLayout(rowl);
        }
    }

    // ===== Снимки и звук =====
    {
        auto* v = card(QStringLiteral("Снимки и звук"));
        {
            auto* rowl = new QHBoxLayout;
            rowl->addWidget(soon(new QLabel(QStringLiteral("Папка снимков и записей:"))));
            auto* ed = new QLineEdit(cfgSnapshotDir_);
            ed->setPlaceholderText(QStringLiteral("не задана"));
            auto* br = new QPushButton(QStringLiteral("Обзор...")); br->setObjectName("tool");
            soon(ed); soon(br);
            rowl->addWidget(ed, 1);
            rowl->addWidget(br);
            v->addLayout(rowl);
        }
        auto* snd = new QCheckBox(QStringLiteral("Звук на развёрнутой камере"));
        snd->setChecked(cfgAudioEnabled_);
        soon(snd);
        v->addWidget(snd);
        {
            auto* rowl = new QHBoxLayout;
            rowl->addWidget(soon(new QLabel(QStringLiteral("Громкость:"))));
            auto* sl = new QSlider(Qt::Horizontal);
            sl->setRange(0, 100); sl->setValue(cfgAudioVolume_); sl->setFixedWidth(160);
            soon(sl);
            rowl->addWidget(sl); rowl->addStretch();
            v->addLayout(rowl);
        }
    }

    // ===== Диагностика =====
    {
        auto* v = card(QStringLiteral("Диагностика"));
        auto* lg = new QCheckBox(QStringLiteral("Вести журнал работы в файл"));
        lg->setChecked(cfgLogEnabled_);
        connect(lg, &QCheckBox::toggled, this, [this](bool on){
            cfgLogEnabled_ = on; saveConfig();
            Journal::inst().setEnabled(on);
        });
        v->addWidget(lg);
        auto* rowl = new QHBoxLayout;
        auto* op = new QPushButton(QStringLiteral("Открыть файл журнала")); op->setObjectName("tool");
        connect(op, &QPushButton::clicked, this, []{
            QDesktopServices::openUrl(QUrl::fromLocalFile(Journal::inst().filePath()));
        });
        rowl->addWidget(op); rowl->addStretch();
        v->addLayout(rowl);
    }

    // ===== Конфигурация =====
    {
        auto* v = card(QStringLiteral("Конфигурация"));
        auto* path = new QLabel(QDir::toNativeSeparators(configFile()));
        path->setObjectName("setHint");
        path->setTextInteractionFlags(Qt::TextSelectableByMouse);
        v->addWidget(path);
        auto* rowl = new QHBoxLayout;
        auto mkBtn = [&](const QString& t){
            auto* b = new QPushButton(t); b->setObjectName("tool"); return b; };
        auto* open = mkBtn(QStringLiteral("Открыть папку"));
        auto* exp  = mkBtn(QStringLiteral("Экспорт..."));
        auto* imp  = mkBtn(QStringLiteral("Импорт..."));
        connect(open, &QPushButton::clicked, this, []{
            QDesktopServices::openUrl(QUrl::fromLocalFile(appDataDir()));
        });
        connect(exp, &QPushButton::clicked, this, [this]{
            QString to = QFileDialog::getSaveFileName(this,
                QStringLiteral("Экспорт конфигурации"), "secvms-config.json",
                "JSON (*.json)");
            if (!to.isEmpty()) { QFile::remove(to); QFile::copy(configFile(), to); }
        });
        connect(imp, &QPushButton::clicked, this, [this]{
            QString from = QFileDialog::getOpenFileName(this,
                QStringLiteral("Импорт конфигурации"), QString(), "JSON (*.json)");
            if (from.isEmpty()) return;
            QFile f(from);
            if (!f.open(QIODevice::ReadOnly) ||
                !QJsonDocument::fromJson(f.readAll()).object().contains("devices")) {
                QMessageBox::warning(this, QStringLiteral("Импорт"),
                    QStringLiteral("Файл не похож на конфигурацию SecVMS."));
                return;
            }
            f.close();
            if (QMessageBox::question(this, QStringLiteral("Импорт"),
                    QStringLiteral("Заменить текущую конфигурацию файлом\n%1?").arg(from))
                != QMessageBox::Yes) return;
            QFile::remove(configFile());
            QFile::copy(from, configFile());
            loadConfig(); rebuildDeviceTable(); rebuildDeviceTiles();
            QMessageBox::information(this, QStringLiteral("Импорт"),
                QStringLiteral("Готово. Открытые вкладки просмотра лучше переоткрыть."));
        });
        rowl->addWidget(open); rowl->addWidget(exp); rowl->addWidget(imp); rowl->addStretch();
        v->addLayout(rowl);
    }

    // ===== О программе =====
    {
        auto* v = card(QStringLiteral("О программе"));
        auto* about = new QLabel(QStringLiteral("SecVMS 1.0 — сборка от %1. Qt %2, FFmpeg.")
                                     .arg(QStringLiteral(__DATE__), QStringLiteral(QT_VERSION_STR)));
        about->setObjectName("setHint");
        v->addWidget(about);
        auto* hint = new QLabel(QStringLiteral(
            "Серые параметры — в разработке: значения уже хранятся в конфигурации."));
        hint->setObjectName("setHint"); hint->setWordWrap(true);
        v->addWidget(hint);
    }

    scroll->setWidget(wrap);
    outer->addWidget(scroll);
    return page;
}

// MAC устройства из ARP-кэша Windows (best-effort, после TCP-контакта)
static QString macFromArp(const QString& ip) {
#ifdef _WIN32
    ULONG mac[2] = {0, 0}; ULONG len = 6;
    quint32 dst = QHostAddress(ip).toIPv4Address();
    if (SendARP(qToBigEndian(dst), 0, mac, &len) == NO_ERROR && len == 6) {
        auto* b = reinterpret_cast<unsigned char*>(mac);
        return QString("%1:%2:%3:%4:%5:%6")
            .arg(b[0],2,16,QChar('0')).arg(b[1],2,16,QChar('0')).arg(b[2],2,16,QChar('0'))
            .arg(b[3],2,16,QChar('0')).arg(b[4],2,16,QChar('0')).arg(b[5],2,16,QChar('0')).toUpper();
    }
#endif
    Q_UNUSED(ip); return QStringLiteral("—");
}

void MainWindow::openAutoSearch() {
    auto* dlg = new QDialog(this);
    dlg->setObjectName("addpanel");
    dlg->setModal(true);
    dlg->setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    dlg->setAttribute(Qt::WA_StyledBackground);
    dlg->setFixedSize(760, 470);
    auto* v = new QVBoxLayout(dlg); v->setContentsMargins(18,14,18,14); v->setSpacing(12);

    auto* hd = new QHBoxLayout;
    auto* ttl = new QLabel(QStringLiteral("Автопоиск")); ttl->setObjectName("addTitle");
    auto* x = new QPushButton(QString::fromUtf8("\xE2\x9C\x95")); x->setObjectName("tabx");
    connect(x, &QPushButton::clicked, dlg, &QDialog::reject);
    hd->addWidget(ttl); hd->addStretch(); hd->addWidget(x);
    v->addLayout(hd);

    // подсеть по умолчанию — из первого не-loopback IPv4
    QString base3 = "192.168.1";
    for (const auto& a2 : QNetworkInterface::allAddresses())
        if (a2.protocol() == QAbstractSocket::IPv4Protocol && !a2.isLoopback()) {
            const QStringList o = a2.toString().split('.');
            if (o.size() == 4) { base3 = o[0]+"."+o[1]+"."+o[2]; break; }
        }
    auto* seg = new QHBoxLayout; seg->setSpacing(6);
    seg->addWidget(new QLabel(QStringLiteral("Сегмент:")));
    auto* fromE = new QLineEdit(base3 + ".1");   fromE->setFixedWidth(120);
    auto* toE   = new QLineEdit(base3 + ".254");  toE->setFixedWidth(120);
    auto* find  = new QPushButton(QStringLiteral("Поиск")); find->setObjectName("primary");
    seg->addWidget(fromE); seg->addWidget(new QLabel(QString::fromUtf8("\xE2\x80\x94"))); seg->addWidget(toE);
    seg->addStretch();
    seg->addWidget(find);
    v->addLayout(seg);

    auto* table = new QTableWidget(0, 6);
    table->setHorizontalHeaderLabels({"", "IP", "Модель устройства", "MAC-адрес", "Порт", "Протокол"});
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table->setColumnWidth(0, 32); table->setColumnWidth(1, 120);
    table->setColumnWidth(3, 150); table->setColumnWidth(4, 60);
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    v->addWidget(table, 1);

    auto* status = new QLabel; status->setObjectName("fieldLbl");
    auto* btns = new QHBoxLayout;
    btns->addWidget(status); btns->addStretch();
    auto* add = new QPushButton(QStringLiteral("Добавить выбранные")); add->setObjectName("primary");
    auto* cancel = new QPushButton(QStringLiteral("Отмена")); cancel->setObjectName("ghost");
    connect(cancel, &QPushButton::clicked, dlg, &QDialog::reject);
    btns->addWidget(cancel); btns->addWidget(add);
    v->addLayout(btns);

    auto knownIps = QSharedPointer<QSet<QString>>::create();
    for (const auto& d : devices_) knownIps->insert(d.ip);
    auto rowByIp = QSharedPointer<QMap<QString,int>>::create();

    // добавить/обновить строку устройства в таблице (по IP, без дублей)
    auto upsert = [table, knownIps, rowByIp](const QString& ip, const QString& model,
                                             const QString& mac, int port, const QString& proto){
        int r;
        if (rowByIp->contains(ip)) {
            r = rowByIp->value(ip);
            auto fill = [&](int c, const QString& s){
                if (!s.isEmpty() && (!table->item(r,c) || table->item(r,c)->text().isEmpty()
                    || table->item(r,c)->text() == "—"))
                    table->item(r,c)->setText(s);
            };
            fill(2, model); fill(3, mac);
            if (port) table->item(r,4)->setText(QString::number(port));
            if (!proto.isEmpty()) table->item(r,5)->setText(
                proto=="xm"?"Xiongmai":proto=="dahua"?"Dahua":"ONVIF/TVT");
            return;
        }
        r = table->rowCount(); table->insertRow(r); (*rowByIp)[ip] = r;
        auto* wrap = new QWidget; auto* wl = new QHBoxLayout(wrap);
        wl->setContentsMargins(0,0,0,0); wl->setAlignment(Qt::AlignCenter);
        auto* cb = new QCheckBox; wl->addWidget(cb);
        table->setCellWidget(r, 0, wrap);
        auto set = [&](int c, const QString& t){ table->setItem(r, c, new QTableWidgetItem(t.isEmpty()?"—":t)); };
        set(1, ip); set(2, model); set(3, mac);
        set(4, port ? QString::number(port) : QString());
        set(5, proto=="xm"?"Xiongmai":proto=="dahua"?"Dahua":proto=="tvt"?"ONVIF/TVT":QString());
        if (knownIps->contains(ip)) {
            cb->setEnabled(false);
            for (int c = 1; c <= 5; ++c) table->item(r, c)->setForeground(QColor("#a0a6ad"));
            table->item(r, 2)->setText((model.isEmpty()?QStringLiteral(""):model)
                                       + QStringLiteral("  (уже добавлен)"));
        }
    };

    auto disco = QSharedPointer<Discovery>(new Discovery, &QObject::deleteLater);
    auto* poolW = new QFutureWatcher<ProbeResult>(dlg);
    auto ips = QSharedPointer<QStringList>::create();
    auto* pool = new QThreadPool(dlg); pool->setMaxThreadCount(64);
    auto running = QSharedPointer<int>::create(0);   // сколько источников ещё активно

    auto finishIfDone = [status, find, table, running]{
        if (*running == 0) {
            status->setText(QStringLiteral("Готово. Найдено устройств: %1").arg(table->rowCount()));
            find->setEnabled(true);
        }
    };

    // 1) WS-Discovery + Dahua broadcast (без учётных данных)
    connect(disco.data(), &Discovery::deviceFound, dlg, [upsert](const Found& f){
        upsert(f.ip, f.model, f.mac.isEmpty()?QString():f.mac, f.port, f.proto);
    });
    connect(disco.data(), &Discovery::finished, dlg, [running, finishIfDone]{
        if (*running > 0) --*running; finishIfDone();
    });

    // 2) Unicast-скан диапазона по портам камер (RTSP OPTIONS, без учётных данных)
    connect(poolW, &QFutureWatcher<ProbeResult>::resultReadyAt, dlg,
            [poolW, ips, upsert](int i){
        const ProbeResult pr = poolW->resultAt(i);
        if (pr.found && i < ips->size())
            upsert(ips->at(i), pr.model, pr.mac, pr.port, pr.proto);
    });
    connect(poolW, &QFutureWatcher<ProbeResult>::progressValueChanged, dlg,
            [status, ips](int done){
        status->setText(QStringLiteral("Опрос сети: %1 из %2...").arg(done).arg(ips->size()));
    });
    connect(poolW, &QFutureWatcher<ProbeResult>::finished, dlg, [running, finishIfDone]{
        if (*running > 0) --*running; finishIfDone();
    });

    connect(find, &QPushButton::clicked, dlg, [=]() mutable {
        if (poolW->isRunning()) return;
        table->setRowCount(0); rowByIp->clear(); ips->clear();
        find->setEnabled(false);
        *running = 2;
        status->setText(QStringLiteral("Поиск устройств..."));
        disco->start(4000);   // объявления устройств
        // диапазон для unicast-скана
        quint32 lo = QHostAddress(fromE->text().trimmed()).toIPv4Address();
        quint32 hi = QHostAddress(toE->text().trimmed()).toIPv4Address();
        if (lo == 0 || hi == 0 || hi < lo || hi - lo > 4096) { --*running; return; }
        for (quint32 a3 = lo; a3 <= hi; ++a3) *ips << QHostAddress(a3).toString();
        QStringList list = *ips;
        poolW->setFuture(QtConcurrent::mapped(pool, list,
            [](const QString& ip){ return probeDeep(ip); }));
    });

    // добавление: учётные данные спрашиваем ЗДЕСЬ (в поиске их нет)
    connect(add, &QPushButton::clicked, dlg, [=]() mutable {
        QStringList chosen;
        for (int r = 0; r < table->rowCount(); ++r) {
            auto* w = table->cellWidget(r, 0);
            auto* cb = w ? w->findChild<QCheckBox*>() : nullptr;
            if (cb && cb->isEnabled() && cb->isChecked()) chosen << table->item(r, 1)->text();
        }
        if (chosen.isEmpty()) { status->setText(QStringLiteral("Отметьте устройства галочками.")); return; }

        // единый логин/пароль для выбранных (у объекта обычно одинаковые)
        QDialog cred(dlg);
        cred.setWindowTitle(QStringLiteral("Учётные данные"));
        auto* cv = new QFormLayout(&cred);
        auto* u = new QLineEdit("admin");
        auto* pw = new QLineEdit; pw->setEchoMode(QLineEdit::Password);
        cv->addRow(QStringLiteral("Логин:"), u);
        cv->addRow(QStringLiteral("Пароль:"), pw);
        auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        connect(bb, &QDialogButtonBox::accepted, &cred, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &cred, &QDialog::reject);
        cv->addRow(bb);
        if (cred.exec() != QDialog::Accepted) return;
        const QString user = u->text().isEmpty() ? "admin" : u->text(), pass = pw->text();

        add->setEnabled(false);
        status->setText(QStringLiteral("Добавление %1 устр...").arg(chosen.size()));
        auto* w2 = new QFutureWatcher<QVector<Device>>(dlg);
        connect(w2, &QFutureWatcher<QVector<Device>>::finished, this, [this, w2, dlg]{
            for (Device d : w2->result()) {
                bool dup = false;
                for (const auto& e : devices_) if (e.ip == d.ip) { dup = true; break; }
                if (dup) continue;
                d.id = nextId_++; devices_.append(d);
            }
            w2->deleteLater();
            saveConfig(); rebuildDeviceTable(); rebuildDeviceTiles();
            dlg->accept();
        });
        w2->setFuture(QtConcurrent::run([chosen, user, pass]{
            QVector<Device> out;
            for (const QString& ip : chosen) {
                ProbeResult pr = probeOne(ip, user, pass, false);
                if (pr.found) out << buildDeviceFromProbe(ip, user, pass, pr);
            }
            return out;
        }));
    });

    connect(dlg, &QDialog::finished, dlg, [poolW, pool, disco]{
        poolW->cancel(); pool->clear(); disco->stop();
    });
    dlg->move(geometry().center() - QPoint(dlg->width()/2, dlg->height()/2));
    dlg->show();
}

void MainWindow::openDeviceView(int devId) {
    const Device* dp = nullptr;
    for (const auto& d : devices_) if (d.id == devId) { dp = &d; break; }
    if (!dp) return;
    const QString tabTitle = QStringLiteral("Просмотр %1")
        .arg(dp->name.isEmpty() ? dp->ip : dp->name);

    // Любой вход в просмотр регистратора = свежий опрос (имена + состояние каналов):
    // и клик по плитке, и клик по уже открытой вкладке (при уходе с вкладки видео
    // глушится, поэтому возврат = повторный вход). Само окно просмотра
    // переиспользуется (не плодим вкладки) — это делает обработчик finished
    // ниже через updateDevice(): если данные не изменились, пересборки нет.

    // окно загрузки: опрашиваем САМ регистратор об именах и состоянии каналов
    auto* load = new QDialog(this);
    load->setObjectName("addpanel");
    load->setModal(true);
    load->setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    load->setAttribute(Qt::WA_StyledBackground);
    load->setFixedSize(320, 120);
    auto* lv = new QVBoxLayout(load); lv->setContentsMargins(24,20,24,20);
    auto* t = new QLabel(QStringLiteral("Опрос регистратора «%1»...\nПолучение списка и состояния камер")
                             .arg(dp->name.isEmpty() ? dp->ip : dp->name));
    t->setAlignment(Qt::AlignCenter); t->setWordWrap(true);
    lv->addStretch(); lv->addWidget(t); lv->addStretch();
    load->move(geometry().center() - QPoint(160, 60));
    load->show();

    Device dcopy = *dp;
    auto* w = new QFutureWatcher<QVector<CamRef>>(this);
    connect(w, &QFutureWatcher<QVector<CamRef>>::finished, this,
            [this, w, devId, tabTitle, load]{
        QVector<CamRef> cams = w->result();
        w->deleteLater();
        load->close(); load->deleteLater();

        // применить свежие данные регистратора (имена + статус)
        Device* d = nullptr;
        for (auto& e : devices_) if (e.id == devId) { d = &e; break; }
        if (!d) return;
        if (!cams.isEmpty()) { d->cams = cams; saveConfig(); }

        if (liveViews_.contains(devId)) {                 // вьюха была закрыта — обновить и открыть
            liveViews_[devId]->updateDevice(*d);
            gotoPage(livePage_[devId], tabTitle);
            return;
        }
        auto* view = new LiveView(*d, cfgHwDecode_, cfgLayouts_, cfgDefaultLayout_);
        connect(view, &LiveView::fullscreenToggled, this, [this](bool on){
            if (topBar_) topBar_->setVisible(!on);
            on ? showFullScreen() : showNormal();
        });
        connect(view, &LiveView::camerasUpdated, this, [this](int id, QVector<CamRef> c){
            for (auto& dv : devices_) if (dv.id == id) { dv.cams = c; break; }
            saveConfig();
        });
        connect(view, &LiveView::layoutAdded, this, [this](const QString& key){
            if (!cfgLayouts_.contains(key)) { cfgLayouts_ << key; saveConfig(); }
            const QStringList p = key.split('x');
            if (p.size() == 2)
                for (auto* v : liveViews_) v->addCustomLayoutButton(p[0].toInt(), p[1].toInt());
        });
        int page = stack_->addWidget(view);
        liveViews_[devId] = view;
        livePage_[devId]  = page;
        gotoPage(page, tabTitle);
    });
    w->setFuture(QtConcurrent::run([dcopy]{ return fetchDeviceCameras(dcopy); }));
}

QWidget* MainWindow::buildDevices() {
    auto* page = new QWidget;
    auto* hmain = new QHBoxLayout(page); hmain->setContentsMargins(0,0,0,0); hmain->setSpacing(0);

    auto* left = new QWidget;
    auto* lv = new QVBoxLayout(left); lv->setContentsMargins(12,12,12,12); lv->setSpacing(8);
    auto* bar = new QHBoxLayout;
    auto mkTool = [&](const QString& t){ auto* b=new QPushButton(t); b->setObjectName("tool"); return b; };
    auto* bAuto = mkTool("Автопоиск");
    auto* bAdd  = mkTool("Добавить");
    auto* bDel  = mkTool("Удалить");
    auto* bImp  = mkTool("Импорт");
    auto* bExp  = mkTool("Экспорт");
    for (auto* b : {bAuto,bAdd,bDel,bImp,bExp}) bar->addWidget(b);
    bar->addStretch();
    lv->addLayout(bar);
    connect(bAuto, &QPushButton::clicked, this, [this]{ openAutoSearch(); });
    connect(bAdd, &QPushButton::clicked, this, [this]{ showAddPanel(true); });
    connect(bDel, &QPushButton::clicked, this, [this]{ deleteCheckedDevices(); });

    devTable_ = new QTableWidget(0, 11);
    devTable_->setHorizontalHeaderLabels(
        {"", "№","Имя","IP","Тип","Модель","Порт","Каналы","Статус","Серийный №","Операции"});
    devTable_->horizontalHeader()->setStretchLastSection(false);
    devTable_->horizontalHeader()->setSectionResizeMode(8, QHeaderView::Stretch); // Статус
    devTable_->setColumnWidth(0, 34);
    devTable_->setColumnWidth(9, 160);
    devTable_->setColumnWidth(10, 90);
    devTable_->verticalHeader()->setVisible(false);
    devTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    devTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    lv->addWidget(devTable_);
    hmain->addWidget(left, 1);

    // ---- модальное окно добавления: по центру, поверх всего ----
    addPanel_ = new QDialog(this);
    addPanel_->setObjectName("addpanel");
    addPanel_->setModal(true);
    addPanel_->setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    addPanel_->setAttribute(Qt::WA_StyledBackground);
    addPanel_->setFixedWidth(430);
    auto* pv = new QVBoxLayout(addPanel_); pv->setContentsMargins(18,14,18,14); pv->setSpacing(12);
    auto* hd = new QHBoxLayout;
    auto* ttl = new QLabel("Доб. устройство"); ttl->setObjectName("addTitle");
    auto* x = new QPushButton(QString::fromUtf8("\xE2\x9C\x95")); x->setObjectName("tabx");
    x->setAutoDefault(false); x->setDefault(false);   // не перехватывать Enter
    connect(x, &QPushButton::clicked, this, [this]{ showAddPanel(false); });
    // Esc -> QDialog::reject -> очистка полей
    connect(addPanel_, &QDialog::rejected, this, [this]{ showAddPanel(false); });
    hd->addWidget(ttl); hd->addStretch(); hd->addWidget(x);
    pv->addLayout(hd);

    auto* form = new QGridLayout; form->setVerticalSpacing(4); form->setHorizontalSpacing(14);
    form->setColumnStretch(0,1); form->setColumnStretch(1,1);
    auto lab = [&](const QString& t){ auto* l=new QLabel(t); l->setObjectName("fieldLbl"); return l; };
    // метка обязательного поля: красная звёздочка
    auto labReq = [&](const QString& t){
        auto* l = new QLabel(t + QStringLiteral(" <span style='color:#e2574c'>*</span>"));
        l->setObjectName("fieldLbl"); l->setTextFormat(Qt::RichText); return l;
    };
    fName_ = new QLineEdit; fName_->setPlaceholderText("Имя");
    fIp_   = new QLineEdit; fIp_->setPlaceholderText("IP/домен");
    fPort_ = new QLineEdit; fPort_->setPlaceholderText("Порт");
    fUser_ = new QLineEdit; fUser_->setPlaceholderText("Имя польз-ля");
    fPass_ = new QLineEdit; fPass_->setEchoMode(QLineEdit::Password); fPass_->setPlaceholderText("Пароль");
    // две колонки — поля уже
    form->addWidget(labReq("Имя устройства:"), 0,0); form->addWidget(labReq("Порт:"), 0,1);
    form->addWidget(fName_, 1,0);
    {   // порт + кнопка автоопределения протокола/порта
        auto* ph = new QHBoxLayout; ph->setSpacing(6);
        ph->addWidget(fPort_, 1);
        auto* det = new QPushButton(QStringLiteral("Определить"));
        det->setObjectName("tool");
        det->setAutoDefault(false); det->setDefault(false);   // Enter не должен жать «Определить»
        det->setToolTip(QStringLiteral("Опросить устройство по IP и определить протокол и порт"));
        det->setCursor(Qt::PointingHandCursor);
        ph->addWidget(det);
        form->addLayout(ph, 1, 1);

        detLbl_ = new QLabel; detLbl_->setObjectName("fieldLbl"); detLbl_->setWordWrap(true);
        form->addWidget(detLbl_, 6, 0, 1, 2);

        connect(det, &QPushButton::clicked, this, [this, det]{
            QString ip = fIp_->text().trimmed();
            if (ip.isEmpty()) { detLbl_->setText(QStringLiteral("Сначала укажите IP/домен.")); return; }
            QString user = fUser_->text().isEmpty() ? "admin" : fUser_->text();
            QString pass = fPass_->text();
            det->setEnabled(false);
            detLbl_->setText(QStringLiteral("Опрос устройства..."));
            auto* w = new QFutureWatcher<QStringList>(this);   // {proto, port, model}
            connect(w, &QFutureWatcher<QStringList>::finished, this, [this, w, det]{
                QStringList r = w->result();
                w->deleteLater();
                det->setEnabled(true);
                if (r.size() < 3) {
                    detLbl_->setText(QStringLiteral(
                        "Не удалось определить: устройство не отвечает ни по одному известному протоколу."));
                    return;
                }
                const QString proto = r[0], model = r[2];
                const int port = r[1].toInt();
                fPort_->setText(QString::number(port));
                QString nice = proto == "xm"    ? QStringLiteral("Xiongmai")
                             : proto == "dahua" ? QStringLiteral("Dahua")
                                                : QStringLiteral("ONVIF/TVT");
                detLbl_->setText(model.isEmpty()
                    ? QStringLiteral("Найдено: %1, порт %2").arg(nice).arg(port)
                    : QStringLiteral("Найдено: %1 %2, порт %3").arg(nice, model).arg(port));
                if (fName_->text().isEmpty() && !model.isEmpty()) fName_->setText(model);
                if (!model.isEmpty()) {   // пополнить базу модель -> протокол/порт
                    cfgKnownModels_[model] = { proto, port };
                    saveConfig();
                }
            });
            w->setFuture(QtConcurrent::run([ip, user, pass]() -> QStringList {
                ProbeResult pr = probeOne(ip, user, pass, false);
                if (!pr.found) return {};
                return { pr.proto, QString::number(pr.port), pr.model };
            }));
        });
    }
    form->addWidget(labReq("IP/домен:"), 2,0);     form->addWidget(labReq("Имя пользователя:"), 2,1);
    form->addWidget(fIp_, 3,0);                    form->addWidget(fUser_, 3,1);
    form->addWidget(labReq("Пароль:"), 4,0);
    form->addWidget(fPass_, 5,0);
    pv->addLayout(form);
    pv->addSpacing(10);

    auto* btns = new QHBoxLayout; btns->addStretch();
    auto* cancel = new QPushButton("Отмена"); cancel->setObjectName("ghost");
    cancel->setAutoDefault(false); cancel->setDefault(false);   // Enter не должен жать «Отмена»
    auto* ok = new QPushButton("Добавить"); ok->setObjectName("primary");
    ok->setDefault(true); ok->setAutoDefault(true);   // Enter -> Добавить (единственная default-кнопка)
    connect(cancel, &QPushButton::clicked, this, [this]{ showAddPanel(false); });
    // Enter в любом поле -> submit; Esc -> закрыть (QDialog::reject)
    for (QLineEdit* f : { fName_, fIp_, fPort_, fUser_, fPass_ })
        connect(f, &QLineEdit::returnPressed, ok, [ok]{ if (ok->isEnabled()) ok->click(); });
    connect(ok, &QPushButton::clicked, this, [this, ok]{
        // ВСЕ поля обязательны — подсказываем первое незаполненное
        auto req = [this](QLineEdit* f, const QString& what) -> bool {
            if (f->text().trimmed().isEmpty()) {
                if (detLbl_) detLbl_->setText(QStringLiteral("Заполните поле: %1").arg(what));
                f->setFocus();
                return false;
            }
            return true;
        };
        if (!req(fName_, QStringLiteral("Имя устройства")))    return;
        if (!req(fIp_,   QStringLiteral("IP/домен")))          return;
        if (!req(fPort_, QStringLiteral("Порт")))              return;
        if (!req(fUser_, QStringLiteral("Имя пользователя")))  return;
        if (!req(fPass_, QStringLiteral("Пароль")))            return;
        QString ip = fIp_->text().trimmed();
        int port = fPort_->text().trimmed().toInt();
        if (port <= 0) {
            if (detLbl_) detLbl_->setText(QStringLiteral("Порт должен быть числом."));
            fPort_->setFocus(); return;
        }
        QString user = fUser_->text();
        QString pass = fPass_->text();
        QString name = fName_->text();

        // опрос устройства — в фоне: интерфейс не виснет на таймаутах офлайн-устройств
        ok->setEnabled(false);
        ok->setText(QStringLiteral("Проверка..."));
        auto* w = new QFutureWatcher<Device>(this);
        connect(w, &QFutureWatcher<Device>::finished, this, [this, w, ok]{
            Device d = w->result();
            w->deleteLater();
            ok->setEnabled(true);
            ok->setText(QStringLiteral("Добавить"));
            if (editingId_ >= 0) {
                for (auto& dv : devices_) if (dv.id == editingId_) {
                    d.id = editingId_;
                    if (d.cams.isEmpty()) d.cams = dv.cams;   // офлайн-правка не теряет камеры
                    dv = d; break;
                }
                editingId_ = -1;
            } else {
                d.id = nextId_++; devices_.append(d);
            }
            saveConfig(); rebuildDeviceTable(); rebuildDeviceTiles();
            if (liveViews_.contains(d.id)) liveViews_[d.id]->updateDevice(d);  // открытая вьюха
            showAddPanel(false);   // очистка полей — внутри showAddPanel(false)
        });
        w->setFuture(QtConcurrent::run([ip, port, user, pass, name]{
            Device d;
            d.name = name; d.ip = ip; d.port = port; d.user = user; d.pass = pass;
            if (port == 37777) {
                // Dahua NVR: инфо через CGI (digest); камеры — только привязанные каналы
                d.proto = "dahua";
                QString devType = dahuaHttpGet(ip, user, pass, "magicBox.cgi?action=getDeviceType");
                QString serial  = dahuaHttpGet(ip, user, pass, "magicBox.cgi?action=getSerialNo");
                QString remote  = dahuaHttpGet(ip, user, pass,
                    "configManager.cgi?action=getConfig&name=RemoteDevice", 9000);
                d.online = !remote.isEmpty();
                if (d.online) {
                    d.type   = "Dahua/NVR";
                    d.model  = devType.section('=', 1).trimmed();
                    d.serial = serial.section('=', 1).trimmed();
                    d.cams   = parseDahuaRemoteDevice(remote);
                    d.channels = d.cams.size();
                }
            } else {
                // Xiongmai DVRIP
                XmClient c;
                bool ok2 = c.login(ip, port, user, pass);
                if (ok2) { c.fetchInfo(); c.fetchCameras(); c.logout(); }
                d.online = ok2;
                if (ok2) {
                    d.proto = "xm"; d.type = "XM/HVR";
                    d.model = c.model; d.serial = c.serial; d.channels = c.channels;
                    for (const auto& cam : c.cameras)
                        d.cams.append({ QString("Камера %1").arg(cam.channel), cam.channel, cam.ip });
                } else {
                    // не Xiongmai — пробуем ONVIF/TVT (RTSP /unicast/cN/s.../live)
                    // TVT/Uniview LAPI — реальные имена + занятые каналы
                    QVector<CamRef> lapi = tvtLapiChannels(ip, user, pass);
                    QVector<int> chans = lapi.isEmpty() ? onvifConnectedChannels(ip, user, pass) : QVector<int>{};
                    int ch = !lapi.isEmpty() ? lapi.size()
                           : (chans.isEmpty() ? onvifChannelCount(ip, user, pass) : chans.size());
                    if (ch > 0) {
                        d.proto = "tvt"; d.type = "TVT/ONVIF";
                        d.port = 80; d.rtspPort = 554;   // управление 80 (ONVIF), видео 554
                        d.online = true; d.channels = ch;
                        onvifDeviceInfo(ip, user, pass, d.model, d.serial);  // паспорт
                        if (!lapi.isEmpty())        d.cams = lapi;                       // имена!
                        else if (!chans.isEmpty())  for (int c : chans) d.cams.append({ QString("Камера %1").arg(c), c, QString() });
                        else                        for (int i = 1; i <= ch; ++i) d.cams.append({ QString("Камера %1").arg(i), i, QString() });
                    }
                }
            }
            return d;
        }));
    });
    btns->addWidget(cancel); btns->addWidget(ok);
    pv->addLayout(btns);

    return page;
}

void MainWindow::addDeviceRow(const Device& d, int num) {
    int r = devTable_->rowCount(); devTable_->insertRow(r);
    auto* wrap = new QWidget;
    auto* wl = new QHBoxLayout(wrap);
    wl->setContentsMargins(0,0,0,0); wl->setAlignment(Qt::AlignCenter);
    wl->addWidget(new QCheckBox);
    devTable_->setCellWidget(r, 0, wrap);
    auto set = [&](int c, const QString& s){
        devTable_->setItem(r, c, new QTableWidgetItem(s.isEmpty() ? "N/A" : s));
    };
    devTable_->setItem(r, 1, new QTableWidgetItem(QString::number(num)));
    set(2, d.name); set(3, d.ip); set(4, d.type); set(5, d.model);
    set(6, QString::number(d.port));
    set(7, d.channels > 0 ? QString::number(d.channels) : QString());
    devTable_->setItem(r, 8, new QTableWidgetItem(
        d.online ? QString::fromUtf8("\xE2\x97\x8F \xD0\x92 \xD1\x81\xD0\xB5\xD1\x82\xD0\xB8")
                 : QString::fromUtf8("\xE2\x97\x8F \xD0\x9D\xD0\xB5 \xD0\xB2 \xD1\x81\xD0\xB5\xD1\x82\xD0\xB8")));
    set(9, d.serial);

    auto* opw = new QWidget;
    auto* ol = new QHBoxLayout(opw);
    ol->setContentsMargins(0,0,0,0); ol->setSpacing(10); ol->setAlignment(Qt::AlignCenter);
    auto opBtn = [&](const QString& ic, const QString& tip){
        auto* b = new QPushButton; b->setObjectName("opbtn");
        b->setIcon(QIcon(iconPix(ic,16))); b->setIconSize(QSize(16,16));
        b->setToolTip(tip); b->setCursor(Qt::PointingHandCursor);
        return b;
    };
    auto* edit = opBtn("edit", "Редактировать");
    auto* del  = opBtn("trash", "Удалить");
    ol->addWidget(edit); ol->addWidget(del);
    devTable_->setCellWidget(r, 10, opw);

    int id = d.id;
    connect(del, &QPushButton::clicked, this, [this, id]{ removeDeviceById(id); });
    connect(edit, &QPushButton::clicked, this, [this, id]{
        for (const auto& dv : devices_) if (dv.id == id) {
            editingId_ = id;
            fName_->setText(dv.name); fIp_->setText(dv.ip);
            fPort_->setText(QString::number(dv.port)); fUser_->setText(dv.user);
            fPass_->setText(dv.pass);
            showAddPanel(true); break;
        }
    });
}

void MainWindow::deleteCheckedDevices() {
    QVector<int> ids;
    for (int i = 0; i < devTable_->rowCount() && i < devices_.size(); ++i) {
        auto* w = devTable_->cellWidget(i, 0);
        auto* cb = w ? w->findChild<QCheckBox*>() : nullptr;
        if (cb && cb->isChecked()) ids.append(devices_[i].id);
    }
    for (int id : ids) removeDeviceById(id);
}

void MainWindow::removeDeviceById(int id) {
    for (int i = 0; i < devices_.size(); ++i)
        if (devices_[i].id == id) { devices_.remove(i); break; }
    // если вьюха этого устройства открыта — остановить потоки и закрыть её вкладку
    if (liveViews_.contains(id)) {
        liveViews_[id]->setActive(false);
        int page = livePage_.value(id, -1);
        if (page >= 0 && tabBox_.contains(page)) closeTab(page);
    }
    saveConfig(); rebuildDeviceTable(); rebuildDeviceTiles();
}

void MainWindow::rebuildDeviceTable() {
    if (!devTable_) return;
    devTable_->setRowCount(0);
    for (int i = 0; i < devices_.size(); ++i) addDeviceRow(devices_[i], i + 1);
}

// ---- единый config.json: settings + devices (с камерами) ----

void MainWindow::saveConfig() {
    QJsonObject root;
    QJsonObject st;
    st["hwdecode"] = cfgHwDecode_;
    st["layouts"]  = QJsonArray::fromStringList(cfgLayouts_);
    st["defaultLayout"] = cfgDefaultLayout_;
    st["heartbeatSec"]  = cfgHeartbeatSec_;
    // зарезервированные (функции в разработке)
    st["defaultStretch"]  = cfgDefaultStretch_;
    st["bufferMs"]        = cfgBufferMs_;
    st["showTitles"]      = cfgShowTitles_;
    st["restoreSession"]  = cfgRestoreSession_;
    st["autoOpenDevice"]  = cfgAutoOpenDev_;
    st["startFullscreen"] = cfgStartFullscreen_;
    st["hideCursor"]      = cfgHideCursor_;
    st["adminPass"]       = cfgAdminPass_;
    st["connTimeoutSec"]  = cfgConnTimeoutSec_;
    st["snapshotDir"]     = cfgSnapshotDir_;
    st["audioEnabled"]    = cfgAudioEnabled_;
    st["audioVolume"]     = cfgAudioVolume_;
    st["logEnabled"]      = cfgLogEnabled_;
    QJsonArray km;
    for (auto it = cfgKnownModels_.begin(); it != cfgKnownModels_.end(); ++it) {
        QJsonObject o;
        o["model"] = it.key(); o["proto"] = it.value().first; o["port"] = it.value().second;
        km.append(o);
    }
    st["knownModels"] = km;
    root["settings"] = st;

    QJsonArray devs;
    for (const auto& d : devices_) {
        QJsonObject o;
        o["name"] = d.name; o["ip"] = d.ip; o["port"] = d.port;
        o["proto"] = d.proto;
        o["rtspPort"] = d.rtspPort; o["user"] = d.user;
        o["password"] = d.pass;   // открытым текстом (личное использование)
        o["type"] = d.type; o["model"] = d.model;
        o["serial"] = d.serial; o["channels"] = d.channels;
        QJsonArray cams;
        for (const auto& c : d.cams) {
            QJsonObject co;
            co["channel"] = c.channel; co["name"] = c.name; co["ip"] = c.ip;
            cams.append(co);
        }
        o["cameras"] = cams;
        devs.append(o);
    }
    root["devices"] = devs;

    // атомарная запись: подмена файла целиком только после успешной записи
    QSaveFile f(configFile());
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(root).toJson());
        f.commit();
    }
}

void MainWindow::loadConfig() {
    devices_.clear();
    // первый запуск на новом ПК: если конфига в AppData ещё нет,
    // но рядом с exe лежит config.json — берём его как стартовый
    if (!QFile::exists(configFile())) {
        QString seed = QDir(QCoreApplication::applicationDirPath()).filePath("config.json");
        if (QFile::exists(seed)) QFile::copy(seed, configFile());
    }
    QFile f(configFile());
    if (f.open(QIODevice::ReadOnly)) {
        QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        QJsonObject st = root["settings"].toObject();
        cfgHwDecode_ = st["hwdecode"].toBool(false);
        cfgLayouts_.clear();
        for (const auto& v : st["layouts"].toArray()) cfgLayouts_ << v.toString();
        cfgDefaultLayout_ = st["defaultLayout"].toInt(4);
        cfgHeartbeatSec_  = qBound(5, st["heartbeatSec"].toInt(15), 300);
        cfgDefaultStretch_  = st["defaultStretch"].toBool(false);
        cfgBufferMs_        = st["bufferMs"].toInt(300);
        cfgShowTitles_      = st["showTitles"].toBool(true);
        cfgRestoreSession_  = st["restoreSession"].toBool(false);
        cfgAutoOpenDev_     = st["autoOpenDevice"].toInt(-1);
        cfgStartFullscreen_ = st["startFullscreen"].toBool(false);
        cfgHideCursor_      = st["hideCursor"].toBool(false);
        cfgAdminPass_       = st["adminPass"].toString();
        cfgConnTimeoutSec_  = st["connTimeoutSec"].toInt(5);
        cfgSnapshotDir_     = st["snapshotDir"].toString();
        cfgAudioEnabled_    = st["audioEnabled"].toBool(false);
        cfgAudioVolume_     = st["audioVolume"].toInt(80);
        cfgLogEnabled_      = st["logEnabled"].toBool(false);
        cfgKnownModels_.clear();
        for (const auto& v : st["knownModels"].toArray()) {
            QJsonObject o = v.toObject();
            cfgKnownModels_[o["model"].toString()] =
                { o["proto"].toString(), o["port"].toInt() };
        }
        for (const auto& v : root["devices"].toArray()) {
            QJsonObject o = v.toObject();
            Device d;
            d.id = nextId_++;
            d.name = o["name"].toString(); d.ip = o["ip"].toString();
            d.port = o["port"].toInt(34567);
            d.proto = o["proto"].toString("xm");
            d.rtspPort = o["rtspPort"].toInt(554);
            if (d.proto == "tvt" && (d.port == 554 || d.port == 0))
                d.port = 80;   // управляющий порт TVT/ONVIF — 80 (миграция старого дефолта)
            d.user = o["user"].toString(); d.pass = o["password"].toString();
            d.type = o["type"].toString();
            d.model = o["model"].toString(); d.serial = o["serial"].toString();
            d.channels = o["channels"].toInt();
            for (const auto& cv : o["cameras"].toArray()) {
                QJsonObject co = cv.toObject();
                d.cams.append({ co["name"].toString(), co["channel"].toInt(), co["ip"].toString() });
            }
            devices_.append(d);
        }
        return;
    }

    // ---- миграция со старых файлов (однократно) ----
    // devices.json (устройства, возможно DPAPI-пароль)
    QFile df(appDataDir() + "/devices.json");
    if (df.open(QIODevice::ReadOnly)) {
        QJsonArray arr = QJsonDocument::fromJson(df.readAll()).array();
        df.close();
        for (const auto& v : arr) {
            QJsonObject o = v.toObject();
            Device d;
            d.id = nextId_++;
            d.name = o["name"].toString(); d.ip = o["ip"].toString();
            d.port = o["port"].toInt(34567); d.user = o["user"].toString();
            d.pass = o.contains("password") ? o["password"].toString()
                                            : decPass(o["pass"].toString());
            d.type = o["type"].toString();
            d.model = o["model"].toString(); d.serial = o["serial"].toString();
            d.channels = o["channels"].toInt();
            devices_.append(d);
        }
    }
    // camnames.json -> камеры первого устройства (каналы по порядку)
    if (!devices_.isEmpty()) {
        QStringList names;
        QFile cf(appDataDir() + "/camnames.json");
        if (cf.open(QIODevice::ReadOnly)) {
            for (const auto& v : QJsonDocument::fromJson(cf.readAll()).array())
                names << v.toString();
        }
        int n = names.isEmpty() ? 20 : names.size();
        for (int i = 0; i < n; ++i) {
            CamRef c;
            c.channel = i + 1;
            c.name = (i < names.size() && !names[i].trimmed().isEmpty())
                         ? names[i].trimmed() : QString("Камера %1").arg(i + 1);
            devices_[0].cams.append(c);
        }
    }
    // настройки из реестра (QSettings)
    QSettings qs;
    cfgHwDecode_ = qs.value("video/hwdecode", false).toBool();
    cfgLayouts_  = qs.value("layouts/custom").toStringList();

    saveConfig();   // с этого момента живём в едином config.json
}

void MainWindow::startHeartbeat() {
    heartbeat_ = new QTimer(this);
    connect(heartbeat_, &QTimer::timeout, this, &MainWindow::checkAllDevices);
    heartbeat_->start(cfgHeartbeatSec_ * 1000);
    QTimer::singleShot(400, this, &MainWindow::checkAllDevices);
}

void MainWindow::checkAllDevices() {
    for (const auto& d : devices_) {
        int id = d.id; QString ip = d.ip;
        int port = d.port;   // управляющий порт (у TVT — 80, у Dahua — 37777, у XM — 34567)
        QString user = d.user, pass = d.pass, proto = d.proto;
        auto* w = new QFutureWatcher<bool>(this);
        connect(w, &QFutureWatcher<bool>::finished, this, [this, w, id]{
            bool on = w->result();
            for (int i = 0; i < devices_.size(); ++i)
                if (devices_[i].id == id) {
                    bool wasChecked = devices_[i].checked;
                    bool statusChanged = (devices_[i].online != on);
                    bool changed = statusChanged || !wasChecked;
                    devices_[i].online = on;
                    devices_[i].checked = true;
                    if (changed) {
                        if (devTable_ && i < devTable_->rowCount())
                            if (auto* it = devTable_->item(i, 8))
                                it->setText(on ? QString::fromUtf8("\xE2\x97\x8F \xD0\x92 \xD1\x81\xD0\xB5\xD1\x82\xD0\xB8")
                                               : QString::fromUtf8("\xE2\x97\x8F \xD0\x9D\xD0\xB5 \xD0\xB2 \xD1\x81\xD0\xB5\xD1\x82\xD0\xB8"));
                        rebuildDeviceTiles();   // плитки выбора: статус обновился
                        // журнал: смену статуса пишем (не первую проверку)
                        if (wasChecked && statusChanged) {
                            QString nm = devices_[i].name.isEmpty() ? devices_[i].ip : devices_[i].name;
                            if (on) Journal::inst().info(nm, QStringLiteral("Регистратор в сети"));
                            else    Journal::inst().warn(nm, QStringLiteral("Регистратор не в сети"));
                        }
                    }
                    break;
                }
            w->deleteLater();
        });
        w->setFuture(QtConcurrent::run([ip, port, user, pass, proto]{
            if (proto == "dahua" || proto == "tvt") {   // достаточно живости порта
                QTcpSocket s;
                s.connectToHost(ip, (quint16)port);
                bool ok = s.waitForConnected(3000);
                s.abort();
                return ok;
            }
            XmClient c; bool ok = c.login(ip, port, user, pass, 3000);
            if (ok) c.logout();
            return ok;
        }));
    }
}

void MainWindow::showAddPanel(bool on) {
    if (!addPanel_) return;
    if (on) {
        addPanel_->adjustSize();
        // по центру главного окна, поверх всего
        addPanel_->move(geometry().center()
                        - QPoint(addPanel_->width() / 2, addPanel_->height() / 2));
        if (detLbl_) detLbl_->clear();   // не показывать прошлый результат «Определить»
        addPanel_->show();
        addPanel_->raise();
        if (fName_) fName_->setFocus();
    } else {
        addPanel_->hide();
        // очистка полей И подписи «Найдено…» при ЛЮБОМ закрытии
        // (класс не оставляет за собой состояние)
        fName_->clear(); fIp_->clear(); fPort_->clear(); fUser_->clear(); fPass_->clear();
        if (detLbl_) detLbl_->clear();
        editingId_ = -1;
    }
}

void MainWindow::gotoPage(int page, const QString& tabTitle) {
    if (page < 0) return;
    if (page != 0 && !tabBtn_.contains(page)) {
        auto* box = new QWidget; box->setAttribute(Qt::WA_Hover);
        auto* bl = new QHBoxLayout(box);
        bl->setContentsMargins(0,0,4,0); bl->setSpacing(4);
        auto* t = new QPushButton(tabTitle); t->setObjectName("tab"); t->setCursor(Qt::PointingHandCursor);
        // Клик по вкладке = вход в просмотр => тоже опрашиваем регистратор (при уходе
        // с вкладки видео глушится, поэтому возврат равнозначен повторному открытию).
        // Само окно просмотра переиспользуется (openDeviceView -> updateDevice).
        connect(t, &QPushButton::clicked, this, [this,page]{
            int devId = -1;
            for (auto it = livePage_.begin(); it != livePage_.end(); ++it)
                if (it.value() == page) { devId = it.key(); break; }
            if (devId >= 0) openDeviceView(devId);
            else            gotoPage(page);
        });
        // зарезервированный слот 18px под крестик (без сдвига), крестик скрыт до наведения
        auto* xslot = new QWidget; xslot->setFixedWidth(18);
        auto* xl = new QHBoxLayout(xslot); xl->setContentsMargins(0,0,0,0);
        auto* xx = new QPushButton(QString::fromUtf8("\xE2\x9C\x95")); xx->setObjectName("tabx");
        xx->setCursor(Qt::PointingHandCursor); xx->setVisible(false);
        connect(xx, &QPushButton::clicked, this, [this,page]{ closeTab(page); });
        xl->addWidget(xx, 0, Qt::AlignCenter);
        bl->addWidget(t); bl->addWidget(xslot);
        box->installEventFilter(this);
        tabsRow_->addWidget(box);
        tabBtn_[page] = t; tabBox_[page] = box; tabX_[page] = xx;
    }
    stack_->setCurrentIndex(page);
    // активна только вьюха текущей страницы (её потоки идут, остальные глушатся)
    for (auto it = liveViews_.begin(); it != liveViews_.end(); ++it)
        it.value()->setActive(livePage_.value(it.key(), -1) == page);
    if (page == 1) {           // страница выбора: обновить статусы устройств
        checkAllDevices();
        rebuildDeviceTiles();
    }
    setActiveTab(page);
}

void MainWindow::closeTab(int page) {
    if (page == 0 || !tabBox_.contains(page)) return;
    QWidget* box = tabBox_.take(page);
    tabBtn_.remove(page); tabX_.remove(page);
    tabsRow_->removeWidget(box);
    box->deleteLater();
    if (stack_->currentIndex() == page) gotoPage(0);
}

bool MainWindow::eventFilter(QObject* o, QEvent* e) {
    if (e->type() == QEvent::Enter || e->type() == QEvent::Leave) {
        for (auto it = tabBox_.begin(); it != tabBox_.end(); ++it) {
            if (it.value() == o) {
                QWidget* box = it.value(); QPushButton* xx = tabX_.value(it.key(), nullptr);
                if (!xx) break;
                bool inside = box->rect().contains(box->mapFromGlobal(QCursor::pos()));
                xx->setVisible(e->type() == QEvent::Enter ? true : inside);
                break;
            }
        }
    }
    return QMainWindow::eventFilter(o, e);
}

void MainWindow::setActiveTab(int page) {
    for (auto it = tabBtn_.begin(); it != tabBtn_.end(); ++it) {
        it.value()->setProperty("active", it.key() == page);
        it.value()->style()->unpolish(it.value());
        it.value()->style()->polish(it.value());
    }
}

void MainWindow::buildPerfPopup(QPushButton* anchor) {
    // всплывающая панель ЦПУ/ОЗУ (как спидометр в Smart PSS)
    perfPopup_ = new QFrame(this, Qt::Popup);
    perfPopup_->setObjectName("perfPopup");
    perfPopup_->setStyleSheet(
        "#perfPopup { background:#ffffff; border:1px solid #dfe3e8; border-radius:6px; }"
        "QLabel { color:#3a414b; font-size:13px; } "
        "#perfBar { background:#eef1f4; border-radius:3px; } "
        "#perfFill { background:#3ca35a; border-radius:3px; }");
    auto* g = new QGridLayout(perfPopup_); g->setContentsMargins(16,12,16,12);
    g->setHorizontalSpacing(12); g->setVerticalSpacing(10);
    auto mkRow = [&](int row, const QString& name, QLabel*& val, QWidget*& bar){
        g->addWidget(new QLabel(name), row, 0);
        bar = new QWidget; bar->setObjectName("perfBar"); bar->setFixedSize(90, 6);
        auto* fill = new QWidget(bar); fill->setObjectName("perfFill");
        fill->setGeometry(0, 0, 0, 6); fill->setProperty("isFill", true);
        g->addWidget(bar, row, 1);
        val = new QLabel("—"); val->setMinimumWidth(38);
        g->addWidget(val, row, 2);
    };
    mkRow(0, QStringLiteral("ЦПУ"), perfCpu_, perfBarCpu_);
    mkRow(1, QStringLiteral("ОЗУ"), perfRam_, perfBarRam_);
    perfPopup_->setFixedSize(190, 78);   // явный размер (иначе adjustSize срезал содержимое)

    connect(anchor, &QPushButton::clicked, this, [this, anchor]{
        samplePerf();
        QPoint p = anchor->mapToGlobal(QPoint(anchor->width() - perfPopup_->width(), anchor->height() + 2));
        QRect scr = anchor->screen()->availableGeometry();
        p.setX(qBound(scr.left() + 4, p.x(), scr.right() - perfPopup_->width() - 4));
        p.setY(qMin(p.y(), scr.bottom() - perfPopup_->height() - 4));
        perfPopup_->move(p);
        perfPopup_->show();
        samplePerf();   // обновить шкалы после того, как попап получил размеры
    });
    // фоновый замер (для актуальности при открытии)
    auto* t = new QTimer(this);
    connect(t, &QTimer::timeout, this, &MainWindow::samplePerf);
    t->start(2000);
    samplePerf();
}

void MainWindow::samplePerf() {
    int cpu = 0, ram = 0;
#ifdef _WIN32
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        auto u64 = [](const FILETIME& f){ return (quint64(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        quint64 idleT = u64(idle), total = u64(kernel) + u64(user);
        quint64 dIdle = idleT - perfPrevIdle_, dTotal = total - perfPrevTotal_;
        if (perfPrevTotal_ && dTotal) cpu = int(100.0 * (dTotal - dIdle) / dTotal + 0.5);
        perfPrevIdle_ = idleT; perfPrevTotal_ = total;
    }
    MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) ram = int(ms.dwMemoryLoad);
#endif
    cpu = qBound(0, cpu, 100); ram = qBound(0, ram, 100);
    if (perfCpu_) perfCpu_->setText(QString::number(cpu) + "%");
    if (perfRam_) perfRam_->setText(QString::number(ram) + "%");
    auto setBar = [](QWidget* bar, int pct){
        if (!bar) return;
        auto* fill = bar->findChild<QWidget*>();
        if (fill) fill->setGeometry(0, 0, bar->width() * pct / 100, bar->height());
    };
    setBar(perfBarCpu_, cpu); setBar(perfBarRam_, ram);
}

void MainWindow::toggleMax() { isMaximized() ? showNormal() : showMaximized(); }

void MainWindow::closeEvent(QCloseEvent* e) {
    Journal::inst().info(QStringLiteral("Система"), QStringLiteral("Завершение работы"));
    for (auto* v : liveViews_) v->setActive(false);   // остановить все потоки
    QMainWindow::closeEvent(e);
}

void MainWindow::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) {
        auto* v = qobject_cast<LiveView*>(stack_->currentWidget());
        if (v && v->inFullscreen()) { v->exitFullscreen(); return; }
    }
    QMainWindow::keyPressEvent(e);
}

void MainWindow::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton && e->position().y() <= TOPBAR_H) {
        dragging_ = true;
        dragPos_ = e->globalPosition().toPoint() - frameGeometry().topLeft();
    }
    QMainWindow::mousePressEvent(e);
}
void MainWindow::mouseMoveEvent(QMouseEvent* e) {
    if (dragging_ && (e->buttons() & Qt::LeftButton) && !isMaximized())
        move(e->globalPosition().toPoint() - dragPos_);
    QMainWindow::mouseMoveEvent(e);
}
void MainWindow::mouseReleaseEvent(QMouseEvent* e) {
    dragging_ = false;
    QMainWindow::mouseReleaseEvent(e);
}
