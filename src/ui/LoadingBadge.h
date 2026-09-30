#pragma once
// LUOT 42 (30/09): chu Loading ve qua QImage CPU, khong qua glyph cache GL.
//
// Goc benh: QPainter::drawText() ve THANG len mot QOpenGLWidget di qua glyph
// cache GL cua Qt. Cache nay HONG sau khi GL resource cua cac tab bi huy/tao
// lai (dong/mo tab, doi kich thuoc GL context) => chu vo bien dang (owner bao
// 30/09, cung ho voi loi man chao LUOT 35 da sua bang cach doi han sang widget
// CPU rieng). O day ta khong the doi han GL widget (Loading phai hien NGAY
// TRONG trang PDF dang tai, khong the tach ra tab khac), nen thay bang: ve chu
// ra mot QImage bang rasterizer CPU (Format_ARGB32_Premultiplied, antialias),
// roi p.drawImage() anh do LEN GL widget. drawImage khong dung glyph cache GL
// nen khong bi hong theo vong doi GL resource.
//
// Dung o CA HAI: ContinuousView.cpp (paintEvent, viewport la QOpenGLWidget)
// va PdfGpuView.cpp (paintGL, chinh no la QOpenGLWidget).

#include <QCache>
#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QRectF>
#include <QString>

// Ve `text` ra QImage bang CPU rasterizer, KHONG qua glyph cache GL. Anh duoc
// CACHE (khoa = text|pointSize|bold|mau|dpr) nen khong ve lai moi khung hinh.
// `dpr` = devicePixelRatioF() cua widget dang ve, de chu net tren man hinh HiDPI.
inline QImage loadingBadgeImage(const QString& text, int pointSize, bool bold,
                                 const QColor& color, qreal dpr)
{
    if (dpr <= 0.0) dpr = 1.0;

    const QString key = text + QLatin1Char('|') + QString::number(pointSize)
                       + QLatin1Char('|') + (bold ? QLatin1Char('b') : QLatin1Char('n'))
                       + QLatin1Char('|') + color.name(QColor::HexArgb)
                       + QLatin1Char('|') + QString::number(dpr, 'f', 2);

    static QCache<QString, QImage> s_cache(64);
    if (const QImage* hit = s_cache.object(key)) return *hit;

    QFont f;
    f.setPointSize(pointSize);
    f.setBold(bold);

    // Kich thuoc logic (chua nhan dpr) tinh bang FontMetrics + le nho de
    // khong cat chu (mot vai font cao hon boundingRect uoc luong, va text
    // nhieu dong dung "\n" can wrap).
    QFontMetrics fm(f);
    const QRect textRect = fm.boundingRect(QRect(0, 0, 100000, 100000),
                                            Qt::AlignHCenter | Qt::TextWordWrap, text);
    const int marginPx = qMax(4, pointSize / 2);
    const QSize logicalSize(textRect.width() + marginPx * 2,
                             textRect.height() + marginPx * 2);

    QImage img(logicalSize * dpr, QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::transparent);

    {
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::TextAntialiasing, true);
        p.setFont(f);
        p.setPen(color);
        p.drawText(QRectF(QPointF(0, 0), logicalSize),
                   Qt::AlignCenter | Qt::TextWordWrap, text);
    }

    s_cache.insert(key, new QImage(img));
    return img;
}

// Ve `img` (da ve san boi loadingBadgeImage, co setDevicePixelRatio) can giua
// trong `targetRect` (toa do logic cua painter dang ve). Thay the truc tiep
// cho drawText(targetRect, Qt::AlignCenter, text) o cac cho da chuyen doi.
inline void drawLoadingBadge(QPainter& p, const QRectF& targetRect, const QImage& img)
{
    const qreal dpr = img.devicePixelRatio() > 0 ? img.devicePixelRatio() : 1.0;
    const QSizeF logicalSize(img.width() / dpr, img.height() / dpr);
    const QPointF topLeft(targetRect.center().x() - logicalSize.width() / 2.0,
                           targetRect.center().y() - logicalSize.height() / 2.0);
    p.drawImage(QRectF(topLeft, logicalSize), img);
}
