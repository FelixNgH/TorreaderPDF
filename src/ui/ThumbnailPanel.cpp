#include "ThumbnailPanel.h"
#include "../core/PdfRenderer.h"
#include "SearchPanel.h"
#include "ThemeTokens.h"
#include "../core/PdfiumLock.h"
#include "../core/DocTaskGate.h"
#include <QElapsedTimer>
#include <QDebug>
#include <QTimer>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QScrollBar>
#include <QMutex>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QMenu>
#include <QPushButton>
#include <QResizeEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QColorDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QDragMoveEvent>
#include <fpdf_doc.h>
#include <fpdf_text.h>
#include <QFrame>
#include <QKeySequence>
#include <QShortcut>
#include <QClipboard>
#include <QApplication>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontMetrics>
#include <QScreen>
#include <QKeyEvent>
#include <QMouseEvent>
#include "NoteInputDialog.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QStyledItemDelegate>
#include <QPainter>
#include <algorithm>
#include <functional>
#include <vector>

// 0927 LUOT 5 (LOI 1): icon nut "mo o sua lon" ve bang QPainter (Win10 thieu
// glyph U+2922 -> o vuong trong). Khai bao o day vi setDarkMode() goi lai
// de doi mau theo theme, da DUNG truoc phan dinh nghia duoi.
static QIcon commentExpandIcon(bool dark);

extern QMutex s_pdfiumMutex;

// Mau chu tuong phan voi mau nen markup nguoi dung chon (WCAG relative
// luminance): nen sang thi chu den, nen toi thi chu trang.
static QString textColorOn(const QColor& bg) {
    const double lum = 0.2126 * bg.redF() + 0.7152 * bg.greenF() + 0.0722 * bg.blueF();
    return lum > 0.5 ? "#000000" : "#FFFFFF";
}

// Qt6 QListWidget with InternalMove does NOT reliably emit rowsMoved
// (it does rowsRemoved+rowsInserted).  Use onDropped callback instead.
class DropListWidget : public QListWidget {
public:
    using QListWidget::QListWidget;
    std::function<void()> onDropped;
protected:
    void dropEvent(QDropEvent* e) override {
        QListWidget::dropEvent(e);
        if (onDropped)
            QTimer::singleShot(0, this, [this]{ if (onDropped) onDropped(); });
    }
};

class BookmarkTreeWidget : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;
    // Called after a successful drop so caller can collect the new order.
    // Qt6 QTreeWidget with InternalMove emits rowsRemoved+rowsInserted,
    // NOT rowsMoved, so we can't rely on the rowsMoved signal.
    std::function<void()> onDropped;

protected:
    // Only top-level items can be dragged
    void startDrag(Qt::DropActions actions) override {
        for (auto* item : selectedItems())
            if (item->parent()) return;
        QTreeWidget::startDrag(actions);
    }
    // Prevent "drop onto item" (reparenting); allow above/below only
    void dragMoveEvent(QDragMoveEvent* e) override {
        QPoint pos = e->position().toPoint();
        QModelIndex idx = indexAt(pos);
        if (idx.isValid()) {
            if (idx.parent().isValid()) { e->ignore(); return; }
            QRect r = visualRect(idx);
            int y = pos.y() - r.top(), h = r.height();
            if (y > h / 4 && y < h * 3 / 4) { e->ignore(); return; }
        }
        QTreeWidget::dragMoveEvent(e);
    }
    void dropEvent(QDropEvent* e) override {
        QModelIndex idx = indexAt(e->position().toPoint());
        if (idx.isValid() && idx.parent().isValid()) { e->ignore(); return; }
        QTreeWidget::dropEvent(e);
        // Fire after the event loop settles so the model reflects the new order.
        if (onDropped) QTimer::singleShot(0, this, [this]{ if (onDropped) onDropped(); });
    }
};

// Filter raw PDFium text: PDFs without ToUnicode tables produce garbage glyph-IDs.
static QString cleanPdfText(const QString& raw) {
    if (raw.isEmpty()) return {};
    QString result;
    result.reserve(raw.size());
    int bad = 0;
    for (QChar c : raw) {
        ushort u = c.unicode();
        bool ok = (u >= 0x0020 && u <= 0x007E)
               || (u >= 0x00A0 && u <= 0x024F)
               || (u >= 0x1E00 && u <= 0x1EFF)
               || (u >= 0x2000 && u <= 0x206F)
               || (u >= 0x4E00 && u <= 0x9FFF)
               || (u >= 0xAC00 && u <= 0xD7A3)
               || (u >= 0x3000 && u <= 0x303F)
               || (u == 0x0009 || u == 0x000A || u == 0x000D);
        if (ok) result += c;
        else    ++bad;
    }
    if (bad * 100 / raw.size() > 40) return {};
    return result.simplified();
}

// Custom delegate to paint the current page highlight in the thumbnail list.
// QSS rule "QListWidget::item" overrides setBackground(), so we paint manually.
class ThumbCurrentPageDelegate : public QStyledItemDelegate {
public:
    explicit ThumbCurrentPageDelegate(ThumbnailPanel* panel) : QStyledItemDelegate(panel), m_panel(panel) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        if (m_panel && index.row() == m_panel->currentPageIndex()) {
            painter->fillRect(option.rect, m_panel->currentPageHighlight());
        }
        QStyledItemDelegate::paint(painter, option, index);
    }

private:
    ThumbnailPanel* m_panel = nullptr;
};

