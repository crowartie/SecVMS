#pragma once
#include <QString>
#include "Types.h"

// Определение ПРЯМЫХ RTSP-адресов камеры (минуя регистратор).
// Порядок: ONVIF GetProfiles+GetStreamUri (камера сама говорит свой URL) →
// шаблоны по подсказке протокола регистратора (Dahua / Hikvision-ISAPI / Xiongmai),
// каждый шаблон проверяется реальным открытием RTSP через FFmpeg.
// Блокирующая функция — звать из рабочего потока (QtConcurrent).
struct DirectResult {
    int     channel = 0;
    QString main, sub;      // прямые URL (с учёткой внутри); пусто = не удалось
    QString how;            // onvif / dahua / hik / tvt / ... (имя шаблона)
    QString why;            // если не удалось — на каком этапе и почему (для Журнала)
};

DirectResult resolveCameraDirect(const CamRef& cam, const QString& fallbackUser,
                                 const QString& fallbackPass, int timeoutMs = 2500);
