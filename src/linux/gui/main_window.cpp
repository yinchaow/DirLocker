#include "main_window.hpp"

#include "daemon_client.hpp"
#include "password_dialog.hpp"
#include "../../core/security_util.hpp"
#include "../../core/version.hpp"

#include <algorithm>
#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDesktopServices>
#include <QDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHelpEvent>
#include <QIcon>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>
#include <QPushButton>
#include <QStatusBar>
#include <QStyledItemDelegate>
#include <QTextBrowser>
#include <QTimer>
#include <QToolBar>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>

namespace dirlocker {

namespace {

constexpr int kRolePath = Qt::UserRole + 1;
constexpr int kRoleLocked = Qt::UserRole + 2;
constexpr int kRoleCustom = Qt::UserRole + 3;
constexpr int kCornerRadius = 6;

struct RowCellLayout {
    QRect statusChip;  // col 1: 28x28
    QRect tagRect;     // col 2: 88x28
    QRect actionBtn;   // col 3 slot 1: 64x28 (解锁 / 锁定)
    QRect openBtn;     // col 3 slot 2: 28x28 (打开)
    QRect moreBtn;     // col 3 slot 3: 28x28 (⋯)
};

RowCellLayout computeCellLayout(const QRect& rect, int col, bool locked) {
    RowCellLayout lay;
    const int cy = rect.top() + rect.height() / 2;
    if (col == 1) {
        const int sz = 28;
        lay.statusChip = QRect(rect.left() + (rect.width() - sz) / 2, cy - sz / 2, sz, sz);
    } else if (col == 2) {
        const int w = qMin(rect.width() - 10, 88);
        const int h = 28;
        lay.tagRect = QRect(rect.left() + (rect.width() - w) / 2, cy - h / 2, w, h);
    } else if (col == 3) {
        const int actionW = 64;
        const int iconW = 28;
        const int gap = 6;
        const int totalW = iconW + gap + actionW + gap + iconW;  // 132
        const int h = 28;
        int startX = rect.left() + (rect.width() - totalW) / 2;
        if (startX < rect.left() + 6) startX = rect.left() + 6;
        const int top = cy - h / 2;
        if (!locked) {
            lay.openBtn = QRect(startX, top, iconW, h);
        }
        lay.actionBtn = QRect(startX + iconW + gap, top, actionW, h);
        lay.moreBtn = QRect(startX + iconW + gap + actionW + gap, top, iconW, h);
    }
    return lay;
}

int hitCellSlot(const QRect& rect, int col, bool locked, const QPoint& pos) {
    RowCellLayout lay = computeCellLayout(rect, col, locked);
    if (col == 1 && lay.statusChip.contains(pos)) return 1;
    if (col == 2 && lay.tagRect.contains(pos)) return 1;
    if (col == 3) {
        if (lay.actionBtn.contains(pos)) return 1;
        if (!locked && lay.openBtn.contains(pos)) return 2;
        if (lay.moreBtn.contains(pos)) return 3;
    }
    return 0;
}

void drawStatusLockChip(QPainter* painter, const QRect& rc, bool locked) {
    const QColor bg = locked ? QColor(227, 242, 253) : QColor(232, 245, 233);
    const QColor border = locked ? QColor(144, 202, 249) : QColor(165, 214, 167);
    const QColor fg = locked ? QColor(13, 71, 161) : QColor(27, 94, 32);

    painter->setBrush(bg);
    painter->setPen(QPen(border, 1.0));
    painter->drawRoundedRect(rc, kCornerRadius, kCornerRadius);

    const qreal cx = rc.center().x() + 0.5;
    const qreal cy = rc.center().y() + 0.5;

    // Shackle arch
    QPen shacklePen(fg, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter->setPen(shacklePen);
    painter->setBrush(Qt::NoBrush);
    if (locked) {
        QPainterPath shackle;
        shackle.moveTo(cx - 3.6, cy - 1.0);
        shackle.lineTo(cx - 3.6, cy - 4.0);
        shackle.arcTo(QRectF(cx - 3.6, cy - 7.6, 7.2, 7.2), 180, -180);
        shackle.lineTo(cx + 3.6, cy - 1.0);
        painter->drawPath(shackle);
    } else {
        QPainterPath shackle;
        shackle.moveTo(cx - 3.6, cy - 1.0);
        shackle.lineTo(cx - 3.6, cy - 4.6);
        shackle.arcTo(QRectF(cx - 3.6, cy - 8.2, 7.2, 7.2), 180, -160);
        painter->drawPath(shackle);
    }

    // Lock body
    QRectF body(cx - 5.4, cy - 1.2, 10.8, 8.2);
    painter->setPen(Qt::NoPen);
    painter->setBrush(fg);
    painter->drawRoundedRect(body, 2.2, 2.2);

    // Keyhole dot
    painter->setBrush(bg);
    painter->drawEllipse(QPointF(cx, cy + 2.9), 1.35, 1.35);
}

void drawFolderActionIcon(QPainter* painter, const QRect& rc, bool hovered) {
    const QColor bg = hovered ? QColor(255, 236, 179) : QColor(255, 248, 225);       // #FFF8E1 / #FFECB3 light yellow chip
    const QColor border = hovered ? QColor(217, 119, 6) : QColor(246, 196, 83);      // #F6C453 / #D97706
    const QColor innerFill = hovered ? QColor(252, 211, 77) : QColor(253, 230, 138); // #FDE68A warm yellow fill
    const QColor fg = QColor(180, 83, 9);                                            // #B45309 deep amber bold stroke

    painter->setBrush(bg);
    painter->setPen(QPen(border, 1.0));
    painter->drawRoundedRect(rc, kCornerRadius, kCornerRadius);

    const qreal cx = rc.center().x() + 0.5;
    const qreal cy = rc.center().y() + 0.5;
    QPainterPath p;
    p.moveTo(cx - 6.2, cy - 4.4);
    p.lineTo(cx - 1.8, cy - 4.4);
    p.lineTo(cx - 0.3, cy - 2.4);
    p.lineTo(cx + 6.2, cy - 2.4);
    p.lineTo(cx + 6.2, cy + 4.6);
    p.lineTo(cx - 6.2, cy + 4.6);
    p.closeSubpath();

    painter->setPen(QPen(fg, 1.9, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->setBrush(innerFill);
    painter->drawPath(p);
    painter->drawLine(QPointF(cx - 6.0, cy - 1.3), QPointF(cx + 6.0, cy - 1.3));
}

void drawMoreDotsIcon(QPainter* painter, const QRect& rc, bool hovered) {
    const QColor bg = hovered ? QColor(243, 244, 246) : QColor(255, 255, 255);
    const QColor border = hovered ? QColor(175, 184, 193) : QColor(208, 215, 222);
    const QColor fg = hovered ? QColor(31, 35, 40) : QColor(95, 99, 104);

    painter->setBrush(bg);
    painter->setPen(QPen(border, 1.0));
    painter->drawRoundedRect(rc, kCornerRadius, kCornerRadius);

    const qreal cx = rc.center().x() + 0.5;
    const qreal cy = rc.center().y() + 0.5;
    painter->setPen(Qt::NoPen);
    painter->setBrush(fg);
    painter->drawEllipse(QPointF(cx - 4.5, cy), 1.4, 1.4);
    painter->drawEllipse(QPointF(cx, cy), 1.4, 1.4);
    painter->drawEllipse(QPointF(cx + 4.5, cy), 1.4, 1.4);
}

enum class MenuIconKind {
    Unlock,
    OpenFolder,
    Lock,
    Key,
    Switch,
    Trash
};

QPixmap renderMenuIconPixmap(MenuIconKind kind, int px) {
    QPixmap pm(px, px);
    pm.fill(Qt::transparent);
    QPainter painter(&pm);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const qreal s = static_cast<qreal>(px) / 16.0;
    QPen pen(QColor(51, 65, 85), 1.4 * s, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    switch (kind) {
        case MenuIconKind::Unlock: {
            QPainterPath sh;
            sh.moveTo(5.2 * s, 7.0 * s);
            sh.lineTo(5.2 * s, 4.8 * s);
            sh.arcTo(QRectF(5.2 * s, 2.2 * s, 5.6 * s, 5.0 * s), 180.0, -150.0);
            painter.drawPath(sh);
            painter.drawRoundedRect(QRectF(3.3 * s, 7.0 * s, 9.4 * s, 6.6 * s), 1.6 * s, 1.6 * s);
            painter.drawLine(QPointF(8.0 * s, 9.6 * s), QPointF(8.0 * s, 11.4 * s));
            break;
        }
        case MenuIconKind::OpenFolder: {
            QPolygonF poly;
            poly << QPointF(2.0 * s, 3.6 * s) << QPointF(6.4 * s, 3.6 * s)
                 << QPointF(7.8 * s, 5.1 * s) << QPointF(14.0 * s, 5.1 * s)
                 << QPointF(14.0 * s, 12.6 * s) << QPointF(2.0 * s, 12.6 * s);
            painter.drawPolygon(poly);
            painter.drawLine(QPointF(2.0 * s, 6.6 * s), QPointF(14.0 * s, 6.6 * s));
            break;
        }
        case MenuIconKind::Lock: {
            QPainterPath sh;
            sh.moveTo(5.2 * s, 7.0 * s);
            sh.lineTo(5.2 * s, 4.8 * s);
            sh.arcTo(QRectF(5.2 * s, 2.2 * s, 5.6 * s, 5.0 * s), 180.0, -180.0);
            sh.lineTo(10.8 * s, 7.0 * s);
            painter.drawPath(sh);
            painter.drawRoundedRect(QRectF(3.3 * s, 7.0 * s, 9.4 * s, 6.6 * s), 1.6 * s, 1.6 * s);
            painter.drawLine(QPointF(8.0 * s, 9.6 * s), QPointF(8.0 * s, 11.4 * s));
            break;
        }
        case MenuIconKind::Key: {
            painter.drawEllipse(QRectF(2.8 * s, 8.2 * s, 4.8 * s, 4.8 * s));
            painter.drawLine(QPointF(7.0 * s, 8.8 * s), QPointF(13.0 * s, 2.8 * s));
            painter.drawLine(QPointF(10.7 * s, 5.1 * s), QPointF(12.3 * s, 6.7 * s));
            painter.drawLine(QPointF(12.4 * s, 3.4 * s), QPointF(13.7 * s, 4.7 * s));
            break;
        }
        case MenuIconKind::Switch: {
            painter.drawLine(QPointF(2.6 * s, 5.2 * s), QPointF(13.2 * s, 5.2 * s));
            painter.drawLine(QPointF(10.7 * s, 3.0 * s), QPointF(13.2 * s, 5.2 * s));
            painter.drawLine(QPointF(10.7 * s, 7.4 * s), QPointF(13.2 * s, 5.2 * s));
            painter.drawLine(QPointF(13.4 * s, 10.8 * s), QPointF(2.8 * s, 10.8 * s));
            painter.drawLine(QPointF(5.3 * s, 8.6 * s), QPointF(2.8 * s, 10.8 * s));
            painter.drawLine(QPointF(5.3 * s, 13.0 * s), QPointF(2.8 * s, 10.8 * s));
            break;
        }
        case MenuIconKind::Trash: {
            painter.drawLine(QPointF(6.0 * s, 4.2 * s), QPointF(6.0 * s, 2.7 * s));
            painter.drawLine(QPointF(6.0 * s, 2.7 * s), QPointF(10.0 * s, 2.7 * s));
            painter.drawLine(QPointF(10.0 * s, 2.7 * s), QPointF(10.0 * s, 4.2 * s));
            painter.drawLine(QPointF(2.6 * s, 4.4 * s), QPointF(13.4 * s, 4.4 * s));
            QPainterPath bin;
            bin.moveTo(3.9 * s, 4.4 * s);
            bin.lineTo(4.6 * s, 13.2 * s);
            bin.lineTo(11.4 * s, 13.2 * s);
            bin.lineTo(12.1 * s, 4.4 * s);
            painter.drawPath(bin);
            painter.drawLine(QPointF(6.7 * s, 6.7 * s), QPointF(6.9 * s, 11.0 * s));
            painter.drawLine(QPointF(9.3 * s, 6.7 * s), QPointF(9.1 * s, 11.0 * s));
            break;
        }
    }
    return pm;
}

QIcon makeMenuIcon(MenuIconKind kind) {
    QIcon icon;
    for (int sz : {16, 24, 32}) {
        icon.addPixmap(renderMenuIconPixmap(kind, sz));
    }
    return icon;
}

}  // namespace

class EmptyTableOverlay : public QWidget {
public:
    explicit EmptyTableOverlay(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
    }

    void setFilteredEmpty(bool filtered) {
        if (filtered_empty_ == filtered) return;
        filtered_empty_ = filtered;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const int bw = qMin(width() - 48, 440);
        const int bh = qMin(height() - 32, 186);
        if (bw < 220 || bh < 120) return;

        const QRect box((width() - bw) / 2, (height() - bh) / 2, bw, bh);
        p.setBrush(QColor(248, 250, 252));
        p.setPen(QPen(QColor(203, 213, 225), 1.2, Qt::DashLine));
        p.drawRoundedRect(box, 10, 10);

        const int cx = box.center().x();
        const int iconCy = box.top() + 50;

        // Soft circular blue badge with crisp folder-lock vector
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(235, 243, 254));
        p.drawEllipse(QPoint(cx, iconCy), 24, 24);

        QPen fpen(QColor(13, 71, 161), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(fpen);
        p.setBrush(QColor(219, 234, 254));
        QPainterPath folder;
        folder.moveTo(cx - 12, iconCy - 8);
        folder.lineTo(cx - 4, iconCy - 8);
        folder.lineTo(cx - 1, iconCy - 4);
        folder.lineTo(cx + 12, iconCy - 4);
        folder.lineTo(cx + 12, iconCy + 9);
        folder.lineTo(cx - 12, iconCy + 9);
        folder.closeSubpath();
        p.drawPath(folder);

        // Mini padlock badge in center
        p.setBrush(QColor(13, 71, 161));
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(QRectF(cx - 4.0, iconCy + 0.5, 8.0, 6.0), 1.5, 1.5);
        p.setPen(QPen(QColor(13, 71, 161), 1.4));
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(cx - 2.6, iconCy - 3.2, 5.2, 5.2), 0, 180 * 16);

        QFont tf = font();
        tf.setPointSize(11);
        tf.setBold(true);
        p.setFont(tf);
        p.setPen(QColor(31, 35, 40));
        const QString title = filtered_empty_
            ? QStringLiteral("未找到匹配的受管目录")
            : QStringLiteral("将要保护的目录拖放到此处");
        p.drawText(QRect(box.left() + 16, iconCy + 34, box.width() - 32, 26),
                   Qt::AlignHCenter | Qt::AlignVCenter, title);

        QFont sf = font();
        sf.setPointSize(9);
        p.setFont(sf);
        p.setPen(QColor(95, 99, 104));
        const QString sub = filtered_empty_
            ? QStringLiteral("尝试修改或清空上方搜索栏关键字")
            : QStringLiteral("或点击左上角「+ 添加并锁定」 · 原目录内文件绝不被修改或加密");
        p.drawText(QRect(box.left() + 16, iconCy + 64, box.width() - 32, 42),
                   Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, sub);
    }

private:
    bool filtered_empty_ = false;
};

class DirTableDelegate : public QStyledItemDelegate {
public:
    explicit DirTableDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent),
          locked_icon_(QStringLiteral(":/icons/dirlocker-folder-32.png")),
          unlocked_icon_(QApplication::style()->standardIcon(QStyle::SP_DirIcon)) {}

    bool setHover(int row, int col, int slot) {
        if (hover_row_ == row && hover_col_ == col && hover_slot_ == slot) {
            return false;
        }
        hover_row_ = row;
        hover_col_ = col;
        hover_slot_ = slot;
        return true;
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);

        const QRect rect = option.rect;
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool rowHovered = option.state.testFlag(QStyle::State_MouseOver);

        if (selected) {
            painter->fillRect(rect, QColor(235, 243, 254));  // #EBF3FE
        } else if (rowHovered) {
            painter->fillRect(rect, QColor(246, 248, 250));  // #F6F8FA
        } else {
            painter->fillRect(rect, Qt::white);
        }

        // Subtle horizontal bottom hairline
        painter->setPen(QColor(238, 241, 244));
        painter->drawLine(rect.left(), rect.bottom(), rect.right(), rect.bottom());

        if (selected && index.column() == 0) {
            painter->fillRect(QRect(rect.left(), rect.top() + 4, 3, rect.height() - 8),
                              QColor(13, 71, 161));  // #0D47A1
        }

        const QString path = index.data(kRolePath).toString();
        const bool locked = index.data(kRoleLocked).toBool();
        const bool custom = index.data(kRoleCustom).toBool();
        const int r = index.row();
        const int c = index.column();
        const RowCellLayout lay = computeCellLayout(rect, c, locked);

        if (c == 0) {
            const QIcon& ic = locked ? locked_icon_ : unlocked_icon_;
            const int iconSz = 30;
            const int iconX = rect.left() + 14;
            const int iconY = rect.top() + (rect.height() - iconSz) / 2;
            ic.paint(painter, QRect(iconX, iconY, iconSz, iconSz));

            QFileInfo fi(path);
            QString folderName = fi.fileName();
            if (folderName.isEmpty()) folderName = path;

            const int textX = iconX + iconSz + 12;
            const int textW = qMax(40, rect.right() - textX - 12);

            QFont titleFont = option.font;
            titleFont.setBold(true);
            titleFont.setPointSizeF(titleFont.pointSizeF() + 0.5);
            painter->setFont(titleFont);
            painter->setPen(QColor(31, 35, 40));  // #1F2328
            QFontMetrics fmTitle(titleFont);
            const QString elidedName = fmTitle.elidedText(folderName, Qt::ElideRight, textW);
            painter->drawText(QRect(textX, rect.top() + 7, textW, 21),
                              Qt::AlignLeft | Qt::AlignVCenter, elidedName);

            QFont pathFont = option.font;
            pathFont.setPointSizeF(qMax(8.0, pathFont.pointSizeF() - 0.8));
            painter->setFont(pathFont);
            painter->setPen(QColor(95, 99, 104));  // #5F6368
            QFontMetrics fmPath(pathFont);
            const QString elidedPath = fmPath.elidedText(path, Qt::ElideMiddle, textW);
            painter->drawText(QRect(textX, rect.top() + 28, textW, 18),
                              Qt::AlignLeft | Qt::AlignVCenter, elidedPath);
        } else if (c == 1) {
            drawStatusLockChip(painter, lay.statusChip, locked);
        } else if (c == 2) {
            const bool hovTag = (hover_row_ == r && hover_col_ == 2 && hover_slot_ == 1);
            const QString text = custom ? QStringLiteral("独立密码") : QStringLiteral("主密码");
            const QColor dotColor = custom ? QColor(217, 119, 6) : QColor(100, 116, 139);
            const QColor fg = custom ? QColor(31, 35, 40) : QColor(71, 85, 105);

            if (hovTag) {
                const QColor hovBg = selected ? QColor(255, 255, 255) : QColor(241, 245, 249);
                const QColor hovBd = selected ? QColor(144, 202, 249) : QColor(203, 213, 225);
                painter->setBrush(hovBg);
                painter->setPen(QPen(hovBd, 1.0));
                painter->drawRoundedRect(lay.tagRect, kCornerRadius, kCornerRadius);
            }

            const qreal dotCx = lay.tagRect.left() + 16.0;
            const qreal dotCy = lay.tagRect.center().y() + 0.5;
            painter->setPen(Qt::NoPen);
            painter->setBrush(dotColor);
            painter->drawEllipse(QPointF(dotCx, dotCy), 3.4, 3.4);

            painter->setFont(option.font);
            painter->setPen(fg);
            QRect textRect(lay.tagRect.left() + 27, lay.tagRect.top(),
                           qMax(20, lay.tagRect.width() - 31), lay.tagRect.height());
            painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, text);
        } else if (c == 3) {
            const bool hov1 = (hover_row_ == r && hover_col_ == 3 && hover_slot_ == 1);
            const bool hov2 = (hover_row_ == r && hover_col_ == 3 && hover_slot_ == 2);
            const bool hov3 = (hover_row_ == r && hover_col_ == 3 && hover_slot_ == 3);

            QFont btnFont = option.font;
            btnFont.setBold(true);
            QString text;
            QColor bg, border, fg;
            if (locked) {
                text = QStringLiteral("解锁");
                bg = hov1 ? QColor(220, 242, 223) : QColor(232, 245, 233);
                border = hov1 ? QColor(46, 125, 50) : QColor(129, 199, 132);
                fg = QColor(27, 94, 32);
            } else {
                text = QStringLiteral("锁定");
                bg = hov1 ? QColor(220, 235, 250) : QColor(235, 243, 254);
                border = hov1 ? QColor(25, 118, 210) : QColor(144, 202, 249);
                fg = QColor(13, 71, 161);
            }
            painter->setBrush(bg);
            painter->setPen(QPen(border, 1.0));
            painter->drawRoundedRect(lay.actionBtn, kCornerRadius, kCornerRadius);
            painter->setFont(btnFont);
            painter->setPen(fg);
            painter->drawText(lay.actionBtn, Qt::AlignCenter, text);

            if (!locked) {
                drawFolderActionIcon(painter, lay.openBtn, hov2);
            }
            drawMoreDotsIcon(painter, lay.moreBtn, hov3);
        }

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QSize s = QStyledItemDelegate::sizeHint(option, index);
        s.setHeight(54);
        return s;
    }

private:
    QIcon locked_icon_;
    QIcon unlocked_icon_;
    int hover_row_ = -1;
    int hover_col_ = -1;
    int hover_slot_ = 0;
};

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("目录密匣 v" DIRLOCKER_VERSION_STR));
    setWindowIcon(createAppIcon(true));
    setAcceptDrops(true);
    resize(920, 560);
    setMinimumSize(760, 440);

    setupUi();
    setupTray();

    monitor_ = new IdleAndHotkeyMonitor(this);
    connect(monitor_, &IdleAndHotkeyMonitor::requestLockAll,
            this, &MainWindow::onAutoLockRequested);
    monitor_->start();

    refreshState();
}

