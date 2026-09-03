#include "LiveView.h"
#include "VideoWall.h"
#include "XmClient.h"
#include "DahuaUtil.h"
#include "StreamUrl.h"
#include "Theme.h"
#include <QApplication>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QMenu>
#include <QLineEdit>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFrame>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QMimeData>
#include <QDialog>
#include <QSpinBox>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QTimer>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QAuthenticator>
#include <QSharedPointer>
#include <functional>

static QPixmap lvIconPix(const QString& name, int px) {
    QString path = QDir(QApplication::applicationDirPath()).filePath("assets/" + name + ".svg");
    return QIcon(path).pixmap(px, px);
}

// ---- превью сетки для «Польз. план» ----
class GridPreview : public QWidget {
public:
    explicit GridPreview(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumSize(300, 220);
    }
    void setRC(int r, int c) { r_ = r; c_ = c; update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor("#2b2f36"));
        QPen dot(QColor("#4a515a")); dot.setStyle(Qt::DotLine);
        p.setPen(dot);
        for (int i = 1; i < 12; ++i) {
            int x = width()  * i / 12, y = height() * i / 12;
            p.drawLine(x, 0, x, height());
            p.drawLine(0, y, width(), y);
        }
        p.setPen(QPen(QColor("#b6d436"), 2));
        for (int i = 1; i < c_; ++i) { int x = width()  * i / c_; p.drawLine(x, 0, x, height()); }
        for (int i = 1; i < r_; ++i) { int y = height() * i / r_; p.drawLine(0, y, width(), y); }
        p.setPen(QPen(QColor("#5a6270"), 1));
        p.drawRect(rect().adjusted(0, 0, -1, -1));
    }
private:
    int r_ = 4, c_ = 4;
};

class LayoutDialog : public QDialog {
public:
    explicit LayoutDialog(QWidget* parent = nullptr) : QDialog(parent) {
        setWindowTitle(QStringLiteral("Польз. план"));
        auto* v = new QVBoxLayout(this);
        auto* top = new QHBoxLayout;
        top->addStretch();
        top->addWidget(new QLabel(QStringLiteral("Строки:")));
        rows_ = new QSpinBox; rows_->setRange(1, 8); rows_->setValue(4);
        top->addWidget(rows_);
        top->addSpacing(14);
        top->addWidget(new QLabel(QStringLiteral("Столбцы:")));
        cols_ = new QSpinBox; cols_->setRange(1, 8); cols_->setValue(4);
        top->addWidget(cols_);
        v->addLayout(top);

        auto* mid = new QHBoxLayout;
        auto* list = new QListWidget; list->setFixedWidth(110);
        struct P { const char* t; int r, c; };
        for (auto pr : { P{"1",1,1}, P{"4",2,2}, P{"6",2,3}, P{"8",2,4}, P{"9",3,3},
                         P{"16",4,4}, P{"20",4,5}, P{"25",5,5}, P{"36",6,6}, P{"64",8,8} }) {
            auto* it = new QListWidgetItem(QString::fromUtf8(pr.t), list);
            it->setData(Qt::UserRole, pr.r);
            it->setData(Qt::UserRole + 1, pr.c);
        }
        connect(list, &QListWidget::currentItemChanged, this,
                [this](QListWidgetItem* it, QListWidgetItem*){
            if (!it) return;
            rows_->setValue(it->data(Qt::UserRole).toInt());
            cols_->setValue(it->data(Qt::UserRole + 1).toInt());
        });
        mid->addWidget(list);
        prev_ = new GridPreview;
        mid->addWidget(prev_, 1);
        v->addLayout(mid);

        auto sync = [this]{ prev_->setRC(rows_->value(), cols_->value()); };
        connect(rows_, QOverload<int>::of(&QSpinBox::valueChanged), this, sync);
        connect(cols_, QOverload<int>::of(&QSpinBox::valueChanged), this, sync);
        sync();

        auto* btns = new QHBoxLayout; btns->addStretch();
        auto* ok = new QPushButton("OK"); ok->setObjectName("primary");
        auto* cancel = new QPushButton(QStringLiteral("Отмена")); cancel->setObjectName("ghost");
        connect(ok, &QPushButton::clicked, this, &QDialog::accept);
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        btns->addWidget(ok); btns->addWidget(cancel);
        v->addLayout(btns);
    }
    int rowsV() const { return rows_->value(); }
    int colsV() const { return cols_->value(); }
private:
    QSpinBox* rows_ = nullptr;
    QSpinBox* cols_ = nullptr;
    GridPreview* prev_ = nullptr;
};

