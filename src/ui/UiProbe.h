#pragma once
#include <QString>
#include "ThemeTokens.h"

class QWidget;

// Bo do giao dien (SPEC_PROBE_LOG_SNAPSHOT 2026-08-16).
// Viec cua bo nay la DO, khong phai chua: chup cua so ra PNG va in mau
// DIEM ANH THAT cua tung widget (w->grab() roi doc pixel), khong doc khuon QSS.
// Bai hoc 08-14: nghiem thu tren dau ra, khong nghiem thu tren ban mau.
namespace UiProbe {

// Dump mot dong cho tung widget can theo doi + khoi token mau o cuoi.
// root: cua so chinh. tokens/dark: bang mau dang dung (de doi chieu voi pixel).
QString dumpWidgets(QWidget* root, const ThemeTokens& tokens, bool dark);

// Chup root ra pngPath va ghi dump (dumpWidgets) ra txtPath cung ten.
// dumpOut: nhan noi dung dump de goi ra log. errOut: ly do khi tra ve false.
bool snapshot(QWidget* root, const QString& pngPath, const QString& txtPath,
              const ThemeTokens& tokens, bool dark,
              QString* dumpOut, QString* errOut);

}  // namespace UiProbe
