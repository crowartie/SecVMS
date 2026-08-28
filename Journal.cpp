#include "Journal.h"
#include <QFile>
#include <QJsonObject>
#include <QJsonDocument>
#include <QTextStream>

Journal& Journal::inst() {
    static Journal j;
    return j;
}

void Journal::setFile(const QString& path) {
    path_ = path;
    entries_.clear();
    loadRecent();
}

void Journal::add(const QString& level, const QString& source, const QString& text) {
    LogEntry e{ QDateTime::currentDateTime(), level, source, text };
    entries_.append(e);
    if (entries_.size() > kMemCap) entries_.remove(0, entries_.size() - kMemCap);
    emit appended(e);

    if (!enabled_ || path_.isEmpty()) return;
    QFile f(path_);
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        QJsonObject o;
        o["t"] = e.time.toString(Qt::ISODate);
        o["l"] = level; o["s"] = source; o["m"] = text;
        f.write(QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n');
    }
}

void Journal::loadRecent(int maxLines) {
    QFile f(path_);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    QStringList lines;
    QTextStream ts(&f);
    while (!ts.atEnd()) {
        lines << ts.readLine();
        if (lines.size() > maxLines * 2) lines.remove(0, lines.size() - maxLines);
    }
    const int start = qMax(0, lines.size() - maxLines);
    for (int i = start; i < lines.size(); ++i) {
        QJsonObject o = QJsonDocument::fromJson(lines[i].toUtf8()).object();
        if (o.isEmpty()) continue;
        entries_.append({ QDateTime::fromString(o["t"].toString(), Qt::ISODate),
                          o["l"].toString(), o["s"].toString(), o["m"].toString() });
    }
}