// дерево с перетаскиванием камер на стену
class OrgTree : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;
protected:
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override {
        if (items.isEmpty()) return nullptr;
        int v = items.first()->data(0, Qt::UserRole).toInt();
        if (v < -1) v = -1;
        auto* md = new QMimeData;
        md->setData("application/x-secvms-cam", QByteArray::number(v));
        return md;
    }
};

// ------------------------------------------------------------------

LiveView::LiveView(const Device& dev, bool hwDecode, const QStringList& layouts,
                   int defaultLayout, int bufferMs, QWidget* parent)
    : QWidget(parent), dev_(dev), hwDecode_(hwDecode),
      defaultLayout_(defaultLayout), bufferMs_(bufferMs), layouts_(layouts)
{
    buildUi();
    if (wall_) wall_->setBuffer(bufferMs_);
    // раскладка рега: если у устройства сохранена своя — применяем её, иначе дефолт
    if (!dev_.layout.isEmpty())
        applyLayoutKey(dev_.layout);
    else if (defaultLayout_ == 1 || defaultLayout_ == 4 ||
             defaultLayout_ == 9 || defaultLayout_ == 16)
        wall_->setLayout(defaultLayout_);
    applyDevice();
    for (const QString& key : layouts_) {
        const QStringList p = key.split('x');
        if (p.size() == 2) addCustomLayoutButton(p[0].toInt(), p[1].toInt());
    }
}

void LiveView::applyLayoutKey(const QString& key) {
    if (!wall_) return;
    if (key.contains('x')) {
        const QStringList p = key.split('x');
        if (p.size() == 2) wall_->setLayoutRC(p[0].toInt(), p[1].toInt());
    } else {
        wall_->setLayout(key.toInt());
    }
}

void LiveView::setBuffer(int ms) {
    bufferMs_ = ms;
    if (wall_) wall_->setBuffer(ms);
}

void LiveView::setConnTimeout(int ms) {
    if (wall_) wall_->setConnTimeout(ms);
}

void LiveView::setShowTitles(bool on) {
    if (wall_) wall_->setShowTitles(on);
}

void LiveView::setScaleMode(bool stretch) {
    if (scaleCombo_) scaleCombo_->setCurrentIndex(stretch ? 1 : 0);   // сработает его connect
    else if (wall_)  wall_->setStretch(stretch);
}

void LiveView::setOpenAllMode(bool autoFit, int maxCells) {
    if (wall_) wall_->setOpenAllMode(autoFit, maxCells);
}

