#include "XmClient.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QtEndian>
#include <QFile>
#include <QDir>

QByteArray XmClient::sofiaHash(const QString& pwd) {
    QByteArray md5 = QCryptographicHash::hash(pwd.toUtf8(), QCryptographicHash::Md5);
    QByteArray out;
    for (int i = 0; i < 8; ++i) {
        int n = (quint8(md5[2*i]) + quint8(md5[2*i+1])) % 62;
        if (n > 9) n += (n > 35) ? 61 : 55; else n += 48;
        out.append(char(n));
    }
    return out;
}

QByteArray XmClient::packet(quint16 msgid, const QByteArray& json) {
    QByteArray data = json;
    data.append('\x0a'); data.append('\x00');
    QByteArray h(20, '\0');
    h[0] = (char)0xff;
    qToLittleEndian<qint32>(session_, h.data() + 4);
    qToLittleEndian<qint32>(seq_,     h.data() + 8);
    qToLittleEndian<quint16>(msgid,   h.data() + 14);
    qToLittleEndian<quint32>((quint32)data.size(), h.data() + 16);
    return h + data;
}

bool XmClient::readN(QByteArray& out, int n, int timeoutMs) {
    while (out.size() < n) {
        if (sock_.bytesAvailable() == 0 && !sock_.waitForReadyRead(timeoutMs)) return false;
        out.append(sock_.read(n - out.size()));
    }
    return true;
}

QByteArray XmClient::sendRecv(quint16 msgid, const QByteArray& json, int timeoutMs) {
    sock_.write(packet(msgid, json));
    sock_.flush();
    ++seq_;
    QByteArray hdr;
    if (!readN(hdr, 20, timeoutMs)) { error = "нет ответа"; return {}; }
    quint32 dlen = qFromLittleEndian<quint32>((const uchar*)hdr.constData() + 16);
    if (dlen == 0 || dlen > 8*1024*1024) return {};
    QByteArray body;
    if (!readN(body, (int)dlen, timeoutMs)) { error = "обрыв ответа"; return {}; }
    int z = body.indexOf('\x00');
    if (z >= 0) body.truncate(z);
    return body;
}

bool XmClient::login(const QString& host, int port, const QString& user,
                     const QString& pass, int timeoutMs) {
    sock_.connectToHost(host, (quint16)port);
    if (!sock_.waitForConnected(timeoutMs)) { error = "нет связи"; return false; }
    QJsonObject o;
    o["EncryptType"] = "MD5";
    o["LoginType"]   = "DVRIP-Web";
    o["UserName"]    = user;
    o["PassWord"]    = QString::fromLatin1(sofiaHash(pass));
    QByteArray resp = sendRecv(1000, QJsonDocument(o).toJson(QJsonDocument::Compact));
    if (resp.isEmpty()) return false;   // сетевая ошибка уже в error — не затирать
                                        // (порт может принять коннект и молчать: файрвол)
    QJsonObject r = QJsonDocument::fromJson(resp).object();
    int ret = r.value("Ret").toInt(-1);
    if (ret != 100) {
        error = (ret == 205 ? "нет такого пользователя"
               : ret == 203 ? "неверный пароль"
               : QString("ошибка входа (Ret=%1)").arg(ret));
        return false;
    }
    sid_ = r.value("SessionID").toString();
    session_ = sid_.toInt(nullptr, 16);
    channels = r.value("ChannelNum").toInt();
    return true;
}

void XmClient::fetchInfo() {
    QJsonObject o; o["Name"] = "SystemInfo"; o["SessionID"] = sid_;
    QByteArray resp = sendRecv(1020, QJsonDocument(o).toJson(QJsonDocument::Compact));
    QJsonObject si = QJsonDocument::fromJson(resp).object().value("SystemInfo").toObject();
    if (!si.isEmpty()) {
        model    = si.value("HardWare").toString();
        firmware = si.value("SoftWareVersion").toString();
        serial   = si.value("SerialNo").toString();
        int dc = si.value("DigChannel").toInt();
        if (dc > 0) channels = dc;
    }
}

void XmClient::fetchCameras() {
    QJsonObject o; o["Name"] = "NetWork.RemoteDeviceV3"; o["SessionID"] = sid_;
    QByteArray resp = sendRecv(1042, QJsonDocument(o).toJson(QJsonDocument::Compact));
    QJsonArray arr = QJsonDocument::fromJson(resp).object()
                        .value("NetWork.RemoteDeviceV3").toArray();
    int ch = 0;
    for (const auto& cv : arr) {
        QJsonObject conn = cv.toObject();
        for (const auto& dv : conn.value("Decoder").toArray()) {
            QJsonObject d = dv.toObject();
            ++ch;   // номер канала считаем ДО фильтра пустых слотов
            QString ip = d.value("IPAddress").toString();
            if (ip.isEmpty() || ip == "192.168.0.10") continue;   // пустой слот
            Cam c;
            c.channel = ch;
            c.name  = d.value("ConfName").toString();
            c.ip    = ip;
            c.port  = d.value("Port").toInt(80);
            c.proto = d.value("Protocol").toString();
            c.user  = d.value("UserName").toString();
            c.pass  = d.value("PassWord").toString();
            c.enable= conn.value("Enable").toBool();
            cameras.append(c);
        }
    }
}

