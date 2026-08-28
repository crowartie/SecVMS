#include "VideoWall.h"
#include "VideoCell.h"
#include <QGridLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QIcon>
#include <QDir>
#include <QCoreApplication>
#include <QTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <cmath>
#include <algorithm>

static const char* kCamMime = "application/x-secvms-cam";

VideoWall::VideoWall(QWidget* parent)
    : QWidget(parent)
{
    setStyleSheet("background:#0a0d10;");
    setAcceptDrops(true);                    // приём перетаскивания из дерева
    grid_ = new QGridLayout(this);
    grid_->setSpacing(1);                    // тонкий зазор, как в Smart PSS
    grid_->setContentsMargins(2, 2, 3, 2);   // 2px от списка камер, 3px справа

    // запуск ячеек по очереди, чтобы не открывать все RTSP-соединения разом
    startTimer_ = new QTimer(this);
    startTimer_->setSingleShot(true);
    connect(startTimer_, &QTimer::timeout, this, &VideoWall::kickStart);

    auto* t = new QTimer(this);
    connect(t, &QTimer::timeout, this, &VideoWall::reconnectTick);
    t->start(5000);
}

void VideoWall::setCameras(const QVector<CamInfo>& cams) {
    // полная замена набора камер (смена устройства): остановить и пересоздать ячейки
    pending_.clear();
    if (startTimer_) startTimer_->stop();
    for (auto* c : cells_) { c->stop(); c->deleteLater(); }
    cells_.clear();
    cams_ = cams;
    for (int i = 0; i < cams_.size(); ++i) {
        auto* c = new VideoCell(this);
        c->setTitle(cams_[i].name);
        c->setStretch(stretch_);
        if (cams_[i].status == 0) c->setOffline(true);   // офлайн по данным регистратора
        connect(c, &VideoCell::doubleClicked,  this, &VideoWall::onCellDouble);
        connect(c, &VideoCell::clicked,        this, &VideoWall::onCellClicked);
        connect(c, &VideoCell::statusChanged,  this, &VideoWall::onCellStatus);
        connect(c, &VideoCell::closeRequested, this, &VideoWall::onCellClose);
        cells_.push_back(c);
    }
    slotCam_.clear();
    slotCam_.fill(-1, cams_.size());   // стена пустая, пока не перетащат регистратор
    populated_ = false;
    selectedCam_ = -1;
    page_ = 0;
    zoomed_ = -1;
    relayout();
    emit layoutChanged();
}

QStringList VideoWall::cameraNames() const {
    QStringList s;
    for (auto& c : cams_) s << c.name;
    return s;
}

QString VideoWall::streamUrlFor(int i, bool big) const {
    const CamInfo& c = cams_[i];
    if (big || useMain_) return c.main.isEmpty() ? c.sub : c.main;  // основной поток
    return c.sub.isEmpty() ? c.main : c.sub;                        // субпоток
}

int VideoWall::pageCount() const {
    if (!populated_) return 1;
    int nSlots = slotCam_.size();
    int shown = (pageCells_ <= 0) ? nSlots : pageCells_;
    if (shown <= 0) return 1;
    return qMax(1, (nSlots + shown - 1) / shown);
}

void VideoWall::setPage(int p) {
    int pc = pageCount();
    if (p < 0) p = 0;
    if (p > pc - 1) p = pc - 1;
    if (p == page_) return;
    page_ = p;
    zoomed_ = -1;
    relayout();
    emit layoutChanged();
}

void VideoWall::setStretch(bool on) {
    stretch_ = on;
    for (auto* c : cells_) c->setStretch(on);   // только отрисовка, поток не трогаем
}

void VideoWall::setHwDecode(bool on) {
    if (hwAll_ == on) return;
    hwAll_ = on;
    if (active_ && populated_) restartAll();    // перезапустить с новым декодером
}

void VideoWall::populate() {
    for (int i = 0; i < cams_.size(); ++i) slotCam_[i] = i;   // от первой клетки по порядку
    populated_ = true;
    page_ = 0;
    zoomed_ = -1;
    relayout();
    emit layoutChanged();
}

void VideoWall::restartAll() {
    for (auto* c : cells_) c->stop();
    applyStreams();
}

QSet<int> VideoWall::visibleSet() const {
    QSet<int> s;
    if (!populated_) return s;
    if (zoomed_ >= 0) { s.insert(zoomed_); return s; }
    int nSlots = slotCam_.size();
    int shown = (pageCells_ <= 0) ? nSlots : pageCells_;
    int start = page_ * shown;
    for (int k = 0; k < shown; ++k) {
        int slot = start + k;
        if (slot >= nSlots) break;
        int cam = slotCam_[slot];
        if (cam >= 0) s.insert(cam);
    }
    return s;
}