QIcon MainWindow::createAppIcon(bool lockedTint) const {
    if (lockedTint) {
        QIcon ic;
        ic.addFile(QStringLiteral(":/icons/dirlocker-16.png"), QSize(16, 16));
        ic.addFile(QStringLiteral(":/icons/dirlocker-32.png"), QSize(32, 32));
        ic.addFile(QStringLiteral(":/icons/dirlocker-48.png"), QSize(48, 48));
        ic.addFile(QStringLiteral(":/icons/dirlocker-64.png"), QSize(64, 64));
        ic.addFile(QStringLiteral(":/icons/dirlocker-256.png"), QSize(256, 256));
        if (!ic.isNull()) return ic;
    }

    QPixmap pm(QStringLiteral(":/icons/dirlocker-64.png"));
    if (pm.isNull()) {
        pm = QPixmap(64, 64);
        pm.fill(Qt::transparent);
    } else {
        pm = pm.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    if (!lockedTint) {
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setBrush(QColor(46, 150, 92));
        p.setPen(QPen(Qt::white, 2.5));
        p.drawEllipse(QRectF(40, 40, 21, 21));
    }
    return QIcon(pm);
}

void MainWindow::setupUi() {
    auto* central = new QWidget(this);
    central->setObjectName(QStringLiteral("centralCanvas"));
    central->setStyleSheet(QStringLiteral(
        "#centralCanvas { background: #f6f8fa; }"
        "QPushButton { background: #ffffff; border: 1px solid #d0d7de; border-radius: 6px; "
        "  color: #1f2328; padding: 6px 12px; }"
        "QPushButton:hover { background: #f3f4f6; border-color: #afb8c1; }"
        "QPushButton:pressed { background: #ebecf0; }"
        "QPushButton:disabled { background: #f6f8fa; color: #9aa0a6; border-color: #e1e4e8; }"
        "QPushButton#primaryBtn { background: #0d47a1; border: 1px solid #0d47a1; "
        "  color: #ffffff; font-weight: 600; padding: 6px 15px; }"
        "QPushButton#primaryBtn:hover { background: #1565c0; border-color: #1565c0; }"
        "QPushButton#primaryBtn:pressed { background: #09357a; border-color: #09357a; }"
        "QPushButton#tonalAccentBtn { background: #ebf3fe; border: 1px solid #90caf9; "
        "  color: #0d47a1; font-weight: 600; padding: 6px 13px; }"
        "QPushButton#tonalAccentBtn:hover { background: #dcebfa; border-color: #1976d2; }"
        "QPushButton#tonalAccentBtn:pressed { background: #cbe2fb; border-color: #0d47a1; }"
        "QLineEdit#searchBox { background: #ffffff; border: 1px solid #d0d7de; border-radius: 6px; "
        "  padding: 5px 10px; color: #1f2328; }"
        "QLineEdit#searchBox:focus { border: 1px solid #0d47a1; }"
        "QFrame#tableCard { background: #ffffff; border: 1px solid #d8dee4; border-radius: 6px; }"
        "QTableWidget { background: #ffffff; border: none; outline: none; }"
        "QHeaderView::section { background: #f8fafc; color: #5f6368; font-weight: 600; "
        "  border: none; border-bottom: 1px solid #d8dee4; padding: 8px 12px; }"
    ));

    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(14, 12, 14, 10);
    root->setSpacing(10);

    // 1. Focused Top Header Bar (Primary + Tonal LockAll + Live Search | Settings menu + Help menu)
    auto* topBar = new QHBoxLayout();
    topBar->setSpacing(8);

    auto* addBtn = new QPushButton(QStringLiteral("＋ 添加并锁定"), this);
    addBtn->setObjectName(QStringLiteral("primaryBtn"));
    lock_all_btn_ = new QPushButton(QStringLiteral("一键全锁 (Ctrl+Alt+L)"), this);
    lock_all_btn_->setObjectName(QStringLiteral("tonalAccentBtn"));
    search_edit_ = new QLineEdit(this);
    search_edit_->setObjectName(QStringLiteral("searchBox"));
    search_edit_->setPlaceholderText(QStringLiteral("搜索目录名称或路径..."));
    search_edit_->setClearButtonEnabled(true);
    search_edit_->setMinimumWidth(210);
    search_edit_->setMaximumWidth(280);

    auto* settingsBtn = new QPushButton(QStringLiteral("设置"), this);
    auto* settingsMenu = new QMenu(settingsBtn);
    auto* secSetAct = settingsMenu->addAction(QStringLiteral("安全设置与快捷键..."));
    auto* healthAct = settingsMenu->addAction(QStringLiteral("保护区体检与一键修复..."));
    settingsMenu->addSeparator();
    auto* masterAct = settingsMenu->addAction(QStringLiteral("修改主密码..."));
    settingsBtn->setMenu(settingsMenu);

    auto* helpBtn = new QPushButton(QStringLiteral("帮助"), this);
    auto* helpMenu = new QMenu(helpBtn);
    auto* recAct = helpMenu->addAction(QStringLiteral("应急恢复说明..."));
    helpMenu->addSeparator();
    auto* aboutAct = helpMenu->addAction(QStringLiteral("关于 目录密匣..."));
    helpBtn->setMenu(helpMenu);

    topBar->addWidget(addBtn);
    topBar->addWidget(lock_all_btn_);
    topBar->addWidget(search_edit_);
    topBar->addStretch();
    topBar->addWidget(settingsBtn);
    topBar->addWidget(helpBtn);
    root->addLayout(topBar);

    // 2. Modern Two-Line Gridless Directory Table (Unified 6px Rounded Card)
    auto* tableCard = new QFrame(this);
    tableCard->setObjectName(QStringLiteral("tableCard"));
    auto* tableLayout = new QVBoxLayout(tableCard);
    tableLayout->setContentsMargins(2, 2, 2, 2);
    tableLayout->setSpacing(0);

    table_ = new QTableWidget(0, 4, this);
    auto mkHeader = [](const QString& text, Qt::Alignment align) {
        auto* it = new QTableWidgetItem(text);
        it->setTextAlignment(align);
        return it;
    };
    table_->setHorizontalHeaderItem(0, mkHeader(QStringLiteral("目录"), Qt::AlignLeft | Qt::AlignVCenter));
    table_->setHorizontalHeaderItem(1, mkHeader(QStringLiteral("状态"), Qt::AlignCenter));
    table_->setHorizontalHeaderItem(2, mkHeader(QStringLiteral("密码方式"), Qt::AlignCenter));
    table_->setHorizontalHeaderItem(3, mkHeader(QStringLiteral("操作"), Qt::AlignCenter));
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
    table_->setColumnWidth(1, 76);
    table_->setColumnWidth(2, 112);
    table_->setColumnWidth(3, 156);
    table_->setShowGrid(false);
    table_->setMouseTracking(true);
    table_->viewport()->setMouseTracking(true);
    table_->viewport()->installEventFilter(this);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(54);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    delegate_ = new DirTableDelegate(table_);
    table_->setItemDelegate(delegate_);
    empty_overlay_ = new EmptyTableOverlay(table_->viewport());
    empty_overlay_->setGeometry(table_->viewport()->rect());
    tableLayout->addWidget(table_);
    root->addWidget(tableCard, 1);

    // 3. Compact Bottom Status Bar
    auto* statusRow = new QHBoxLayout();
    statusRow->setContentsMargins(2, 0, 2, 0);
    summary_label_ = new QLabel(QStringLiteral("正在连接后台服务..."), this);
    summary_label_->setStyleSheet(QStringLiteral("color: #5f6368; font-size: 9pt;"));
    policy_label_ = new QLabel(this);
    policy_label_->setStyleSheet(QStringLiteral("color: #5f6368; font-size: 9pt;"));
    statusRow->addWidget(summary_label_);
    statusRow->addStretch();
    statusRow->addWidget(policy_label_);
    root->addLayout(statusRow);

    setCentralWidget(central);

    connect(addBtn, &QPushButton::clicked, this, &MainWindow::onAddAndLock);
    connect(lock_all_btn_, &QPushButton::clicked, this, &MainWindow::onLockAll);
    connect(search_edit_, &QLineEdit::textChanged, this, [this]() { rebuildTableView(); });
    connect(secSetAct, &QAction::triggered, this, &MainWindow::onOpenSettings);
    connect(healthAct, &QAction::triggered, this, &MainWindow::onHealthCheck);
    connect(masterAct, &QAction::triggered, this, &MainWindow::onChangeMasterPassword);
    connect(recAct, &QAction::triggered, this, &MainWindow::onShowRecoveryGuide);
    connect(aboutAct, &QAction::triggered, this, &MainWindow::onShowAbout);

    connect(table_, &QTableWidget::clicked, this, &MainWindow::onTableClicked);
    connect(table_, &QTableWidget::cellDoubleClicked, this, &MainWindow::onTableDoubleClicked);
    connect(table_, &QTableWidget::customContextMenuRequested, this, &MainWindow::onTableContextMenu);
}

void MainWindow::setupTray() {
    tray_ = new QSystemTrayIcon(createAppIcon(true), this);
    tray_->setToolTip(QStringLiteral("目录密匣"));

    auto* menu = new QMenu(this);
    auto* openAct = menu->addAction(QStringLiteral("打开主界面"));
    tray_lock_all_act_ = menu->addAction(QStringLiteral("一键全部锁定 (Ctrl+Alt+L)"));
    menu->addSeparator();
    auto* settingsAct = menu->addAction(QStringLiteral("安全设置与快捷键..."));
    auto* healthAct = menu->addAction(QStringLiteral("保护区体检与修复..."));
    auto* masterAct = menu->addAction(QStringLiteral("修改主密码..."));
    auto* aboutAct = menu->addAction(QStringLiteral("关于 目录密匣"));
    menu->addSeparator();
    auto* quitAct = menu->addAction(QStringLiteral("退出 目录密匣"));

    connect(openAct, &QAction::triggered, this, &MainWindow::showMainWindow);
    connect(tray_lock_all_act_, &QAction::triggered, this, &MainWindow::onLockAll);
    connect(settingsAct, &QAction::triggered, this, &MainWindow::onOpenSettings);
    connect(healthAct, &QAction::triggered, this, &MainWindow::onHealthCheck);
    connect(masterAct, &QAction::triggered, this, &MainWindow::onChangeMasterPassword);
    connect(aboutAct, &QAction::triggered, this, &MainWindow::onShowAbout);
    connect(quitAct, &QAction::triggered, this, &MainWindow::onQuitRequested);
    connect(tray_, &QSystemTrayIcon::activated, this, &MainWindow::onTrayActivated);
    connect(tray_, &QSystemTrayIcon::messageClicked, this, &MainWindow::showMainWindow);

    tray_->setContextMenu(menu);
    tray_->show();
}

void MainWindow::hideAndLockMainWindow() {
    if (require_master_on_open_) {
        main_window_unlocked_ = false;
    }
    hide();
}

void MainWindow::updateHotkeyDisplay() {
    if (lock_all_btn_) {
        lock_all_btn_->setText(QStringLiteral("一键全锁 (%1)").arg(hotkey_));
    }
    if (tray_lock_all_act_) {
        tray_lock_all_act_->setText(QStringLiteral("一键全部锁定 (%1)").arg(hotkey_));
    }
}

void MainWindow::showMainWindow() {
    refreshState();
    if (has_master_password_ && require_master_on_open_ && !main_window_unlocked_) {
        if (main_auth_prompt_open_) return;
        main_auth_prompt_open_ = true;
        QWidget* dlgParent = isVisible() ? this : nullptr;
        while (true) {
            VerifyMasterDialog dlg(
                QStringLiteral("打开主窗口 - 验证主密码"),
                QStringLiteral("已开启“打开主窗口验证主密码”，请输入主密码以解锁界面："),
                dlgParent);
            if (dlg.exec() != QDialog::Accepted) {
                main_auth_prompt_open_ = false;
                return;
            }
            JsonValue params = JsonValue::object();
            params.set("password", dlg.password().toStdString());
            auto reply = DaemonClient::send("verify_master", std::move(params));
            if (!reply.transport_ok) {
                showMessageZh(dlgParent, QMessageBox::Critical, QStringLiteral("错误"),
                              QString::fromStdString(reply.message));
                main_auth_prompt_open_ = false;
                return;
            }
            if (reply.ok) {
                main_window_unlocked_ = true;
                break;
            }
            const int cd = static_cast<int>(reply.data.get("cooldown_seconds").as_int(0));
            showMessageZh(dlgParent, QMessageBox::Warning,
                          cd > 0 ? QStringLiteral("请稍后再试") : QStringLiteral("密码错误"),
                          QString::fromStdString(reply.message));
            if (cd > 0) {
                main_auth_prompt_open_ = false;
                return;
            }
        }
        main_auth_prompt_open_ = false;
    }

    showNormal();
    raise();
    activateWindow();
    refreshState();
    ensureMasterPasswordInitialized();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (!really_quit_ && tray_ && tray_->isVisible()) {
        hideAndLockMainWindow();
        event->ignore();
        return;
    }
    event->accept();
}

void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event && event->type() == QEvent::WindowStateChange) {
        if (isMinimized() && require_master_on_open_ && tray_ && tray_->isVisible()) {
            QTimer::singleShot(0, this, &MainWindow::hideAndLockMainWindow);
        }
    }
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData() && event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void MainWindow::dropEvent(QDropEvent* event) {
    if (!event->mimeData() || !event->mimeData()->hasUrls()) return;

    QStringList dirs;
    QStringList stubs;
    QStringList nonDirs;
    for (const QUrl& u : event->mimeData()->urls()) {
        if (!u.isLocalFile()) {
            nonDirs << u.toString();
            continue;
        }
        QString local = u.toLocalFile();
        QFileInfo fi(local);
        if (fi.exists() && fi.isDir() && !fi.isSymLink()) {
            dirs << local;
        } else if (fi.exists() && fi.isFile() && !fi.isSymLink() &&
                   (local.endsWith(QStringLiteral(".desktop"), Qt::CaseInsensitive) ||
                    local.endsWith(QStringLiteral(".dlock"), Qt::CaseInsensitive))) {
            stubs << local;
        } else {
            nonDirs << local;
        }
    }

    if (!nonDirs.isEmpty()) {
        QStringList shown = nonDirs.mid(0, 6);
        QString extra = nonDirs.size() > 6
            ? QStringLiteral("\n等共 %1 项").arg(nonDirs.size())
            : QString();
        showMessageZh(
            this, QMessageBox::Warning, QStringLiteral("拖入错误"),
            QStringLiteral("拖入内容中包含非目录项目（请仅拖入真实目录）：\n• ") +
                shown.join(QStringLiteral("\n• ")) + extra);
        return;
    }

    if (!dirs.isEmpty()) {
        lockPathsBatchFlow(dirs);
    }
    if (!stubs.isEmpty() && dirs.isEmpty()) {
        if (stubs.size() == 1) {
            handleExternalCommand(QStringLiteral("unlock"), stubs.first());
        } else {
            showMessageZh(this, QMessageBox::Information, QStringLiteral("提示"),
                          QStringLiteral("请双击加密目录入口或逐个拖入解锁。"));
        }
    }
}

