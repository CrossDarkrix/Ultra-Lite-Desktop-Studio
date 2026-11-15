#include "WidgetWindow.h"
#include <QColorDialog>
#include <QContextMenuEvent>
#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>

WidgetWindow::WidgetWindow(WidgetConfig cfg, QWidget *parent)
    : QWidget(parent), cfg_(std::move(cfg)) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnBottomHint);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    // Never take activation (v0.9.6): a click on a widget used to
    // activate it, which raised it within the bottom band, triggered a
    // restack, and made the taskbar repaint once when coming back from
    // an application window (user report: desktop -> app -> desktop =
    // taskbar flickers). Mouse input is delivered without activation,
    // so buttons, dragging and the icon proxy are unaffected; only
    // foreground churn goes away.
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setGeometry(cfg_.x, cfg_.y, std::max(40, cfg_.w), std::max(30, cfg_.h));
    setMouseTracking(true);
    // Rescue (v0.14.7): an html_js widget saved with bg_alpha 0
    // (possible up to v0.14.6) is invisible and unclickable on load.
    // Restore a visible, operable alpha for that type only.
    if (cfg_.type == QLatin1String("html_js") && cfg_.bgAlpha <= 0) cfg_.bgAlpha = 155;
    // The window itself is always fully opaque (v0.13.1): transparency
    // lives only in the background color's alpha (bgAlpha, used by
    // drawPanel), so no setWindowOpacity here or anywhere else.
}

void WidgetWindow::applyBgAlpha(bool save) {
    cfg_.bgAlpha = std::clamp(cfg_.bgAlpha, 0, 255);
    update();
    if (save) emit requestSave();
}

void WidgetWindow::addOpacityMenu(QMenu &menu) {
    // The effects overlay has no background panel — its scene is the
    // content, and its elements carry their own alpha settings in the
    // effects editor — so a background-transparency menu would be a
    // no-op there. Every other widget gets the menu.
    if (cfg_.type == QLatin1String("effects_overlay")) return;
    // JSHTML (v0.14.7): the plugin view covers the whole window and
    // its page is transparent, so bgAlpha shows through the page
    // itself. At 0% the widget becomes fully invisible AND the
    // per-pixel hit test of the translucent window stops delivering
    // clicks — the widget can no longer be operated or restored
    // (user report). Other widgets keep opaque content (text, icons)
    // that stays visible and clickable; a JSHTML page has no such
    // guaranteed content. Exclude html_js like the effects overlay;
    // pages control their own background inside the HTML.
    if (cfg_.type == QLatin1String("html_js")) return;
    QMenu *sub = menu.addMenu(tr("透明度（背景のみ）"));
    const int cur = cfg_.bgAlphaPercent();
    for (int pct : {100, 90, 80, 70, 60, 50, 40, 30, 20, 10, 0}) {
        QAction *a = sub->addAction(tr("%1%").arg(pct));
        a->setCheckable(true);
        a->setChecked(cur == pct);
        connect(a, &QAction::triggered, this, [this, pct] {
            cfg_.setBgAlphaPercent(pct);
            applyBgAlpha(true);
        });
    }
    sub->addSeparator();
    QAction *dlg = sub->addAction(tr("透明度を設定…"));
    connect(dlg, &QAction::triggered, this, [this] { openOpacityDialog(); });
}

