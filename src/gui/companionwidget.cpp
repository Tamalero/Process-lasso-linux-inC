#include "companionwidget.h"
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>

CompanionWidget::CompanionWidget(QWidget *parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFixedHeight(34);
    setCursor(Qt::SizeAllCursor);     // default cursor = drag hint

    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(10, 4, 8, 4);
    lay->setSpacing(5);

    m_cpuLabel = new QLabel(QStringLiteral("CPU —"), this);
    m_cpuLabel->setMinimumWidth(82);
    m_cpuLabel->setStyleSheet(
        QStringLiteral("color:#cdd6f4;background:transparent;font-weight:bold;"));
    lay->addWidget(m_cpuLabel);

    lay->addSpacing(6);

    auto makeBtn = [this](const QString &text, int fixedW = 0) -> QPushButton * {
        auto *b = new QPushButton(text, this);
        b->setFixedHeight(24);
        if (fixedW > 0) b->setFixedWidth(fixedW);
        b->setStyleSheet(normalBtnStyle());
        b->setCursor(Qt::ArrowCursor);
        return b;
    };

    m_showBtn  = makeBtn(QStringLiteral("Show"),   52);
    m_maxBtn   = makeBtn(QStringLiteral("Max"),    44);
    m_gameBtn  = makeBtn(QStringLiteral("▶ Game"), 74);
    m_quitBtn  = makeBtn(QStringLiteral("Quit"),   44);
    m_closeBtn = makeBtn(QStringLiteral("✕"),      26);

    lay->addWidget(m_showBtn);
    lay->addWidget(m_maxBtn);
    lay->addWidget(m_gameBtn);
    lay->addWidget(m_quitBtn);
    lay->addWidget(m_closeBtn);

    connect(m_showBtn,  &QPushButton::clicked, this, &CompanionWidget::showHideRequested);
    connect(m_maxBtn,   &QPushButton::clicked, this, &CompanionWidget::maximizeRequested);
    connect(m_gameBtn,  &QPushButton::clicked, this, &CompanionWidget::gamingToggleRequested);
    connect(m_quitBtn,  &QPushButton::clicked, this, &CompanionWidget::quitRequested);
    connect(m_closeBtn, &QPushButton::clicked, this, &CompanionWidget::hide);

    adjustSize();

    // Default position: top-right of the primary screen
    if (const QScreen *s = QGuiApplication::primaryScreen()) {
        const QRect sg = s->availableGeometry();
        move(sg.right() - width() - 20, sg.top() + 40);
    }
}

// ── public slots ─────────────────────────────────────────────────────────────

void CompanionWidget::updateCpu(const QList<double> &percpu)
{
    if (percpu.isEmpty()) return;
    double sum = 0;
    for (double v : percpu) sum += v;
    m_cpuPct = sum / percpu.size();

    const QString color = m_cpuPct > 80 ? QStringLiteral("#f38ba8")
                        : m_cpuPct > 40 ? QStringLiteral("#f9e2af")
                                        : QStringLiteral("#a6e3a1");
    m_cpuLabel->setText(QStringLiteral("CPU %1%").arg(m_cpuPct, 0, 'f', 1));
    m_cpuLabel->setStyleSheet(
        QStringLiteral("color:%1;background:transparent;font-weight:bold;").arg(color));
    update(); // repaint left-edge CPU bar
}

void CompanionWidget::setGamingActive(bool active)
{
    m_gamingActive = active;
    updateGameBtn();
}

void CompanionWidget::setMainVisible(bool visible)
{
    m_showBtn->setText(visible ? QStringLiteral("Hide") : QStringLiteral("Show"));
}

// ── private ───────────────────────────────────────────────────────────────────

void CompanionWidget::updateGameBtn()
{
    if (m_gamingActive) {
        m_gameBtn->setText(QStringLiteral("⏹ Game"));
        m_gameBtn->setStyleSheet(activeBtnStyle());
    } else {
        m_gameBtn->setText(QStringLiteral("▶ Game"));
        m_gameBtn->setStyleSheet(normalBtnStyle());
    }
}

QString CompanionWidget::normalBtnStyle()
{
    return QStringLiteral(
        "QPushButton{background:#313244;color:#cdd6f4;border:1px solid #45475a;"
        "border-radius:4px;padding:2px 6px;font-size:12px;}"
        "QPushButton:hover{background:#45475a;}"
        "QPushButton:pressed{background:#1e1e2e;}");
}

QString CompanionWidget::activeBtnStyle()
{
    return QStringLiteral(
        "QPushButton{background:#1e4a2a;color:#a6e3a1;border:1px solid #a6e3a1;"
        "border-radius:4px;padding:2px 6px;font-size:12px;}"
        "QPushButton:hover{background:#2a5c36;}"
        "QPushButton:pressed{background:#1e4a2a;}");
}

// ── mouse events (drag) ───────────────────────────────────────────────────────

void CompanionWidget::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton)
        m_dragPos = e->globalPosition().toPoint() - frameGeometry().topLeft();
    QWidget::mousePressEvent(e);
}

void CompanionWidget::mouseMoveEvent(QMouseEvent *e)
{
    if (e->buttons() & Qt::LeftButton)
        move(e->globalPosition().toPoint() - m_dragPos);
    QWidget::mouseMoveEvent(e);
}

// ── paint ─────────────────────────────────────────────────────────────────────

void CompanionWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Dark rounded background
    p.setPen(QPen(QColor(QStringLiteral("#313244")), 1));
    p.setBrush(QColor(QStringLiteral("#1e1e2e")));
    p.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 8, 8);

    // Thin CPU accent bar on the left edge, height proportional to load
    if (m_cpuPct > 0) {
        const int maxH = height() - 10;
        const int barH = qMax(2, static_cast<int>(maxH * m_cpuPct / 100.0));
        const QColor barColor = m_cpuPct > 80 ? QColor(QStringLiteral("#f38ba8"))
                              : m_cpuPct > 40 ? QColor(QStringLiteral("#f9e2af"))
                                              : QColor(QStringLiteral("#a6e3a1"));
        p.setPen(Qt::NoPen);
        p.setBrush(barColor);
        p.drawRoundedRect(2, height() - 5 - barH, 3, barH, 1, 1);
    }
}
