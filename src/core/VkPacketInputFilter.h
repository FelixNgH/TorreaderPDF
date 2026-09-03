#pragma once
// LOC GO TIENG VIET QUA VK_PACKET (SPEC 2026-09-03, ban v5) — CHI WINDOWS.
//
// V4 SAI O DAU (log may owner, TORREADER_KEYLOG=1): cho WM_KEYDOWN(VK_PACKET) di qua
//   => Qt dich keydown theo BO CUC BAN PHIM (scan 0x4000 -> 'a', keylog [KeyPress]
//   key=192 text="à") VA CHEN TRUOC; WM_CHAR Unicode dung ('ệ', wParam=0x1EC7) đến
//   sau bi chon choat. V2/V3 nguoc lai: nuot keydown truoc TranslateMessage cua bo
//   chia Qt => WM_CHAR khong bao gio sinh ra => ky tu bay hoi.
//
// V5 = MAT XICH CUOI, lam ca hai viec ma v2-v4 chi lam mot nua:
//   - WM_KEYDOWN/WM_SYSKEYDOWN VK_PACKET: TU GOI TranslateMessage(msg) — Windows
//     sinh WM_CHAR mang dung ky tu Unicode (MSDN: packet => 1-2 WM_CHAR, UTF-16
//     nam trong wScan) day vao hang doi cua thread; RUNG TRA VE TRUE (nuot) de Qt
//     khong thay keydown, khong dich ra 'a'. Bo chia Qt chay loc nay TRUOC
//     TranslateMessage cua no va `continue` khi nuot => khong the co dich lan hai.
//     TranslateMessage tra 0 => KHONG NUOT (return false): ro ri rang theo rang
//     buoc "khong chac thi dung nuot", lui ve hanh vi v4, ky tu khong bay hoi.
//   - WM_CHAR/WM_SYSCHAR: tra ve false nhu cu => Qt chen DUNG MOT ky tu (Qt >=5.11
//     tu xu ly WM_CHAR cua VK_PACKET, khong can keydown cho). KHONG CHEN DOI:
//     nguyen nhan chen 'a' cua v4 (keydown di qua) da bi nuot o nhanh tren.
//   - WM_KEYUP/WM_SYSKEYUP VK_PACKET: NUOT, cho can xung — keyup khong mang ky tu
//     nen khong co gi mat; press chua bao gio den Qt thi release dong gia se danh
//     lua widget theo trang thai bam (auto-repeat, editor vim...). Nuot la canh
//     an toan hon cho di qua.
//   - Phim that (wParam != VK_PACKET): return false NGAY, khong log, khong doi gi.
//
// BAO LUAT vong sau: neu log con cho thay ky tu mat hoac chen doi thi ban v6 den,
//   dung .bak-v4-0903 (van quan sat duoc) hoac .bak-v3-0903 lam moc lui.
//
// CHAN DOAN: TORREADER_KEYLOG=1 -> them dong "[VkPacket]" vao cung file
// %TEMP%\torreader_keylog.txt (KeylogProbe cung ghi vao day):
//   - MOI WM_CHAR/SYSCHAR: wParam hex + ky tu + scan hex + flag UNICODE khi
//     wParam > 0xFF -> nhin log biet ky tu Unicode co toi cua so hay khong.
//   - VK_PACKET keydown: ghi ro TRANSLATED+SWALLOW hay FAILED->PASS — bang chung
//     duong nao doan keydown da chay.
#ifdef Q_OS_WIN

#include <windows.h>

#include <QAbstractNativeEventFilter>
#include <QDir>
#include <QFile>
#include <QString>
#include <QTextStream>

class VkPacketInputFilter : public QAbstractNativeEventFilter {
public:
    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override
    {
        if (eventType != QLatin1String("windows_generic_MSG") || !message)
            return false;
        const MSG *msg = static_cast<const MSG *>(message);
        const UINT scan = UINT((msg->lParam >> 16) & 0xFFFF);   // 16 bit: wScan mang ky tu cua packet

        switch (msg->message) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (msg->wParam == VK_PACKET) {
                // Dich LAI toan bo qua dong cua Qt (bo chia chay loc TRUOC
                // TranslateMessage cua no): tu dich o day, roi nuot.
                const bool translated = TranslateMessage(msg) != FALSE;
                log("KEYDOWN wParam=E7 scan=0x" + QString::number(scan, 16)
                    + (translated ? " -> TRANSLATED (tu goi TranslateMessage) + SWALLOW"
                                  : " -> TranslateMessage FAIL -> PASS (khong chac thi dung nuot)"));
                if (translated && result)
                    *result = 0;
                return translated;
            }
            return false;                                   // phim that: nguyen ven
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (msg->wParam == VK_PACKET) {
                log("KEYUP wParam=E7 -> SWALLOW (keyup khong mang ky tu, can voi press da nuot)");
                if (result)
                    *result = 0;
                return true;
            }
            return false;
        case WM_CHAR:
        case WM_SYSCHAR: {
            const UINT wc = UINT(msg->wParam);
            log(QString(msg->message == WM_CHAR ? "CHAR" : "SYSCHAR")
                + " wParam=0x" + QString::number(wc, 16)
                + "='" + QString(QChar(ushort(wc))) + '\''
                + " scan=0x" + QString::number(scan, 16)
                + (wc > 0xFF ? " UNICODE" : "")
                + " -> PASS (Qt chen)");
            return false;
        }
        default:
            return false;
        }
    }

private:
    void log(const QString &line)
    {
        if (!m_logChecked) {
            m_logChecked = true;
            if (!qEnvironmentVariableIsEmpty("TORREADER_KEYLOG")) {
                m_log.setFileName(QDir::tempPath() + QLatin1String("/torreader_keylog.txt"));
                m_log.open(QIODevice::Append | QIODevice::Text);
            }
        }
        if (!m_log.isOpen())
            return;
        QTextStream ts(&m_log);
        ts << "[VkPacket] " << line << '\n';
        ts.flush();
    }

    QFile m_log;
    bool m_logChecked = false;
};

#endif // Q_OS_WIN
