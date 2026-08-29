#pragma once
#include <QObject>
#include <QString>
#include <QDateTime>
#include <QVector>

// Журнал событий приложения: пишет строки в файл (JSON-lines) и держит
// последние записи в памяти для показа на странице «Журнал».
// Синглтон — доступен из любого места (Journal::inst()).
struct LogEntry {
    QDateTime time;
    QString   level;    // info / warn / error
    QString   source;   // имя устройства/камеры или "Система"
    QString   text;
};

class Journal : public QObject {
    Q_OBJECT
public:
    static Journal& inst();
    void   setFile(const QString& path);   // куда писать (из настроек)
    void   setEnabled(bool on) { enabled_ = on; }
    bool   enabled() const { return enabled_; }
    QString filePath() const { return path_; }

    void add(const QString& level, const QString& source, const QString& text);
    void info (const QString& src, const QString& t) { add("info",  src, t); }
    void warn (const QString& src, const QString& t) { add("warn",  src, t); }
    void error(const QString& src, const QString& t) { add("error", src, t); }

    const QVector<LogEntry>& entries() const { return entries_; }
    void clear();                           // очистить и файл, и записи в памяти
    void loadRecent(int maxLines = 2000);   // подтянуть хвост файла при старте

signals:
    void appended(const LogEntry& e);

private:
    Journal() = default;
    QVector<LogEntry> entries_;
    QString path_;
    bool    enabled_ = true;
    static const int kMemCap = 5000;
};