void VideoWall::showWall() {
    active_ = true;
    applyStreams();
}

void VideoWall::hideWall() {
    active_ = false;
    pending_.clear();
    if (startTimer_) startTimer_->stop();
    for (auto* c : cells_) c->stop();   // освободить регистратор/канал, пока не смотрим
}

void VideoWall::applyStreams() {
    if (!active_) return;
    QSet<int> vis = visibleSet();
    bool big = (zoomed_ >= 0) || (pageCells_ == 1);
    // остановить то, что больше не видно; при зуме камеры страницы НЕ глушим —
    // возврат в сетку тогда мгновенный (всё уже играет)
    if (zoomed_ < 0)
        for (int i = 0; i < cells_.size(); ++i)
            if (!vis.contains(i) && cells_[i]->isPlaying())
                cells_[i]->stop();
    // видимые — в очередь на запуск (порядок по индексу)
    pending_.clear();
    QList<int> order = vis.values();
    std::sort(order.begin(), order.end());
    for (int i : order) {
        if (cams_[i].status == 0) continue;   // офлайн по регистратору — не стримим
        const QString url = streamUrlFor(i, big);
        if (!cells_[i]->isPlaying() || cells_[i]->url() != url)
            pending_.append(i);
    }
    kickStart();
}

void VideoWall::kickStart() {
    if (pending_.isEmpty()) return;
    int i = pending_.takeFirst();
    bool big = (zoomed_ >= 0) || (pageCells_ == 1);
    if (visibleSet().contains(i))
        cells_[i]->play(streamUrlFor(i, big), hwAll_);  // GPU только по флагу настроек:
                                                        // D3D11VA на этом железе подвисает
    if (!pending_.isEmpty())
        startTimer_->start(100);   // следующая ячейка через 100 мс
}

void VideoWall::setLayout(int cells) {
    pageCells_ = cells;   // 0 = все
    rowsOv_ = colsOv_ = 0;
    page_ = 0;
    zoomed_ = -1;
    relayout();
    emit layoutChanged();
}

void VideoWall::setLayoutRC(int rows, int cols) {
    if (rows < 1 || cols < 1) return;
    rowsOv_ = rows; colsOv_ = cols;
    pageCells_ = rows * cols;
    page_ = 0;
    zoomed_ = -1;
    relayout();
    emit layoutChanged();
}

void VideoWall::focusCamera(int index) {
    if (index < 0 || index >= cells_.size()) return;
    zoomed_ = index;
    relayout();
    emit layoutChanged();
}

void VideoWall::setCameraTitles(const QStringList& titles) {
    for (int i = 0; i < cams_.size() && i < titles.size(); ++i) {
        if (titles[i].trimmed().isEmpty()) continue;
        cams_[i].name = titles[i].trimmed();
        cells_[i]->setTitle(cams_[i].name);
    }
}

int VideoWall::camStatus(int i) const {
    return (i >= 0 && i < cells_.size()) ? cells_[i]->status() : VideoCell::Connecting;
}

void VideoWall::onCellClicked(VideoCell* c) {
    int idx = cells_.indexOf(c);
    selectedCam_ = idx;
    for (int i = 0; i < cells_.size(); ++i)
        cells_[i]->setSelected(i == idx);
    emit cameraSelected(idx);
}

void VideoWall::onCellStatus(VideoCell* c) {
    int idx = cells_.indexOf(c);
    if (idx >= 0) emit cameraStatusChanged(idx, c->status());
}

void VideoWall::onCellClose(VideoCell* c) {
    int cam = cells_.indexOf(c);
    if (cam < 0) return;
    if (zoomed_ == cam) zoomed_ = -1;            // закрыли развёрнутую — вернуться в сетку
    for (int s = 0; s < slotCam_.size(); ++s)
        if (slotCam_[s] == cam) slotCam_[s] = -1;
    if (selectedCam_ == cam) { selectedCam_ = -1; c->setSelected(false); }
    relayout();                                   // applyStreams остановит её поток
    emit layoutChanged();
}

void VideoWall::onCellDouble(VideoCell* c) {
    int idx = cells_.indexOf(c);
    if (zoomed_ >= 0) {          // вернуть сетку
        zoomed_ = -1;
        relayout();
        emit layoutChanged();
    } else {
        focusCamera(idx);
    }
}