// ── Constructor ───────────────────────────────────────────────────────────────
ThumbnailPanel::ThumbnailPanel(QWidget* parent) : QWidget(parent) {

    // ── Thumbnails tab ────────────────────────────────────────────────────
    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    QFont f = m_list->font();
    f.setPointSize(8);
    m_list->setFont(f);
    m_list->setViewMode(QListWidget::IconMode);
    m_list->setIconSize({110, 150});
    m_list->setResizeMode(QListWidget::Adjust);
    m_list->setSpacing(2);
    m_list->setDragEnabled(true);
    m_list->setAcceptDrops(true);
    m_list->setDropIndicatorShown(true);
    m_list->setDragDropMode(QAbstractItemView::InternalMove);
    m_list->setDefaultDropAction(Qt::MoveAction);
    m_list->setItemDelegate(new ThumbCurrentPageDelegate(this));

    connect(m_list, &QListWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        auto sel = m_list->selectedItems();
        if (sel.size() > 1) {
            QList<int> pages;
            for (auto* it : sel) pages.append(m_list->row(it));
            std::sort(pages.begin(), pages.end());
            QMenu menu;
            menu.addAction(
                QString("Extract %1 Pages to New File…").arg(pages.size()),
                [this, pages]{ emit extractPagesRequested(pages); });
            menu.exec(m_list->mapToGlobal(pos));
        } else if (auto* item = m_list->itemAt(pos)) {
            emit pageContextMenu(m_list->row(item), m_list->mapToGlobal(pos));
        }
    });
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem* item){
        emit pageClicked(m_list->row(item));
    });

    // ── Bookmarks tab ─────────────────────────────────────────────────────
    m_outline = new BookmarkTreeWidget(this);
    m_outline->setHeaderHidden(true);
    m_outline->setRootIsDecorated(true);
    m_outline->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_outline->setDragEnabled(true);
    m_outline->setAcceptDrops(true);
    m_outline->setDropIndicatorShown(true);
    m_outline->setDragDropMode(QAbstractItemView::InternalMove);

    connect(m_outline, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int){
        // During Ctrl/Shift multi-select, don't navigate — preserve the selection.
        if (m_outline->selectedItems().size() > 1) return;
        int page = item->data(0, Qt::UserRole).toInt();
        if (page >= 0) emit pageClicked(page);
    });
    m_outline->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_outline, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        auto sel = m_outline->selectedItems();
        if (sel.size() > 1) {
            QList<int> pages;
            for (auto* it : sel) {
                int pg = it->data(0, Qt::UserRole).toInt();
                if (pg >= 0 && !pages.contains(pg)) pages.append(pg);
            }
            std::sort(pages.begin(), pages.end());
            if (pages.isEmpty()) return;
            QMenu menu;
            menu.addAction(
                QString("Extract %1 Pages to New File…").arg(pages.size()),
                [this, pages]{ emit extractPagesRequested(pages); });
            menu.exec(m_outline->mapToGlobal(pos));
        } else if (auto* item = m_outline->itemAt(pos)) {
            int page = item->data(0, Qt::UserRole).toInt();
            if (page >= 0)
                emit pageContextMenu(page, m_outline->mapToGlobal(pos));
        }
    });

    // ── Content tab ───────────────────────────────────────────────────────
    // Re-use BookmarkTreeWidget so we get the same onDropped mechanism.
    m_contentTree = new BookmarkTreeWidget(this);
    m_contentTree->setHeaderHidden(true);
    m_contentTree->setRootIsDecorated(false);
    m_contentTree->setDragEnabled(true);
    m_contentTree->setAcceptDrops(true);
    m_contentTree->setDropIndicatorShown(true);
    m_contentTree->setDragDropMode(QAbstractItemView::InternalMove);
    m_contentTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    connect(m_contentTree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int){
        int page = item->data(0, Qt::UserRole).toInt();
        qDebug() << "[Content] itemClicked page=" << page;
        if (page >= 0) emit pageClicked(page);
    });
    // Ctrl+A / Ctrl+C: select all / copy text from content tab
    auto* copyShortcut = new QShortcut(QKeySequence::Copy, m_contentTree);
    copyShortcut->setContext(Qt::WidgetShortcut);
    connect(copyShortcut, &QShortcut::activated, this, [this]() {
        QStringList lines;
        for (auto* item : m_contentTree->selectedItems())
            lines.append(item->text(0));
        if (!lines.isEmpty())
            QApplication::clipboard()->setText(lines.join("\n"));
    });
    auto* selAllShortcut = new QShortcut(QKeySequence::SelectAll, m_contentTree);
    selAllShortcut->setContext(Qt::WidgetShortcut);
    connect(selAllShortcut, &QShortcut::activated, m_contentTree, &QTreeWidget::selectAll);

    // ── Properties tab ────────────────────────────────────────────────────
    m_propertiesTree = new QTreeWidget(this);
    m_propertiesTree->setHeaderLabels({"Property", "Value"});
    m_propertiesTree->setRootIsDecorated(false);
    m_propertiesTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_propertiesTree->header()->setStretchLastSection(true);
    m_propertiesTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);

    // ── Search panel (hidden, kept for future version) ────────────────────
    m_searchPanel = new SearchPanel(this);

    // ── OCR panel (tab id 5, SPEC_OCR_TAB_AND_SELECT phan 1) ─────────────
    // File rieng OcrPanel.{h,cpp}: doc hon vi ThumbnailPanel.cpp da 1100+ dong
    // va panel OCR co logic rieng (trang thai/tien do) khong thuoc ve thumbnail.
    m_ocrPanel = new OcrPanel(this);

    // ── 2×2 tab-button grid + stacked content ────────────────────────────
    auto makeTabBtn = [](const QString& text) {
        auto* btn = new QPushButton(text);
        btn->setCheckable(true);
        btn->setObjectName("sidebarTab");
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        btn->setFixedHeight(26);
        return btn;
    };

    auto* thumbBtn = makeTabBtn("Thumbnails");
    auto* booksBtn = makeTabBtn("Bookmarks");
    auto* contBtn  = makeTabBtn("Comments");
    auto* propBtn  = makeTabBtn("Properties");
    auto* searchBtn = makeTabBtn("Search");
    auto* ocrBtn   = makeTabBtn("OCR");
    thumbBtn->setChecked(true);

    m_tabGroup = new QButtonGroup(this);
    m_tabGroup->setExclusive(true);
    m_tabGroup->addButton(thumbBtn, 0);
    m_tabGroup->addButton(booksBtn, 1);
    m_tabGroup->addButton(contBtn,  2);
    m_tabGroup->addButton(propBtn,  3);
    m_tabGroup->addButton(searchBtn, 4);
    m_tabGroup->addButton(ocrBtn,   5);

    auto* tabGrid = new QWidget;
    tabGrid->setObjectName("sidebarTabGrid");
    auto* gl      = new QGridLayout(tabGrid);
    gl->setContentsMargins(0, 0, 0, 0);
    gl->setSpacing(1);
    gl->setColumnStretch(0, 1);
    gl->setColumnStretch(1, 1);
    gl->addWidget(thumbBtn,  0, 0);
    gl->addWidget(booksBtn,  0, 1);
    gl->addWidget(contBtn,   1, 0);
    gl->addWidget(propBtn,   1, 1);
    gl->addWidget(searchBtn, 2, 0);
    gl->addWidget(ocrBtn,    2, 1);

    // Comments panel (idx 2): markup tools on top + list of PDF comments below
    m_commentsPanel = new QWidget;
    {
        auto* cpLay = new QVBoxLayout(m_commentsPanel);
        cpLay->setContentsMargins(4, 4, 4, 4);
        cpLay->setSpacing(4);
        auto* toolWrap = new QWidget;
        auto* tgl = new QGridLayout(toolWrap);
        tgl->setContentsMargins(0, 0, 0, 0);
        tgl->setSpacing(2);
        struct ToolDef { const char* label; int id; };
        // Pick = id 0 (ViewTool::Pan): chon/keo markup co san, dat dau luoi vi
        // la trang thai mac dinh (SPEC_FIX_PICK_TOOL). Chon chu chi con o nut
        // Select tren toolbar (id 10) — KHONG dua id 10 vao luoi nay nua.
        const ToolDef tools[] = {
            {"Pick", 0}, {"Line", 2}, {"Arrow", 3}, {"Rect", 4},
            // 🔴 0903 owner chot: BO nut "Note" (id 1) khoi bang cong cu.
            // Ly do: Note cua app KHONG phai annotation that — no chen VAT THE vao
            // noi dung trang (`TRNote`) roi dat annot thanh HIDDEN, nen dinh chat vao
            // bo dung trang va la goc cua loat loi 02-03/09. Text da duoc viet lai
            // thanh FreeText THAT nen thay duoc vai tro ghi chu.
            // ⚠️ CHI bo NUT TAO. Duong DOC/HIEN Note cu trong file khach GIU NGUYEN —
            //    khong duoc lam hong tai lieu da luu.
            {"Ellipse", 5}, {"Cloud", 6}, {"Text", 7},
            {"Freehand", 8}, {"Highlight", 9}
        };
        int r = 0, c = 0;
        for (const auto& td : tools) {
            auto* b = new QPushButton(QString::fromUtf8(td.label));
            // Ten de bo do giao dien tim duoc bang findChildren (chi doc, khong
            // co quy tac QSS nao dung ten nay nen khong doi ve ngoai).
            b->setObjectName(QStringLiteral("markupTool"));
            b->setFixedHeight(24);
            const int id = td.id;
            if (id == 0)
                b->setToolTip(QStringLiteral("Select and move existing markup"));
            m_toolButtons.insert(id, b);
            connect(b, &QPushButton::clicked, this, [this, id]{ setActiveToolButton(id); updateSizeComboForTool(id); emit annotToolSelected(id); });
            tgl->addWidget(b, r, c);
            if (++c == 2) { c = 0; ++r; }
        }
        setActiveToolButton(0);
        cpLay->addWidget(toolWrap);
        // Insert Image (SPEC_INSERT_IMAGE_2026-08-30): mot nut Insert kich hoat
        // hộp chon anh (PNG trong suot duoc uu tien). Khong phai ViewTool.
        auto* insertBtn = new QPushButton(QStringLiteral("📷 Image"));   // 0903: vao luoi, nhan ngan cho vua nua o
        insertBtn->setObjectName(QStringLiteral("insertImageTool"));
        insertBtn->setFixedHeight(24);
        QString insertTip;
        if (m_dark) insertTip = darkHC().fg; else insertTip = lightHC().fg;
        insertBtn->setStyleSheet(
            QStringLiteral("color:%1;background:%2;border:1px solid %3;border-radius:4px;")
                .arg(insertTip)
                .arg(m_dark ? darkHC().bgAlt : lightHC().bgAlt)
                .arg(m_dark ? darkHC().border : lightHC().border));
        connect(insertBtn, &QPushButton::clicked, this, [this]{ emit insertImageRequested(); });
        // 🔴 0903 owner chot: dua Insert Image VAO LUOI lam o thu 10, dap cho
        // trong cua Note vua bo => 9 + 1 = 10 nut = dung 5 hang x 2 cot, het o le.
        tgl->addWidget(insertBtn, r, c);
        auto* propWrap = new QWidget;
        auto* pgl = new QGridLayout(propWrap);
        pgl->setContentsMargins(0, 2, 0, 2);
        pgl->setSpacing(3);
        m_sizeCombo = new QComboBox;
        m_sizeCombo->addItems({ "1 px", "2 px", "3 px", "4 px", "5 px", "6 px",
                                "8 px", "10 px", "12 px", "16 px", "20 px", "24 px" });
        m_sizeCombo->setCurrentIndex(1);
        m_colorBtn = new QPushButton;
        updateColorBtnStyle();
        auto* fillChk = new QCheckBox("Fill");
        auto* fillOpacityCombo = new QComboBox;
        fillOpacityCombo->addItems({"25%","50%","75%","100%"});
        fillOpacityCombo->setCurrentIndex(1);
        pgl->addWidget(m_sizeCombo, 0, 0);
        pgl->addWidget(m_colorBtn, 0, 1);
        pgl->addWidget(fillChk,    0, 2);
        pgl->addWidget(fillOpacityCombo, 0, 3);
        cpLay->addWidget(propWrap);
        auto emitStyle = [this]{ emit annotStyleChanged(m_annColor, m_annWidth, m_annFill, m_annFillOpacity, m_annFontSize); };
        // 🔴 0903: HAI NGUON CHAN LY cho co chu. ThumbnailPanel::m_annFontSize mac dinh 24
        // (dung o combo owner nhin thay), con AnnotStyle::fontSize ben MainWindow mac dinh 11.
        // annotStyleChanged CHI phat khi owner DOI combo — khong doi thi MainWindow giu 11
        // => thanh cong cu ghi 24 ma chu thich tao ra dung 11 (o zoom 53% = 5,8 px, nhoe thanh vet).
        // Phat MOT LAN ngay sau khi dung xong widget de hai ben khop tu dau.
        QMetaObject::invokeMethod(this, emitStyle, Qt::QueuedConnection);
        connect(m_sizeCombo, &QComboBox::currentIndexChanged, this, [this, emitStyle](int i){
            if (i < 0) return;
            if (m_sizeIsFont) {
                const int pts[] = { 8, 12, 16, 24, 32, 48, 64, 96, 144, 200 };
                m_annFontSize = pts[qBound(0, i, 9)];
            } else {
                const double w[] = { 1,2,3,4,5,6,8,10,12,16,20,24 };
                m_annWidth = w[qBound(0, i, 11)];
            }
            emitStyle();
        });
        connect(m_colorBtn, &QPushButton::clicked, this, [this, emitStyle]{
            QColor c = QColorDialog::getColor(m_annColor, this, "Markup color");
            if (c.isValid()) {
                m_annColor = c;
                updateColorBtnStyle();
                emitStyle();
            }
        });
        connect(fillChk, &QCheckBox::toggled, this, [this, emitStyle](bool on){
            m_annFill = on;
            emitStyle();
        });
        {
            const int vals[] = {25, 50, 75, 100};
            connect(fillOpacityCombo, &QComboBox::currentIndexChanged, this, [this, vals, emitStyle](int i){
                m_annFillOpacity = vals[qBound(0, i, 3)];
                emitStyle();
            });
        }
        auto* sep = new QFrame;
        sep->setFrameShape(QFrame::HLine);
        sep->setFrameShadow(QFrame::Plain);
        sep->setObjectName("sidebarSep");
        sep->setStyleSheet(
            QStringLiteral("color:%1;").arg(m_dark ? darkHC().border : lightHC().border));
        m_commentsSep = sep;
        cpLay->addWidget(sep);
        m_commentsHint = new QLabel(QStringLiteral("Editing comments \u2014 single-page view only"));
        QFont hintFont = m_commentsHint->font();
        hintFont.setPointSizeF(hintFont.pointSizeF() - 1);
        m_commentsHint->setFont(hintFont);
        m_commentsHint->setStyleSheet(
            QStringLiteral("color:%1;").arg(m_dark ? darkHC().fgDim : lightHC().fgDim));
        cpLay->addWidget(m_commentsHint);
        m_commentsList = new QListWidget;
        connect(m_commentsList, &QListWidget::itemClicked, this, [this](QListWidgetItem* it){
            if (it) emit commentActivated(it->data(Qt::UserRole).toInt(), it->data(Qt::UserRole + 1).toInt());
        });
        cpLay->addWidget(m_commentsList, 1);
    }

    m_stack = new QStackedWidget;
    m_stack->setFrameShape(QFrame::NoFrame);
    m_stack->addWidget(m_list);           // idx 0 — Thumbnails
    m_stack->addWidget(m_outline);        // idx 1 — Bookmarks
    m_stack->addWidget(m_commentsPanel);  // idx 2 — Comments
    m_stack->addWidget(m_propertiesTree); // idx 3 — Properties
    m_stack->addWidget(m_searchPanel);    // idx 4 — Search
    m_stack->addWidget(m_ocrPanel);       // idx 5 — OCR

    connect(m_tabGroup, &QButtonGroup::idClicked, m_stack, &QStackedWidget::setCurrentIndex);
    connect(m_tabGroup, &QButtonGroup::idClicked, this, [this](int id) {
        if (id == 3 && m_propertiesTree->topLevelItemCount() == 0
                && m_doc && m_doc->isOpen())
            buildProperties();
        // ponytail: lazy comment loading — load only when user opens the tab
        if (id == 2)
            emit requestComments();
        if (id == 4)
            m_searchPanel->focusInput();
        if (id == 5)
            m_ocrPanel->refresh();
    });
    // Relay search signals from SearchPanel out through ThumbnailPanel
    connect(m_searchPanel, &SearchPanel::searchRequested,
            this, &ThumbnailPanel::searchRequested);
    connect(m_searchPanel, &SearchPanel::resultSelected,
            this, &ThumbnailPanel::searchResultSelected);
    connect(m_searchPanel, &SearchPanel::searchCleared,
            this, &ThumbnailPanel::searchCleared);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(tabGrid, 0);
    layout->addWidget(m_stack, 1);

    // Wire drag-reorder: pages
    connect(m_list->model(), &QAbstractItemModel::rowsMoved, this,
            [this](const QModelIndex&, int, int, const QModelIndex&, int) {
        QTimer::singleShot(0, this, [this]() {
            QList<int> newOrder;
            for (int i = 0; i < m_list->count(); ++i)
                newOrder.append(m_list->item(i)->data(Qt::UserRole).toInt());
            emit pagesReordered(newOrder);
        });
    });

    // Wire drag-reorder: bookmarks (top-level only).
    // Qt6 QTreeWidget with InternalMove does NOT reliably emit rowsMoved —
    // use the onDropped callback in BookmarkTreeWidget instead.
    static_cast<BookmarkTreeWidget*>(m_outline)->onDropped = [this]() {
        QList<int> newOrder;
        for (int i = 0; i < m_outline->topLevelItemCount(); ++i)
            newOrder.append(m_outline->topLevelItem(i)->data(0, Qt::UserRole + 1).toInt());
        emit bookmarksReordered(newOrder);
    };

    // Wire drag-reorder: content tab (same page-reorder as thumbnails).
    static_cast<BookmarkTreeWidget*>(m_contentTree)->onDropped = [this]() {
        QList<int> newOrder;
        for (int i = 0; i < m_contentTree->topLevelItemCount(); ++i)
            newOrder.append(m_contentTree->topLevelItem(i)->data(0, Qt::UserRole).toInt());
        emit pagesReordered(newOrder);
    };
    updateSizeComboForTool(0);
}