void MainWindow::onTrayActivated(QSystemTrayIcon::ActivationReason reason) {
    if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
        if (isVisible()) {
            hideAndLockMainWindow();
        } else {
            showMainWindow();
        }
    }
}

void MainWindow::refreshState() {
    auto reply = DaemonClient::send("status");
    if (!reply.transport_ok) {
        summary_label_->setText(QString::fromStdString(reply.message));
        return;
    }
    if (!reply.ok) {
        summary_label_->setText(QString::fromStdString(reply.message));
        return;
    }

    has_master_password_ = reply.data.get("has_master_password").as_bool(false);
    const auto& st = reply.data.get("settings");
    if (st.is_object()) {
        auto_relock_minutes_ = static_cast<int>(st.get("auto_relock_minutes").as_int(5));
        require_master_on_open_ = st.get("require_master_on_open").as_bool(false);
        const std::string rawHk = st.get("hotkey").as_string_or("Ctrl+Alt+L");
        HotkeySpec hkSpec;
        std::string normHk;
        if (HotkeySpec::parse(rawHk, hkSpec, &normHk)) {
            hotkey_ = QString::fromStdString(normHk);
        } else {
            hotkey_ = QStringLiteral("Ctrl+Alt+L");
        }
    }
    if (!require_master_on_open_) {
        main_window_unlocked_ = true;
    }
    if (monitor_) {
        monitor_->updatePolicy(auto_relock_minutes_);
        monitor_->setHotkey(hotkey_);
    }
    updateHotkeyDisplay();

    rows_.clear();
    const auto& arr = reply.data.get("entries");
    if (arr.is_array()) {
        for (const auto& item : arr.as_array()) {
            UiDirRow r;
            r.path = QString::fromStdString(item.get("path").as_string());
            r.locked = item.get("locked").as_bool(false);
            r.use_custom_password = item.get("use_custom_password").as_bool(false);
            rows_.push_back(std::move(r));
        }
    }

    rebuildTableView();
}

