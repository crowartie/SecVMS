#pragma once
#include <QProxyStyle>
#include <QPainter>
#include <QPainterPath>
#include <QStyleOption>
#include <QtMath>

// Кастомный индикатор чекбокса/таблицы: рисуем сами (QSS image: на indicator в этой
// сборке Qt не рендерится). Пусто = белый квадрат с рамкой; включено = синяя заливка
// + белая ГАЛОЧКА. Всё остальное делегируется базовому стилю (Fusion).
class CheckStyle : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;
    void drawPrimitive(PrimitiveElement pe, const QStyleOption* opt,
                       QPainter* p, const QWidget* w) const override {
        if (pe == PE_IndicatorCheckBox || pe == PE_IndicatorItemViewItemCheck) {
            const bool on  = opt->state & State_On;
            const bool dis = !(opt->state & State_Enabled);
            const bool hov = opt->state & State_MouseOver;
            QRectF r(opt->rect);
            qreal side = qMin(r.width(), r.height());
            if (side > 17) { QPointF c = r.center(); r = QRectF(c.x()-8.5, c.y()-8.5, 17, 17); }
            r = r.adjusted(1, 1, -1, -1);
            p->save();
            p->setRenderHint(QPainter::Antialiasing, true);
            QColor border = dis ? QColor("#cfd5db")
                          : ((on || hov) ? QColor("#1f6fd6") : QColor("#8b93a0"));
            QColor bg = on ? (dis ? QColor("#a9c4e8") : QColor("#1f6fd6"))
                           : (dis ? QColor("#eef1f4") : QColor("#ffffff"));
            QPainterPath path; path.addRoundedRect(r, 3, 3);
            p->fillPath(path, bg);
            QPen bp(border); bp.setWidthF(1.2); p->setPen(bp); p->drawPath(path);
            if (on) {                                   // белая галочка
                QPen cp(QColor("#ffffff")); cp.setWidthF(2.0);
                cp.setCapStyle(Qt::RoundCap); cp.setJoinStyle(Qt::RoundJoin);
                p->setPen(cp);
                QPolygonF poly;
                poly << QPointF(r.x()+r.width()*0.24, r.y()+r.height()*0.54)
                     << QPointF(r.x()+r.width()*0.43, r.y()+r.height()*0.72)
                     << QPointF(r.x()+r.width()*0.78, r.y()+r.height()*0.30);
                p->drawPolyline(poly);
            }
            p->restore();
            return;
        }
        QProxyStyle::drawPrimitive(pe, opt, p, w);
    }
};
