#pragma once
#include <QWidget>
#include <QMap>
#include <QVector>
#include "Types.h"

class VideoWall;
struct CamInfo;
class QComboBox;
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
             int defaultLayout = 4, int bufferMs = 300, QWidget* parent = nullptr);

    int  deviceId() const { return dev_.id; }
    void setActive(bool on);              // вкладка выбрана: запустить/остановить потоки
    void setHwDecode(bool on);            // из настроек, на лету
    void setBuffer(int ms);               // буфер сглаживания видео, мс (на лету)
    void setConnTimeout(int ms);          // таймаут подключения к потоку, мс (на лету)
    void setShowTitles(bool on);          // подписи камер в ячейках (на лету)
    void setScaleMode(bool stretch);      // масштаб: false=оригинал, true=заполнить
    void setOpenAllMode(bool autoFit, int maxCells);  // режим «Открыть все»
    void updateDevice(const Device& dev); // данные устройства изменились (имя/адрес)
    QVector<int> shownChannels() const;   // каналы по слотам стены (-1 = пусто) — для сессии
    QString currentLayoutKey() const;     // текущая раскладка стены ("16"/"5x4") — для сессии
    void restoreShown(const QVector<int>& chans, const QString& layoutKey);  // восстановить
    void addCustomLayoutButton(int rows, int cols);
    void removeCustomLayoutButton(const QString& key);
    bool inFullscreen() const { return fullscreen_; }
    void enterFullscreen() { if (!fullscreen_) toggleFullscreen(); }  // режим поста при старте
    void exitFullscreen();

signals:
    void fullscreenToggled(bool on);      // спрятать/вернуть шапку главного окна
    void camerasUpdated(int deviceId, QVector<CamRef> cams);  // имена/IP -> в конфиг
    void layoutAdded(const QString& key); // сохранена новая сетка "RxC"
    void layoutChanged(int deviceId, const QString& key);  // выбрана раскладка -> в конфиг рега

private:
    void buildUi();
    QVector<CamInfo> camInfos() const;    // камеры устройства -> CamInfo (имена/URL/статус)
    void applyDevice();                   // камеры устройства -> стена (URL из данных)
    void populateOrgTree();
    void refreshTreeIcons();
    void onTreeActivated(QTreeWidgetItem* it, int col);
    void updatePageLabel();
    void toggleFullscreen();
    void openLayoutDialog();

    void applyLayoutKey(const QString& key);   // применить "1/4/9/16" или "RxC" к стене

    Device       dev_;
    bool         hwDecode_ = false;
    int          defaultLayout_ = 4;
    int          bufferMs_ = 300;
    QStringList  layouts_;

    VideoWall*   wall_        = nullptr;
    QWidget*     livePanel_   = nullptr;
    QWidget*     liveToolbar_ = nullptr;
    QTreeWidget* orgTree_     = nullptr;
    QComboBox*   scaleCombo_  = nullptr;
    QLabel*      pageLbl_     = nullptr;
    QHBoxLayout* barLay_      = nullptr;
    QPushButton* editBtn_     = nullptr;
    QVector<QTreeWidgetItem*> camItems_;
    QMap<QString, QPushButton*> customBtns_;
    bool  fullscreen_ = false;
};
