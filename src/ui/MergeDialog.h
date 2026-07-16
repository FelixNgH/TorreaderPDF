#pragma once
#include <QDialog>
#include "core/PdfEditor.h"

class QListWidget;
class QProgressBar;

class MergeDialog : public QDialog {
    Q_OBJECT
public:
    explicit MergeDialog(PdfEditor* editor, QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* e) override;

private slots:
    void onAddFiles();
    void onRemoveSelected();
    void onMoveUp();
    void onMoveDown();
    void onMerge();

private:
    void renumber();

    QListWidget*  m_fileList    = nullptr;
    QProgressBar* m_progressBar = nullptr;
    PdfEditor*    m_editor;
    bool          m_merging     = false;
};
