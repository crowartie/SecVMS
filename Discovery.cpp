#include "Discovery.h"
#include <QUdpSocket>
#include <QTimer>
#include <QNetworkInterface>
#include <QNetworkDatagram>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <QUrl>

static const char* WS_ADDR = "239.255.255.250";
static const quint16 WS_PORT = 3702;
static const quint16 DAHUA_PORT = 37810;

Discovery::Discovery(QObject* parent) : QObject(parent) {}

// Собрать WS-Discovery Probe (пустой Types — совпадает со ВСЕМИ ONVIF-устройствами:
// камеры, NVR, энкодеры; иначе NVR часто не отвечают).
static QByteArray wsProbe() {
    QString id = "urn:uuid:" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    return QString(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
        "xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
        "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\">"
        "<s:Header>"
        "<a:Action s:mustUnderstand=\"1\">"
        "http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</a:Action>"
        "<a:MessageID>%1</a:MessageID>"
        "<a:To s:mustUnderstand=\"1\">urn:schemas-xmlsoap-org:ws:2005:04:discovery</a:To>"
        "</s:Header><s:Body><d:Probe/></s:Body></s:Envelope>").arg(id).toUtf8();
}

static QByteArray dahuaProbe() {
    return QByteArrayLiteral("{\"method\":\"DHDiscover.search\",\"params\":{\"mac\":\"\",\"uni\":1}}");
}

void Discovery::start(int durationMs) {
    seen_.clear();
    // по сокету на каждый рабочий IPv4-интерфейс: multicast уходит только
    // в ОДИН NIC, поэтому опрашиваем каждый отдельно (Wi-Fi/Ethernet/VPN)
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning) ||
            (flags & QNetworkInterface::IsLoopBack))
            continue;
        for (const QNetworkAddressEntry& e : iface.addressEntries()) {
            QHostAddress ip = e.ip();
            if (ip.protocol() != QAbstractSocket::IPv4Protocol) continue;

            auto* s = new QUdpSocket(this);
            if (!s->bind(ip, 0, QAbstractSocket::ShareAddress |
                                 QAbstractSocket::ReuseAddressHint)) {
                s->deleteLater(); continue;
            }
            s->setSocketOption(QAbstractSocket::MulticastTtlOption, 1);
            s->setSocketOption(QAbstractSocket::MulticastLoopbackOption, 0);
            connect(s, &QUdpSocket::readyRead, this, [this, s]{
                while (s->hasPendingDatagrams()) {
                    QNetworkDatagram dg = s->receiveDatagram();
                    const QByteArray d = dg.data();
                    const QString from = dg.senderAddress().toString().section('%', 0, 0);
                    if (d.contains("ProbeMatch"))      parseWs(d, from);
                    else if (d.trimmed().startsWith('{')) parseDahua(d, from);
                }
            });
            ws_sockets_.push_back(s);

            // разослать оба запроса из этого интерфейса (по 2 раза — UDP теряет)
            QByteArray wp = wsProbe(), dp = dahuaProbe();
            QHostAddress bcast = e.broadcast().isNull() ? QHostAddress::Broadcast : e.broadcast();
            for (int k = 0; k < 2; ++k) {
                s->writeDatagram(wp, QHostAddress(WS_ADDR), WS_PORT);
                s->writeDatagram(dp, bcast, DAHUA_PORT);
                s->writeDatagram(dp, QHostAddress::Broadcast, DAHUA_PORT);
            }
        }
    }
    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    connect(timer_, &QTimer::timeout, this, [this]{ stop(); emit finished(); });
    timer_->start(durationMs);
}

void Discovery::stop() {
    if (timer_) { timer_->stop(); }
    for (auto* s : ws_sockets_) s->deleteLater();
    ws_sockets_.clear();
}

// URL-декод и разбор onvif://.../key/value из Scopes
static QString scopeValue(const QString& scopes, const QString& key) {
    QRegularExpression re("onvif://[^/]+/" + key + "/([^ \\t\\r\\n<]+)");
    auto m = re.match(scopes);
    return m.hasMatch() ? QUrl::fromPercentEncoding(m.captured(1).toUtf8()) : QString();
}

void Discovery::parseWs(const QByteArray& data, const QString& from) {
    const QString xml = QString::fromUtf8(data);
    // только ONVIF-устройства (иначе ловятся Windows-ПК, принтеры — у них WSD на 3702)
    if (!xml.contains("onvif", Qt::CaseInsensitive)) return;
    // XAddrs: http://IP[:port]/onvif/device_service — берём IP и порт
    QRegularExpression reX("<[^>]*XAddrs>([^<]+)<");
    auto mx = reX.match(xml);
    QString ip = from; int port = 80;
    if (mx.hasMatch()) {
        QUrl u(mx.captured(1).split(' ').first());
        if (!u.host().isEmpty()) ip = u.host();
        if (u.port() > 0) port = u.port();
    }
    if (ip.isEmpty() || seen_.contains(ip)) return;

    QRegularExpression reS("<[^>]*Scopes>([\\s\\S]*?)<");
    QString scopes = reS.match(xml).captured(1);
    QString name = scopeValue(scopes, "name").trimmed();
    QString hw   = scopeValue(scopes, "hardware").trimmed();
    QString mac  = scopeValue(scopes, "MAC");

    Found f;
    f.ip = ip; f.port = port; f.vendor = "ONVIF";
    if (name == hw || hw.isEmpty())      f.model = name;
    else if (name.isEmpty())             f.model = hw;
    else if (hw.contains(name))          f.model = hw;
    else if (name.contains(hw))          f.model = name;
    else                                 f.model = name + " " + hw;
    f.mac = mac;
    // подсказка протокола по имени вендора (точный proto определится при добавлении)
    QString low = (name + " " + scopes).toLower();
    if (low.contains("dahua"))            { f.vendor = "Dahua";    f.proto = "dahua"; }
    else if (low.contains("xiongmai") || low.contains("xm"))
                                          { f.vendor = "Xiongmai"; f.proto = "xm"; }
    seen_.push_back(ip);
    emit deviceFound(f);
}

void Discovery::parseDahua(const QByteArray& data, const QString& from) {
    QJsonObject o = QJsonDocument::fromJson(data).object();
    QJsonObject info = o.value("params").toObject().value("deviceInfo").toObject();
    if (info.isEmpty()) info = o.value("params").toObject();
    QString ip = info.value("IPv4Address").toObject().value("IPAddress").toString();
    if (ip.isEmpty()) ip = from;
    if (ip.isEmpty() || seen_.contains(ip)) return;
    Found f;
    f.ip = ip; f.vendor = "Dahua"; f.proto = "dahua"; f.port = 37777;
    f.model = info.value("DeviceType").toString();
    f.mac   = info.value("Mac").toString();
    seen_.push_back(ip);
    emit deviceFound(f);
}
