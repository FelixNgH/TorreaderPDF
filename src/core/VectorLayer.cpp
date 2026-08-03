#include "VectorLayer.h"
#include <fpdf_edit.h>
#include <fpdf_transformpage.h>
#include <fpdfview.h>
#include <QMutex>
#include <QHash>
#include <QElapsedTimer>
#include <QDebug>
#include <algorithm>
#include <cmath>

extern QMutex s_pdfiumMutex;

static std::atomic<quint64> s_uidCounter{1};

VectorLayer::VectorLayer() : m_uid(s_uidCounter.fetch_add(1, std::memory_order_relaxed)) {}

constexpr float kTextScale = 8.0f;
constexpr qint64 kMaxTextBytes = 200LL * 1024 * 1024;
constexpr qint64 kMaxImageBytes = 200LL * 1024 * 1024;

void VectorLayer::clear() {
    m_ready = false;
    m_complete = false;
    m_page = -1;
    m_pageSize = {};
    m_verts.clear();
    m_colors.clear();
    m_widths.clear();
    m_texts.clear();
    m_fillVerts.clear();
    m_fillColors.clear();
    m_depths.clear();
    m_fillDepths.clear();
    m_images.clear();
    m_clips.clear();
    m_clipIdx.clear();
    m_fillClipIdx.clear();
    m_noteObjIdx.clear();
    m_buildObjCount = 0;
}

static float polyArea2(const QVector<QPointF>& p) {
    double a = 0;
    for (int i = 0, n = p.size(); i < n; ++i) {
        const QPointF& u = p[i]; const QPointF& v = p[(i + 1) % n];
        a += u.x() * v.y() - v.x() * u.y();
    }
    return float(a);
}
static bool pointInTri(const QPointF& p, const QPointF& a, const QPointF& b, const QPointF& c) {
    auto cross = [](const QPointF& o, const QPointF& u, const QPointF& v) {
        return (u.x()-o.x())*(v.y()-o.y()) - (u.y()-o.y())*(v.x()-o.x());
    };
    const double eps = 1e-9;
    const double d1 = cross(a, b, p), d2 = cross(b, c, p), d3 = cross(c, a, p);
    const bool neg = (d1 < -eps) || (d2 < -eps) || (d3 < -eps);
    const bool pos = (d1 >  eps) || (d2 >  eps) || (d3 >  eps);
    return !(neg && pos);
}