QWidget* VideoWall::emptyCell(int n) {
    while (empties_.size() <= n) {
        auto* f = new QWidget(this);
        f->setAutoFillBackground(true);
        QPalette pal = f->palette();
        pal.setColor(QPalette::Window, QColor("#16191d"));   // пустой слот чуть светлее фона
        f->setPalette(pal);
        // тёмная иконка-заглушка камеры по центру, как в оригинале
        auto* l = new QVBoxLayout(f);
        l->setContentsMargins(0, 0, 0, 0);
        auto* ic = new QLabel(f);
        QString svg = QDir(QCoreApplication::applicationDirPath()).filePath("assets/cam_empty.svg");
        ic->setPixmap(QIcon(svg).pixmap(72, 72));
        ic->setAlignment(Qt::AlignCenter);
        l->addWidget(ic);
        empties_.push_back(f);
    }
    return empties_[n];
}

void VideoWall::relayout() {
    // спрятать ВСЕ ячейки (включая те, что не попадали в layout — иначе висят в (0,0))
    for (auto* c : cells_) c->hide();
    for (auto* e : empties_) e->hide();
    slotWidgets_.clear();
    while (grid_->count()) {
        auto* it = grid_->takeAt(0);
        delete it;
    }
    int rows = 1, cols = 1;
    if (zoomed_ >= 0) {
        grid_->addWidget(cells_[zoomed_], 0, 0);
        cells_[zoomed_]->show();
    } else {
        int nSlots = slotCam_.size();
        int shown = (pageCells_ <= 0) ? qMax(1, nSlots) : pageCells_;
        if (rowsOv_ > 0 && colsOv_ > 0) { rows = rowsOv_; cols = colsOv_; }
        else {
            cols = (int)std::ceil(std::sqrt((double)shown));
            rows = (int)std::ceil((double)shown / cols);
        }
        int start = page_ * shown;
        int ne = 0;
        // сетка ЖЁСТКАЯ rows×cols: камеры вписываются в слоты,
        // пустые слоты — заглушки (размер ячеек не зависит от числа камер)
        for (int k = 0; k < rows * cols && k < shown; ++k) {
            int slot = start + k;
            int cam = (slot < nSlots) ? slotCam_[slot] : -1;
            QWidget* w = (cam >= 0) ? (QWidget*)cells_[cam] : emptyCell(ne++);
            grid_->addWidget(w, k / cols, k % cols);
            w->show();
            slotWidgets_.push_back(w);
        }
    }
    // равные доли всем строкам/столбцам (и сброс старых факторов)
    for (int r = 0; r < 12; ++r) grid_->setRowStretch(r, r < rows ? 1 : 0);
    for (int c = 0; c < 12; ++c) grid_->setColumnStretch(c, c < cols ? 1 : 0);
    applyStreams();   // подтянуть потоки под новый видимый набор
}

void VideoWall::reconnectTick() {
    if (!active_) return;
    for (auto* c : cells_)
        if (c->isVisible() && c->isPlaying()) c->ensureAlive();
}

// ---- перетаскивание из дерева «Организация» ----

void VideoWall::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasFormat(kCamMime)) e->acceptProposedAction();
}

void VideoWall::dropEvent(QDropEvent* e) {
    if (!e->mimeData()->hasFormat(kCamMime)) return;
    int cam = e->mimeData()->data(kCamMime).toInt();
    e->acceptProposedAction();
    if (cam < 0) { populate(); return; }        // регистратор/группа: все от первой клетки
    if (cam >= cams_.size()) return;

    // одиночная камера — в слот под курсором (иначе в первый)
    QPoint pos = e->position().toPoint();
    int shown = (pageCells_ <= 0) ? qMax(1, (int)slotCam_.size()) : pageCells_;
    int slot = page_ * shown;                   // по умолчанию первый слот страницы
    for (int k = 0; k < slotWidgets_.size(); ++k)
        if (slotWidgets_[k]->geometry().contains(pos)) { slot = page_ * shown + k; break; }
    while (slotCam_.size() <= slot) slotCam_.append(-1);

    // если камера уже стояла в другом слоте — убрать оттуда
    for (int s = 0; s < slotCam_.size(); ++s)
        if (slotCam_[s] == cam) slotCam_[s] = -1;
    slotCam_[slot] = cam;
    populated_ = true;
    zoomed_ = -1;
    relayout();
    emit layoutChanged();
}