ThumbnailPanel::~ThumbnailPanel() {}

// ── resizeEvent ───────────────────────────────────────────────────────────────
void ThumbnailPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    QTimer::singleShot(0, this, [this]() { requestVisibleThumbnails(); });
}

// ── requestVisibleThumbnails ───────────────────────────────────────────────────
// 🔴 0928 LƯỢT 15b — VÙNG ĐANG XEM PHẢI LÀ "HÀNG THẬT SỰ CÓ MẶT TRONG KHUNG".
// Lượt 15 đo bằng `m_list->itemAt(QPoint(0, 1))` và nó LUÔN trả nullptr:
// `QListView::indexAt` mở đầu bằng
//     if (flow == LeftToRight && p.x() <= header->width()) return model->index(0,0,header);
// Tức là x=0 được coi là điểm bấm lên ô HEADER (rè dọc trái), nó trả về index
// thuộc model của header — khác model của list ⇒ `QListWidget::itemFromIndex`
// trả nullptr ⇒ `topRow = 0` MÃI MÃI. Log CEO đo đúng thế: 61 lần `vung=0-3`
// trong khi khung đang hiện 181–185. Hệ quả: xếp hàng theo cự ly tới vùng 0–3
// rơi thành tuần tự 0,1,2,… nên trang 181 phải chờ 180 lượt (~27 s).
// Nay duyệt `visualItemRect` của từng hàng và lấy đúng dải giao với
// `viewport()->rect()` — không đoán bằng công thức chia lưới, không đụng header.
void ThumbnailPanel::requestVisibleThumbnails() {
    int n = m_list->count();
    if (n == 0) return;

    const QRect vp = m_list->viewport()->rect();
    int first = -1, last = -1;
    for (int r = 0; r < n; ++r) {
        // `QListWidget::visualItemRect` che (hides) bản gốc của QAbstractItemView
        // và nhận con trỏ ITEM, không nhận QModelIndex.
        if (!m_list->visualItemRect(m_list->item(r)).intersects(vp))
            continue;
        if (first < 0) first = r;
        last = r;
    }
    if (first < 0) {
        qDebug() << "[thumbq] DO viewport rong — bo qua lan do nay (chua layout)";
        return;
    }

    // ponytail: không tính lại lưới (số cột × số hàng) — số hàng thật đã có trong
    // `last - first + 1`, còn `ahead` chỉ là biên an toàn quanh vùng đang xem nên
    // lệch vài hàng không làm hỏng thứ tự xếp.
    const int ahead = (last - first + 1) + 2;   // + 2 hàng đệm phía dưới

    if (m_thumbPool && !m_thumbPool->isOpen())
        qDebug() << "[perf] thumb requestVisibleThumbnails but pool is NOT open";
    // 🔴 0928 LƯỢT 15 — BÁO VÙNG ĐANG XEM TRƯỚC KHI XẾP. Đây là thứ quyết định
    // thứ tự ra khỏi hàng: không có nó thì hàng chỉ biết "ô nào priority 0"
    // mà không biết "ô nào ĐANG HIỆN", nên 330 việc nền ngang hàng chen vào
    // giữa 5 ô đang xem. Đặt TRƯỚC vòng xếp để yêu cầu của chính lượt cuộn này
    // cũng được xếp theo vùng mới.
    if (m_thumbPool && m_thumbPool->isOpen())
        m_thumbPool->setVisibleBand(first, last,
                                    m_list->verticalScrollBar()->value(),
                                    m_list->verticalScrollBar()->maximum());
    for (int i = first; i < qMin(first + ahead, n); ++i) {
        auto* item = m_list->item(i);
        if (!item || !item->icon().isNull()) continue;
        int priority = (i <= last) ? 0 : 1;
        if (m_thumbPool && m_thumbPool->isOpen())
            m_thumbPool->requestThumbnail(i, priority);
    }
}

// ── setDocument ───────────────────────────────────────────────────────────────
void ThumbnailPanel::setDocument(PdfDocument* doc, PdfRenderer* renderer,
                                  ThumbnailRenderPool* pool, bool forceRebuild) {
    // Epoch must always track the current pool, even when we don't rebuild the list.
    // If we skip the epoch update on early-return, thumbnails rendered after a
    // pool-reopen (epoch++) will all be gated out by the stale m_acceptEpoch.
    m_acceptEpoch = pool ? pool->epoch() : 0;

    if (!forceRebuild && doc == m_doc && renderer == m_renderer
        && pool == m_thumbPool && m_list->count() > 0) {
        ++m_dbgEarlyReturn;
        return;
    }

    if (!m_pendingThumbs.isEmpty()) {
        qDebug() << "[perf] thumb DROP stale pending n=" << m_pendingThumbs.size()
                 << "(document changed)";
        m_pendingThumbs.clear();
    }
    m_doc = doc;
    // Disconnect the PREVIOUS pool's relay so a background tab's in-flight
    // thumbnails stop reaching this panel (SPEC_THUMB_DISPLAY 31/08: the old
    // per-worker direct connections below were never torn down on tab switch,
    // so every tab kept feeding the visible list -> double delivery + cross-tab
    // pollution + wrong-pageCount DROPPED-EARLY strands).
    if (m_thumbPool) disconnect(m_thumbPoolConn);
    m_renderer  = renderer;
    m_thumbPool = pool;
    if (m_ocrPanel) m_ocrPanel->setDocument(doc);    if (m_thumbPool) {
        m_thumbPoolConn = connect(m_thumbPool, &ThumbnailRenderPool::thumbnailReady,
                                  this, &ThumbnailPanel::onPageReady);
        if (!m_thumbPoolConn)
            qDebug() << "[perf] thumb pool CONNECT FAILED";
        else
            qDebug() << "[perf] thumb pool connected ok isOpen=" << m_thumbPool->isOpen()
                     << "pool=" << (void*)m_thumbPool << "panel=" << (void*)this;
    }
    // ponytail: no per-worker direct connections — the pool relay already forwards
    // every ThumbnailWorker::thumbnailReady. The old "fallback" doubled delivery and
    // leaked across tabs (only the relay was disconnected on switch, not the workers).
    m_list->clear();
    m_currentPage = -1;
    m_contentGen.fetchAndAddOrdered(1);
    m_bookmarkGen.fetchAndAddOrdered(1);
    m_contentTree->clear();
    m_propertiesTree->clear();

    if (!m_doc || !m_renderer) return;

    int n = m_doc->pageCount();

    for (int i = 0; i < n; ++i) {
        auto* item = new QListWidgetItem(QString::number(i + 1), m_list);
        item->setSizeHint({125, 170});
        item->setData(Qt::UserRole, i);
    }
    flushPendingThumbs();

    // ThumbnailRenderPool only — never fall back to PdfRenderer.
    // If pool is unavailable, thumbnails stay empty (blank placeholder).
    if (m_thumbPool && m_thumbPool->isOpen()) {
        qDebug() << "[perf] thumb prefetch called n=" << n << "poolOpen=1";
        static const int kBatch = 20;
        int firstBatch = qMin(n, kBatch);
        for (int i = 0; i < qMin(n, 4); ++i)
            m_thumbPool->requestThumbnail(i, 0);
        m_thumbPool->prefetchRange(0, firstBatch - 1);
        qDebug() << "[perf] thumb prefetch batch 0-" << (firstBatch - 1)
                 << "(deferred rest=" << (n - firstBatch) << ")";
        for (int start = firstBatch; start < n; start += kBatch) {
            int end = qMin(start + kBatch, n);
            int s = start, e = end;
            QTimer::singleShot(100 * (s / kBatch), this, [this, s, e]() {
                if (m_thumbPool && m_thumbPool->isOpen()) {
                    m_thumbPool->prefetchRange(s, e - 1);
                    qDebug() << "[perf] thumb prefetch batch" << s << "-" << (e - 1);
                }
            });
        }
    } else if (m_thumbPool) {
        qDebug() << "[perf] thumb pool not yet open, will retry in 200ms";
        QTimer::singleShot(200, this, [this, n]() {
            if (!m_thumbPool || !m_doc) return;
            if (m_thumbPool->isOpen()) {
                qDebug() << "[perf] thumb prefetch (delayed) n=" << n << "poolOpen=1";
                static const int kBatch = 20;
                int firstBatch = qMin(n, kBatch);
                for (int i = 0; i < qMin(n, 4); ++i)
                    m_thumbPool->requestThumbnail(i, 0);
                m_thumbPool->prefetchRange(0, firstBatch - 1);
                qDebug() << "[perf] thumb prefetch batch 0-" << (firstBatch - 1)
                         << "(deferred rest=" << (n - firstBatch) << ")";
                for (int start = firstBatch; start < n; start += kBatch) {
                    int end = qMin(start + kBatch, n);
                    int s = start, e = end;
                    QTimer::singleShot(100 * (s / kBatch), this, [this, s, e]() {
                        if (m_thumbPool && m_thumbPool->isOpen()) {
                            m_thumbPool->prefetchRange(s, e - 1);
                            qDebug() << "[perf] thumb prefetch batch" << s << "-" << (e - 1);
                        }
                    });
                }
            } else {
                qDebug() << "[perf] thumb pool FAILED to open after retry — thumbnails will be empty";
            }
        });
    } else {
        qDebug() << "[perf] thumb pool not available — thumbnails will be empty";
    }

    if (m_scrollConn) disconnect(m_scrollConn);
    // 🔴 0928 LƯỢT 15b — DEBOUNCE 50 ms. Một cú cuộn bánh xe 60 nấc bắn ra 60
    // lần `valueChanged`; đo ngay từng lần thì lúc nào cũng kịp lúc nào không,
    // và phép `visualItemRect` có thể chạy khi `doItemsLayout` chưa xong. Gom lại
    // một lần đo sau khi bánh xe đã đứng yên là đủ — vùng đang xem không đổi
    // trong 50 ms đó.
    m_scrollDebounce.setSingleShot(true);
    m_scrollDebounce.setInterval(50);
    m_scrollConn = connect(m_list->verticalScrollBar(), &QScrollBar::valueChanged,
            this, [this]() { m_scrollDebounce.start(); });
    connect(&m_scrollDebounce, &QTimer::timeout, this,
            [this]() { requestVisibleThumbnails(); });

    // Request thumbnails for the current visible region
    QTimer::singleShot(0, this, [this]() { requestVisibleThumbnails(); });

    buildBookmarks();
    // Properties built lazily on tab click; pre-populate now only if already visible
    if (m_stack->currentIndex() == 3)
        buildProperties();
}

