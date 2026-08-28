#pragma once
#include "Types.h"
#include <QMap>
#include <QRegularExpression>

// Парсер ответа configManager.cgi?action=getConfig&name=RemoteDevice регистратора Dahua:
// записи есть ТОЛЬКО для каналов с привязанной камерой — это и фильтр пустых каналов,
// и источник имени (VideoInputs[0].Name) и IP камеры.
inline QVector<CamRef> parseDahuaRemoteDevice(const QString& body) {
    struct Tmp { bool enable = false; QString ip, name; };
    QMap<int, Tmp> m;   // индекс канала (0-based) -> данные
    static const QRegularExpression re(
        R"(_INFO_(\d+)\.(Enable|Address|VideoInputs\[0\]\.Name)=(.*))");
    for (const QString& line : body.split('\n')) {
        auto mt = re.match(line.trimmed());
        if (!mt.hasMatch()) continue;
        int i = mt.captured(1).toInt();
        const QString k = mt.captured(2);
        const QString v = mt.captured(3).trimmed();
        if      (k == "Enable")  m[i].enable = (v == "true");
        else if (k == "Address") m[i].ip = v;
        else if (!v.isEmpty())   m[i].name = v;   // пустая строка не затирает имя
    }
    QVector<CamRef> out;
    for (auto it = m.begin(); it != m.end(); ++it) {
        if (!it.value().enable) continue;
        CamRef c;
        c.channel = it.key() + 1;
        c.name = it.value().name.isEmpty()
                     ? QString("Камера %1").arg(c.channel) : it.value().name;
        c.ip = it.value().ip;
        out << c;
    }
    return out;
}
