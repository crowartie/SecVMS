#pragma once
#include "Types.h"
#include <QObject>
#include <QVector>

class QUdpSocket;
class QTimer;

// Найденное при обнаружении устройство (без учётных данных).
struct Found {
    QString ip;
    QString model;
    QString mac;
    QString vendor;    // "ONVIF" / "Dahua" / "Xiongmai" / ...
    QString proto;     // наш proto для добавления: xm/dahua/tvt (может быть пустым)
    int     port = 0;  // управляющий/сервисный порт
};

// Обнаружение устройств в локальном сегменте БЕЗ логина/пароля:
// ONVIF WS-Discovery (multicast 239.255.255.250:3702) + вендорные UDP-броадкасты.
// Устройства сами объявляют себя; показываем только откликнувшихся.
class Discovery : public QObject {
    Q_OBJECT
public:
    explicit Discovery(QObject* parent = nullptr);
    void start(int durationMs = 4000);   // разослать запросы и слушать ответы
    void stop();

signals:
    void deviceFound(const Found& dev);  // по одному, по мере ответов
    void finished();

private:
    void parseWs(const QByteArray& data, const QString& from);
    void parseDahua(const QByteArray& data, const QString& from);

    QVector<QUdpSocket*> ws_sockets_;   // по сокету на интерфейс
    QTimer*     timer_ = nullptr;
    QVector<QString> seen_;             // dedup по ip
};