// ── setCurrentPage ────────────────────────────────────────────────────────────
void ThumbnailPanel::setCurrentPage(int pageIndex) {
    if (m_currentPage >= 0 && m_currentPage < m_list->count())
        m_list->item(m_currentPage)->setBackground(Qt::transparent);
    m_currentPage = pageIndex;
    if (pageIndex >= 0 && pageIndex < m_list->count()) {
        auto* item = m_list->item(pageIndex);
        item->setBackground(currentPageHighlight());
        m_list->scrollToItem(item, QAbstractItemView::PositionAtCenter);
        QTimer::singleShot(0, this, [this]() { requestVisibleThumbnails(); });
    }
    syncBookmarkToPage(pageIndex);
    if (m_ocrPanel) m_ocrPanel->setCurrentPage(pageIndex);
    m_list->viewport()->update();
}

// Chuyen tab sidebar theo id (dung cho probe --uiprobe va dieu khien tu ma).
void ThumbnailPanel::selectTab(int id) {
    if (m_tabGroup) {
        if (auto* b = m_tabGroup->button(id)) b->setChecked(true);
    }
    if (m_stack) m_stack->setCurrentIndex(id);
    if (id == 5 && m_ocrPanel) m_ocrPanel->refresh();
    emit sidebarTabChanged(id);
}

// ── thumbnailForPage ──────────────────────────────────────────────────────────
QImage ThumbnailPanel::thumbnailForPage(int pageIndex) const {
    if (!m_list || pageIndex < 0 || pageIndex >= m_list->count())
        return {};
    auto* item = m_list->item(pageIndex);
    if (!item) return {};
    QIcon icon = item->icon();
    if (icon.isNull()) return {};
    QList<QSize> sizes = icon.availableSizes();
    if (sizes.isEmpty()) return {};
    QPixmap pix = icon.pixmap(sizes.first());
    if (pix.isNull()) return {};
    return pix.toImage();
}

// ── clearThumbnails ───────────────────────────────────────────────────────────
void ThumbnailPanel::clearThumbnails() {
    m_doc = nullptr;
    if (m_scrollConn) disconnect(m_scrollConn);
    m_renderer = nullptr;
    // 🔴 0928 LƯỢT 22 (reviewer mục 5): panel còn giữ con trỏ POOL của tab đang
    // đóng — pool chết ở closeJob nền, trong lúc scrollEvent gọi m_thumbPool->
    // isOpen()/requestThumbnail (568-584) là đọc vùng đã free. Ngắt relay + null.
    if (m_thumbPool) { disconnect(m_thumbPoolConn); m_thumbPool = nullptr; }
    m_list->clear();
    m_pendingThumbs.clear();
    m_outline->clear();
    m_contentGen.fetchAndAddOrdered(1);
    m_bookmarkGen.fetchAndAddOrdered(1);
    m_contentTree->clear();
    m_propertiesTree->clear();
    m_currentPage = -1;
    m_annotMgr   = nullptr;
    m_annotPages = 0;
    setCommentsLoading(false);
    setComments({});
    // Panel OCR phai ve trang thai "khong co tai lieu" ngay (SPEC_OCRPANEL_POLISH).
    if (m_ocrPanel) m_ocrPanel->setDocument(nullptr);
}

// ── Search helpers (kept as no-ops; panel hidden) ─────────────────────────────
void ThumbnailPanel::addSearchResult(const SearchResult& result) {
    m_searchPanel->addResult(result);
}
void ThumbnailPanel::clearSearchResults() { m_searchPanel->clearResults(); }
void ThumbnailPanel::resetSearch() { m_searchPanel->reset(); }
void ThumbnailPanel::setSearchResults(const QString& query, const QList<SearchResult>& results) {
    m_searchPanel->reset();
    m_searchPanel->setQuery(query);
    for (const SearchResult& r : results)
        m_searchPanel->addResult(r);
}
void ThumbnailPanel::setSearchProgress(int pagesScanned, int totalPages) {
    m_searchPanel->setSearchProgress(pagesScanned, totalPages);
}
void ThumbnailPanel::activateSearch() {
    if (m_tabGroup->button(4)) {
        m_tabGroup->button(4)->setChecked(true);
        m_stack->setCurrentIndex(4);
    }
    m_searchPanel->focusInput();
}

// ── setActiveToolButton ────────────────────────────────────────────────────────
void ThumbnailPanel::setActiveToolButton(int id) {
    m_activeTool = id;
    applyToolButtonStyles();
}

// ── activateToolFromGrid ───────────────────────────────────────────────────────
void ThumbnailPanel::activateToolFromGrid(int id) {
    setActiveToolButton(id);
    updateSizeComboForTool(id);
    emit annotToolSelected(id);
}

// ── applyToolButtonStyles ──────────────────────────────────────────────────────
void ThumbnailPanel::applyToolButtonStyles() {
    const ThemeTokens& t = m_dark ? darkHC() : lightHC();
    for (auto it = m_toolButtons.constBegin(); it != m_toolButtons.constEnd(); ++it) {
        if (it.key() == m_activeTool)
            it.value()->setStyleSheet(
                QStringLiteral("background:%1; color:%2; border:1px solid %3; font-weight:bold;")
                    .arg(t.selBg, t.selFg, t.focus));
        else
            it.value()->setStyleSheet("");
    }
}

// ── updateColorBtnStyle ────────────────────────────────────────────────────────
void ThumbnailPanel::updateColorBtnStyle() {
    if (!m_colorBtn) return;
    // Nen = mau markup nguoi dung chon (du lieu), chu = tu tinh theo do choi
    // cua nen de luon tuong phan.
    m_colorBtn->setStyleSheet(
        QStringLiteral("background:%1; color:%2;").arg(m_annColor.name(), textColorOn(m_annColor)));
}

// ── updateSizeComboForTool ─────────────────────────────────────────────────────
void ThumbnailPanel::updateSizeComboForTool(int toolId) {
    const bool wantFont = (toolId == 7);
    if (wantFont == m_sizeIsFont && m_sizeCombo->count() > 0) return;
    m_sizeIsFont = wantFont;
    QSignalBlocker block(m_sizeCombo);
    m_sizeCombo->clear();
    if (wantFont) {
        const int pts[] = { 8, 12, 16, 24, 32, 48, 64, 96, 144, 200 };
        int sel = 3;
        for (int i = 0; i < 10; ++i) {
            m_sizeCombo->addItem(QString("%1 pt").arg(pts[i]));
            if (qFuzzyCompare(m_annFontSize, double(pts[i]))) sel = i;
        }
        m_sizeCombo->setCurrentIndex(sel);
        m_annFontSize = pts[sel];
    } else {
        const int px[] = { 1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 24 };
        int sel = 1;
        for (int i = 0; i < 12; ++i) {
            m_sizeCombo->addItem(QString("%1 px").arg(px[i]));
            if (qFuzzyCompare(m_annWidth, double(px[i]))) sel = i;
        }
        m_sizeCombo->setCurrentIndex(sel);
        m_annWidth = px[sel];
    }
}

// ── setDarkMode ───────────────────────────────────────────────────────────────
void ThumbnailPanel::setDarkMode(bool dark) {
    m_dark = dark;
    if (m_commentsHint)
        m_commentsHint->setStyleSheet(
            QStringLiteral("color:%1;").arg(dark ? darkHC().fgDim : lightHC().fgDim));
    if (m_commentsSep)
        m_commentsSep->setStyleSheet(
            QStringLiteral("color:%1;").arg(dark ? darkHC().border : lightHC().border));
    applyToolButtonStyles();
    if (m_ocrPanel) m_ocrPanel->setDarkMode(dark);
    if (m_currentPage >= 0 && m_currentPage < m_list->count())
        m_list->item(m_currentPage)->setBackground(currentPageHighlight());
    // 0927 LUOT 5 vong 2: icon + vien cua nut "mo o sua lon" ve theo mau CUNG
    // luc DUNG giu chuot. setComments() chay mot lan khi nap danh sach, nen
    // doi theme sau do se de lai icon mau cu — phai ve lai tai day.
    if (m_commentsList) {
        for (int i = 0; i < m_commentsList->count(); ++i) {
            QWidget* row = m_commentsList->itemWidget(m_commentsList->item(i));
            if (!row) continue;
            for (auto* b : row->findChildren<QPushButton*>()) {
                if (!b->property("trExpand").toBool()) continue;
                b->setIcon(commentExpandIcon(dark));
                b->setStyleSheet(
                    QStringLiteral(
                        "QPushButton { background:transparent; border:1px solid %1; border-radius:3px; }"
                        "QPushButton:hover { border:1px solid %2; }")
                        .arg(dark ? darkHC().border : lightHC().border,
                             dark ? darkHC().accent : lightHC().accent));
            }
        }
    }
}

// ── currentPageHighlight ───────────────────────────────────────────────────────
QColor ThumbnailPanel::currentPageHighlight() const {
    QColor c(m_dark ? darkHC().accent : lightHC().accent);
    c.setAlpha(90);
    return c;
}