void XmClient::fetchTitles() {
    // имена каналов; в зависимости от прошивки отвечает msgid 1048 либо 1042
    auto ask = [&](quint16 msgid) -> QStringList {
        QJsonObject o; o["Name"] = "ChannelTitle"; o["SessionID"] = sid_;
        QByteArray resp = sendRecv(msgid, QJsonDocument(o).toJson(QJsonDocument::Compact));
        QJsonArray arr = QJsonDocument::fromJson(resp).object()
                            .value("ChannelTitle").toArray();
        QStringList out;
        for (const auto& v : arr) out << v.toString();
        return out;
    };
    titles = ask(1048);
    if (titles.isEmpty()) titles = ask(1042);
}

void XmClient::fetchStatus() {
    // NetWork.ChnStatus: регистратор сам знает, какие цифровые каналы онлайн.
    // Реальный формат (снят с NBD80N36RA-KL, прошивка 2026):
    //   [{"ChnName":"D01","CurRes":"720P/...","MaxRes":"12M","Status":"Connected"},
    //    {...,"Status":"NoConfig"}, {"ChnName":"","Status":""}, ...]
    //   Status: "Connected"=онлайн, "Disconnect(ed)"=офлайн, "NoConfig"/""=нет камеры.
    // Индекс в массиве (1..N) = номер канала. Часть прошивок вместо строки Status
    // отдаёт числовое/булево поле nIsOnline — поддерживаем и это.
    QJsonObject o; o["Name"] = "NetWork.ChnStatus"; o["SessionID"] = sid_;
    QByteArray resp = sendRecv(1042, QJsonDocument(o).toJson(QJsonDocument::Compact));
    QJsonValue val = QJsonDocument::fromJson(resp).object().value("NetWork.ChnStatus");
    if (!val.isArray()) return;
    QJsonArray arr = val.toArray();

    // 1=онлайн, 0=офлайн, -1=нет камеры/неизвестно
    auto interpret = [](const QJsonValue& v) -> int {
        if (v.isString()) {
            QString s = v.toString().trimmed().toLower();
            if (s.isEmpty() || s == "noconfig") return -1;      // канал без камеры
            // важно проверять "disconnect" ДО "connect" (подстрока!)
            if (s.startsWith("disconnect") || s.contains("offline") ||
                s.contains("fail") || s.contains("lost") || s.contains("novideo") ||
                s == "0" || s == "off") return 0;
            if (s.startsWith("connect") || s.contains("online") ||
                s == "1" || s == "on")  return 1;
            return -1;
        }
        if (v.isBool())   return v.toBool() ? 1 : 0;
        if (v.isDouble()) return v.toInt() != 0 ? 1 : 0;
        return -1;
    };

    int idx = 0;
    for (const auto& v : arr) {
        ++idx;                                  // канал по порядку (1..N)
        if (v.isObject()) {
            QJsonObject e = v.toObject();
            int ch = e.contains("Channel") ? e.value("Channel").toInt() + 1 : idx;
            int st = -1;
            for (const char* k : { "Status", "nIsOnline", "IsOnline", "Online",
                                   "ChannelState", "State", "ChnState" }) {
                if (e.contains(k)) { st = interpret(e.value(k)); break; }
            }
            if (st < 0 && e.contains("VideoLoss"))   // VideoLoss=true => офлайн
                st = e.value("VideoLoss").toBool() ? 0 : 1;
            if (st >= 0) chnStatus[ch] = st;
        } else {
            int st = interpret(v);
            if (st >= 0) chnStatus[idx] = st;
        }
    }
}

void XmClient::logout() {
    sock_.disconnectFromHost();
    if (sock_.state() != QAbstractSocket::UnconnectedState)
        sock_.waitForDisconnected(500);
}

// ---- архив ----

