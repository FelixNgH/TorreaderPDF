#pragma once
#include <QObject>
#include <QEvent>
#include <QKeyEvent>
#include <QInputMethodEvent>
#include <QFile>
#include <QTextStream>
#include <QDir>
#include <QLineEdit>

// Chan log phim/IME tam cho QLineEdit khi TORREADER_KEYLOG=1 — khong anh huong
// ban phat hanh (mac dinh tat). Ghi vao %TEMP%\torreader_keylog.txt (app GUI,
// console vo dung). Dung de bat loi go tieng Viet: UniKey gui qua phim hay qua
// input method, app nhan duoc gi thi ghi day.
class KeylogProbe : public QObject {
public:
    explicit KeylogProbe(QObject* parent = nullptr) : QObject(parent) {
        m_f.setFileName(QDir::tempPath() + QLatin1String("/torreader_keylog.txt"));
        m_f.open(QIODevice::Append | QIODevice::Text);
        QTextStream ts(&m_f);
        ts << "=== keylog begin ===\n";
        ts.flush();
    }
    ~KeylogProbe() override { if (m_f.isOpen()) m_f.close(); }

    bool eventFilter(QObject* watched, QEvent* ev) override {
        Q_UNUSED(watched);
        if (!m_f.isOpen()) return false;
        QTextStream ts(&m_f);
        switch (ev->type()) {
        case QEvent::KeyPress: {
            auto* ke = static_cast<QKeyEvent*>(ev);
            QString ucs;
            const QString t = ke->text();
            for (const QChar& c : t) ucs += QString::number(c.unicode()) + QLatin1Char(',');
            ts << "[KeyPress] key=" << ke->key()
               << " text=\"" << t << "\" unicode=[" << ucs << "]"
               << " mods=" << int(ke->modifiers())
               << " nativeScan=" << ke->nativeScanCode()
               << " nativeVirt=" << ke->nativeVirtualKey()
               << " autoRepeat=" << ke->isAutoRepeat() << "\n";
            break;
        }
        case QEvent::InputMethod: {
            auto* im = static_cast<QInputMethodEvent*>(ev);
            ts << "[InputMethod] commit=\"" << im->commitString()
               << "\" preedit=\"" << im->preeditString()
               << "\" replStart=" << im->replacementStart()
               << " replLen=" << im->replacementLength() << "\n";
            break;
        }
        case QEvent::InputMethodQuery: {
            auto* imq = static_cast<QInputMethodQueryEvent*>(ev);
            ts << "[InputMethodQuery] queries=0x" << Qt::hex << imq->queries() << "\n";
            break;
        }
        default:
            break;
        }
        ts.flush();
        return false;   // khong nuot su kien
    }

private:
    QFile m_f;
};

// Cai probe len mot QLineEdit. Khong lam gi ca khi thieu TORREADER_KEYLOG.
inline void installKeylogProbe(QLineEdit* edit) {
    if (!edit || qEnvironmentVariableIsEmpty("TORREADER_KEYLOG")) return;
    auto* probe = new KeylogProbe(edit);
    edit->installEventFilter(probe);
}