// ── onPageReady ───────────────────────────────────────────────────────────────
void ThumbnailPanel::acceptFromFullRender(int pageIndex, const QImage& fullImg)
{
    if (fullImg.isNull() || pageIndex < 0 || !m_list || pageIndex >= m_list->count()) return;
    auto* item = m_list->item(pageIndex);
    if (!item || !item->icon().isNull()) return;   // da co thumbnail roi thi thoi
    const int tw = int(PdfRenderer::kThumbMaxPx);
    // ⚠️ KHONG dat ten bien la `small` — tren Windows do la MACRO (typedef char small trong
    // rpcndr.h) nen `QImage small` bi dich thanh `QImage char` => C2628.
    QImage thumbImg = fullImg.width() > tw
                    ? fullImg.scaledToWidth(tw, Qt::SmoothTransformation)
                    : fullImg;
    item->setIcon(QIcon(QPixmap::fromImage(thumbImg)));
    ++m_dbgAccepted;
    qDebug() << "[thumb] TAI SU DUNG anh day du page=" << pageIndex
             << "tu" << fullImg.width() << "px ->" << thumbImg.width() << "px (khoi doc lai trang)";
    emit lowResPageAvailable(pageIndex, thumbImg);
}

void ThumbnailPanel::onPageReady(int pageIndex, const QImage& image, quint64 epoch, bool draft) {
    struct _SlotMs {
        QElapsedTimer t; const char* name; int pg;
        _SlotMs(const char* n, int p) : name(n), pg(p) { t.start(); }
        ~_SlotMs() { if (t.elapsed() > 50) qDebug().noquote() << "[slotms]" << name << "ms=" << t.elapsed() << "page=" << pg; }
    };
    _SlotMs _sm("thumbOnPageReady", pageIndex);
    if (m_acceptEpoch != 0 && epoch != m_acceptEpoch) {
        qDebug() << "[perf] thumb DROP stale page=" << pageIndex
                 << "epoch=" << epoch << "accept=" << m_acceptEpoch;
        ++m_dbgDropped;
        return;
    }
    qDebug() << "[perf] thumb recv page=" << pageIndex
             << "imgW=" << image.width()
             << "listCount=" << (m_list ? m_list->count() : -1)
             << "hasDoc=" << (m_doc != nullptr)
             << "sender=" << (sender() ? sender()->metaObject()->className() : "null")
             << "panel=" << (void*)this;
    if (!m_doc || pageIndex < 0 || !m_list || pageIndex >= m_list->count()) {
        qDebug() << "[perf] thumb DROPPED-EARLY page=" << pageIndex
                 << "reason=listNotReady listCount=" << (m_list ? m_list->count() : -1)
                 << "hasDoc=" << (m_doc != nullptr);
        if (pageIndex >= 0) {
            ++m_dbgPending;
            m_pendingThumbs[pageIndex] = {epoch, image};
        }
        return;
    }
    // 🔴 SUA 2026-08-31 — GOC CUA "file kho A4 khong hien thumbnail nao".
    // Chot cu so anh voi NUA CHIEU RONG TRANG TINH BANG POINT — mot phep so sai don vi
    // (pixel so voi point). Voi A0 rong 2384pt thi nguong 1192px nen anh 900px lot;
    // nhung voi A4 rong 612pt thi nguong chi 306px => MOI thumbnail 900px deu bi VUT SACH.
    // (kThumbMaxPx vua duoc nang 400 -> 900 trong ngay nen loi nay moi lo ra.)
    // Y dinh that cua chot: chan anh KICH THUOC TRANG DAY DU lot vao thanh ben.
    // Nen so voi tran thumbnail, dung don vi pixel ca hai ve.
    if (image.width() > int(PdfRenderer::kThumbMaxPx * 1.5)) {
        qDebug() << "[perf] thumb REJECTED page=" << pageIndex
                 << "imgW=" << image.width() << "tran=" << int(PdfRenderer::kThumbMaxPx * 1.5);
        ++m_dbgRejected;
        return;
    }
    if (auto* item = m_list->item(pageIndex)) {
        // Dem MOT lan cho moi trang — prefetch duoc goi tu nhieu cho (openFile +
        // setDocument batch + requestVisibleThumbnails) nen cung mot trang co the
        // den nhieu luot; dem moi luot lam m_dbgAccepted vuot so trang, vo nghia.
        if (item->icon().isNull()) ++m_dbgAccepted;
        item->setIcon(QIcon(QPixmap::fromImage(image)));
    }
    // Day ban tho sang che do xem lien tuc: co san anh mo de ve ngay khi pan/zoom.
    // ⛔ 0928 LƯỢT 15 — KHÔNG đưa BẢN NHÁP vào đây. `setPageLowRes` cắm cờ
    // `daCoGiDeNhin` (ContinuousView.cpp:1680) và chính cờ đó CHẶN lịch render
    // thật ⇒ một ô trắng 5 giây sẽ thành trang nửa hình vĩnh viễn ở vùng chính.
    // Bản nháp chỉ dành cho ô thumbnail; lần vẽ trọn sau sẽ ghi đè.
    if (!image.isNull() && !draft) emit lowResPageAvailable(pageIndex, image);
}

void ThumbnailPanel::flushPendingThumbs() {
    if (m_pendingThumbs.isEmpty()) return;
    int n = m_list ? m_list->count() : 0;
    int staleDropped = 0;
    for (auto it = m_pendingThumbs.begin(); it != m_pendingThumbs.end();) {
        if (it.value().first != m_acceptEpoch) {
            ++staleDropped;
            it = m_pendingThumbs.erase(it);
            continue;
        }
        int pg = it.key();
        if (pg >= 0 && pg < n) {
            if (auto* item = m_list->item(pg)) {
                if (item->icon().isNull()) ++m_dbgAccepted;
                item->setIcon(QIcon(QPixmap::fromImage(it.value().second)));
            }
            it = m_pendingThumbs.erase(it);
        } else {
            ++it;
        }
    }
    if (staleDropped)
        qDebug() << "[perf] thumb DROP stale pending n=" << staleDropped << "(gen mismatch during flush)";
    if (!m_pendingThumbs.isEmpty())
        qDebug() << "[perf] thumb" << m_pendingThumbs.size()
                 << "pending thumb(s) still waiting for list to grow";
}

// ── buildBookmarks (async) ────────────────────────────────────────────────────
struct BmEntry { int depth; QString title; int page; };

void ThumbnailPanel::buildBookmarks() {
    m_outline->clear();
    if (!m_doc || !m_doc->isOpen()) return;

    FPDF_DOCUMENT doc   = m_doc->raw();
    int           myGen = m_bookmarkGen.fetchAndAddOrdered(1) + 1;

    auto* watcher = new QFutureWatcher<QVector<BmEntry>>(this);

    // 🔴 0928 LƯỢT 22 (reviewer mục 2): token ĐĂNG KÝ LÚC SPAWN (UI thread) + RAII
    // move — beginClose thấy cả task còn xếp hàng.
    // 0928 LƯỢT 14: token sổ việc nền — FPDFBookmark_* đọc doc; QFuture bị bỏ
    // rơi (chỉ giữ trong watcher) nên trước đây đóng tab lúc đang dựng bookmark
    // là đọc vùng đã free.
    trdoc::Task task(doc, "thumbPanel/bookmark");
    auto future = QtConcurrent::run([task = std::move(task), doc, myGen, this]() -> QVector<BmEntry> {
        QVector<BmEntry> entries;
        TimedPdfiumLock lock(__FILE__, __LINE__);
        if (m_bookmarkGen.loadAcquire() != myGen) return entries;

        std::function<void(FPDF_BOOKMARK, int)> walk;
        walk = [&](FPDF_BOOKMARK bm, int depth) {
            while (bm) {
                if (m_bookmarkGen.loadAcquire() != myGen) return;

                unsigned long len = FPDFBookmark_GetTitle(bm, nullptr, 0);
                std::vector<char> buf(len + 2, 0);
                FPDFBookmark_GetTitle(bm, buf.data(), len);
                QString title = QString::fromUtf16(
                    reinterpret_cast<const char16_t*>(buf.data()));
                if (title.isEmpty()) title = "(untitled)";

                FPDF_DEST dest = FPDFBookmark_GetDest(doc, bm);
                // Fallback: bookmark uses /A GoTo action instead of /Dest directly
                if (!dest) {
                    FPDF_ACTION action = FPDFBookmark_GetAction(bm);
                    if (action && FPDFAction_GetType(action) == PDFACTION_GOTO)
                        dest = FPDFAction_GetDest(doc, action);
                }
                int page = dest ? FPDFDest_GetDestPageIndex(doc, dest) : -1;

                entries.append({depth, title, page});

                FPDF_BOOKMARK child = FPDFBookmark_GetFirstChild(doc, bm);
                if (child) walk(child, depth + 1);
                bm = FPDFBookmark_GetNextSibling(doc, bm);
            }
        };
        walk(FPDFBookmark_GetFirstChild(doc, nullptr), 0);
        return entries;
    });

    connect(watcher, &QFutureWatcher<QVector<BmEntry>>::finished, this,
            [this, watcher, myGen]() {
        watcher->deleteLater();
        if (m_bookmarkGen.loadAcquire() != myGen) return;

        auto entries = watcher->result();
        m_outline->clear();

        if (entries.isEmpty()) {
            auto* none = new QTreeWidgetItem(m_outline, {"(No bookmarks)"});
            none->setDisabled(true);
            return;
        }

        // Rebuild tree from flat list with depth info using a parent stack.
        QVector<QTreeWidgetItem*> stack;
        int topIdx = 0;
        for (const auto& e : entries) {
            QTreeWidgetItem* item;
            if (e.depth == 0 || stack.isEmpty()) {
                item = new QTreeWidgetItem(m_outline, {e.title});
                item->setData(0, Qt::UserRole + 1, topIdx++);
                item->setFlags(item->flags() | Qt::ItemIsDropEnabled);
                stack = {item};
            } else {
                while (stack.size() > e.depth) stack.pop_back();
                item = new QTreeWidgetItem(stack.last(), {e.title});
                if ((int)stack.size() <= e.depth) stack.append(item);
                else stack[e.depth] = item;
            }
            item->setData(0, Qt::UserRole, e.page);
        }
        m_outline->expandAll();

        // Sync to current page after bookmarks are loaded
        if (m_currentPage >= 0) syncBookmarkToPage(m_currentPage);
    });
    watcher->setFuture(future);
}

// ── syncBookmarkToPage ────────────────────────────────────────────────────────
void ThumbnailPanel::syncBookmarkToPage(int pageIndex) {
    // When the Bookmarks tab is active, user may be making a multi-selection — don't interfere.
    if (m_stack->currentIndex() == 1) return;
    if (pageIndex < 0 || m_outline->topLevelItemCount() == 0) return;

    QTreeWidgetItem* best     = nullptr;
    int              bestPage = -1;

    std::function<void(QTreeWidgetItem*)> search = [&](QTreeWidgetItem* item) {
        int pg = item->data(0, Qt::UserRole).toInt();
        if (pg >= 0 && pg <= pageIndex && pg > bestPage) {
            bestPage = pg;
            best     = item;
        }
        for (int i = 0; i < item->childCount(); ++i)
            search(item->child(i));
    };
    for (int i = 0; i < m_outline->topLevelItemCount(); ++i)
        search(m_outline->topLevelItem(i));

    if (best) {
        QSignalBlocker b(m_outline);
        m_outline->clearSelection();
        best->setSelected(true);
        m_outline->scrollToItem(best, QAbstractItemView::EnsureVisible);
    }
}