void MainWindow::rebuildTableView() {
    QString selectedPath;
    int prevIdx = selectedRowIndex();
    if (prevIdx >= 0 && prevIdx < static_cast<int>(rows_.size())) {
        selectedPath = rows_[static_cast<size_t>(prevIdx)].path;
    }

    const QString filter = search_edit_ ? search_edit_->text().trimmed() : QString();
    visible_indices_.clear();
    int locked_count = 0;
    int unlocked_count = 0;

    for (size_t i = 0; i < rows_.size(); ++i) {
        const auto& r = rows_[i];
        if (r.locked) ++locked_count;
        else ++unlocked_count;

        if (filter.isEmpty() || r.path.contains(filter, Qt::CaseInsensitive)) {
            visible_indices_.push_back(i);
        }
    }

    table_->setRowCount(static_cast<int>(visible_indices_.size()));
    int rowToSelect = -1;
    for (int vis = 0; vis < static_cast<int>(visible_indices_.size()); ++vis) {
        const auto& r = rows_[visible_indices_[static_cast<size_t>(vis)]];
        if (!selectedPath.isEmpty() && r.path == selectedPath) {
            rowToSelect = vis;
        }
        auto* dirItem = new QTableWidgetItem();
        dirItem->setData(kRolePath, r.path);
        dirItem->setData(kRoleLocked, r.locked);
        dirItem->setData(kRoleCustom, r.use_custom_password);
        dirItem->setToolTip(r.path);

        auto* stItem = new QTableWidgetItem();
        stItem->setData(kRolePath, r.path);
        stItem->setData(kRoleLocked, r.locked);
        stItem->setData(kRoleCustom, r.use_custom_password);

        auto* pwItem = new QTableWidgetItem();
        pwItem->setData(kRolePath, r.path);
        pwItem->setData(kRoleLocked, r.locked);
        pwItem->setData(kRoleCustom, r.use_custom_password);

        auto* actItem = new QTableWidgetItem();
        actItem->setData(kRolePath, r.path);
        actItem->setData(kRoleLocked, r.locked);
        actItem->setData(kRoleCustom, r.use_custom_password);

        table_->setItem(vis, 0, dirItem);
        table_->setItem(vis, 1, stItem);
        table_->setItem(vis, 2, pwItem);
        table_->setItem(vis, 3, actItem);
    }

    if (rowToSelect >= 0) {
        table_->selectRow(rowToSelect);
    }
    if (empty_overlay_) {
        empty_overlay_->setFilteredEmpty(!rows_.empty());
        empty_overlay_->setGeometry(table_->viewport()->rect());
        empty_overlay_->setVisible(visible_indices_.empty());
    }

    QString summary = QStringLiteral("受管目录 %1 个 · 已锁定 %2 · 已解锁 %3")
        .arg(rows_.size()).arg(locked_count).arg(unlocked_count);
    if (!filter.isEmpty()) {
        summary += QStringLiteral("（筛选显示 %1 项）").arg(visible_indices_.size());
    }
    summary_label_->setText(summary);

    if (policy_label_) {
        bool hkOk = monitor_ && monitor_->hotkeyRegistered();
        QString hkText = hkOk
            ? QStringLiteral("热键 %1 已就绪").arg(hotkey_)
            : QStringLiteral("热键 %1 未注册").arg(hotkey_);
        QString idleText = auto_relock_minutes_ > 0
            ? QStringLiteral("闲置 %1 分钟自动回锁").arg(auto_relock_minutes_)
            : QStringLiteral("闲置回锁已关闭");
        QString gateText = require_master_on_open_
            ? QStringLiteral("开窗需验密")
            : QStringLiteral("严格防占用开启");
        policy_label_->setText(hkText + QStringLiteral("  |  ") + idleText + QStringLiteral("  |  ") + gateText);
    }

    if (tray_) {
        tray_->setIcon(createAppIcon(unlocked_count == 0));
        tray_->setToolTip(
            QStringLiteral("目录密匣（已锁定 %1 / 已解锁 %2）")
                .arg(locked_count).arg(unlocked_count));
    }
}

