#pragma once
#include <QString>
#include <QList>
#include <QMap>
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