// ── buildContentTree ──────────────────────────────────────────────────────────
void ThumbnailPanel::buildContentTree() {
    m_contentTree->clear();
    if (!m_doc || !m_doc->isOpen()) return;

    FPDF_DOCUMENT doc  = m_doc->raw();
    int           n    = m_doc->pageCount();
    int           myGen = m_contentGen.fetchAndAddOrdered(1) + 1;

    auto* watcher = new QFutureWatcher<QList<QPair<int,QString>>>(this);

    // 🔴 0928 LƯỢT 22: token lúc SPAWN + RAII move (xem bookmark — cùng khuôn).
    // 0928 LƯỢT 14: token sổ việc nền — xem buildBookmarks. Task này còn tự
    // FPDF_LoadPage + FPDFText_LoadPage + tự đóng, tức nắm handle ngoài PageCache.
    trdoc::Task task(doc, "thumbPanel/noiDung");
    auto future = QtConcurrent::run([task = std::move(task), doc, n, myGen, this]() {
        QList<QPair<int,QString>> results;
        for (int i = 0; i < n; ++i) {
            if (m_contentGen.loadAcquire() != myGen || task.cancelled()) break;
            QString preview;
            {
                TimedPdfiumLock lock(__FILE__, __LINE__);
                if (m_contentGen.loadAcquire() != myGen) break;
                FPDF_PAGE page = FPDF_LoadPage(doc, i);
                if (page) {
                    FPDF_TEXTPAGE tp = FPDFText_LoadPage(page);
                    if (tp) {
                        int cnt = qMin(FPDFText_CountChars(tp), 120);
                        std::vector<unsigned short> buf(static_cast<size_t>(cnt) + 1, 0);
                        FPDFText_GetText(tp, 0, cnt, buf.data());
                        preview = cleanPdfText(
                            QString::fromUtf16(
                                reinterpret_cast<const char16_t*>(buf.data()))
                        ).left(100);
                        FPDFText_ClosePage(tp);
                    }
                    FPDF_ClosePage(page);
                }
            }
            if (preview.isEmpty()) preview = "(font encoding not supported)";
            results.append({i, preview});
        }
        return results;
    });

    connect(watcher, &QFutureWatcher<QList<QPair<int,QString>>>::finished,
            this, [this, watcher, myGen] {
        watcher->deleteLater();
        if (m_contentGen.loadAcquire() != myGen) return;
        auto results = watcher->result();
        m_contentTree->clear();
        for (const auto& [idx, text] : results) {
            auto* item = new QTreeWidgetItem(m_contentTree,
                {QString("p.%1  %2").arg(idx + 1).arg(text)});
            item->setData(0, Qt::UserRole, idx);
            item->setFlags(item->flags() | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
        }
    });
    watcher->setFuture(future);
}

// ── buildProperties (async — avoids blocking main thread on s_pdfiumMutex) ────
void ThumbnailPanel::buildProperties() {
    m_propertiesTree->clear();
    if (!m_doc || !m_doc->isOpen()) return;

    FPDF_DOCUMENT doc      = m_doc->raw();
    QString       filePath = m_doc->filePath();
    int           pages    = m_doc->pageCount();
    int           myGen    = m_propsGen.fetchAndAddOrdered(1) + 1;

    struct Props {
        int     fileVersion = 0;
        QString title, author, subject, creator, producer, created, modified;
    };

    auto* watcher = new QFutureWatcher<Props>(this);

    // 🔴 0928 LƯỢT 22: token lúc SPAWN + RAII move (xem bookmark — cùng khuôn).
    // 0928 LƯỢT 14: token sổ việc nền — xem buildBookmarks.
    trdoc::Task task(doc, "thumbPanel/thuocTinh");
    auto future = QtConcurrent::run([task = std::move(task), doc, myGen, this]() -> Props {
        Props p;
        auto getMeta = [&](const char* tag) -> QString {
            TimedPdfiumLock lk(__FILE__, __LINE__);
            if (m_propsGen.loadAcquire() != myGen) return {};
            unsigned long len = FPDF_GetMetaText(doc, tag, nullptr, 0);
            if (!len) return {};
            std::vector<char> buf(len + 2, 0);
            FPDF_GetMetaText(doc, tag, buf.data(), len);
            return QString::fromUtf16(
                reinterpret_cast<const char16_t*>(buf.data())).trimmed();
        };
        { TimedPdfiumLock lk(__FILE__, __LINE__); FPDF_GetFileVersion(doc, &p.fileVersion); }
        p.title    = getMeta("Title");
        p.author   = getMeta("Author");
        p.subject  = getMeta("Subject");
        p.creator  = getMeta("Creator");
        p.producer = getMeta("Producer");
        p.created  = getMeta("CreationDate");
        p.modified = getMeta("ModDate");
        return p;
    });

    connect(watcher, &QFutureWatcher<Props>::finished, this,
            [this, watcher, myGen, filePath, pages]() {
        watcher->deleteLater();
        if (m_propsGen.loadAcquire() != myGen) return;
        Props p = watcher->result();

        auto add = [&](const QString& k, const QString& v) {
            if (v.isEmpty()) return;
            new QTreeWidgetItem(m_propertiesTree, {k, v});
        };
        auto fmtDate = [](const QString& d) -> QString {
            if (d.startsWith("D:") && d.length() >= 10)
                return d.mid(2, 4) + "-" + d.mid(6, 2) + "-" + d.mid(8, 2);
            return d;
        };

        add("Pages", QString::number(pages));
        if (p.fileVersion > 0)
            add("PDF Version", QString("PDF %1.%2")
                .arg(p.fileVersion / 10).arg(p.fileVersion % 10));
        QFileInfo fi(filePath);
        if (fi.exists()) {
            qint64 b = fi.size();
            add("File Size", b < 1024*1024
                ? QString("%1 KB").arg(b / 1024)
                : QString("%1 MB").arg(b / (1024*1024)));
            add("File Path", fi.absoluteFilePath());
        }
        add("Title",    p.title);
        add("Author",   p.author);
        add("Subject",  p.subject);
        add("Creator",  p.creator);
        add("Producer", p.producer);
        add("Created",  fmtDate(p.created));
        add("Modified", fmtDate(p.modified));
        m_propertiesTree->resizeColumnToContents(0);
    });
    watcher->setFuture(future);
}

// 0927 LUOT 4 (VIEC A) - dong dau + dau ... cho o chi-hien-thi.
static QString firstLineEllipsis(const QString& full);


// 🔴 0927 LUOT 5 (LOI 2) - ELIDE THEO BE RONG THAT cua o.
// QLineEdit KHONG tu elide: no CUON den con tro, nen mot dong dai se
// hien phan CUOI ("...Viet co dau") dung khi chu da vua duoc gan. O day ta
// cat bang chinh QFontMetrics cua o (cung font) theo be rong do duoc, nen
// thoa man "Dong mot tieng Viet co..." luon ra phan DAU.
//  - doc duoc het dong  -> hien nguyen dong + "\u2026"
//  - vuot be rong        -> ElideRight theo be rong that
// - chi tao text moi khi KHAC text cu (tranh vong lap resize).
static void applyRowElide(QLineEdit* le) {
    if (!le || !le->isReadOnly()) return;
    const QString first = le->property("trFirstLine").toString();
    if (first.isEmpty()) return;
    // tru 8 px: padding 2 ben + du phong cho con tro/diem danh dau.
    const int avail = qMax(16, le->width() - 8);
    const QFontMetrics fm(le->font());
    const QString shown = (fm.horizontalAdvance(first) <= avail)
        ? first + QString::fromUtf8("\xE2\x80\xA6")
        : fm.elidedText(first, Qt::ElideRight, avail);
    if (le->text() == shown) return;
    le->setText(shown);
    le->setCursorPosition(0);
}

// 0927 LUOT 5 (LOI 1) - ky tu Unicode "\u2922" (\xE2\xA4\xA2) tren Win10
// khong co glyph => nhan "LUC" thanh O VUONG TRONG (do CEO tren may test).
// Khong doi font, khong doi he thong: VEE ICON BANG QPainter (2 mui ten cheo).
// QIcon + stylesheet; nhan van giu dung property trExpand de probe tim thay.
//
// 🔴 VONG 2 (aider-review CHANGES_REQUESTED): mau KHONG hard-code #D4D4D4
// - xam sang do tren nen SANG thi icon tang hinh. Lay token fgDim cua theme
// (cung kieu chu ma app dung o dong 442), truyen `dark` vao hàm.
// VONG 2: ve THANG 12x12 (dung bang setIconSize) de khong phai downscale
// 16 -> 12 (do 16/12 = 1.33, lam mo canh).
static QIcon commentExpandIcon(bool dark) {
    QPixmap pm(12, 12);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(dark ? darkHC().fgDim : lightHC().fgDim),
                  1.2, Qt::SolidLine, Qt::RoundCap));
    // Mui ten: 1 goc len-phai + 1 goc xuong-trai (khu 12x12: 1..5 va 6..10).
    auto arrow = [&p](int x0, int y0, int dx, int dy) {
        p.drawLine(x0, y0, x0 + dx * 2, y0 + dy * 2);          // than
        p.drawLine(x0 + dx * 2, y0 + dy * 2, x0, y0 + dy * 3); // canh doc
        p.drawLine(x0 + dx * 2, y0 + dy * 2, x0 + dx * 3, y0); // canh ngang
    };
    arrow(1, 1, +1, +1);    // len phai
    arrow(10, 10, -1, -1);  // xuong trai
    p.end();
    return QIcon(pm);
}

