#pragma once
#include "Types.h"
#include <QUrl>

// Единый построитель RTSP-URL по протоколу устройства.
// Логин/пароль percent-кодируются (у паролей бывают ! % @ и т.п.).
inline QString streamUrl(const Device& d, int channel, bool main) {
    const QString u = QString::fromLatin1(QUrl::toPercentEncoding(d.user));
    const QString p = QString::fromLatin1(QUrl::toPercentEncoding(d.pass));
    const int sub = main ? 0 : 1;

    if (d.proto == "dahua")
        return QString("rtsp://%1:%2@%3:%4/cam/realmonitor?channel=%5&subtype=%6")
            .arg(u, p, d.ip).arg(d.rtspPort).arg(channel).arg(sub);

    if (d.proto == "tvt")   // TVT/TVOS: /unicast/c<ch>/s<0|1>/live
        return QString("rtsp://%1:%2@%3:%4/unicast/c%5/s%6/live")
            .arg(u, p, d.ip).arg(d.rtspPort).arg(channel).arg(sub);

    // Xiongmai: креды в пути
    return QString("rtsp://%1:%2/user=%3&password=%4&channel=%5&stream=%6.sdp?real_stream")
        .arg(d.ip).arg(d.rtspPort).arg(u, p).arg(channel).arg(sub);
}