bool MainWindow::ensureMasterPasswordInitialized() {
    refreshState();
    if (has_master_password_) return true;

    MasterPasswordDialog dlg(false, this);
    if (dlg.exec() != QDialog::Accepted) return false;

    JsonValue params = JsonValue::object();
    params.set("old_password", "");
    params.set("new_password", dlg.newPassword().toStdString());
    auto reply = DaemonClient::send("set_master_password", std::move(params));
    if (!reply.transport_ok || !reply.ok) {
        showMessageZh(this, QMessageBox::Critical, QStringLiteral("错误"),
                      QString::fromStdString(reply.message));
        return false;
    }
    refreshState();
    return true;
}

void MainWindow::openFolderInFileManager(const QString& path) {
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

bool MainWindow::lockNewPathFlow(const QString& path) {
    return lockPathsBatchFlow({path});
}

bool MainWindow::lockPathsBatchFlow(const QStringList& paths) {
    if (paths.isEmpty()) return false;
    if (!ensureMasterPasswordInitialized()) return false;

    QStringList newTargets;
    QStringList errors;
    int relocked = 0;

    for (const QString& p : paths) {
        JsonValue qp = JsonValue::object();
        qp.set("path", p.toStdString());
        auto q = DaemonClient::send("query", std::move(qp));
        if (!q.transport_ok || !q.ok) {
            errors << (p + QStringLiteral(" (") + QString::fromStdString(q.message) + QStringLiteral(")"));
            continue;
        }
        bool protectable = q.data.get("protectable").as_bool(false);
        if (!protectable) {
            errors << (p + QStringLiteral(" (") +
                       QString::fromStdString(q.data.get("reason").as_string()) + QStringLiteral(")"));
            continue;
        }
        QString canonical = QString::fromStdString(q.data.get("canonical_path").as_string_or(p.toStdString()));
        bool managed = q.data.get("managed").as_bool(false);
        bool locked = q.data.get("locked").as_bool(false);
        if (managed) {
            if (locked) {
                errors << (canonical + QStringLiteral(" (已处于锁定状态)"));
            } else {
                JsonValue lp = JsonValue::object();
                lp.set("path", canonical.toStdString());
                auto lr = DaemonClient::send("lock", std::move(lp));
                if (lr.transport_ok && lr.ok) ++relocked;
                else errors << (canonical + QStringLiteral(" (") + QString::fromStdString(lr.message) + QStringLiteral(")"));
            }
            continue;
        }
        if (!newTargets.contains(canonical)) {
            newTargets << canonical;
        }
    }

    int newlyLocked = 0;
    if (!newTargets.isEmpty()) {
        QString summary = (newTargets.size() == 1)
            ? newTargets.first()
            : QStringLiteral("批量锁定 %1 个目录：\n• ").arg(newTargets.size()) +
                  newTargets.mid(0, 3).join(QStringLiteral("\n• ")) +
                  (newTargets.size() > 3
                       ? QStringLiteral("\n等共 %1 个目录").arg(newTargets.size())
                       : QString());

        LockDirDialog dlg(summary, this);
        if (dlg.exec() != QDialog::Accepted) {
            if (relocked > 0) refreshState();
            return relocked > 0;
        }

        for (const QString& target : newTargets) {
            JsonValue params = JsonValue::object();
            params.set("path", target.toStdString());
            params.set("use_custom_password", dlg.useCustomPassword());
            if (dlg.useCustomPassword()) {
                params.set("password", dlg.customPassword().toStdString());
            }
            auto reply = DaemonClient::send("add_and_lock", std::move(params));
            if (reply.transport_ok && reply.ok) {
                ++newlyLocked;
            } else {
                errors << (target + QStringLiteral(" (") +
                           QString::fromStdString(reply.message) + QStringLiteral(")"));
            }
        }
    }

    refreshState();
    int total = newlyLocked + relocked;
    if (total > 0 && tray_) {
        QString msg = (total == 1 && paths.size() == 1 && newTargets.size() == 1)
            ? QStringLiteral("已锁定目录：\n") + newTargets.first()
            : QStringLiteral("成功锁定 %1 个目录").arg(total);
        tray_->showMessage(QStringLiteral("目录密匣"), msg, QSystemTrayIcon::Information, 2500);
    }
    if (!errors.isEmpty()) {
        QString extra = errors.size() > 6
            ? QStringLiteral("\n等共 %1 项").arg(errors.size())
            : QString();
        showMessageZh(
            this, QMessageBox::Warning,
            total > 0 ? QStringLiteral("部分目录已跳过") : QStringLiteral("无法锁定"),
            QStringLiteral("以下目录未能锁定：\n• ") +
                errors.mid(0, 6).join(QStringLiteral("\n• ")) + extra);
    }
    return total > 0;
}

bool MainWindow::unlockPathFlow(const QString& path, bool useCustomPassword) {
    QWidget* dlgParent = isVisible() ? this : nullptr;
    while (true) {
        UnlockDirDialog dlg(path, useCustomPassword, true, dlgParent);
        if (dlg.exec() != QDialog::Accepted) return false;

        JsonValue params = JsonValue::object();
        params.set("path", path.toStdString());
        params.set("password", dlg.password().toStdString());
        auto reply = DaemonClient::send("unlock", std::move(params));
        if (!reply.transport_ok) {
            showMessageZh(dlgParent, QMessageBox::Critical, QStringLiteral("错误"),
                          QString::fromStdString(reply.message));
            return false;
        }
        if (reply.ok) {
            refreshState();
            if (dlg.openAfterUnlock()) {
                openFolderInFileManager(path);
            }
            return true;
        }
        const int cd = static_cast<int>(reply.data.get("cooldown_seconds").as_int(0));
        showMessageZh(dlgParent, QMessageBox::Warning,
                      cd > 0 ? QStringLiteral("请稍后再试") : QStringLiteral("密码错误"),
                      QString::fromStdString(reply.message));
        if (cd > 0) return false;
    }
}

void MainWindow::handleExternalCommand(const QString& action, const QString& path) {
    refreshState();
    if (action == "show") {
        showMainWindow();
        return;
    }
    if (action == "lock-all") {
        lockAllInternal(QStringLiteral("一键全部锁定"), true);
        return;
    }
    if (path.isEmpty()) {
        showMainWindow();
        return;
    }

    QWidget* dlgParent = isVisible() ? this : nullptr;
    JsonValue qp = JsonValue::object();
    if (action == QStringLiteral("unlock-id")) {
        qp.set("id", path.toStdString());
    } else {
        qp.set("path", path.toStdString());
    }
    auto q = DaemonClient::send("query", std::move(qp));
    if (!q.transport_ok || !q.ok) {
        showMessageZh(dlgParent, QMessageBox::Critical, QStringLiteral("目录密匣 错误"),
                      QString::fromStdString(q.message));
        return;
    }

    const QString canonical = QString::fromStdString(q.data.get("canonical_path").as_string_or(path.toStdString()));
    const bool managed = q.data.get("managed").as_bool(false);
    const bool locked = q.data.get("locked").as_bool(false);
    const bool useCustom = q.data.get("use_custom_password").as_bool(false);
    const bool protectable = q.data.get("protectable").as_bool(false);
    const QString reason = QString::fromStdString(q.data.get("reason").as_string());

    if (!protectable) {
        showMessageZh(dlgParent, QMessageBox::Warning, QStringLiteral("无法操作此目录"), reason);
        return;
    }

    if (action == QStringLiteral("unlock") || action == QStringLiteral("unlock-id")) {
        if (!managed) {
            showMessageZh(dlgParent, QMessageBox::Information, QStringLiteral("提示"),
                          QStringLiteral("该目录尚未加入目录密匣保护。"));
            return;
        }
        if (locked) {
            unlockPathFlow(canonical, useCustom);
        } else {
            openFolderInFileManager(canonical);
        }
        return;
    }

    if (action == "lock") {
        if (managed && locked) {
            if (tray_) {
                tray_->showMessage(QStringLiteral("目录密匣"),
                                   QStringLiteral("该目录已处于锁定状态"),
                                   QSystemTrayIcon::Information, 2000);
            }
            return;
        }
        if (managed && !locked) {
            JsonValue lp = JsonValue::object();
            lp.set("path", canonical.toStdString());
            auto r = DaemonClient::send("lock", std::move(lp));
            if (!r.ok) {
                showMessageZh(dlgParent, QMessageBox::Warning, QStringLiteral("锁定失败"),
                              QString::fromStdString(r.message));
            } else {
                refreshState();
                if (tray_) {
                    tray_->showMessage(QStringLiteral("目录密匣"),
                                       QStringLiteral("已重新锁定：\n") + canonical,
                                       QSystemTrayIcon::Information, 2000);
                }
            }
            return;
        }
        lockNewPathFlow(canonical);
        return;
    }

    // "toggle"
    if (managed) {
        if (locked) {
            unlockPathFlow(canonical, useCustom);
        } else {
            JsonValue lp = JsonValue::object();
            lp.set("path", canonical.toStdString());
            auto r = DaemonClient::send("lock", std::move(lp));
            if (!r.ok) {
                showMessageZh(dlgParent, QMessageBox::Warning, QStringLiteral("锁定失败"),
                              QString::fromStdString(r.message));
            } else {
                refreshState();
                if (tray_) {
                    tray_->showMessage(QStringLiteral("目录密匣"),
                                       QStringLiteral("已锁定：\n") + canonical,
                                       QSystemTrayIcon::Information, 2000);
                }
            }
        }
    } else {
        lockNewPathFlow(canonical);
    }
}

int MainWindow::selectedRowIndex() const {
    auto ranges = table_->selectedRanges();
    if (ranges.isEmpty()) return -1;
    int visRow = ranges.first().topRow();
    if (visRow < 0 || visRow >= static_cast<int>(visible_indices_.size())) return -1;
    size_t idx = visible_indices_[static_cast<size_t>(visRow)];
    if (idx >= rows_.size()) return -1;
    return static_cast<int>(idx);
}

void MainWindow::onAddAndLock() {
    if (!ensureMasterPasswordInitialized()) return;
    FolderPickerDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;
    QString dir = dlg.selectedDirectory();
    if (dir.isEmpty()) return;
    lockNewPathFlow(dir);
}

void MainWindow::onUnlockSelected() {
    int idx = selectedRowIndex();
    if (idx < 0) {
        showMessageZh(this, QMessageBox::Information, QStringLiteral("提示"),
                      QStringLiteral("请先在列表中选择一个目录。"));
        return;
    }
    const auto& r = rows_[static_cast<size_t>(idx)];
    if (!r.locked) {
        openFolderInFileManager(r.path);
        return;
    }
    unlockPathFlow(r.path, r.use_custom_password);
}

void MainWindow::onLockSelected() {
    int idx = selectedRowIndex();
    if (idx < 0) {
        showMessageZh(this, QMessageBox::Information, QStringLiteral("提示"),
                      QStringLiteral("请先在列表中选择一个目录。"));
        return;
    }
    const auto& r = rows_[static_cast<size_t>(idx)];
    if (r.locked) {
        showMessageZh(this, QMessageBox::Information, QStringLiteral("提示"),
                      QStringLiteral("该目录已经是锁定状态。"));
        return;
    }
    JsonValue params = JsonValue::object();
    params.set("path", r.path.toStdString());
    auto reply = DaemonClient::send("lock", std::move(params));
    if (!reply.transport_ok || !reply.ok) {
        showMessageZh(this, QMessageBox::Warning, QStringLiteral("锁定失败"),
                      QString::fromStdString(reply.message));
        return;
    }
    refreshState();
}

void MainWindow::lockAllInternal(const QString& reason, bool showNotifyWhenZero) {
    auto reply = DaemonClient::send("lock_all");
    if (!reply.transport_ok) return;
    int cnt = static_cast<int>(reply.data.get("locked_count").as_int(0));
    int skipped = static_cast<int>(reply.data.get("skipped_count").as_int(0));
    refreshState();
    if (!tray_) return;

    if (skipped > 0) {
        QStringList items;
        const auto& skArr = reply.data.get("skipped_items");
        if (skArr.is_array()) {
            const size_t lim = std::min<size_t>(skArr.as_array().size(), 3);
            for (size_t i = 0; i < lim; ++i) {
                items.push_back(QString::fromStdString(skArr.as_array()[i].as_string()));
            }
        }
        QString preview = items.join(QStringLiteral("、"));
        if (skipped > 3) preview += QStringLiteral(" 等");
        QString msg;
        if (cnt > 0) {
            msg = QStringLiteral("%1：已锁定 %2 个目录，另有 %3 个因正在使用被跳过（%4）。点击打开主界面查看。")
                      .arg(reason).arg(cnt).arg(skipped).arg(preview);
        } else {
            msg = QStringLiteral("%1：有 %2 个目录因正在使用跳过锁定（%3），请先关闭占用程序后再试。")
                      .arg(reason).arg(skipped).arg(preview);
        }
        tray_->showMessage(QStringLiteral("目录密匣"), msg, QSystemTrayIcon::Warning, 4500);
        return;
    }

    if (cnt > 0 || showNotifyWhenZero) {
        tray_->showMessage(QStringLiteral("目录密匣"),
                           QStringLiteral("%1：共锁定 %2 个目录").arg(reason).arg(cnt),
                           QSystemTrayIcon::Information, 2500);
    }
}

void MainWindow::onLockAll() {
    lockAllInternal(QStringLiteral("一键全锁"), true);
}

void MainWindow::onAutoLockRequested(const QString& reason) {
    lockAllInternal(reason, false);
}

void MainWindow::onChangeDirPassword() {
    int idx = selectedRowIndex();
    if (idx < 0) {
        showMessageZh(this, QMessageBox::Information, QStringLiteral("提示"),
                      QStringLiteral("请先选择一个目录。"));
        return;
    }
    const auto& r = rows_[static_cast<size_t>(idx)];
    ChangeDirPasswordDialog dlg(r.path, r.use_custom_password, this);
    if (dlg.exec() != QDialog::Accepted) return;

    JsonValue params = JsonValue::object();
    params.set("path", r.path.toStdString());
    params.set("current_password", dlg.currentPassword().toStdString());
    params.set("use_custom_password", dlg.useCustomPassword());
    if (dlg.useCustomPassword()) {
        params.set("new_password", dlg.newCustomPassword().toStdString());
    }
    auto reply = DaemonClient::send("change_dir_password", std::move(params));
    if (!reply.transport_ok || !reply.ok) {
        showMessageZh(this, QMessageBox::Warning, QStringLiteral("修改失败"),
                      QString::fromStdString(reply.message));
        return;
    }
    refreshState();
    showMessageZh(
        this, QMessageBox::Information, QStringLiteral("成功"),
        dlg.useCustomPassword() ? QStringLiteral("已切换为「独立密码」方式。")
                                : QStringLiteral("已切换为「主密码」方式。"));
}

void MainWindow::onEditCustomPassword() {
    int idx = selectedRowIndex();
    if (idx < 0) {
        showMessageZh(this, QMessageBox::Information, QStringLiteral("提示"),
                      QStringLiteral("请先选择一个目录。"));
        return;
    }
    const auto& r = rows_[static_cast<size_t>(idx)];
    if (!r.use_custom_password) {
        showMessageZh(
            this, QMessageBox::Information, QStringLiteral("提示"),
            QStringLiteral(
                "该目录当前采用「主密码」，并非独立密码。\n\n"
                "• 如需修改主密码，请点击右上角「设置 → 修改主密码」。\n"
                "• 如需为该目录改用独立密码，请点击「切换密码方式」。"));
        return;
    }
    EditCustomPasswordDialog dlg(r.path, this);
    if (dlg.exec() != QDialog::Accepted) return;

    JsonValue params = JsonValue::object();
    params.set("path", r.path.toStdString());
    params.set("current_password", dlg.currentPassword().toStdString());
    params.set("use_custom_password", true);
    params.set("new_password", dlg.newCustomPassword().toStdString());
    auto reply = DaemonClient::send("change_dir_password", std::move(params));
    if (!reply.transport_ok || !reply.ok) {
        showMessageZh(this, QMessageBox::Warning, QStringLiteral("修改失败"),
                      QString::fromStdString(reply.message));
        return;
    }
    refreshState();
    showMessageZh(this, QMessageBox::Information, QStringLiteral("成功"),
                  QStringLiteral("该目录的独立密码已更新。"));
}

void MainWindow::onRemoveSelected() {
    int idx = selectedRowIndex();
    if (idx < 0) {
        showMessageZh(this, QMessageBox::Information, QStringLiteral("提示"),
                      QStringLiteral("请先选择一个目录。"));
        return;
    }
    const auto& r = rows_[static_cast<size_t>(idx)];
    UnlockDirDialog dlg(r.path, r.use_custom_password, false, this);
    dlg.setWindowTitle(QStringLiteral("验证密码以移出管理列表"));
    if (dlg.exec() != QDialog::Accepted) return;

    JsonValue params = JsonValue::object();
    params.set("path", r.path.toStdString());
    params.set("password", dlg.password().toStdString());
    auto reply = DaemonClient::send("remove", std::move(params));
    if (!reply.transport_ok || !reply.ok) {
        showMessageZh(this, QMessageBox::Warning, QStringLiteral("移出失败"),
                      QString::fromStdString(reply.message));
        return;
    }
    refreshState();
}

void MainWindow::onChangeMasterPassword() {
    refreshState();
    MasterPasswordDialog dlg(has_master_password_, this);
    if (dlg.exec() != QDialog::Accepted) return;

    JsonValue params = JsonValue::object();
    params.set("old_password", dlg.oldPassword().toStdString());
    params.set("new_password", dlg.newPassword().toStdString());
    auto reply = DaemonClient::send("set_master_password", std::move(params));
    if (!reply.transport_ok || !reply.ok) {
        showMessageZh(this, QMessageBox::Warning, QStringLiteral("修改失败"),
                      QString::fromStdString(reply.message));
        return;
    }
    refreshState();
    showMessageZh(this, QMessageBox::Information, QStringLiteral("成功"),
                  QStringLiteral("主密码已更新。"));
}

void MainWindow::onOpenSettings() {
    refreshState();
    if (!ensureMasterPasswordInitialized()) return;
    QWidget* dlgParent = isVisible() ? this : nullptr;
    if (monitor_) {
        monitor_->setHotkeyPaused(true);
    }
    auto probe = [this](const QString& cand, QString& err) -> bool {
        if (!monitor_) return true;
        return monitor_->testHotkeyAvailable(cand, &err);
    };
    SettingsDialog dlg(auto_relock_minutes_, require_master_on_open_, hotkey_, probe, dlgParent);
    const int res = dlg.exec();
    if (monitor_) {
        monitor_->setHotkeyPaused(false);
    }
    if (res != QDialog::Accepted) return;

    JsonValue params = JsonValue::object();
    params.set("password", dlg.masterPassword().toStdString());
    params.set("auto_relock_minutes", dlg.autoRelockMinutes());
    params.set("require_master_on_open", dlg.requireMasterOnOpen());
    params.set("hotkey", dlg.hotkey().toStdString());

    auto reply = DaemonClient::send("update_settings", std::move(params));
    if (!reply.transport_ok || !reply.ok) {
        showMessageZh(dlgParent, QMessageBox::Warning, QStringLiteral("保存失败"),
                      QString::fromStdString(reply.message));
        return;
    }
    refreshState();
    showMessageZh(dlgParent, QMessageBox::Information, QStringLiteral("成功"),
                  QStringLiteral("安全设置已保存。"));
}

void MainWindow::onHealthCheck() {
    refreshState();
    if (!ensureMasterPasswordInitialized()) return;
    QWidget* dlgParent = isVisible() ? this : nullptr;
    QString verifiedPass;
    while (true) {
        VerifyMasterDialog verifyDlg(
            QStringLiteral("保护区体检与一键修复"),
            QStringLiteral("体检将核查各受保护目录与隐藏保护区状态，请先验证主密码："),
            dlgParent);
        if (verifyDlg.exec() != QDialog::Accepted) return;

        JsonValue params = JsonValue::object();
        params.set("password", verifyDlg.password().toStdString());
        auto reply = DaemonClient::send("verify_master", std::move(params));
        if (!reply.transport_ok) {
            showMessageZh(dlgParent, QMessageBox::Critical, QStringLiteral("错误"),
                          QString::fromStdString(reply.message));
            return;
        }
        if (reply.ok) {
            verifiedPass = verifyDlg.password();
            break;
        }
        const int cd = static_cast<int>(reply.data.get("cooldown_seconds").as_int(0));
        showMessageZh(dlgParent, QMessageBox::Warning,
                      cd > 0 ? QStringLiteral("请稍后再试") : QStringLiteral("验证失败"),
                      QString::fromStdString(reply.message));
        if (cd > 0) return;
    }

    HealthCheckDialog dlg(verifiedPass, dlgParent);
    dlg.exec();
    if (dlg.changed()) {
        refreshState();
    }
}

void MainWindow::onShowRecoveryGuide() {
    if (!ensureMasterPasswordInitialized()) return;
    VerifyMasterDialog verifyDlg(
        QStringLiteral("验证主密码"),
        QStringLiteral("应急恢复说明包含手动恢复命令，为防止绕过保护，请先验证主密码："),
        this);
    if (verifyDlg.exec() != QDialog::Accepted) return;

    JsonValue params = JsonValue::object();
    params.set("password", verifyDlg.password().toStdString());
    auto reply = DaemonClient::send("verify_master", std::move(params));
    if (!reply.transport_ok || !reply.ok) {
        showMessageZh(this, QMessageBox::Warning, QStringLiteral("验证失败"),
                      QString::fromStdString(reply.message));
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("应急恢复说明"));
    dlg.resize(650, 520);
    auto* lay = new QVBoxLayout(&dlg);
    auto* tip = new QLabel(
        QStringLiteral("<b style=\"color:#1f2328;\">目录密匣不会修改或加密目录内任何文件数据，所有原始文件均完整保留。请按实际现象选择对应恢复方式：</b>"),
        &dlg);
    tip->setTextFormat(Qt::RichText);
    tip->setWordWrap(true);
    tip->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    lay->addWidget(tip);

    auto* browser = new QTextBrowser(&dlg);
    browser->setOpenExternalLinks(false);
    browser->setStyleSheet(QStringLiteral(
        "QTextBrowser { background: #ffffff; border: 1px solid #d0d7de; border-radius: 6px; padding: 6px 10px; }"));
    browser->setHtml(QStringLiteral(
        "<div style=\"font-size:13px; color:#1f2328; line-height:1.45;\">"
        "<div style=\"color:#0d47a1; font-weight:700; font-size:14px; margin-bottom:4px;\">"
        "1. 情形一：原目录不见了、原地只剩蓝锁加密入口（最常见：忘记密码 / 误删状态文件 / 卸载后）</div>"
        "<div style=\"margin-left:10px;\">真实目录完整存放在以下隐藏系统路径之一：</div>"
        "<div style=\"margin-left:20px; margin-top:2px;\">• 系统根目录：<span style=\"font-family:monospace; background-color:#f1f5f9; color:#b45309;\">&nbsp;/var/lib/dirlocker/vault/&nbsp;</span></div>"
        "<div style=\"margin-left:20px; margin-bottom:4px;\">• 独立分区：<span style=\"font-family:monospace; background-color:#f1f5f9; color:#b45309;\">&nbsp;所在分区根目录/.dirlocker_vault_do_not_delete/&nbsp;</span></div>"
        "<div style=\"margin-left:10px;\">请在终端依次执行（可用 <span style=\"font-family:monospace; background-color:#f1f5f9; color:#0c4a6e;\">&nbsp;sudo ls -la&nbsp;</span> 查看保护区中的 ID）：</div>"
        "<div style=\"margin-left:14px; margin-top:4px; font-weight:600;\">(0) 查看入口文件记录的 ID：</div>"
        "<pre style=\"background-color:#f1f5f9; color:#0c4a6e; font-family:'Noto Sans Mono','DejaVu Sans Mono',monospace; margin:2px 0 6px 24px;\"> cat \"原目录路径.desktop\" </pre>"
        "<div style=\"margin-left:14px; font-weight:600;\">(1) 解除不可变标记并移回原路径：</div>"
        "<pre style=\"background-color:#f1f5f9; color:#0c4a6e; font-family:'Noto Sans Mono','DejaVu Sans Mono',monospace; margin:2px 0 6px 24px;\">"
        " sudo chattr -i \"保护区ID目录路径\" 2&gt;/dev/null\n"
        " sudo mv \"保护区ID目录路径\" \"原目录路径\" </pre>"
        "<div style=\"margin-left:14px; font-weight:600;\">(2) 恢复属主与正常访问权限：</div>"
        "<pre style=\"background-color:#f1f5f9; color:#0c4a6e; font-family:'Noto Sans Mono','DejaVu Sans Mono',monospace; margin:2px 0 6px 24px;\">"
        " sudo chown \"$(id -u):$(id -g)\" \"原目录路径\"\n"
        " sudo chmod 755 \"原目录路径\" </pre>"
        "<div style=\"margin-left:14px; font-weight:600;\">(3) 删除残余加密入口文件（<span style=\"color:#b91c1c;\">须先移除防删标记</span>）：</div>"
        "<pre style=\"background-color:#f1f5f9; color:#0c4a6e; font-family:'Noto Sans Mono','DejaVu Sans Mono',monospace; margin:2px 0 12px 24px;\">"
        " sudo chattr -i -a \"原目录路径.desktop\" \"原目录路径.dlock\" 2&gt;/dev/null\n"
        " sudo rm -f \"原目录路径.desktop\" \"原目录路径.dlock\" </pre>"
        "<div style=\"color:#0d47a1; font-weight:700; font-size:14px; margin-top:6px; margin-bottom:4px;\">"
        "2. 情形二：原目录仍在原地，但打开提示权限不足（旧版兼容）</div>"
        "<div style=\"margin-left:10px;\">直接在终端执行：</div>"
        "<pre style=\"background-color:#f1f5f9; color:#0c4a6e; font-family:'Noto Sans Mono','DejaVu Sans Mono',monospace; margin:2px 0 4px 24px;\">"
        " sudo chattr -i \"目标目录路径\" 2&gt;/dev/null\n"
        " sudo chown \"$(id -u):$(id -g)\" \"目标目录路径\"\n"
        " sudo chmod 755 \"目标目录路径\" </pre>"
        "</div>"));
    lay->addWidget(browser);

    auto* btnRow = new QHBoxLayout();
    auto* copyBtn = new QPushButton(QStringLiteral("复制全部说明"), &dlg);
    copyBtn->setMinimumWidth(116);
    connect(copyBtn, &QPushButton::clicked, &dlg, [browser, copyBtn]() {
        if (QClipboard* cb = QApplication::clipboard()) {
            cb->setText(browser->toPlainText());
        }
        copyBtn->setText(QStringLiteral("✓ 已复制到剪贴板"));
        QTimer::singleShot(1800, copyBtn, [copyBtn]() {
            copyBtn->setText(QStringLiteral("复制全部说明"));
        });
    });
    btnRow->addWidget(copyBtn);
    btnRow->addStretch();
    auto* okBtn = new QPushButton(QStringLiteral("确定"), &dlg);
    okBtn->setDefault(true);
    okBtn->setMinimumWidth(86);
    connect(okBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    btnRow->addWidget(okBtn);
    lay->addLayout(btnRow);
    dlg.exec();
}

void MainWindow::onShowAbout() {
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("关于 目录密匣"));
    dlg.setMinimumWidth(556);

    auto* root = new QVBoxLayout(&dlg);
    root->setSizeConstraint(QLayout::SetMinimumSize);
    root->setContentsMargins(22, 18, 22, 16);
    root->setSpacing(12);

    auto* headerRow = new QHBoxLayout();
    headerRow->setSpacing(14);
    auto* iconLabel = new QLabel(&dlg);
    iconLabel->setPixmap(createAppIcon(true).pixmap(48, 48));
    iconLabel->setFixedSize(48, 48);
    headerRow->addWidget(iconLabel, 0, Qt::AlignTop);

    auto* titleCol = new QVBoxLayout();
    titleCol->setSpacing(4);
    auto* titleRow = new QHBoxLayout();
    titleRow->setSpacing(8);
    titleRow->setContentsMargins(0, 0, 0, 0);
    auto* titleLabel = new QLabel(
        QStringLiteral("<span style=\"font-size:15pt; font-weight:700; color:#0d47a1;\">目录密匣</span>"),
        &dlg);
    titleLabel->setTextFormat(Qt::RichText);
    auto* verBadge = new QLabel(QStringLiteral("v") + QStringLiteral(DIRLOCKER_VERSION_STR), &dlg);
    verBadge->setStyleSheet(QStringLiteral(
        "QLabel { background: #ebf3fe; color: #0d47a1; border: 1px solid #90caf9; "
        "border-radius: 9px; padding: 1px 8px; font-size: 9pt; font-weight: 600; }"));
    titleRow->addWidget(titleLabel);
    titleRow->addWidget(verBadge);
    titleRow->addStretch();

    auto* subLabel = new QLabel(
        QStringLiteral("<span style=\"color:#5f6368;\">轻量级本地目录密码保护工具</span>"),
        &dlg);
    subLabel->setTextFormat(Qt::RichText);
    titleCol->addLayout(titleRow);
    titleCol->addWidget(subLabel);
    headerRow->addLayout(titleCol, 1);
    root->addLayout(headerRow);

    auto* line = new QFrame(&dlg);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    root->addWidget(line);

    auto* metaLabel = new QLabel(
        QStringLiteral(
            "<div style=\"line-height:165%;\">"
            "<span style=\"color:#5f6368;\">适用平台：</span><span style=\"color:#1f2328;\">银河麒麟 V10 SP1 / Ubuntu 20.04 (x64)</span><br/>"
            "<span style=\"color:#5f6368;\">开发人员：</span><span style=\"color:#1f2328;\">yinchaow &amp; Gemini</span><br/>"
            "<span style=\"color:#5f6368;\">开源协议：</span><span style=\"color:#1f2328;\">MIT License</span><br/>"
            "<span style=\"color:#5f6368;\">项目主页：</span>"
            "<a style=\"color:#0d47a1; text-decoration:none; font-weight:500;\" "
            "href=\"https://github.com/yinchaow/dirlocker\">https://github.com/yinchaow/dirlocker</a>"
            "</div>"),
        &dlg);
    metaLabel->setTextFormat(Qt::RichText);
    metaLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    metaLabel->setOpenExternalLinks(true);
    root->addWidget(metaLabel);

    auto* featCard = new QFrame(&dlg);
    featCard->setObjectName(QStringLiteral("aboutFeatCard"));
    featCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    featCard->setStyleSheet(QStringLiteral(
        "#aboutFeatCard { background: #f6f8fa; border: 1px solid #d0d7de; border-radius: 6px; }"));
    auto* cardLay = new QVBoxLayout(featCard);
    cardLay->setContentsMargins(14, 10, 14, 10);
    cardLay->setSpacing(6);

    auto* featHeader = new QLabel(QStringLiteral("<b style=\"color:#0d47a1;\">核心功能</b>"), featCard);
    featHeader->setTextFormat(Qt::RichText);
    cardLay->addWidget(featHeader);

    auto* featBody = new QLabel(
        QStringLiteral(
            "<div style=\"line-height:165%; color:#1f2328;\">"
            "<span style=\"color:#0d47a1;\">•</span> <b>双重密码体系</b>：支持统一主密码与单目录独立密码，随时自由切换与改密<br/>"
            "<span style=\"color:#0d47a1;\">•</span> <b>快捷验证解锁</b>：已加密目录独立标识，双击即可输入密码解锁进入<br/>"
            "<span style=\"color:#0d47a1;\">•</span> <b>批量拖拽加锁</b>：支持将单个或多个目录直接拖入窗口批量锁定<br/>"
            "<span style=\"color:#0d47a1;\">•</span> <b>全锁与防占用</b>：支持 <span style=\"font-family:monospace; background-color:#e2e8f0; color:#0c4a6e;\">&nbsp;%1&nbsp;</span> 一键全锁与闲置自动回锁，程序占用目录时自动跳过并提示<br/>"
            "<span style=\"color:#0d47a1;\">•</span> <b>主界保护与防爆破</b>：可选打开主窗口验证主密码，连续输错密码触发退避冷却时间<br/>"
            "<span style=\"color:#0d47a1;\">•</span> <b>无损与体检修复</b>：零文件内容改动，内置保护区健康体检与安全无损一键修复"
            "</div>").arg(hotkey_.toHtmlEscaped()),
        featCard);
    featBody->setTextFormat(Qt::RichText);
    featBody->setWordWrap(true);
    featBody->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    cardLay->addWidget(featBody);
    root->addWidget(featCard);

    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch();
    auto* okBtn = new QPushButton(QStringLiteral("确定"), &dlg);
    okBtn->setDefault(true);
    okBtn->setMinimumWidth(86);
    connect(okBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    btnRow->addWidget(okBtn);
    root->addLayout(btnRow);

    dlg.exec();
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event) {
    if (table_ && obj == table_->viewport()) {
        if (event->type() == QEvent::Resize) {
            if (empty_overlay_) {
                empty_overlay_->setGeometry(table_->viewport()->rect());
            }
        } else if (event->type() == QEvent::Leave) {
            if (delegate_ && delegate_->setHover(-1, -1, 0)) {
                table_->viewport()->update();
            }
            table_->viewport()->unsetCursor();
        } else if (event->type() == QEvent::MouseMove) {
            auto* me = static_cast<QMouseEvent*>(event);
            QModelIndex idx = table_->indexAt(me->pos());
            int slot = 0;
            if (idx.isValid() && idx.row() < static_cast<int>(visible_indices_.size())) {
                const bool locked = rows_[visible_indices_[static_cast<size_t>(idx.row())]].locked;
                slot = hitCellSlot(table_->visualRect(idx), idx.column(), locked, me->pos());
            }
            const bool interactive = (idx.isValid() && idx.column() == 3 && slot > 0);
            if (interactive) table_->viewport()->setCursor(Qt::PointingHandCursor);
            else table_->viewport()->unsetCursor();

            if (delegate_ && delegate_->setHover(idx.row(), idx.column(), slot)) {
                table_->viewport()->update();
            }
        } else if (event->type() == QEvent::ToolTip) {
            auto* he = static_cast<QHelpEvent*>(event);
            QModelIndex idx = table_->indexAt(he->pos());
            if (idx.isValid() && idx.row() >= 0 &&
                idx.row() < static_cast<int>(visible_indices_.size())) {
                const auto& r = rows_[visible_indices_[static_cast<size_t>(idx.row())]];
                const int slot = hitCellSlot(table_->visualRect(idx), idx.column(), r.locked, he->pos());
                QString tip;
                if (idx.column() == 1 && slot == 1) {
                    tip = r.locked ? QStringLiteral("当前状态：已锁定（双击可解锁）")
                                   : QStringLiteral("当前状态：已解锁（双击可锁定）");
                } else if (idx.column() == 2 && slot == 1) {
                    tip = r.use_custom_password ? QStringLiteral("双击可切换为主密码")
                                                : QStringLiteral("双击可切换为独立密码");
                } else if (idx.column() == 3) {
                    if (slot == 1) tip = r.locked ? QStringLiteral("输入密码解锁并打开")
                                                  : QStringLiteral("立即重新锁定此目录");
                    else if (slot == 2) tip = QStringLiteral("打开目录");
                    else if (slot == 3) tip = QStringLiteral("更多操作（改密、切换方式、移出列表）");
                } else if (idx.column() == 0) {
                    tip = r.path;
                }
                if (!tip.isEmpty()) {
                    QToolTip::showText(he->globalPos(), tip, table_->viewport());
                    return true;
                }
            }
            QToolTip::hideText();
            return true;
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::onTableClicked(const QModelIndex& index) {
    if (!index.isValid() || index.column() != 3) return;
    int row = index.row();
    if (row < 0 || row >= static_cast<int>(visible_indices_.size())) return;

    const auto& r = rows_[visible_indices_[static_cast<size_t>(row)]];
    const QRect cellRect = table_->visualRect(index);
    const QPoint vpPos = table_->viewport()->mapFromGlobal(QCursor::pos());
    const int slot = hitCellSlot(cellRect, 3, r.locked, vpPos);
    if (slot == 0) return;

    table_->selectRow(row);
    if (slot == 1) {
        if (r.locked) onUnlockSelected();
        else onLockSelected();
    } else if (slot == 2 && !r.locked) {
        openFolderInFileManager(r.path);
    } else if (slot == 3) {
        const RowCellLayout lay = computeCellLayout(cellRect, 3, r.locked);
        showRowMenu(row, table_->viewport()->mapToGlobal(QPoint(lay.moreBtn.right() + 1, lay.moreBtn.bottom() + 3)), true, true);
    }
}

void MainWindow::onTableDoubleClicked(int row, int col) {
    if (col == 3) return;
    if (row < 0 || row >= static_cast<int>(visible_indices_.size())) return;
    const auto& r = rows_[visible_indices_[static_cast<size_t>(row)]];
    if (col == 1) {
        if (r.locked) unlockPathFlow(r.path, r.use_custom_password);
        else onLockSelected();
        return;
    }
    if (col == 2) {
        onChangeDirPassword();
        return;
    }
    if (r.locked) unlockPathFlow(r.path, r.use_custom_password);
    else openFolderInFileManager(r.path);
}

void MainWindow::showRowMenu(int visRow, const QPoint& anchorGlobalPos, bool alignRight, bool moreMenuOnly) {
    if (visRow < 0 || visRow >= static_cast<int>(visible_indices_.size())) return;
    table_->selectRow(visRow);
    const auto& r = rows_[visible_indices_[static_cast<size_t>(visRow)]];

    QMenu menu(this);
    if (!moreMenuOnly) {
        if (r.locked) {
            auto* a = menu.addAction(makeMenuIcon(MenuIconKind::Unlock), QStringLiteral("解锁并打开"));
            connect(a, &QAction::triggered, this, &MainWindow::onUnlockSelected);
        } else {
            auto* o = menu.addAction(makeMenuIcon(MenuIconKind::OpenFolder), QStringLiteral("打开目录"));
            connect(o, &QAction::triggered, this, [this, r]() { openFolderInFileManager(r.path); });
            auto* l = menu.addAction(makeMenuIcon(MenuIconKind::Lock), QStringLiteral("重新锁定"));
            connect(l, &QAction::triggered, this, &MainWindow::onLockSelected);
        }
    }
    if (r.use_custom_password) {
        auto* ec = menu.addAction(makeMenuIcon(MenuIconKind::Key), QStringLiteral("修改独立密码"));
        connect(ec, &QAction::triggered, this, &MainWindow::onEditCustomPassword);
        auto* cp = menu.addAction(makeMenuIcon(MenuIconKind::Switch), QStringLiteral("切换为主密码"));
        connect(cp, &QAction::triggered, this, &MainWindow::onChangeDirPassword);
    } else {
        auto* cp = menu.addAction(makeMenuIcon(MenuIconKind::Switch), QStringLiteral("切换为独立密码"));
        connect(cp, &QAction::triggered, this, &MainWindow::onChangeDirPassword);
    }
    menu.addSeparator();
    auto* rm = menu.addAction(makeMenuIcon(MenuIconKind::Trash), QStringLiteral("移出保护列表"));
    connect(rm, &QAction::triggered, this, &MainWindow::onRemoveSelected);

    QPoint showPos = anchorGlobalPos;
    if (alignRight) {
        showPos.rx() -= menu.sizeHint().width();
    }
    menu.exec(showPos);
}

void MainWindow::onTableContextMenu(const QPoint& pos) {
    int visRow = table_->indexAt(pos).row();
    if (visRow < 0 || visRow >= static_cast<int>(visible_indices_.size())) return;
    showRowMenu(visRow, table_->viewport()->mapToGlobal(pos), false, false);
}

void MainWindow::onQuitRequested() {
    refreshState();
    int unlocked = 0;
    for (const auto& r : rows_) {
        if (!r.locked) ++unlocked;
    }
    if (unlocked > 0) {
        int ans = askQuitRelockZh(this, unlocked);
        if (ans < 0) return;
        if (ans > 0) {
            DaemonClient::send("lock_all");
        }
    }
    really_quit_ = true;
    qApp->quit();
}

}  // namespace dirlocker