void ThumbnailPanel::setComments(const QList<AnnotInfo>& comments) {
    if (!m_commentsList) return;
    qDebug().noquote() << "[comments] setComments n=" << comments.size();
    m_commentsList->clear();
    for (const AnnotInfo& a : comments) {
        auto* item = new QListWidgetItem(m_commentsList);
        item->setData(Qt::UserRole, a.pageIndex);
        item->setData(Qt::UserRole + 1, a.indexInPage);

        auto* row = new QWidget(m_commentsList);
        row->setAttribute(Qt::WA_TranslucentBackground);
        row->setStyleSheet("background: transparent;");
        auto* lay = new QHBoxLayout(row);
        lay->setContentsMargins(6, 1, 6, 1);
        lay->setSpacing(4);

        auto* lbl = new QLabel(QString("p.%1  %2  —").arg(a.pageIndex + 1).arg(a.type), row);
        lbl->setAttribute(Qt::WA_TransparentForMouseEvents);
        lbl->setStyleSheet("background: transparent;");
        lay->addWidget(lbl, 0);

        // 🔴 0927 LƯỢT 4 (VIỆC A) — phân 2 chế độ dựa trên NỘI DUNG, không theo
        // nguồn annot. Quy tắc: có '\n' HOẶC dài hơn bề rộng ô ⇒ CHỈ-HIỂN-THỊ
        // (hiện dòng đầu + '…', không cho gõ tại chỗ) vì sửa trong QLineEdit
        // 1 dòng rồi editingFinished sẽ LƯU LẠI chuỗi đã mất xuống dòng.
        // Ngắn + vừa ô ⇒ vẫn sửa tại chỗ như cũ (đường cũ, không đổi).
        const QString full = a.text;
        // Đo bề rộng bằng CHÍNH font của ô (đã trừ 0.5pt) — dùng biến tạm
        // trước khi tạo ô, tránh phụ thuộc thứ tự khai báo.
        QFont efRow = lbl->font();
        efRow.setPointSizeF(efRow.pointSizeF() - 0.5);
        const QFontMetrics fmRow(efRow);
        const bool hasNewline = full.contains(QLatin1Char('\n'));
        const bool tooWide = !hasNewline && (fmRow.horizontalAdvance(full) > 120);
        const bool displayOnly = hasNewline || tooWide;
        // Dung LAI cho ca o va property (truoc do tach 2 lan o hai cho).
        const QString firstLine = displayOnly ? firstLineEllipsis(full) : full;

        auto* edit = new QLineEdit(firstLine, row);
        edit->setFixedHeight(20);
        edit->setFont(efRow);
        edit->setProperty("trPage", a.pageIndex);
        edit->setProperty("trIdx", a.indexInPage);
        edit->setProperty("trOrig", full);
        // 0927 LUOT 5 (LOI 2): dong DAU de applyRowElide() cat theo be rong that.
        // firstLineEllipsis() da tach san phan truoc '\n' — dung lai no, tranh
        // viet lai logic tach 2 lan o hai cho (dich sanh lech nhau de bi so).
        // applyRowElide() tu them lai dau \u2026, nen tien o day bo no ra.
        edit->setProperty("trFirstLine", displayOnly
            ? firstLine.left(firstLine.length() - 1) : firstLine);
        edit->setProperty("trOwn", a.isOwn);   // 0927 L4: popup co sua duoc hay khong
        // 🔴 Ô CHỈ-HIỂN-THỊ: readOnly = KHÔNG sửa tại chỗ + không báo placeholder
        // gợi ý sai (placeholder chỉ có nghĩa khi ô rỗng và sửa được).
        edit->setReadOnly(displayOnly);
        // 🔴 0927 LUOT 5 (LOI 2): o CHI-HIEN-THI luc dau phai hien TU KY TU 0
        // (chu "Dong mot tieng Viet co dau" => "Dong mot tieng Viet...").
        // Nguyen nhan: QLineEdit KHONG tu elide — no CUON ban do chu de con tro
        // hien duoc. Sau setText() con tro mac dinh o CUOI, va QLineEdit sap xep
        // lai khi duoc gan vao danh sach co the day nhin sang phan CUOI
        // ("Viet co dau..." do CEO thay tren may test).
        // Fix: dat con tro ve 0 truoc, ep layout, roi setCursorPosition(0) LAI
        // mot lan nua (sau khi widget da co kich thoc that trong danh sach).
        if (displayOnly) {
            edit->setCursorPosition(0);
            edit->home(false);
            edit->setCursorPosition(0);
        }
        edit->installEventFilter(this);
        // Đề 7: tooltip của hàng hiện TOÀN VĂN (nhiều dòng).
        edit->setToolTip(full);
        if (!a.isOwn) {
            // 🔴 P0 (0921): chú thích của phần mềm khác — sửa /Contents qua ô này sẽ
            // làm lệch với /AP gốc (hai bản sự thật). Chỉ-đọc + nói rõ lý do.
            const QString why = QStringLiteral(
                "Chú thích của phần mềm khác — sửa chữ sẽ làm lệch với bản vẽ gốc");
            edit->setReadOnly(true);
            edit->setPlaceholderText(why);
            edit->setToolTip(full + QLatin1Char('\n') + why);
        } else if (!displayOnly) {
            edit->setPlaceholderText(QStringLiteral("Add a comment…"));
            edit->setClearButtonEnabled(true);
        }
        lay->addWidget(edit, 1);

        const int pg = a.pageIndex, ix = a.indexInPage;
        const bool canEdit = a.isOwn;

        // 🔴 Đề 3: nút "⤢" cuối mỗi hàng — CHỈ với chú thích CỦA TA. Chú thích
        // của phần mềm khác KHÔNG có nút sửa (đề 6); nó mở popup CHỈ ĐỌC để đọc
        // đủ chữ, nút OK tắt.
        QPushButton* expand = new QPushButton(row);
        // 0927 LUOT 5 (LOI 1): ICON VE BANG QPAINTER, khong dung ky tu Unicode
        // "\u2922" (Win10 thieu glyph => o vuong trong). Giu ten ky tu cu trong
        // property de probe -sidebar-edit-probe cua LƯỢT 4 van khop nut.
        expand->setText(QString::fromUtf8("\xE2\xA4\xA2"));
        expand->setIcon(commentExpandIcon(m_dark));
        expand->setIconSize(QSize(12, 12));
        expand->setFixedSize(18, 18);
        expand->setFocusPolicy(Qt::NoFocus);
        expand->setToolTip(canEdit
            ? QStringLiteral("Mở ô sửa lớn (nhiều dòng)")
            : QStringLiteral("Xem đủ chữ (chỉ đọc — chú thích của phần mềm khác)"));
        expand->setProperty("trPage", a.pageIndex);
        expand->setProperty("trIdx", a.indexInPage);
        expand->setProperty("trExpand", true);
        // Vong 2: vien cua nut cung lay token (hard-code #666 cung bi mo tren
        // nen sang) — cung kieu voi cac QSS khac cua panel.
        expand->setStyleSheet(
            QStringLiteral(
                "QPushButton { background:transparent; border:1px solid %1; border-radius:3px; }"
                "QPushButton:hover { border:1px solid %2; }")
                .arg(m_dark ? darkHC().border : lightHC().border,
                     m_dark ? darkHC().accent : lightHC().accent));
        lay->addWidget(expand, 0);
        connect(expand, &QPushButton::clicked, this, [this, row, pg, ix, canEdit] {
            openCommentPopup(row, pg, ix, canEdit);
        });

        if (canEdit && !displayOnly) {
            // Đường SỬA TẠI CHỖ cũ — chỉ mở khi ô KHÔNG nhiều dòng/nhẹp ⇒ không
            // bao giờ lưu chuỗi đã mất xuống dòng (nguy cơ mất dữ liệu đã bị chặn).
            connect(edit, &QLineEdit::editingFinished, this, [this, pg, ix] {
                auto* senderEdit = qobject_cast<QLineEdit*>(sender());
                if (!senderEdit) return;
                QString orig = senderEdit->property("trOrig").toString();
                QString txt = senderEdit->text();
                if (txt == orig) return;
                QTimer::singleShot(0, this, [this, pg, ix, txt] {
                    emit commentTextEdited(pg, ix, txt);
                });
            });
        } else {
            // Đề 2: bấm vào ô (hoặc Enter/F2 khi hàng đang chọn) ⇒ mở popup.
            // Ô readOnly nên không có editingFinished ⇒ không mất dữ liệu.
            // Bam vao o: eventFilter bat MouseButtonPress (ben tren).
        }

        m_commentsList->addItem(item);
        item->setSizeHint(QSize(0, 26));
        m_commentsList->setItemWidget(item, row);
        // 0927 LUOT 5 (LOI 2): cat theo be rong that LAN CUOI (sau khi setItemWidget
        // da do width cua row) + ve con tro ve ky tu 0 de chan thuc "cuon het".
        if (displayOnly) { applyRowElide(edit); edit->setCursorPosition(0); }
    }
    if (m_commentsList->count() == 0)
        m_commentsList->addItem(new QListWidgetItem(QStringLiteral("(no comments yet)")));
}

// 0927 LƯỢT 4 (VIỆC A) — dòng đầu + '…' cho ô chỉ-hiển-thị.
static QString firstLineEllipsis(const QString& full) {
    const int nl = full.indexOf(QLatin1Char('\n'));
    const QString first = (nl >= 0) ? full.left(nl) : full;
    return first + QString::fromUtf8("\xE2\x80\xA6");  // …
}

// 🔴 0927 LƯỢT 4 (VIỆC A đề 4) — POPUP = DÙNG LẠI NoteInputDialog ở chế độ
// nhiều dòng (QPlainTextEdit, Enter xuống dòng, Ctrl+Enter OK, Esc huỷ).
// KHÔNG viết hộp thứ hai. parent = chính panel (không phải hàng) để popup không
// bị xoá khi list dựng lại; neo NGAY DƯỚI hàng, neo trong màn hình.
void ThumbnailPanel::openCommentPopup(QWidget* row, int page, int indexInPage, bool editable) {
    if (!m_commentsList || !row) return;
    if (m_cmtPopup) { m_cmtPopup->close(); m_cmtPopup = nullptr; m_cmtPopupEdit = nullptr; }

    // Lấy NỘI DUNG GỐC (không phải chuỗi đã cắt '…' của ô chỉ-hiển-thị).
    QString full;
    if (auto* le = row->findChild<QLineEdit*>())
        full = le->property("trOrig").toString();

    // 🔴 0927 LUOT 6 / VIEC 2: popup sidebar lay CUNG theme dang chay
    // cua app (m_dark do setDarkMode() gan) — giong het hop nhap cua MainWindow,
    // khong con ep nen TOI. Khong mo cua so thu hai.
    auto* dlg = new NoteInputDialog(full, m_commentsPanel, /*singleLine=*/false, m_dark);
    m_cmtPopup = dlg;
    dlg->setWindowTitle(editable ? QStringLiteral("Edit comment") : QStringLiteral("Comment"));
    dlg->setMinimumWidth(360);
    // 🔴 0927 LUOT 5 (LOI 3): giu page/index de probe lay duoc toa do hang.
    dlg->setProperty("trPopupPage", page);
    dlg->setProperty("trPopupIdx", indexInPage);

    m_cmtPopupEdit = dlg->findChild<QPlainTextEdit*>();
    if (m_cmtPopupEdit) {
        // 🔴 Đề 6: popup CHỈ ĐỌC cho chú thích của phần mềm khác ⇒ tắt nút OK
        // (không cho ghi đè /Contents của họ). Esc/Cancel vẫn đóng được.
        m_cmtPopupEdit->setReadOnly(!editable);
        if (auto* bb = dlg->findChild<QDialogButtonBox*>()) {
            if (auto* ok = bb->button(QDialogButtonBox::Ok)) {
                ok->setEnabled(editable);
                if (!editable) ok->setToolTip(QStringLiteral(
                    "Chú thích của phần mềm khác — không sửa được"));
            }
        }
        // Đề 4: cao 4–14 dòng theo nội dung, quá thì cuộn (QPlainTextEdit tự
        // cuộn khi vượt số dòng hiển thị).
        const int lines = qBound(1, m_cmtPopupEdit->document()->blockCount(), 14);
        const int lh = qMax(1, m_cmtPopupEdit->fontMetrics().lineSpacing());
        m_cmtPopupEdit->setFixedHeight(qBound(4, lines, 14) * lh + 12);
        m_cmtPopupEdit->setFocus();
    }

    // Neo NGAY DƯỚI hàng; nằm ngoài màn hình ⇒ đẩy vào trong.
    const QPoint anchor = row->mapToGlobal(QPoint(0, row->height()));
    if (QScreen* scr = QApplication::screenAt(anchor)) {
        const QRect avail = scr->availableGeometry();
        QSize sz = dlg->sizeHint();
        sz.setWidth(qMax(360, sz.width()));
        int x = anchor.x();
        int y = anchor.y();
        if (x + sz.width() > avail.right())  x = avail.right() - sz.width();
        if (y + sz.height() > avail.bottom()) y = avail.top();
        if (x < avail.left()) x = avail.left();
        if (y < avail.top()) y = avail.top();
        dlg->move(x, y);
    }

    // Đề 5: OK ⇒ emit commentTextEdited với chuỗi GIỮ NGUYÊN '\n' ⇒ đi đúng
    // đường sửa hiện có (/Contents '\r' + /AP nhiều dòng). Huỷ ⇒ không đổi gì.
    if (editable) {
        connect(dlg, &QDialog::accepted, this, [this, page, indexInPage] {
            if (m_cmtPopupEdit)
                emit commentTextEdited(page, indexInPage, m_cmtPopupEdit->toPlainText());
        });
    }
    connect(dlg, &QDialog::finished, this, [this] {
        // Giải phóng con trỏ TRƯỚC khi dialog tự xoá.
        QDialog* d = m_cmtPopup;
        m_cmtPopup = nullptr;
        m_cmtPopupEdit = nullptr;
        if (d) d->deleteLater();
    });

    dlg->show();                       // không exec(): sidebar phải tương tác được
    dlg->raise();
    dlg->activateWindow();
}

