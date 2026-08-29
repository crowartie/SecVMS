#pragma once
#include <QDateTime>
#include <QVector>
#include <QThread>
#include <QImage>
#include <QString>
#include <atomic>
#include "Types.h"

// Один фрагмент записи в архиве регистратора («мини-файл»).
struct ArchSeg { QDateTime b, e; };

// Слить прилегающие фрагменты (зазор <= gapSec) в непрерывные отрезки — для ползунка.
QVector<ArchSeg> archMerge(QVector<ArchSeg> in, int gapSec = 3);

// Список записей канала (1-based) за день. Блокирующие — звать из QtConcurrent.
// stream: 0 = основной поток, 1 = субпоток.
QVector<ArchSeg> xmQuerySegments(const Device& d, int channel, const QDate& day, QString* err,
                                 int stream = 0);
QVector<ArchSeg> dahuaQuerySegments(const Device& d, int channel, const QDate& day, QString* err);

// Поток воспроизведения архива Xiongmai: DVRIP OPPlayBack ByName с ЦЕПОЧКОЙ файлов
// (имя файла однозначно задаёт канал — «ByTime» на части прошивок игнорирует канал).
// Файлы за интервал проигрываются подряд, дыры пропускаются; кадры отдаются с
// АБСОЛЮТНОЙ меткой времени (из заголовков I-кадров XM-контейнера).
class XmPlayThread : public QThread {
    Q_OBJECT
public:
    explicit XmPlayThread(QObject* parent = nullptr) : QThread(parent) {}
    void begin(const Device& d, int channel, const QDateTime& from, const QDateTime& to,
               int stream, double speed);
    void stopAndWait();
    void setTarget(int w, int h) { tw_ = w; th_ = h; }   // размер экрана: скейл в потоке
    void setSpeed(double s) { speed_ = s; }              // 1/2/4/8 — на лету
    void setPaused(bool p)  { paused_ = p; }
signals:
    void frame(const QImage& img, const QDateTime& ts);
    void ended();                       // интервал доигран / файлы закончились
    void failed(const QString& why);
protected:
    void run() override;
private:
    Device    dev_;
    int       channel_ = 1;
    int       stream_ = 0;
    QDateTime from_, to_;
    std::atomic<bool>   stop_{false};
    std::atomic<bool>   paused_{false};
    std::atomic<double> speed_{1.0};
    std::atomic<int>    tw_{0}, th_{0};
};

// Скачивание участка архива в MP4 (без перекодирования). Отдельный поток с прогрессом.
// mainStream=true — качаем основной поток (выше разрешение).
class ArchiveDownloader : public QThread {
    Q_OBJECT
public:
    explicit ArchiveDownloader(QObject* parent = nullptr) : QThread(parent) {}
    void begin(const Device& d, int channel, const QDateTime& from, const QDateTime& to,
               bool mainStream, const QString& outPath);
    void cancel() { stop_ = true; }
signals:
    void progress(int percent);         // 0..100 по времени
    void done(const QString& path);     // успех
    void failed(const QString& why);
protected:
    void run() override;
private:
    void runDahua();
    void runXm();
    Device    dev_;
    int       channel_ = 1;
    QDateTime from_, to_;
    bool      main_ = true;
    QString   out_;
    std::atomic<bool> stop_{false};
};