void LiveView::buildUi() {
    auto* hmain = new QHBoxLayout(this); hmain->setContentsMargins(0,0,0,0); hmain->setSpacing(0);

    // левая панель: белый бокс с поиском и деревом
    livePanel_ = new QWidget; livePanel_->setObjectName("orgpanel"); livePanel_->setFixedWidth(224);
    livePanel_->setAttribute(Qt::WA_StyledBackground);
    auto* lv = new QVBoxLayout(livePanel_);
    lv->setContentsMargins(8, 8, 6, 8);
    auto* box = new QFrame; box->setObjectName("orgBox");
    box->setAttribute(Qt::WA_StyledBackground);
    auto* bl = new QVBoxLayout(box); bl->setContentsMargins(8, 8, 8, 8); bl->setSpacing(8);
    auto* search = new QLineEdit; search->setObjectName("orgSearch"); search->setPlaceholderText("Поиск...");
    bl->addWidget(search);
    orgTree_ = new OrgTree; orgTree_->setHeaderHidden(true); orgTree_->setIndentation(14);
    orgTree_->setDragEnabled(true);
    orgTree_->setDragDropMode(QAbstractItemView::DragOnly);
    bl->addWidget(orgTree_, 1);
    lv->addWidget(box, 1);
    connect(orgTree_, &QTreeWidget::itemDoubleClicked, this, &LiveView::onTreeActivated);
    // ПКМ по регистратору (корень дерева): открыть все / только в сети / закрыть все
    orgTree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(orgTree_, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint& pos){
        if (!wall_) return;
        QTreeWidgetItem* it = orgTree_->itemAt(pos);
        if (!it || it->data(0, Qt::UserRole).toInt() != -1) return;  // только узел регистратора
        QMenu menu(this);
        menu.setStyleSheet(Theme::menuQss());
        QAction* aAll    = menu.addAction(QStringLiteral("Открыть все камеры"));
        QAction* aOnline = menu.addAction(QStringLiteral("Открыть все камеры, которые в сети"));
        menu.addSeparator();
        QAction* aClose  = menu.addAction(QStringLiteral("Закрыть все камеры"));
        aClose->setEnabled(wall_->hasDisplayed());   // серым, если нет выведенных
        QAction* ch = menu.exec(orgTree_->viewport()->mapToGlobal(pos));
        if      (ch == aAll)    wall_->openAll(false);
        else if (ch == aOnline) wall_->openAll(true);
        else if (ch == aClose) {
            if (Theme::Opt::confirmCloseAll &&
                QMessageBox::question(this, QStringLiteral("SecVMS"),
                    QStringLiteral("Закрыть все камеры?")) != QMessageBox::Yes) return;
            wall_->closeAll();
        }
    });
    connect(search, &QLineEdit::textChanged, this, [this](const QString& q){
        for (int i = 0; i < orgTree_->topLevelItemCount(); ++i) {
            std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* it){
                bool leaf = it->data(0, Qt::UserRole).toInt() >= 0;
                if (leaf) it->setHidden(!q.isEmpty() &&
                    !it->text(0).contains(q, Qt::CaseInsensitive));
                for (int k = 0; k < it->childCount(); ++k) walk(it->child(k));
            };
            walk(orgTree_->topLevelItem(i));
        }
    });
    hmain->addWidget(livePanel_);

    // правая часть: стена + панель
    auto* right = new QWidget;
    auto* v = new QVBoxLayout(right); v->setContentsMargins(0,0,0,0); v->setSpacing(0);

    wall_ = new VideoWall();
    wall_->setHwDecode(hwDecode_);
    connect(wall_, &VideoWall::layoutChanged, this, [this]{
        updatePageLabel(); refreshTreeIcons();
    });
    connect(wall_, &VideoWall::cameraStatusChanged, this,
            [this](int, int){ refreshTreeIcons(); });
    connect(wall_, &VideoWall::cameraSelected, this, [this](int cam){
        if (orgTree_ && cam >= 0 && cam < camItems_.size())
            orgTree_->setCurrentItem(camItems_[cam]);
    });
    v->addWidget(wall_, 1);

    liveToolbar_ = new QWidget; liveToolbar_->setObjectName("livebar"); liveToolbar_->setFixedHeight(44);
    auto* th = new QHBoxLayout(liveToolbar_); th->setContentsMargins(8,0,8,0); th->setSpacing(6);
    auto lbtn = [&](const QString& ic, const QString& tip){
        auto* b = new QPushButton; b->setObjectName("lbtn");
        QIcon icn(lvIconPix(ic, 18));
        QString hov = QDir(QApplication::applicationDirPath()).filePath("assets/" + ic + "_h.svg");
        if (QFile::exists(hov)) icn.addPixmap(QIcon(hov).pixmap(18, 18), QIcon::Active);
        b->setIcon(icn); b->setIconSize(QSize(18,18));
        b->setToolTip(tip); b->setCursor(Qt::PointingHandCursor);
        return b;
    };
    th->addStretch();
    auto* prev = lbtn("chevron_left", "Пред. страница");
    pageLbl_ = new QLabel("1/1"); pageLbl_->setObjectName("pagelbl"); pageLbl_->setAlignment(Qt::AlignCenter);
    auto* next = lbtn("chevron_right", "След. страница");
    connect(prev, &QPushButton::clicked, this, [this]{ if (wall_) wall_->setPage(wall_->currentPage()-1); });
    connect(next, &QPushButton::clicked, this, [this]{ if (wall_) wall_->setPage(wall_->currentPage()+1); });
    th->addWidget(prev); th->addWidget(pageLbl_); th->addWidget(next);
    th->addStretch();

    scaleCombo_ = new QComboBox;
    auto* scale = scaleCombo_;
    scale->addItem("Оригинал"); scale->addItem("Полноэкранный режим");
    scale->setItemData(0, "Оригинал", Qt::ToolTipRole);
    scale->setItemData(1, "Полноэкранный режим", Qt::ToolTipRole);
    scale->setFixedWidth(112);
    scale->setToolTip(scale->currentText());
    connect(scale, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, scale](int i){
                if (wall_) wall_->setStretch(i == 1);
                scale->setToolTip(scale->currentText());
            });
    th->addWidget(scale);
    struct L { const char* ic; int n; };
    for (auto l : { L{"grid1",1}, L{"grid4",4}, L{"grid9",9}, L{"grid16",16} }) {
        auto* b = lbtn(l.ic, QString::number(l.n));
        connect(b, &QPushButton::clicked, this, [this,l]{
            if (wall_) { wall_->setLayout(l.n); emit layoutChanged(dev_.id, wall_->layoutKey()); }
        });
        th->addWidget(b);
    }
    editBtn_ = lbtn("edit", QStringLiteral("Польз. план (своя сетка)"));
    connect(editBtn_, &QPushButton::clicked, this, [this]{ openLayoutDialog(); });
    th->addWidget(editBtn_);
    auto* fs = lbtn("fullscreen", "Полноэкранный (Esc — выход)");
    connect(fs, &QPushButton::clicked, this, [this]{ toggleFullscreen(); });
    th->addWidget(fs);
    barLay_ = th;

    v->addWidget(liveToolbar_);
    hmain->addWidget(right, 1);
    updatePageLabel();
}

