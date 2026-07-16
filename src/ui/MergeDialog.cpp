#include "MergeDialog.h"
#include <QListWidget>
#include <QListWidgetItem>
#include <QProgressBar>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QCloseEvent>
#include <QtConcurrent>
#include <QCollator>

MergeDialog::MergeDialog(PdfEditor* editor, QWidget* parent)
    : QDialog(parent), m_editor(editor) {
    setWindowTitle("Merge PDFs — TorReader");
    resize(520, 380);

    auto* hint = new QLabel("Drag rows to reorder  •  Add files then click Merge", this);
    hint->setStyleSheet("color: gray; font-size: 11px; padding: 4px;");

    m_fileList = new QListWidget(this);
    m_fileList->setDragDropMode(QAbstractItemView::InternalMove);
    m_fileList->setDefaultDropAction(Qt::MoveAction);
    m_fileList->setSpacing(1);

    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setVisible(false);

    auto* btnLayout = new QVBoxLayout;
    auto* addBtn    = new QPushButton("Add Files…", this);
    auto* removeBtn = new QPushButton("Remove", this);
    auto* upBtn     = new QPushButton("▲ Up", this);
    auto* downBtn   = new QPushButton("▼ Down", this);
    btnLayout->addWidget(addBtn);
    btnLayout->addWidget(removeBtn);
    btnLayout->addSpacing(8);
    btnLayout->addWidget(upBtn);
    btnLayout->addWidget(downBtn);
    btnLayout->addStretch();

    auto* midRow = new QHBoxLayout;
    midRow->addWidget(m_fileList, 1);
    midRow->addLayout(btnLayout);

    auto* mergeBtn = new QPushButton("Merge…", this);
    mergeBtn->setDefault(true);
    mergeBtn->setStyleSheet("font-weight: bold; padding: 6px 20px;");
    auto* bottomRow = new QHBoxLayout;
    bottomRow->addWidget(m_progressBar, 1);
    bottomRow->addWidget(mergeBtn);

    auto* main = new QVBoxLayout(this);
    main->addWidget(hint);
    main->addLayout(midRow, 1);
    main->addLayout(bottomRow);

    connect(addBtn,    &QPushButton::clicked, this, &MergeDialog::onAddFiles);
    connect(removeBtn, &QPushButton::clicked, this, &MergeDialog::onRemoveSelected);
    connect(upBtn,     &QPushButton::clicked, this, &MergeDialog::onMoveUp);
    connect(downBtn,   &QPushButton::clicked, this, &MergeDialog::onMoveDown);
    connect(mergeBtn,  &QPushButton::clicked, this, &MergeDialog::onMerge);
    connect(m_editor,  &PdfEditor::progress,  m_progressBar, &QProgressBar::setValue, Qt::QueuedConnection);
}

void MergeDialog::closeEvent(QCloseEvent* e) {
    if (m_merging) {
        e->ignore();
        return;
    }
    QDialog::closeEvent(e);
}

void MergeDialog::renumber() {
    for (int i = 0; i < m_fileList->count(); ++i) {
        auto* item = m_fileList->item(i);
        QString name = QFileInfo(item->data(Qt::UserRole).toString()).fileName();
        item->setText(QString("%1. %2").arg(i + 1).arg(name));
    }
}

void MergeDialog::onAddFiles() {
    QStringList files = QFileDialog::getOpenFileNames(
        this, "Select PDF Files", {}, "PDF Files (*.pdf)");
    QCollator coll;
    coll.setNumericMode(true);
    coll.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(files.begin(), files.end(), [&](const QString& a, const QString& b) {
        return coll.compare(a, b) < 0;
    });
    for (const QString& path : files) {
        auto* item = new QListWidgetItem(m_fileList);
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);
    }
    renumber();
}

void MergeDialog::onRemoveSelected() {
    delete m_fileList->currentItem();
    renumber();
}

void MergeDialog::onMoveUp() {
    int row = m_fileList->currentRow();
    if (row > 0) {
        auto* item = m_fileList->takeItem(row);
        m_fileList->insertItem(row - 1, item);
        m_fileList->setCurrentItem(item);
    }
    renumber();
}

void MergeDialog::onMoveDown() {
    int row = m_fileList->currentRow();
    if (row < m_fileList->count() - 1) {
        auto* item = m_fileList->takeItem(row);
        m_fileList->insertItem(row + 1, item);
        m_fileList->setCurrentItem(item);
    }
    renumber();
}

void MergeDialog::onMerge() {
    if (m_fileList->count() < 2) {
        QMessageBox::warning(this, "TorReader", "Add at least 2 PDF files to merge.");
        return;
    }
    QStringList paths;
    for (int i = 0; i < m_fileList->count(); ++i)
        paths << m_fileList->item(i)->data(Qt::UserRole).toString();

    QString out = QFileDialog::getSaveFileName(
        this, "Save Merged PDF", {}, "PDF Files (*.pdf)");
    if (out.isEmpty()) return;

    m_merging = true;
    setEnabled(false);
    m_progressBar->setValue(0);
    m_progressBar->setVisible(true);

    auto* watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher]() {
        bool ok = watcher->result();
        watcher->deleteLater();
        m_merging = false;
        setEnabled(true);
        m_progressBar->setValue(100);
        m_progressBar->setVisible(false);
        QMessageBox::information(this, "TorReader",
            ok ? "Merge complete!" : "Merge failed:\n" + m_editor->lastError());
        if (ok) accept();
    });
    watcher->setFuture(QtConcurrent::run([this, paths, out]() -> bool {
        return m_editor->merge(paths, out);
    }));
}