void WidgetWindow::openOpacityDialog() {
    QDialog dlg(this);
    dlg.setWindowTitle(tr("透明度の設定（背景のみ）"));
    auto *slider = new QSlider(Qt::Horizontal);
    slider->setRange(0, 100);
    auto *spin = new QSpinBox;
    spin->setRange(0, 100);
    spin->setSuffix(QStringLiteral("%"));
    const int orig = cfg_.bgAlphaPercent();
    slider->setValue(orig);
    spin->setValue(orig);
    connect(slider, &QSlider::valueChanged, this, [this, spin](int v) {
        QSignalBlocker b(spin);
        spin->setValue(v);
        cfg_.setBgAlphaPercent(v);
        applyBgAlpha(false); // live preview; saved on OK
    });
    connect(spin, &QSpinBox::valueChanged, this, [this, slider](int v) {
        QSignalBlocker b(slider);
        slider->setValue(v);
        cfg_.setBgAlphaPercent(v);
        applyBgAlpha(false);
    });
    auto *note = new QLabel(tr("背景色にだけ適用されます。文字・アイコン・バーなど中身はそのまま表示されます。"));
    note->setWordWrap(true);
    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("背景の不透明度")));
    row->addWidget(slider, 1);
    row->addWidget(spin);
    auto *ok = new QPushButton(tr("OK"));
    auto *cancel = new QPushButton(tr("キャンセル"));
    connect(ok, &QPushButton::clicked, &dlg, &QDialog::accept);
    connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    auto *btns = new QHBoxLayout;
    btns->addStretch();
    btns->addWidget(ok);
    btns->addWidget(cancel);
    auto *root = new QVBoxLayout(&dlg);
    root->addWidget(note);
    root->addLayout(row);
    root->addLayout(btns);
    if (dlg.exec() == QDialog::Accepted) {
        applyBgAlpha(true);
    } else {
        cfg_.setBgAlphaPercent(orig);
        applyBgAlpha(false);
    }
}

void WidgetWindow::setEditMode(bool on) {
    editMode_ = on;
    onEditModeChanged();
}

void WidgetWindow::applyLayerFlags(bool widgetsOnTop) {
    // Three bands (v0.9.11): the effects overlay stays at the very
    // bottom; normal widgets are PLAIN windows by default — the
    // "gadget layer" just above the desktop shell and just below
    // application windows. Plain + WA_ShowWithoutActivating is
    // self-maintaining: apps open above them, clicks never move them,
    // and the shell never rises above them, so nothing needs periodic
    // re-seating (which was the flicker source all along). The tray
    // toggle still offers the topmost band for those who want it.
    const bool isFx = type() == QLatin1String("effects_overlay");
    Qt::WindowFlags want = Qt::FramelessWindowHint | Qt::Tool;
    if (isFx)
        want |= Qt::WindowStaysOnBottomHint;
    else if (widgetsOnTop)
        want |= Qt::WindowStaysOnTopHint;
    if (windowFlags() == want) return;
    const bool wasVisible = isVisible();
    setWindowFlags(want);
    if (wasVisible) show();
}

void WidgetWindow::drawPanel(QPainter &p, const QRect &rect) const {
    QColor bg(cfg_.bg);
    bg.setAlpha(std::max(0, std::min(255, cfg_.bgAlpha)));
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawRoundedRect(rect.adjusted(0, 0, -1, -1), 16, 16);
    if (editMode_) {
        // Selected widget (v0.15.0): solid, thicker frame — it is
        // the one Delete will remove. Others keep the dashed frame.
        p.setPen(selected_ ? QPen(accent(), 2.5) : QPen(accent(), 1.5, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(rect.adjusted(1, 1, -2, -2), 16, 16);
        // resize grip (bottom-right)
        p.setPen(Qt::NoPen);
        p.setBrush(accent());
        p.drawRect(rect.right() - 13, rect.bottom() - 13, 10, 10);
    }
}

void WidgetWindow::drawTitle(QPainter &p, const QRect &rect) const {
    // Same title block as every Python widget: Segoe UI 10 bold, near-white,
    // at (+14, +10), vertically centered in a 24px band.
    if (cfg_.title.isEmpty()) return;
    p.setFont(QFont(QStringLiteral("Segoe UI"), 10, QFont::Bold));
    p.setPen(QColor(245, 248, 255));
    p.drawText(QRectF(rect.left() + 14, rect.top() + 10, rect.width() - 28, 24),
               Qt::AlignLeft | Qt::AlignVCenter, cfg_.title);
}

void WidgetWindow::paintEvent(QPaintEvent *) {
    QPainter p(this);
    const QRect r = rect();
    drawPanel(p, r);
    paintContent(p, r);
}

static bool inGrip(const QRect &r, const QPoint &pos) {
    return pos.x() >= r.right() - 16 && pos.y() >= r.bottom() - 16;
}

void WidgetWindow::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) return;
    emit requestSelect(this);
    if (onPressed(e->pos())) { // interactive control hit (e.g. media buttons)
        pressHandled_ = true;
        return;
    }
    pressPos_ = e->globalPosition().toPoint();
    moved_ = false;
    if (editMode_) {
        // The effects overlay is NOT editable (v0.15.1, the user's
        // request): it is a full-screen canvas, so any accidental
        // drag in edit mode shifts the whole scene off the screen.
        // No drag, no resize — it stays exactly where it belongs.
        if (cfg_.type != QLatin1String("effects_overlay")) {
            startGeom_ = geometry();
            if (inGrip(rect(), e->pos())) {
                resizing_ = true;
            } else {
                dragging_ = true;
                dragOffset_ = pressPos_ - frameGeometry().topLeft();
            }
        }
    }
}