bool VectorLayer::build(FPDF_DOCUMENT doc, int pageIndex) {
    clear();
    QElapsedTimer t;
    t.start();
    FPDF_PAGE page = FPDF_LoadPage(doc, pageIndex);
    if (!page) return false;

    double pageW = FPDF_GetPageWidth(page);
    double pageH = FPDF_GetPageHeight(page);
    m_pageSize = QSizeF(pageW, pageH);
    m_page = pageIndex;

    float cbL = 0.f, cbB = 0.f, cbR = 0.f, cbT = 0.f;
    bool haveBox = FPDFPage_GetCropBox(page, &cbL, &cbB, &cbR, &cbT);
    if (!haveBox) haveBox = FPDFPage_GetMediaBox(page, &cbL, &cbB, &cbR, &cbT);
    if (!haveBox || cbR <= cbL || cbT <= cbB) { cbL = 0.f; cbB = 0.f;
                                               cbR = float(pageW); cbT = float(pageH); }
    const double originX = double(cbL);
    const double topY    = double(cbT);
    qDebug().noquote() << "[vector] box page=" << pageIndex
                       << "crop=" << cbL << cbB << cbR << cbT
                       << "pageWH=" << pageW << pageH;

    uint8_t vr = 0, vg = 0, vb = 0, va = 255;
    float   curWidth = 0.0f;
    QVector<float> curDash;
    float   dashRun = 0.0f;
    float   curDepth = 1.0f;
    float   curClip = 0.0f;

    auto emitSeg = [&](float x0, float y0, float x1, float y1) {
        m_verts.append(x0); m_verts.append(y0);
        m_verts.append(x1); m_verts.append(y1);
        m_colors.append(vr); m_colors.append(vg); m_colors.append(vb); m_colors.append(va);
        m_widths.append(curWidth);
        m_depths.append(curDepth);
        m_clipIdx.append(curClip);
    };

    auto emitStroke = [&](float x0, float y0, float x1, float y1) {
        if (curDash.isEmpty()) { emitSeg(x0, y0, x1, y1); return; }
        float cycle = 0.0f;
        for (float d : curDash) cycle += d;
        if (cycle <= 1e-4f) { emitSeg(x0, y0, x1, y1); return; }
        const float segLen = std::hypot(x1 - x0, y1 - y0);
        if (segLen <= 1e-6f) return;
        const float ux = (x1 - x0) / segLen, uy = (y1 - y0) / segLen;
        float travelled = 0.0f;
        while (travelled < segLen) {
            float pos = std::fmod(dashRun, cycle);
            int idx = 0; float acc = 0.0f;
            for (; idx < curDash.size(); ++idx) { if (pos < acc + curDash[idx]) break; acc += curDash[idx]; }
            if (idx >= curDash.size()) { idx = curDash.size() - 1; acc = cycle - curDash[idx]; }
            const float remainInDash = (acc + curDash[idx]) - pos;
            const float step = qMin(remainInDash, segLen - travelled);
            const bool on = (idx % 2 == 0);
            if (on && step > 1e-4f) {
                emitSeg(x0 + ux * travelled,          y0 + uy * travelled,
                        x0 + ux * (travelled + step), y0 + uy * (travelled + step));
            }
            travelled += step;
            dashRun   += step;
        }
    };

    QVector<QPointF> poly;
    QVector<QVector<QPointF>> subpaths;
    auto appendPoly = [&](float px, float py) { poly.append(QPointF(px, py)); };

    auto flattenCubic = [&](float x0, float y0, float cx1, float cy1,
                            float cx2, float cy2, float x3, float y3) {
        const int N = 4;
        float prevX = x0, prevY = y0;
        for (int i = 1; i <= N; ++i) {
            float t = float(i) / N;
            float u = 1.0f - t;
            float u2 = u * u, u3 = u2 * u;
            float t2 = t * t, t3 = t2 * t;
            float px = u3*x0 + 3*u2*t*cx1 + 3*u*t2*cx2 + t3*x3;
            float py = u3*y0 + 3*u2*t*cy1 + 3*u*t2*cy2 + t3*y3;
            emitStroke(prevX, prevY, px, py);
            appendPoly(px, py);
            prevX = px; prevY = py;
        }
    };

    auto pushTri = [&](const QPointF& a, const QPointF& b, const QPointF& c,
                       uint8_t r, uint8_t g, uint8_t bl, uint8_t al) {
        if (m_fillVerts.size() > 6'000'000) return;
        const QPointF pts[3] = {a, b, c};
        for (const QPointF& q : pts) {
            m_fillVerts.append(float(q.x())); m_fillVerts.append(float(q.y()));
            m_fillColors.append(r); m_fillColors.append(g); m_fillColors.append(bl); m_fillColors.append(al);
            m_fillDepths.append(curDepth);
            m_fillClipIdx.append(curClip);
        }
    };
    auto triangulate = [&](const QVector<QPointF>& src,
                           uint8_t r, uint8_t g, uint8_t bl, uint8_t al) {
        QVector<QPointF> pts;
        pts.reserve(src.size());
        for (const QPointF& q : src) {
            if (!pts.isEmpty()
                && std::fabs(pts.last().x() - q.x()) < 1e-4
                && std::fabs(pts.last().y() - q.y()) < 1e-4) continue;
            pts.append(q);
        }
        if (pts.size() > 2
            && std::fabs(pts.first().x() - pts.last().x()) < 1e-4
            && std::fabs(pts.first().y() - pts.last().y()) < 1e-4) pts.removeLast();
        if (pts.size() < 3) return;
        if (pts.size() > 256) {
            for (int i = 1; i + 1 < pts.size(); ++i) pushTri(pts[0], pts[i], pts[i + 1], r, g, bl, al);
            return;
        }
        QVector<int> idx(pts.size());
        for (int i = 0; i < pts.size(); ++i) idx[i] = i;
        const bool ccw = polyArea2(pts) > 0;
        int guard = 0;
        while (idx.size() > 3 && guard++ < 4096) {
            bool clipped = false;
            for (int i = 0; i < idx.size(); ++i) {
                const QPointF& a = pts[idx[(i + idx.size() - 1) % idx.size()]];
                const QPointF& b = pts[idx[i]];
                const QPointF& c = pts[idx[(i + 1) % idx.size()]];
                const double cr = (b.x()-a.x())*(c.y()-a.y()) - (b.y()-a.y())*(c.x()-a.x());
                if ((ccw && cr <= 0) || (!ccw && cr >= 0)) continue;
                bool bad = false;
                for (int j = 0; j < idx.size() && !bad; ++j) {
                    if (j == i || j == (i + idx.size() - 1) % idx.size() || j == (i + 1) % idx.size()) continue;
                    if (pointInTri(pts[idx[j]], a, b, c)) bad = true;
                }
                if (bad) continue;
                pushTri(a, b, c, r, g, bl, al);
                idx.remove(i);
                clipped = true;
                break;
            }
            if (!clipped) break;
        }
        if (idx.size() == 3) { pushTri(pts[idx[0]], pts[idx[1]], pts[idx[2]], r, g, bl, al); }
        else if (idx.size() > 3) {
            for (int i = 1; i + 1 < idx.size(); ++i)
                pushTri(pts[idx[0]], pts[idx[i]], pts[idx[i + 1]], r, g, bl, al);
        }
    };

    int nObj = FPDFPage_CountObjects(page);
    constexpr int kHeavyThreshold = 2000; // ponytail: nguong THAP (2000 object) — do thuc te: 308k object build het 332ms, 137k het ~150ms; trang duoi nguong nay dung raster nen la du.
    if (nObj <= kHeavyThreshold) {
        FPDF_ClosePage(page);
        qDebug().noquote() << "[vector] bo qua page=" << pageIndex
                           << "objects=" << nObj << "(trang nhe)";
        return false;
    }
    qDebug().noquote() << "[vector] build BAT DAU page=" << pageIndex
                       << "objects=" << nObj;
    qint64 textBytes = 0;
    qint64 imgBytes = 0;
    bool complete = true;
    const char* completeReason = "none";
    int dbgFillMode = 0, dbgSubEmpty = 0, dbgColFail = 0;
    int dbgFillTranslucent = 0;
    int dbgFillBlack = 0;
    int dbgBigFillLog = 0;
    int imgsNative = 0, imgsFallback = 0;
    int dbgTextOpaque = 0;
    int dbgTextAlphaMax255 = 0;
    m_clips.clear();
    m_clips.append(QRectF());
    QHash<const void*, float> clipCache;
    int dbgClipPtr = 0, dbgClipNoGeom = 0, dbgClipTooBig = 0;

    for (int oi = 0; oi < nObj; ++oi) {
        curDepth = 1.0f - float(oi + 1) / float(nObj + 1);
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, oi);
        if (!obj) continue;

        curClip = 0.0f;
        if (FPDF_CLIPPATH cp = FPDFPageObj_GetClipPath(obj)) {
            auto it = clipCache.constFind((const void*)cp);
            if (it != clipCache.constEnd()) {
                curClip = *it;
            } else {
                ++dbgClipPtr;
                double gl = -1e30, gb = -1e30, gr = 1e30, gt = 1e30;
                bool any = false;
                const int np = FPDFClipPath_CountPaths(cp);
                for (int pi = 0; pi < np && pi < 8; ++pi) {
                    const int ns = FPDFClipPath_CountPathSegments(cp, pi);
                    if (ns <= 0) continue;
                    double l = 1e30, b = 1e30, r = -1e30, t = -1e30;
                    for (int si2 = 0; si2 < ns; ++si2) {
                        FPDF_PATHSEGMENT s = FPDFClipPath_GetPathSegment(cp, pi, si2);
                        if (!s) continue;
                        float px = 0, py = 0;
                        if (!FPDFPathSegment_GetPoint(s, &px, &py)) continue;
                        l = qMin(l, double(px)); r = qMax(r, double(px));
                        b = qMin(b, double(py)); t = qMax(t, double(py));
                    }
                    if (r <= l || t <= b) continue;
                    gl = qMax(gl, l); gb = qMax(gb, b);
                    gr = qMin(gr, r); gt = qMin(gt, t);
                    any = true;
                }
                if (!any) ++dbgClipNoGeom;
                float idx = 0.0f;
                if (any && gr > gl && gt > gb) {
                    QRectF rc(gl - originX, topY - gt, gr - gl, gt - gb);
                    const double cover = (rc.width() * rc.height()) / qMax(1.0, pageW * pageH);
                    if (cover < 0.995 && m_clips.size() < 64) {
                        int found = -1;
                        for (int ci = 1; ci < m_clips.size(); ++ci) {
                            const QRectF& e = m_clips[ci];
                            if (std::fabs(e.x() - rc.x()) < 0.5
                                && std::fabs(e.y() - rc.y()) < 0.5
                                && std::fabs(e.width() - rc.width()) < 0.5
                                && std::fabs(e.height() - rc.height()) < 0.5) {
                                found = ci;
                                break;
                            }
                        }
                        if (found >= 0) {
                            idx = float(found);
                        } else {
                            m_clips.append(rc);
                            idx = float(m_clips.size() - 1);
                        }
                    } else if (cover >= 0.995) {
                        ++dbgClipTooBig;
                    }
                }
                clipCache.insert((const void*)cp, idx);
                curClip = idx;
            }
        }

        if (FPDFPageObj_GetType(obj) == FPDF_PAGEOBJ_TEXT) {
            bool isOwnNote = false;
            const int nMarks = FPDFPageObj_CountMarks(obj);
            for (int mi = 0; mi < nMarks && !isOwnNote; ++mi) {
                FPDF_PAGEOBJECTMARK mk = FPDFPageObj_GetMark(obj, mi);
                if (!mk) continue;
                unsigned short buf[64] = {0};
                unsigned long outLen = 0;
                if (FPDFPageObjMark_GetName(mk, buf, sizeof(buf), &outLen) && outLen > 0) {
                    const QString nm = QString::fromUtf16(reinterpret_cast<const char16_t*>(buf));
                    if (nm.startsWith("TRNote")) isOwnNote = true;
                }
            }
            if (isOwnNote) m_noteObjIdx.append(oi);
            if (textBytes >= kMaxTextBytes) { if (complete) completeReason = "capText"; complete = false; continue; }
            float l = 0, b = 0, r = 0, tp = 0;
            if (!FPDFPageObj_GetBounds(obj, &l, &b, &r, &tp)) continue;
            if (r <= l || tp <= b) continue;
            unsigned int tr=0,tg=0,tb=0,ta=0;
            FPDF_BITMAP bmp = FPDFTextObj_GetRenderedBitmap(doc, page, obj, kTextScale);
            if (!bmp) continue;
            int bw = FPDFBitmap_GetWidth(bmp), bh = FPDFBitmap_GetHeight(bmp);
            if (bw > 0 && bh > 0) {
                QImage view((const uchar*)FPDFBitmap_GetBuffer(bmp), bw, bh,
                            FPDFBitmap_GetStride(bmp), QImage::Format_ARGB32);
                TextTile tile;
                tile.img = view.copy().convertToFormat(QImage::Format_Alpha8);
                tile.isAlpha = true;
                if (FPDFPageObj_GetFillColor(obj,&tr,&tg,&tb,&ta))
                    tile.color = qRgb(qMin(255u,tr),qMin(255u,tg),qMin(255u,tb));
                if (tile.img.isNull()) { FPDFBitmap_Destroy(bmp); continue; }
                if (qAlpha(tile.img.pixel(0, 0)) > 250) ++dbgTextOpaque;
                {
                    int w = tile.img.width(), h = tile.img.height();
                    if (w > 0 && h > 0
                        && qAlpha(tile.img.pixel(0,     0))     > 250
                        && qAlpha(tile.img.pixel(w - 1, 0))     > 250
                        && qAlpha(tile.img.pixel(0,     h - 1)) > 250
                        && qAlpha(tile.img.pixel(w - 1, h - 1)) > 250)
                        ++dbgTextAlphaMax255;
                }
                tile.rectPt = QRectF(l - originX, topY - tp, r - l, tp - b);
                tile.depth = curDepth;
                tile.clipIdx = curClip;
                tile.isNote = isOwnNote;
                textBytes += tile.img.sizeInBytes();
                m_texts.append(tile);
            }
            FPDFBitmap_Destroy(bmp);
            continue;
        }

        if (FPDFPageObj_GetType(obj) == FPDF_PAGEOBJ_IMAGE) {
            if (imgBytes >= kMaxImageBytes) { if (complete) completeReason = "capImage"; complete = false; continue; }
            float l = 0, b = 0, r = 0, tp = 0;
            if (!FPDFPageObj_GetBounds(obj, &l, &b, &r, &tp)) continue;
            if (r <= l || tp <= b) continue;
            FPDF_BITMAP bmp = FPDFImageObj_GetBitmap(obj);
            bool fromNative = (bmp != nullptr);
            if (fromNative) ++imgsNative; else ++imgsFallback;
            if (!bmp) bmp = FPDFImageObj_GetRenderedBitmap(doc, page, obj);
            if (!bmp) continue;
            int bw = FPDFBitmap_GetWidth(bmp), bh = FPDFBitmap_GetHeight(bmp);
            if (bw > 0 && bh > 0) {
                const int fmt = FPDFBitmap_GetFormat(bmp);
                QImage::Format qfmt = QImage::Format_ARGB32;
                if (fmt == FPDFBitmap_BGR)        qfmt = QImage::Format_BGR888;
                else if (fmt == FPDFBitmap_BGRx)  qfmt = QImage::Format_RGB32;
                else if (fmt == FPDFBitmap_Gray)  qfmt = QImage::Format_Grayscale8;
                QImage view((const uchar*)FPDFBitmap_GetBuffer(bmp), bw, bh,
                            FPDFBitmap_GetStride(bmp), qfmt);
                TextTile tile;
                tile.img = view.convertToFormat(QImage::Format_ARGB32);
                tile.rectPt = QRectF(l - originX, topY - tp, r - l, tp - b);
                tile.depth = curDepth;
                tile.clipIdx = curClip;
                imgBytes += tile.img.sizeInBytes();
                m_images.append(tile);
            }
            FPDFBitmap_Destroy(bmp);
            continue;
        }

        if (FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_PATH) { if (complete) completeReason = "objType"; complete = false; continue; }

        int fillMode = 0;
        FPDF_BOOL strokeBool = 0;
        FPDFPath_GetDrawMode(obj, &fillMode, &strokeBool);
        bool hasStroke = (strokeBool != 0);
        if (!hasStroke && fillMode == 0) continue;

        unsigned int sr = 0, sg = 0, sb = 0, sa = 0;
        if (!FPDFPageObj_GetStrokeColor(obj, &sr, &sg, &sb, &sa)) continue;

        vr = (uint8_t)qMin(255u, sr);
        vg = (uint8_t)qMin(255u, sg);
        vb = (uint8_t)qMin(255u, sb);
        va = (uint8_t)qMin(255u, sa);

        FS_MATRIX mat{};
        FPDFPageObj_GetMatrix(obj, &mat);

        const float mscale = std::sqrt(std::fabs(mat.a * mat.d - mat.b * mat.c));
        float swRaw = 0.0f;
        FPDFPageObj_GetStrokeWidth(obj, &swRaw);
        curWidth = swRaw * (mscale > 1e-6f ? mscale : 1.0f);

        curDash.clear();
        float dashPhase = 0.0f;
        const int dashN = FPDFPageObj_GetDashCount(obj);
        if (dashN > 0 && dashN <= 16) {
            QVector<float> raw(dashN);
            if (FPDFPageObj_GetDashArray(obj, raw.data(), dashN)) {
                FPDFPageObj_GetDashPhase(obj, &dashPhase);
                bool allZero = true;
                for (float& d : raw) { d *= (mscale > 1e-6f ? mscale : 1.0f); if (d > 1e-4f) allZero = false; }
                if (!allZero) {
                    curDash = raw;
                    if (curDash.size() % 2 == 1) curDash += curDash;
                    dashPhase *= (mscale > 1e-6f ? mscale : 1.0f);
                }
            }
        }
        dashRun = dashPhase;

        poly.clear();
        subpaths.clear();

        int nSeg = FPDFPath_CountSegments(obj);
        float curX = 0, curY = 0;
        bool hasCur = false;
        float subX = 0, subY = 0;

        for (int si = 0; si < nSeg; ++si) {
            FPDF_PATHSEGMENT seg = FPDFPath_GetPathSegment(obj, si);
            if (!seg) continue;
            int segType = FPDFPathSegment_GetType(seg);
            float sx = 0, sy = 0;
            FPDFPathSegment_GetPoint(seg, &sx, &sy);

            float mx = mat.a * sx + mat.c * sy + mat.e - originX;
            float my = mat.b * sx + mat.d * sy + mat.f;
            my = (float)topY - my;

            switch (segType) {
            case FPDF_SEGMENT_MOVETO:
                if (poly.size() >= 3) subpaths.append(poly);
                poly.clear();
                curX = mx; curY = my; hasCur = true;
                subX = mx; subY = my;
                dashRun = dashPhase;
                poly.append(QPointF(mx, my));
                break;
            case FPDF_SEGMENT_LINETO:
                if (hasCur) {
                    emitStroke(curX, curY, mx, my);
                }
                curX = mx; curY = my;
                poly.append(QPointF(mx, my));
                break;
            case FPDF_SEGMENT_BEZIERTO: {
                float cx1 = mx, cy1 = my;
                float cx2 = 0, cy2 = 0, ex = 0, ey = 0;
                if (si + 1 < nSeg) {
                    FPDF_PATHSEGMENT s2 = FPDFPath_GetPathSegment(obj, si + 1);
                    if (s2) {
                        float px = 0, py = 0;
                        FPDFPathSegment_GetPoint(s2, &px, &py);
                        cx2 = mat.a * px + mat.c * py + mat.e - originX;
                        cy2 = (float)topY - (mat.b * px + mat.d * py + mat.f);
                    }
                    ++si;
                }
                if (si + 1 < nSeg) {
                    FPDF_PATHSEGMENT s3 = FPDFPath_GetPathSegment(obj, si + 1);
                    if (s3) {
                        float px = 0, py = 0;
                        FPDFPathSegment_GetPoint(s3, &px, &py);
                        ex = mat.a * px + mat.c * py + mat.e - originX;
                        ey = (float)topY - (mat.b * px + mat.d * py + mat.f);
                    }
                    ++si;
                }
                if (hasCur) {
                    flattenCubic(curX, curY, cx1, cy1, cx2, cy2, ex, ey);
                }
                curX = ex; curY = ey;
                break;
            }
            }
            if (hasCur && FPDFPathSegment_GetClose(seg)) {
                emitStroke(curX, curY, subX, subY);
                hasCur = false;
            }
        }
        if (fillMode != 0) ++dbgFillMode;
        if (poly.size() >= 3) subpaths.append(poly);
        if (fillMode != 0 && subpaths.isEmpty()) ++dbgSubEmpty;
        if (fillMode != 0 && !subpaths.isEmpty() && m_fillVerts.size() <= 6'000'000) {
            unsigned int fr = 0, fg = 0, fb = 0, fa = 0;
            if (FPDFPageObj_GetFillColor(obj, &fr, &fg, &fb, &fa) && fa > 0) {
                uint8_t fillR = (uint8_t)qMin(255u, fr);
                uint8_t fillG = (uint8_t)qMin(255u, fg);
                uint8_t fillB = (uint8_t)qMin(255u, fb);
                uint8_t fillA = (uint8_t)qMin(255u, fa);
                if (fillA < 255) ++dbgFillTranslucent;
                if (fillR < 32 && fillG < 32 && fillB < 32) ++dbgFillBlack;
                if (dbgBigFillLog < 12) {
                    float bbL = 1e30f, bbB = 1e30f, bbR = -1e30f, bbT = -1e30f;
                    for (const QVector<QPointF>& sp : subpaths)
                        for (const QPointF& p : sp) {
                            bbL = qMin(bbL, (float)p.x()); bbB = qMin(bbB, (float)p.y());
                            bbR = qMax(bbR, (float)p.x()); bbT = qMax(bbT, (float)p.y());
                        }
                    float bbW = bbR - bbL, bbH = bbT - bbB;
                    if (bbW > 0 && bbH > 0 && (double)bbW * bbH > 0.02 * pageW * pageH) {
                        ++dbgBigFillLog;
                        qDebug().noquote() << "[vector] BIG FILL page=" << pageIndex
                                           << "obj=" << oi
                                           << "x=" << bbL + originX << "y=" << topY - bbT
                                           << "w=" << bbW << "h=" << bbH
                                           << "rgba=" << fillR << fillG << fillB << fillA
                                           << "fillMode=" << fillMode
                                           << "stroke=" << strokeBool
                                           << "nSub=" << subpaths.size();
                    }
                }
                for (const QVector<QPointF>& sp : subpaths) {
                    triangulate(sp, fillR, fillG, fillB, fillA);
                }
            } else {
                ++dbgColFail;
            }
        }
        if (m_fillVerts.size() > 6'000'000) { if (complete) completeReason = "capFill"; complete = false; }
    }

    {
        int clipDumpN = qMin(4, (int)m_clips.size() - 1);
        for (int i = 1; i <= clipDumpN; ++i) {
            const QRectF& cr = m_clips[i];
            qDebug().noquote() << "[vector] clipRect i=" << i
                               << "x=" << cr.x() << "y=" << cr.y()
                               << "w=" << cr.width() << "h=" << cr.height()
                               << "pageW=" << pageW << "pageH=" << pageH;
        }
    }

    if (m_widths.size() > 12'000'000) {
        qDebug().noquote() << "[vector] CANH BAO qua nhieu doan sau dash=" << m_widths.size();
        if (complete) completeReason = "capSeg";
        complete = false;
    }
    if (m_fillVerts.size() > 6'000'000) {
        qDebug().noquote() << "[vector] fill CAP";
    }

    FPDF_ClosePage(page);
    m_ready = true;
    m_complete = complete;
    m_buildObjCount = nObj;
    ++m_tilesGen;
    qDebug().noquote() << "[vector] build DONE page=" << pageIndex
                       << "paths=" << nObj
                       << "segs=" << m_widths.size()
                       << "verts=" << m_verts.size() / 2
                       << "texts=" << m_texts.size()
                       << "imgs=" << m_images.size()
                       << "imgsNative=" << imgsNative
                       << "imgsFallback=" << imgsFallback
                       << "fills=" << m_fillVerts.size() / 6
                       << "fillObjs=" << dbgFillMode
                        << "subEmpty=" << dbgSubEmpty
                        << "colFail=" << dbgColFail
                        << "textOpaqueBg=" << dbgTextOpaque
                        << "textAll4Corners=" << dbgTextAlphaMax255
                        << "fillA255less=" << dbgFillTranslucent
                         << "fillBlack=" << dbgFillBlack
                          << "clips=" << (m_clips.size() - 1)
                         << "clipPtr=" << dbgClipPtr
                         << "clipNoGeom=" << dbgClipNoGeom
                          << "clipTooBig=" << dbgClipTooBig
                          << "complete=" << (complete ? 1 : 0)
                         << "completeReason=" << completeReason
                       << "ms=" << t.elapsed();
    return true;
}