QVector<CamInfo> LiveView::camInfos() const {
    QVector<CamInfo> cams;
    for (const auto& c : dev_.cams) {
        CamInfo ci;
        ci.name = c.name.isEmpty() ? QString("Камера %1").arg(c.channel) : c.name;
        ci.sub  = streamUrl(dev_, c.channel, false);
        ci.main = streamUrl(dev_, c.channel, true);
        ci.status = c.status;   // состояние по данным регистратора
        ci.udp    = dev_.rtspUdp;   // транспорт RTSP устройства
        ci.directMode = dev_.directCams;   // для подписи «почему через регистратор»
        // ПРЯМОЕ подключение: если включено и адреса камеры известны — прямые URL,
        // а ретрансляция регистратора остаётся запасом для автоотката
        if (dev_.directCams && !c.directSub.isEmpty()) {
            ci.direct = true;
            ci.fbSub  = ci.sub;  ci.fbMain = ci.main;
            ci.sub    = c.directSub;
            ci.main   = c.directMain.isEmpty() ? c.directSub : c.directMain;
        }
        cams << ci;
    }
    return cams;
}

void LiveView::applyDevice() {
    wall_->setCameras(camInfos());   // полная пересборка стены (сброс показа)
    populateOrgTree();
}

void LiveView::updateDevice(const Device& dev) {
    // Три уровня обновления при повторном заходе/опросе регистратора:
    //  1) СТРУКТУРНОЕ (адрес/порт/логин/число камер) — меняются URL потоков,
    //     нужна полная пересборка стены (applyDevice).
    //  2) МЕТА (только имя/IP/состояние каналов) — обновляем на месте (refreshMeta):
    //     выведенный набор камер и раскладка СОХРАНЯЮТСЯ, переключение вкладок
    //     не сбрасывает показ, но статусы/подписи/офлайн-пометки свежие.
    //  3) без изменений — только перерисовать дерево.
    const bool structural =
        dev.ip != dev_.ip || dev.rtspPort != dev_.rtspPort ||
        dev.user != dev_.user || dev.pass != dev_.pass ||
        dev.rtspUdp != dev_.rtspUdp ||        // смена транспорта = перезапуск потоков
        dev.cams.size() != dev_.cams.size();
    bool meta = (dev.directCams != dev_.directCams);   // вкл/выкл прямого режима = смена URL
    if (!structural && !meta)
        for (int i = 0; i < dev.cams.size(); ++i)
            if (dev.cams[i].status     != dev_.cams[i].status     ||
                dev.cams[i].name       != dev_.cams[i].name       ||
                dev.cams[i].ip         != dev_.cams[i].ip         ||
                dev.cams[i].directSub  != dev_.cams[i].directSub  ||
                dev.cams[i].directMain != dev_.cams[i].directMain) { meta = true; break; }
    dev_ = dev;
    if (structural)      applyDevice();
    else if (meta)     { wall_->refreshMeta(camInfos()); populateOrgTree(); }
    else                 populateOrgTree();
}