void WidgetWindow::mouseMoveEvent(QMouseEvent *e) {
    if (!editMode_ || (!dragging_ && !resizing_)) return;
    const QPoint gp = e->globalPosition().toPoint();
    if ((gp - pressPos_).manhattanLength() > 3) moved_ = true;
    if (dragging_) {
        move(gp - dragOffset_);
    } else if (resizing_) {
        const int nw = std::max(60, startGeom_.width() + (gp.x() - pressPos_.x()));
        const int nh = std::max(40, startGeom_.height() + (gp.y() - pressPos_.y()));
        resize(nw, nh);
    }
}

void WidgetWindow::mouseReleaseEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) return;
    if (pressHandled_) { // action already fired on press; swallow the release
        pressHandled_ = false;
        return;
    }
    const bool wasEditing = dragging_ || resizing_;
    dragging_ = resizing_ = false;
    if (editMode_) {
        if (wasEditing && moved_) {
            cfg_.x = x(); cfg_.y = y(); cfg_.w = width(); cfg_.h = height();
            emit requestSave();
        }
        return;
    }
    if (!moved_) onClicked(e->pos());
}

void WidgetWindow::wheelEvent(QWheelEvent *e) {
    if (!editMode_) onWheel(e->angleDelta().y());
}

void WidgetWindow::contextMenuEvent(QContextMenuEvent *e) {
    showContextMenu(e->globalPos());
}

void WidgetWindow::showContextMenu(const QPoint &globalPos) {
    QMenu menu;
    QAction *editAct = menu.addAction(editMode_ ? tr("編集モードを終了") : tr("編集モード (移動/リサイズ)"));
    menu.addSeparator();
    addContextActions(menu);
    menu.addSeparator();
    QAction *colorAct = menu.addAction(tr("アクセント色を変更…"));
    QAction *bgAct = menu.addAction(tr("背景色を変更…"));
    addOpacityMenu(menu);
    QAction *removeAct = menu.addAction(tr("このウィジェットを削除"));
    menu.addSeparator();
    QAction *saveAct = menu.addAction(tr("設定を保存"));
    QAction *quitAct = menu.addAction(tr("終了"));
    QAction *chosen = menu.exec(globalPos);
    if (chosen == editAct) {
        emit requestToggleEdit();
    } else if (chosen == colorAct) {
        const QColor c = QColorDialog::getColor(accent(), this, tr("アクセント色"));
        if (c.isValid()) { cfg_.color = c.name(); update(); emit requestSave(); }
    } else if (chosen == bgAct) {
        const QColor c = QColorDialog::getColor(QColor(cfg_.bg), this, tr("背景色"));
        if (c.isValid()) { cfg_.bg = c.name(); update(); emit requestSave(); }
    } else if (chosen == removeAct) {
        emit requestRemove(this);
    } else if (chosen == saveAct) {
        emit requestSave();
    } else if (chosen == quitAct) {
        emit requestQuit();
    }
}
