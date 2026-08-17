#pragma once
#include <QString>

// Mot bang mau, hai bo gia tri (VSCode High Contrast lam moc).
// Chi chinh mau o day — buildQss dung chung mot khuon cho ca 2 theme.
// selBg/hoverBg la mau BAN TRONG SUOT (alpha 0.10-0.28). Qt tron chung voi nen
// cua WIDGET CHA. => Moi widget dung selBg/hoverBg PHAI nam tren nen DAC va
// TRUNG TINH (bg/bgAlt). Dat chung tren mang mau dam thi ket qua ra mau dam,
// khong phai mau nhat nhu bang token goi y. Da dinh mot lan o sidebarTabGrid.
struct ThemeTokens {
    const char* bg;       // nen chinh
    const char* bgAlt;    // nen phu (toolbar, sidebar, statusbar)
    const char* deskBg;   // mau MAT BAN, phia sau to giay
    const char* fg;       // chu chinh
    const char* fgDim;    // chu mo
    const char* border;   // vien - QUAN TRONG NHAT trong phong cach HC
    const char* accent;   // mau nhan
    const char* focus;    // vien tieu diem
    // selBg: co y lech chuan VSCode High Contrast (dark=transparent, light=alpha 10%)
    // vi o app nay nguoi dung khong nhan ra vung dang chon — to nen du ro nhung van
    // giu vien 1px @focus@. selFg = mau chu binh thuong, khong dao mau.
    const char* selBg;    // nen muc dang chon
    const char* selFg;    // mau chu muc dang chon (bang fg, khong doi mau chu)
    const char* warnBg;   // nen ghi chu / canh bao nhe
    const char* hoverBg;  // nen khi re chuot - mau co alpha 10%
    const char* sliderBg;       // cuc nam thanh truot - mau thuong
    const char* sliderHover;    // cuc nam khi re chuot
    const char* sliderActive;   // cuc nam khi keo (pressed)
};

inline ThemeTokens darkHC() {
    return { "#000000", "#000000", "#000000", "#FFFFFF", "#D6D6DD", "#6FC3DF",
             "#1AEBFF", "#F38518", "rgba(255,255,255,0.18)", "#FFFFFF", "#000000",
             "rgba(255,255,255,0.10)",
             "rgba(111,195,223,0.60)", "rgba(111,195,223,0.80)", "rgba(111,195,223,1.0)" };
}

inline ThemeTokens lightHC() {
    return { "#FFFFFF", "#FFFFFF", "#505050", "#000000", "#292929", "#0F4A85",
             "#0F4A85", "#006BBD", "rgba(15,74,133,0.28)", "#000000", "#FFFFFF",
             "rgba(15,74,133,0.10)",
             "rgba(15,74,133,0.40)", "rgba(15,74,133,0.80)", "rgba(15,74,133,1.0)" };
}