QVector<int> LiveView::shownChannels() const {
    // выведенный набор камер как НОМЕРА КАНАЛОВ (индексы нестабильны при переопросе)
    QVector<int> out;
    if (!wall_ || !wall_->isPopulated()) return out;
    for (int cam : wall_->slotCams())
        out << (cam >= 0 && cam < dev_.cams.size() ? dev_.cams[cam].channel : -1);
    return out;
}

QString LiveView::currentLayoutKey() const {
    return wall_ ? wall_->layoutKey() : QString();
}

void LiveView::restoreShown(const QVector<int>& chans, const QString& layoutKey) {
    if (!wall_ || chans.isEmpty()) return;
    if (!layoutKey.isEmpty()) applyLayoutKey(layoutKey);   // та же сетка, что была при закрытии
    QVector<int> cells;   // не "slots" — это макрос Qt
    for (int ch : chans) {
        int idx = -1;
        if (ch > 0)
            for (int i = 0; i < dev_.cams.size(); ++i)
                if (dev_.cams[i].channel == ch) { idx = i; break; }
        cells << idx;
    }
    wall_->setSlotCams(cells);
}

void LiveView::setHwDecode(bool on) {
    hwDecode_ = on;
    if (wall_) wall_->setHwDecode(on);   // видимые потоки перезапустятся сами
}

void LiveView::setActive(bool on) {
    if (!wall_) return;
    if (on) {
        wall_->showWall();   // имена/состояние уже получены при открытии (openDeviceView)
    } else {
        if (fullscreen_) exitFullscreen();
        wall_->hideWall();
    }
}

void LiveView::populateOrgTree() {
    if (!orgTree_ || !wall_) return;
    orgTree_->clear();
    camItems_.clear();
    auto* dev = new QTreeWidgetItem(orgTree_);
    QString rootText = dev_.name.isEmpty() ? dev_.ip : dev_.name;
    QString tip = dev_.ip;
    if (dev_.directCams) {            // постоянный индикатор прямого режима: сколько камер с адресами
        int n = 0;
        for (const auto& c : dev_.cams) if (!c.directSub.isEmpty()) ++n;
        rootText += QStringLiteral("  [напрямую %1/%2]").arg(n).arg(dev_.cams.size());
        tip += QStringLiteral("\nПрямой режим: адреса определены у %1 из %2 камер").arg(n).arg(dev_.cams.size());
    } else tip += QStringLiteral("\nВидео через регистратор (прямой режим выключен в Настройках)");
    dev->setText(0, rootText);
    dev->setToolTip(0, tip);
    dev->setIcon(0, QIcon(lvIconPix("devices", 15)));
    dev->setData(0, Qt::UserRole, -1);
    const QStringList names = wall_->cameraNames();
    for (int i = 0; i < names.size(); ++i) {
        auto* c = new QTreeWidgetItem(dev);
        c->setText(0, names[i]);
        c->setIcon(0, QIcon(lvIconPix("cam_idle", 15)));
        c->setData(0, Qt::UserRole, i);
        camItems_.push_back(c);
    }
    orgTree_->expandAll();
    refreshTreeIcons();
}

