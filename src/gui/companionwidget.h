#pragma once
#include <QLabel>
#include <QPushButton>
#include <QWidget>

// Small frameless always-on-top floating panel.
// Draggable by clicking on the CPU label area.
// Provides: show/hide window, maximize, gaming mode toggle, quit app, hide panel.
class CompanionWidget : public QWidget {
    Q_OBJECT
public:
    explicit CompanionWidget(QWidget *parent = nullptr);

    void updateCpu(const QList<double> &percpu);
    void setGamingActive(bool active);
    void setMainVisible(bool visible);

signals:
    void showHideRequested();
    void maximizeRequested();
    void gamingToggleRequested();
    void quitRequested();

protected:
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void paintEvent(QPaintEvent *) override;

private:
    QLabel      *m_cpuLabel  = nullptr;
    QPushButton *m_showBtn   = nullptr;
    QPushButton *m_maxBtn    = nullptr;
    QPushButton *m_gameBtn   = nullptr;
    QPushButton *m_quitBtn   = nullptr;
    QPushButton *m_closeBtn  = nullptr;   // hides the panel (not quit app)
    double       m_cpuPct    = 0.0;
    bool         m_gamingActive = false;
    QPoint       m_dragPos;

    void updateGameBtn();
    static QString normalBtnStyle();
    static QString activeBtnStyle();    // gaming-mode active state
};
