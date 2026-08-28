#pragma once
#include <QString>
#include <QVector>

// камера внутри устройства (канал регистратора)
struct CamRef {
    QString name;        // отображаемое имя (с самой камеры)
    int     channel = 0; // номер канала на регистраторе
    QString ip;          // IP камеры (справочно)
    int     status = -1; // состояние по данным регистратора: 1=онлайн, 0=офлайн, -1=неизвестно
};

// одно сохранённое устройство (регистратор) со своими камерами
struct Device {
    int     id = 0;
    QString name, ip, user, pass, type, model, serial;
    QString proto = "xm";      // "xm" (Xiongmai DVRIP) | "dahua" (NVR Dahua)
    int     port = 34567;      // управляющий порт (34567 DVRIP / 37777 Dahua)
    int     rtspPort = 554;    // RTSP-ретрансляция
    int     channels = 0;
    bool    online = false;
    bool    checked = false;   // false = статус ещё не проверялся («Проверка...»)
    QVector<CamRef> cams;
};