void ThumbnailPanel::closeCommentPopup() {
    if (m_cmtPopup) { m_cmtPopup->close(); m_cmtPopup = nullptr; }
    m_cmtPopupEdit = nullptr;
}

void ThumbnailPanel::setCommentsLoading(bool loading) {
    if (!m_commentsList) return;
    m_commentsList->clear();
    if (loading) {
        auto* item = new QListWidgetItem(QStringLiteral("Loading comments\u2026"), m_commentsList);
        item->setFlags(Qt::NoItemFlags);
    }
}

void ThumbnailPanel::setCommentsProgress(int scanned, int total) {
    if (!m_commentsList) return;
    if (m_commentsList->count() == 1) {
        auto* item = m_commentsList->item(0);
        if (item && item->flags() == Qt::NoItemFlags)
            item->setText(QStringLiteral("Scanning comments\u2026 %1/%2").arg(scanned).arg(total));
    }
}

void ThumbnailPanel::setAnnotMgr(AnnotationManager* mgr, int pageCount) {
    m_annotMgr   = mgr;
    m_annotPages = pageCount;
}

void ThumbnailPanel::selectCommentFor(int pageIndex, int annotIndex) {
    if (!m_commentsList) return;
    QSignalBlocker blocker(m_commentsList);
    for (int i = 0; i < m_commentsList->count(); ++i) {
        auto* item = m_commentsList->item(i);
        if (item && item->data(Qt::UserRole).toInt() == pageIndex &&
            item->data(Qt::UserRole + 1).toInt() == annotIndex) {
            selectTab(2);
            m_commentsList->setCurrentItem(item);
            m_commentsList->scrollToItem(item);
            qDebug().noquote() << "[comments] sync PDF→list page=" << pageIndex << "idx=" << annotIndex << "found=1";
            return;
        }
    }
    m_commentsList->clearSelection();
    qDebug().noquote() << "[comments] sync PDF→list page=" << pageIndex << "idx=" << annotIndex << "found=0";
}

// 🔍 0927 LƯỢT 4 (VIỆC C) — probe đọc trạng thái ô sửa chú thích.
// CHỈ đọc; KHÔNG tự khai "đạt" thay người chấm.
int ThumbnailPanel::probeCommentRowCount() const {
    return m_commentsList ? m_commentsList->count() : -1;
}

ThumbnailPanel::CommentRowProbe ThumbnailPanel::probeCommentRow(int page, int indexInPage,
                                                                int row) const {
    CommentRowProbe r;
    if (!m_commentsList) return r;
    if (row < 0) {
        row = 0;
        for (int i = 0; i < m_commentsList->count(); ++i) {
            auto* it = m_commentsList->item(i);
            if (it && it->data(Qt::UserRole).toInt() == page
                && it->data(Qt::UserRole + 1).toInt() == indexInPage) { row = i; break; }
        }
    }
    auto* it = m_commentsList->item(row);
    if (!it) return r;
    QWidget* w = m_commentsList->itemWidget(it);
    if (!w) return r;
    r.exists = true;
    r.page = it->data(Qt::UserRole).toInt();
    r.indexInPage = it->data(Qt::UserRole + 1).toInt();
    if (auto* le = w->findChild<QLineEdit*>()) {
        r.hasEdit = true;
        r.text = le->text();
        r.editableInPlace = !le->isReadOnly();
        r.displayOnly = le->isReadOnly();
        r.tooltipHasAllText = (le->toolTip() == le->property("trOrig").toString());
        r.isOwn = le->property("trOwn").toBool();
    }
    for (auto* b : w->findChildren<QPushButton*>())
        if (b->property("trExpand").toBool()) { r.hasExpandBtn = true; break; }
    if (m_cmtPopupEdit) {
        r.popupLines = m_cmtPopupEdit->document()->blockCount();
        if (auto* bb = m_cmtPopup->findChild<QDialogButtonBox*>())
            if (auto* ok = bb->button(QDialogButtonBox::Ok))
                r.popupOkEnabled = ok->isEnabled();
    }
    return r;
}

bool ThumbnailPanel::probeOpenCommentPopup(int page, int indexInPage, int row,
                                           const QString& byButton) {
    if (!m_commentsList) return false;
    if (row < 0) {
        row = 0;
        for (int i = 0; i < m_commentsList->count(); ++i) {
            auto* it = m_commentsList->item(i);
            if (it && it->data(Qt::UserRole).toInt() == page
                && it->data(Qt::UserRole + 1).toInt() == indexInPage) { row = i; break; }
        }
    }
    auto* it = m_commentsList->item(row);
    if (!it) return false;
    QWidget* w = m_commentsList->itemWidget(it);
    if (!w) return false;

    // ĐÚNG ĐƯỜNG: click giả lập = QPushButton::click() (phát tín hiệu clicked y
    // như người dùng bấm chuột) hoặc bắn QKeyEvent y như bàn phím.
    if (!byButton.isEmpty()) {
        for (auto* b : w->findChildren<QPushButton*>()) {
            if (!b->property("trExpand").toBool()) continue;
            if (b->text() == byButton) { b->click(); return m_cmtPopup != nullptr; }
        }
        return false;
    }
    auto* le = w->findChild<QLineEdit*>();
    if (!le) return false;
    const int key = (byButton == QLatin1String("F2")) ? Qt::Key_F2 : Qt::Key_Return;
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(le, &press);
    QKeyEvent rel(QEvent::KeyRelease, key, Qt::NoModifier);
    QApplication::sendEvent(le, &rel);
    return m_cmtPopup != nullptr;
}

// 🔴 0927 LUOT 5 (LOI 3) - lay toa do popup + hang de harness in ra.
// rowGlobal: tim hang tuong ung trong danh sach theo page/indexInPage ma
// popup dang mo; dung hien trang thi -1 de -1.
bool ThumbnailPanel::probePopupGeometry(QRect* popupGlobal, QRect* rowGlobal) const {
    if (popupGlobal) *popupGlobal = QRect();
    if (rowGlobal)   *rowGlobal   = QRect();
    if (!m_cmtPopup) return false;
    if (popupGlobal) *popupGlobal = m_cmtPopup->geometry();
    if (rowGlobal && m_commentsList) {
        const QVariant vp = m_cmtPopup->property("trPopupPage");
        const QVariant vi = m_cmtPopup->property("trPopupIdx");
        if (vp.isValid() && vi.isValid()) {
            for (int i = 0; i < m_commentsList->count(); ++i) {
                auto* it = m_commentsList->item(i);
                if (!it || it->data(Qt::UserRole).toInt() != vp.toInt()) continue;
                if (it->data(Qt::UserRole + 1).toInt() != vi.toInt()) continue;
                if (QWidget* rw = m_commentsList->itemWidget(it)) {
                    *rowGlobal = QRect(rw->mapToGlobal(QPoint(0, 0)), rw->size());
                    break;
                }
            }
        }
    }
    return true;
}

void ThumbnailPanel::closeCommentPopupIfOpen() { closeCommentPopup(); }

bool ThumbnailPanel::probeSetPopupTextAndAccept(const QString& text) {
    if (!m_cmtPopup || !m_cmtPopupEdit) return false;
    m_cmtPopupEdit->setPlainText(text);
    if (!m_cmtPopupEdit->isReadOnly())
        m_cmtPopup->accept();          // đúng đường OK của hộp
    else
        m_cmtPopup->reject();
    return true;
}

bool ThumbnailPanel::eventFilter(QObject* o, QEvent* e) {
    auto* le = qobject_cast<QLineEdit*>(o);
    if (le) {
        // 0927 LUOT 5 (LOI 2): be rong o chay theo sidebar -> cat lai cho vua.
        if (e->type() == QEvent::Resize) applyRowElide(le);
        const int pg = le->property("trPage").toInt();
        const int ix = le->property("trIdx").toInt();
        if (e->type() == QEvent::FocusIn) {
            if (m_commentsList) {
                for (int i = 0; i < m_commentsList->count(); ++i) {
                    auto* it = m_commentsList->item(i);
                    if (it && it->data(Qt::UserRole).toInt() == pg &&
                        it->data(Qt::UserRole + 1).toInt() == ix) {
                        if (m_commentsList->currentItem() != it)
                            m_commentsList->setCurrentItem(it);
                        break;
                    }
                }
            }
        }
        // 🔴 0927 LUOT 4 (VIEC A de 2): o CHI-HIEN-THI (readOnly) —
        // bam vao hoac Enter/F2 = mo popup. Đuong cua nguoi dung: phai
        // chuyen sang chinh no bang chuot hay phim, khong phai ngoi rat ba lenh.
        // o sua TAI CHO (readOnly=false) thi bo qua — gõ tay theo duong cu.
        if (le->isReadOnly()) {
            if (e->type() == QEvent::MouseButtonPress) {
                if (auto* w = le->parentWidget())
                    openCommentPopup(w, pg, ix, /*editable=*/le->property("trOwn").toBool());
                return true;
            }
            if (e->type() == QEvent::KeyPress) {
                auto* ke = static_cast<QKeyEvent*>(e);
                if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter ||
                    ke->key() == Qt::Key_F2) {
                    if (auto* w = le->parentWidget())
                        openCommentPopup(w, pg, ix, /*editable=*/le->property("trOwn").toBool());
                    return true;
                }
            }
        }
    }
    return QWidget::eventFilter(o, e);
}

#include "ThumbnailPanel.moc"
