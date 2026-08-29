#pragma once
#include <QDateTime>
#include <QVector>
#include <QThread>
#include <QImage>
#include <atomic>
#include "Types.h"

// Один фрагмент записи в архиве регистратора («мини-файл»).
struct ArchSeg { QDateTime b, e; };

// Слить прилегающие фрагменты (зазор <= gapSec) в непрерывные отрезки — для ползунка.
QVector<ArchSeg> archMerge(QVector<ArchSeg> in, int gapSec = 3);

// Список записей канала (1-based) за день. Блокирующие — звать из QtConcurrent.
QVector<ArchSeg> xmQuerySegments(const Device& d, int channel, const QDate& day, QString* err);
QVector<ArchSeg> dahuaQuerySegments(const Device& d, int channel, const QDate& day, QString* err);

// Поток воспроизведения архива Xiongmai: DVRIP OPPlayBack ByTime — регистратор САМ
// «сшивает» файлы и пропускает дыры; здесь разбираем контейнер XM (кадры 0x1FC/0x1FD),
// декодируем FFmpeg и отдаём кадры с АБСОЛЮТНОЙ меткой времени (из заголовков I-кадров).
class XmPlayThread : public QThread {
    Q_OBJECT
public:
    explicit XmPlayThread(QObject* parent = nullptr) : QThread(parent) {}
    void begin(const Device& d, int channel, const QDateTime& from, const QDateTime& to);
    void stopAndWait();
    void setTarget(int w, int h) { tw_ = w; th_ = h; }   // размер экрана: скейл в потоке
signals:
    void frame(const QImage& img, const QDateTime& ts);
    void ended();                       // интервал доигран / поток закончился
    void failed(const QString& why);
protected:
    void run() override;
private:
    Device    dev_;
    int       channel_ = 1;
    QDateTime from_, to_;
    std::atomic<bool> stop_{false};
    std::atomic<int>  tw_{0}, th_{0};
};