QVector<XmClient::ArcFile> XmClient::fileQuery(int channel1, const QDateTime& from,
                                               const QDateTime& to, int stream) {
    // OPFileQuery отдаёт максимум 64 файла за запрос — листаем, сдвигая BeginTime.
    // Канал в запросе 0-based; имя файла кодирует канал и точный участок (для ByName).
    QVector<ArcFile> out;
    QDateTime cursor = from;
    for (int guard = 0; guard < 256; ++guard) {
        QJsonObject q;
        q["BeginTime"]      = cursor.toString("yyyy-MM-dd HH:mm:ss");
        q["EndTime"]        = to.toString("yyyy-MM-dd HH:mm:ss");
        q["Channel"]        = channel1 - 1;
        q["DriverTypeMask"] = "0x0000FFFF";
        q["Event"]          = "*";
        q["StreamType"]     = stream == 1 ? "0x00000001" : "0x00000000";
        q["Type"]           = "h264";
        QJsonObject o; o["Name"] = "OPFileQuery"; o["SessionID"] = sid_; o["OPFileQuery"] = q;
        QByteArray resp = sendRecv(1440, QJsonDocument(o).toJson(QJsonDocument::Compact), 8000);
        if (resp.isEmpty()) break;
        QJsonArray arr = QJsonDocument::fromJson(resp).object().value("OPFileQuery").toArray();
        if (arr.isEmpty()) break;
        QDateTime last;
        int added = 0;
        for (const auto& v : arr) {
            QJsonObject f = v.toObject();
            ArcFile af;
            af.b = QDateTime::fromString(f["BeginTime"].toString(), "yyyy-MM-dd HH:mm:ss");
            af.e = QDateTime::fromString(f["EndTime"].toString(),   "yyyy-MM-dd HH:mm:ss");
            af.name    = f["FileName"].toString();
            af.channel = channel1;
            af.sizeKb  = f["FileLength"].toVariant().toLongLong();
            if (af.b.isValid() && af.e.isValid() && af.e > af.b && !af.name.isEmpty()) {
                out.append(af); ++added;
                if (!last.isValid() || af.e > last) last = af.e;
            }
        }
        if (arr.size() < 64 || !last.isValid() || added == 0) break;   // последняя страница
        cursor = last.addSecs(1);
        if (cursor >= to) break;
    }
    return out;
}

bool XmClient::playClaimByName(const QString& file) {
    QJsonObject par;
    par["FileName"]   = file;
    par["PlayMode"]   = "ByName";      // имя файла однозначно задаёт канал и участок
    par["StreamType"] = 0;
    par["TransMode"]  = "TCP";
    par["Value"]      = 0;
    QJsonObject pb;
    pb["Action"]    = "Claim";
    pb["StartTime"] = "2000-00-00 00:00:00";
    pb["EndTime"]   = "2000-00-00 00:00:00";
    pb["Parameter"] = par;
    QJsonObject o; o["Name"] = "OPPlayBack"; o["SessionID"] = sid_; o["OPPlayBack"] = pb;
    QByteArray resp = sendRecv(1424, QJsonDocument(o).toJson(QJsonDocument::Compact), 6000);
    int ret = QJsonDocument::fromJson(resp).object().value("Ret").toInt(-1);
    if (ret != 100) error = QString("отказ воспроизведения (Ret=%1)").arg(ret);
    return ret == 100;
}

void XmClient::playStartByName(const QString& file) {
    QJsonObject par;
    par["FileName"]   = file;
    par["PlayMode"]   = "ByName";
    par["StreamType"] = 0;
    par["TransMode"]  = "TCP";
    par["Value"]      = 0;
    QJsonObject pb;
    pb["Action"]    = "Start";
    pb["StartTime"] = "2000-00-00 00:00:00";
    pb["EndTime"]   = "2000-00-00 00:00:00";
    pb["Parameter"] = par;
    QJsonObject o; o["Name"] = "OPPlayBack"; o["SessionID"] = sid_; o["OPPlayBack"] = pb;
    sock_.write(packet(1420, QJsonDocument(o).toJson(QJsonDocument::Compact)));   // без ожидания
    sock_.flush();
    ++seq_;
}

void XmClient::playControl(const QString& action) {
    QJsonObject pb; pb["Action"] = action;   // Fast/Slow/Pause/Continue
    QJsonObject o; o["Name"] = "OPPlayBack"; o["SessionID"] = sid_; o["OPPlayBack"] = pb;
    sock_.write(packet(1420, QJsonDocument(o).toJson(QJsonDocument::Compact)));
    sock_.flush();
    ++seq_;
}

void XmClient::playStop(const QString& file) {
    QJsonObject par; par["FileName"] = file; par["PlayMode"] = "ByName";
    par["StreamType"] = 0; par["TransMode"] = "TCP"; par["Value"] = 0;
    QJsonObject pb; pb["Action"] = "Stop";
    pb["StartTime"] = "2000-00-00 00:00:00"; pb["EndTime"] = "2000-00-00 00:00:00";
    pb["Parameter"] = par;
    QJsonObject o; o["Name"] = "OPPlayBack"; o["SessionID"] = sid_; o["OPPlayBack"] = pb;
    sock_.write(packet(1420, QJsonDocument(o).toJson(QJsonDocument::Compact)));
    sock_.flush();
    ++seq_;
}