void LiveView::refreshTreeIcons() {
    if (!wall_) return;
    for (int i = 0; i < camItems_.size(); ++i) {
        const char* ic = "cam_idle";
        if (wall_->camStatus(i) == 2)            ic = "cam_err";
        else if (wall_->isCameraVisible(i))      ic = "cam_on";
        camItems_[i]->setIcon(0, QIcon(lvIconPix(ic, 15)));
    }
}

void LiveView::onTreeActivated(QTreeWidgetItem* it, int) {
    if (!it || !wall_) return;
    int idx = it->data(0, Qt::UserRole).toInt();
    if (idx >= 0)       wall_->focusCamera(idx);
    else                wall_->populate();
}

void LiveView::updatePageLabel() {
    if (!wall_ || !pageLbl_) return;
    pageLbl_->setText(QString("%1/%2")
        .arg(wall_->currentPage() + 1).arg(wall_->pageCount()));
}

void LiveView::toggleFullscreen() {
    fullscreen_ = !fullscreen_;
    if (livePanel_)   livePanel_->setVisible(!fullscreen_);
    if (liveToolbar_) liveToolbar_->setVisible(!fullscreen_);
    emit fullscreenToggled(fullscreen_);
}

void LiveView::exitFullscreen() {
    if (fullscreen_) toggleFullscreen();
}

void LiveView::openLayoutDialog() {
    LayoutDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted || !wall_) return;
    int r = dlg.rowsV(), c = dlg.colsV();
    wall_->setLayoutRC(r, c);
    emit layoutChanged(dev_.id, wall_->layoutKey());
    const QString key = QString("%1x%2").arg(r).arg(c);
    if (customBtns_.contains(key)) return;
    auto ans = QMessageBox::question(this, QStringLiteral("Польз. план"),
        QStringLiteral("Сохранить сетку %1×%2 как кнопку на панели?").arg(r).arg(c),
        QMessageBox::Yes | QMessageBox::No);
    if (ans == QMessageBox::Yes) {
        addCustomLayoutButton(r, c);
        emit layoutAdded(key);     // конфиг и остальные вьюхи обновит MainWindow
        updatePageLabel();
    }
}

void LiveView::addCustomLayoutButton(int rows, int cols) {
    const QString key = QString("%1x%2").arg(rows).arg(cols);
    if (customBtns_.contains(key) || !barLay_ || !editBtn_) return;
    auto* b = new QPushButton(QString::number(rows * cols));
    b->setToolTip(QStringLiteral("Сетка %1×%2 (%3 камер)").arg(rows).arg(cols).arg(rows*cols));
    b->setCursor(Qt::PointingHandCursor);
    b->setStyleSheet(Theme::customBtnQss());
    connect(b, &QPushButton::clicked, this, [this, rows, cols]{
        if (wall_) { wall_->setLayoutRC(rows, cols); emit layoutChanged(dev_.id, wall_->layoutKey()); }
    });
    barLay_->insertWidget(barLay_->indexOf(editBtn_), b);
    customBtns_[key] = b;
}

void LiveView::removeCustomLayoutButton(const QString& key) {
    auto it = customBtns_.find(key);
    if (it == customBtns_.end()) return;
    it.value()->deleteLater();
    customBtns_.erase(it);
}

