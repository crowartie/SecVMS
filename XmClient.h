#pragma once
#include <QString>
#include <QList>
#include <QMap>
#include <QPair>
#include <QVector>
#include <QDateTime>
#include <QTcpSocket>

// Минимальный клиент протокола Xiongmai/Sofia (DVRIP, порт 34567):
// логин, инфо об устройстве, список привязанных камер (RemoteDeviceV3).
class XmClient {
public:
    struct Cam { QString name, ip, user, pass, proto; int port = 80, channel = 0; bool enable = false; };

    bool    login(const QString& host, int port, const QString& user,
                  const QString& pass, int timeoutMs = 4000);
    void    fetchInfo();      // SystemInfo -> model, firmware, channels
    void    fetchCameras();   // NetWork.RemoteDeviceV3 -> cameras
    void    fetchTitles();    // ChannelTitle -> titles (имена каналов с регистратора)
    void    fetchStatus();    // NetWork.ChnStatus -> online/offline цифровых каналов
    void    logout();

    // ---- архив (OPFileQuery / OPPlayBack) ----
    // список записей канала (0-based) за интервал; постранично по 64 файла
    QVector<QPair<QDateTime,QDateTime>> fileQuery(int channel0, const QDateTime& from,
                                                  const QDateTime& to);
    bool    playClaim(int channel0, const QDateTime& from, const QDateTime& to);  // 1424 Claim
    void    playStart(int channel0, const QDateTime& from, const QDateTime& to);  // 1420 Start (без ожидания)
    QTcpSocket& sock() { return sock_; }   // читать медиапоток воспроизведения напрямую

    QString model, firmware, serial;
    int     channels = 0;
    QList<Cam> cameras;
    QStringList titles;
    QMap<int,int> chnStatus;   // канал(1..N) -> 1=онлайн, 0=офлайн
    QString error;

private:
    QByteArray  sofiaHash(const QString& pwd);
    QByteArray  packet(quint16 msgid, const QByteArray& json);
    QByteArray  sendRecv(quint16 msgid, const QByteArray& json, int timeoutMs = 4000);
    bool        readN(QByteArray& out, int n, int timeoutMs);

    QTcpSocket sock_;
    qint32     session_ = 0;
    qint32     seq_ = 0;
    QString    sid_;
};
