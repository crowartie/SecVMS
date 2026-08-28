#pragma once
#include <QWidget>
#include <QMap>
#include <QVector>
#include "Types.h"

class VideoWall;
class QTreeWidget;
class QTreeWidgetItem;
class QLabel;
class QPushButton;
class QHBoxLayout;

// Страница просмотра ОДНОГО регистратора: дерево камер + видеостена + панель.
// На каждый открытый регистратор создаётся свой экземпляр (своя вкладка).
class LiveView : public QWidget {
    Q_OBJECT
public:
    LiveView(const Device& dev, bool hwDecode, const QStringList& layouts,
             int defaultLayout = 4, QWidget* parent = nullptr);

    int  deviceId() const { return dev_.id; }
    void setActive(bool on);              // вкладка выбрана: запустить/остановить потоки
    void setHwDecode(bool on);            // из настроек, на лету
    void updateDevice(const Device& dev); // данные устройства изменились (имя/адрес)
    void addCustomLayoutButton(int rows, int cols);
    bool inFullscreen() const { return fullscreen_; }
    void exitFullscreen();

signals:
    void fullscreenToggled(bool on);      // спрятать/вернуть шапку главного окна
    void camerasUpdated(int deviceId, QVector<CamRef> cams);  // имена/IP -> в конфиг
    void layoutAdded(const QString& key); // сохранена новая сетка "RxC"

private:
    void buildUi();
    void applyDevice();                   // камеры устройства -> стена (URL из данных)
    void populateOrgTree();
    void refreshTreeIcons();
    void onTreeActivated(QTreeWidgetItem* it, int col);
    void updatePageLabel();
    void toggleFullscreen();
    void openLayoutDialog();

    Device       dev_;
    bool         hwDecode_ = false;
    int          defaultLayout_ = 4;
    QStringList  layouts_;

    VideoWall*   wall_        = nullptr;
    QWidget*     livePanel_   = nullptr;
    QWidget*     liveToolbar_ = nullptr;
    QTreeWidget* orgTree_     = nullptr;
    QLabel*      pageLbl_     = nullptr;
    QHBoxLayout* barLay_      = nullptr;
    QPushButton* editBtn_     = nullptr;
    QVector<QTreeWidgetItem*> camItems_;
    QMap<QString, QPushButton*> customBtns_;
    bool  fullscreen_ = false;
};
