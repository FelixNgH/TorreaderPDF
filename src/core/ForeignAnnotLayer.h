#pragma once
#include <QImage>
#include <QRect>
#include <fpdfview.h>

class ForeignAnnotLayer {
public:
    bool build(FPDF_DOCUMENT doc, int pageIndex, int maxPx);

    bool          isReady()   const { return m_ready; }
    int           pageIndex() const { return m_page; }
    const QImage& image()     const { return m_img; }

    // Vung sac net theo zoom that. Goi khi DANG giu khoa pdfium (giong build()).
    bool buildRegion(FPDF_DOCUMENT doc, int pageIndex, double scale, QRect regionPx);

    bool          regionReady() const { return m_regReady; }
    int           regionPage()  const { return m_regPage; }
    double        regionScale() const { return m_regScale; }
    QRect         regionRect()  const { return m_regRect; }
    const QImage& regionImage() const { return m_regImg; }

private:
    QImage m_img;
    int    m_page  = -1;
    bool   m_ready = false;

    QImage m_regImg;
    QRect  m_regRect;
    double m_regScale = 0.0;
    int    m_regPage  = -1;
    bool   m_regReady = false;
};
