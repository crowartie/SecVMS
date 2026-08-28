#include "Errors.h"
#include <QHash>

namespace Err {

QString text(int code) {
    static const QHash<int, QString> t = {
        { CamConnecting,  QStringLiteral("Подключение...") },
        { CamUnavailable, QStringLiteral("Камера недоступна") },
        { CamNoSignal,    QStringLiteral("Нет сигнала") },
        { DevUnreachable, QStringLiteral("Регистратор недоступен") },
        { DevAuthFailed,  QStringLiteral("Неверный логин или пароль") },
    };
    return t.value(code, QStringLiteral("Неизвестная ошибка"));
}

QString withCode(int code) {
    return text(code) + QStringLiteral(" (E%1)").arg(code);
}

}
