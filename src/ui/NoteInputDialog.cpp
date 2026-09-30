#include "NoteInputDialog.h"
#include "ThemeTokens.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QLineEdit>
#include <QDialogButtonBox>
#include <QShortcut>
#include <QKeyEvent>
#include "KeylogProbe.h"

// 🔴 0927 LUOT 6 / VIEC 2 - HOP NHAP THEO MAU APP.
// Truoc day hinh nay EP NEN TOI (#3C3F41) bang setStyleSheet cua rieng no,
// nen app o che do SANG (mac dinh) bat len lai den hop TOI. Nay hinh lay DUNG
// bang mau chung cua MainWindow (ThemeTokens), dung chung voi buildQss().
//
// 🔴 VY SAO KHONG BO QUA QSS CUA APP: qApp->setStyleSheet(buildQss(t)) dat
// stylesheet o MUC qApp, nen moi widget (ke ca QDialog) da keo ve. QSS rieng
// cua hinh co do uu tien CAO HON nen ghi de dung mau nen cua app - dung la
// thu pham 21/09. Bay gio hinh chi them phan CHƯA CO trong buildQss
// (nhieu dong + gợi ý), moi mau lay tu cung bang token.
NoteInputDialog::NoteInputDialog(const QString& initialText, QWidget* parent, bool singleLine,
                                 bool darkTheme)
    : QDialog(parent), m_singleLine(singleLine) {
    const ThemeTokens& t = darkTheme ? darkHC() : lightHC();
    setWindowTitle("Add Note");
    setMinimumWidth(420);
    // Phan QSS rieng: chi bo sung thu app thieu. Moi mau deu lay tu `t` nen doi
    // theme la doi het - khong con mau nao hard-code.
    setStyleSheet(QString::fromLatin1(
        "QPlainTextEdit { background:%1; color:%2; border:1px solid %3; padding:4px; }"
        "QPlainTextEdit:focus { border:1px solid %4; }"
        "QLabel { background:transparent; color:%2; }"
        "QLabel#noteHint { color:%5; font-size:11px; }"
        "QDialogButtonBox QPushButton:hover { border:1px solid %4; }"
    ).arg(QLatin1String(t.bg), QLatin1String(t.fg), QLatin1String(t.border),
          QLatin1String(t.focus), QLatin1String(t.fgDim)));

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(8);
    layout->setContentsMargins(12, 12, 12, 12);

    layout->addWidget(new QLabel("Note:"));
    if (singleLine) {
        m_lineEdit = new QLineEdit;
        m_lineEdit->setText(initialText);
        installKeylogProbe(m_lineEdit);   // 0903: bat loi go tieng Viet (TORREADER_KEYLOG=1)
        m_lineEdit->setMinimumWidth(280);
        layout->addWidget(m_lineEdit);
        m_lineEdit->setFocus();
    } else {
        // 0927 BƯỚC 1: QPlainTextEdit (KHÔNG QTextEdit) — Enter ăn vào 1 dòng
        // mới, không phải thoát hộp. Ô cao ~5 dòng, co giãn theo nội dung.
        m_plainEdit = new QPlainTextEdit;
        m_plainEdit->setPlainText(initialText);
        m_plainEdit->setTabChangesFocus(true);
        installKeylogProbe(m_plainEdit);
        layout->addWidget(m_plainEdit, 1);
        m_plainEdit->setFocus();

        // Gợi ý nhỏ + nút OK tắt theo (Ctrl+Enter cũng OK).
        // 🔴 0927 LUOT 6: mau CHỮ lay tu t.fgDim (ma mo) cua theme dang
        // chay — chu nen phai DOC DUOC o ca hai theme. O theme sang fgDim =
        // #292929 tren nen #FFFFFF (tuong phan ~11:1); o theme toi fgDim =
        // #D6D6DD tren nen #000000 (~15:1).
        auto* hint = new QLabel("Enter = xuống dòng   •   Ctrl+Enter = OK");
        hint->setObjectName("noteHint");
        layout->addWidget(hint);
    }

    layout->addWidget(new QLabel("Author (optional):"));
    m_author = new QLineEdit;
    layout->addWidget(m_author);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    // 🔴 0927 BƯỚC 1: Ctrl+Enter = OK. Dùng QShortcut kiểu text, KHÔNG dựa
    // keyPressEvent của ô: ô này nằm trong QDialog, Enter tới QPlainTextEdit
    // sẽ chèn dòng nên không tới ô nhận. QShortcut bắt được ở mức cửa sổ,
    // chạy cả khi focus nằm trong ô hay nút.
    m_okShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), this);
    connect(m_okShortcut, &QShortcut::activated, this, &QDialog::accept);
}

QString NoteInputDialog::text()   const { return m_singleLine ? m_lineEdit->text() : m_plainEdit->toPlainText(); }
QString NoteInputDialog::author() const { return m_author->text(); }