int VectorLayer::rebuildNoteTiles(FPDF_DOCUMENT doc, FPDF_PAGE page) {
    QElapsedTimer t;
    t.start();

    m_texts.erase(std::remove_if(m_texts.begin(), m_texts.end(),
                                  [](const TextTile& tt) { return tt.isNote; }),
                  m_texts.end());

    double pageW = FPDF_GetPageWidth(page);
    double pageH = FPDF_GetPageHeight(page);
    float cbL = 0.f, cbB = 0.f, cbR = 0.f, cbT = 0.f;
    bool haveBox = FPDFPage_GetCropBox(page, &cbL, &cbB, &cbR, &cbT);
    if (!haveBox) haveBox = FPDFPage_GetMediaBox(page, &cbL, &cbB, &cbR, &cbT);
    if (!haveBox || cbR <= cbL || cbT <= cbB) { cbL = 0.f; cbB = 0.f;
                                               cbR = float(pageW); cbT = float(pageH); }
    const double originX = double(cbL);
    const double topY    = double(cbT);

    int count = 0;
    int nObj = FPDFPage_CountObjects(page);
    bool needFullScan = (nObj != m_buildObjCount) || m_noteObjIdx.isEmpty();

    if (!needFullScan) {
        for (int oi : m_noteObjIdx) {
            FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, oi);
            if (!obj || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_TEXT) { needFullScan = true; break; }
        }
    }

    if (needFullScan) {
        m_noteObjIdx.clear();
        for (int oi = 0; oi < nObj; ++oi) {
            FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, oi);
            if (!obj) continue;
            if (FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_TEXT) continue;

            bool isOwnNote = false;
            const int nMarks = FPDFPageObj_CountMarks(obj);
            for (int mi = 0; mi < nMarks && !isOwnNote; ++mi) {
                FPDF_PAGEOBJECTMARK mk = FPDFPageObj_GetMark(obj, mi);
                if (!mk) continue;
                unsigned short buf[64] = {0};
                unsigned long outLen = 0;
                if (FPDFPageObjMark_GetName(mk, buf, sizeof(buf), &outLen) && outLen > 0) {
                    const QString nm = QString::fromUtf16(reinterpret_cast<const char16_t*>(buf));
                    if (nm.startsWith("TRNote")) isOwnNote = true;
                }
            }
            if (!isOwnNote) continue;
            m_noteObjIdx.append(oi);

            float l = 0, b = 0, r = 0, tp = 0;
            if (!FPDFPageObj_GetBounds(obj, &l, &b, &r, &tp)) continue;
            if (r <= l || tp <= b) continue;
            unsigned int tr=0,tg=0,tb=0,ta=0;
            FPDF_BITMAP bmp = FPDFTextObj_GetRenderedBitmap(doc, page, obj, kTextScale);
            if (!bmp) continue;
            int bw = FPDFBitmap_GetWidth(bmp), bh = FPDFBitmap_GetHeight(bmp);
            if (bw > 0 && bh > 0) {
                QImage view((const uchar*)FPDFBitmap_GetBuffer(bmp), bw, bh,
                            FPDFBitmap_GetStride(bmp), QImage::Format_ARGB32);
                TextTile tile;
                tile.img = view.copy().convertToFormat(QImage::Format_Alpha8);
                tile.isAlpha = true;
                if (FPDFPageObj_GetFillColor(obj,&tr,&tg,&tb,&ta))
                    tile.color = qRgb(qMin(255u,tr),qMin(255u,tg),qMin(255u,tb));
                if (!tile.img.isNull()) {
                    tile.rectPt = QRectF(l - originX, topY - tp, r - l, tp - b);
                    tile.depth = 1.0f - float(oi + 1) / float(nObj + 1);
                    tile.isNote = true;
                    m_texts.append(tile);
                    ++count;
                }
            }
            FPDFBitmap_Destroy(bmp);
        }
        m_buildObjCount = nObj;
    } else {
        for (int oi : m_noteObjIdx) {
            FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, oi);
            if (!obj) continue;

            float l = 0, b = 0, r = 0, tp = 0;
            if (!FPDFPageObj_GetBounds(obj, &l, &b, &r, &tp)) continue;
            if (r <= l || tp <= b) continue;
            unsigned int tr=0,tg=0,tb=0,ta=0;
            FPDF_BITMAP bmp = FPDFTextObj_GetRenderedBitmap(doc, page, obj, kTextScale);
            if (!bmp) continue;
            int bw = FPDFBitmap_GetWidth(bmp), bh = FPDFBitmap_GetHeight(bmp);
            if (bw > 0 && bh > 0) {
                QImage view((const uchar*)FPDFBitmap_GetBuffer(bmp), bw, bh,
                            FPDFBitmap_GetStride(bmp), QImage::Format_ARGB32);
                TextTile tile;
                tile.img = view.copy().convertToFormat(QImage::Format_Alpha8);
                tile.isAlpha = true;
                if (FPDFPageObj_GetFillColor(obj,&tr,&tg,&tb,&ta))
                    tile.color = qRgb(qMin(255u,tr),qMin(255u,tg),qMin(255u,tb));
                if (!tile.img.isNull()) {
                    tile.rectPt = QRectF(l - originX, topY - tp, r - l, tp - b);
                    tile.depth = 1.0f - float(oi + 1) / float(nObj + 1);
                    tile.isNote = true;
                    m_texts.append(tile);
                    ++count;
                }
            }
            FPDFBitmap_Destroy(bmp);
        }
    }

    qDebug().noquote() << "[vector] noteTiles rebuilt n=" << count << "scanned=" << (needFullScan ? nObj : m_noteObjIdx.size()) << "ms=" << t.elapsed();
    ++m_tilesGen;
    return count;
}

int VectorLayer::translateNoteTiles(const QRectF& hitRect, const QPointF& d) {
    if (hitRect.isEmpty()) return 0;
    int count = 0;
    for (TextTile& tt : m_texts) {
        if (tt.isNote && tt.rectPt.intersects(hitRect)) {
            tt.rectPt.translate(d);
            ++count;
        }
    }
    return count;
}
