#include "VectorLayer.h"
#include "PageCache.h"
#include <fpdf_edit.h>
#include <fpdf_transformpage.h>
#include <fpdfview.h>
#include <QMutex>
#include <QHash>
#include <QElapsedTimer>
#include <QDebug>
#include <QVarLengthArray>
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
    m_rotation = 0;
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
    m_fillOpaqueFloats = 0;
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
    // Trang muon TU PageCache (chu so huu duy nhat) — khong tu Close. Caller giu s_pdfiumMutex.
    FPDF_PAGE page = PageCache::acquire(doc, pageIndex);
    if (!page) return false;

    // FPDF_GetPageWidth/Height DA ap /Rotate -> day la co HIEN THI, KHONG phai co hop crop.
    double pageW = FPDF_GetPageWidth(page);
    double pageH = FPDF_GetPageHeight(page);
    const int rot = FPDFPage_GetRotation(page) & 3;   // 0..3
    m_rotation = rot;
    m_page = pageIndex;

    float cbL = 0.f, cbB = 0.f, cbR = 0.f, cbT = 0.f;
    bool haveBox = FPDFPage_GetCropBox(page, &cbL, &cbB, &cbR, &cbT);
    if (!haveBox) haveBox = FPDFPage_GetMediaBox(page, &cbL, &cbB, &cbR, &cbT);
    if (!haveBox || cbR <= cbL || cbT <= cbB) {
        // Khong doc duoc hop: suy nguoc tu co hien thi (bo xoay ra)
        cbL = 0.f; cbB = 0.f;
        if (rot & 1) { cbR = float(pageH); cbT = float(pageW); }
        else         { cbR = float(pageW); cbT = float(pageH); }
    }
    // m_pageSize = khong gian hinh hoc CHUA xoay (moi toa do phat ra deu nam trong hop nay)
    m_pageSize = QSizeF(double(cbR) - double(cbL), double(cbT) - double(cbB));
    const double originX = double(cbL);
    const double topY    = double(cbT);
    qDebug().noquote() << "[vector] box page=" << pageIndex
                       << "crop=" << cbL << cbB << cbR << cbT
                       << "rot=" << rot
                       << "layerWH=" << m_pageSize.width() << m_pageSize.height()
                       << "dispWH=" << pageW << pageH;

    uint8_t vr = 0, vg = 0, vb = 0, va = 255;
    float   curWidth = 0.0f;
    QVector<float> curDash;
    float   dashRun = 0.0f;
    float   curDepth = 1.0f;
    float   curClip = 0.0f;
    bool    curStroke = false;
    const QVector<QVector<QPointF>>* curClipTris = nullptr;

    // Cat doan [p0,p1] vao TAM GIAC t[0..2] (luon loi). Tra ve true + khoang tham so [a,b] con lai.
    auto clipSegToTri = [](float x0, float y0, float x1, float y1,
                           const QPointF* t, double& a, double& b) -> bool {
        const double area2 = (t[1].x()-t[0].x())*(t[2].y()-t[0].y())
                           - (t[1].y()-t[0].y())*(t[2].x()-t[0].x());
        if (std::fabs(area2) < 1e-12) return false;
        const double s = (area2 > 0) ? 1.0 : -1.0;
        for (int e = 0; e < 3; ++e) {
            const QPointF& A = t[e];
            const QPointF& B = t[(e + 1) % 3];
            const double ex = B.x() - A.x(), ey = B.y() - A.y();
            const double f0 = s * (ex * (double(y0) - A.y()) - ey * (double(x0) - A.x()));
            const double f1 = s * (ex * (double(y1) - A.y()) - ey * (double(x1) - A.x()));
            const double df = f1 - f0;
            if (std::fabs(df) < 1e-12) { if (f0 < 0.0) return false; continue; }
            const double tt = -f0 / df;
            if (df > 0.0) { if (tt > a) a = tt; }
            else          { if (tt < b) b = tt; }
            if (a > b) return false;
        }
        return b > a + 1e-9;
    };

    int dbgSegClipped = 0;
    int dbgSegRescued = 0;
    int dbgStrokeSkipped = 0;
    auto emitSegRaw = [&](float x0, float y0, float x1, float y1) {

        m_verts.append(x0); m_verts.append(y0);
        m_verts.append(x1); m_verts.append(y1);
        m_colors.append(vr); m_colors.append(vg); m_colors.append(vb); m_colors.append(va);
        m_widths.append(curWidth);
        m_depths.append(curDepth);
        m_clipIdx.append(curClip);
    };

    // ponytail: khoang cach nho nhat giua 2 doan thang — dung de cuu net nam sat mep clip
    auto segSegDist = [](double ax0,double ay0,double ax1,double ay1,
                         double bx0,double by0,double bx1,double by1) -> double {
        auto ptSeg = [](double px,double py,double x0,double y0,double x1,double y1)->double{
            const double dx=x1-x0, dy=y1-y0;
            const double L2=dx*dx+dy*dy;
            double t = (L2>1e-12) ? ((px-x0)*dx + (py-y0)*dy)/L2 : 0.0;
            t = t<0.0?0.0:(t>1.0?1.0:t);
            const double qx=x0+t*dx, qy=y0+t*dy;
            return std::hypot(px-qx, py-qy);
        };
        double d = ptSeg(ax0,ay0,bx0,by0,bx1,by1);
        d = qMin(d, ptSeg(ax1,ay1,bx0,by0,bx1,by1));
        d = qMin(d, ptSeg(bx0,by0,ax0,ay0,ax1,ay1));
        d = qMin(d, ptSeg(bx1,by1,ax0,ay0,ax1,ay1));
        return d;
    };

    auto emitSeg = [&](float x0, float y0, float x1, float y1) {
        if (!curClipTris || curClipTris->isEmpty()) { emitSegRaw(x0, y0, x1, y1); return; }
        QVarLengthArray<QPair<double, double>, 16> segs;
        segs.append(qMakePair(0.0, 1.0));
        for (const QVector<QPointF>& tris : *curClipTris) {
            QVarLengthArray<QPair<double, double>, 16> next;
            for (const QPair<double, double>& sg : segs) {
                for (int ti = 0; ti + 2 < tris.size(); ti += 3) {
                    double a = 0.0, b = 1.0;
                    if (!clipSegToTri(x0, y0, x1, y1, tris.constData() + ti, a, b)) continue;
                    const double na = qMax(a, sg.first), nb = qMin(b, sg.second);
                    if (nb > na + 1e-9) next.append(qMakePair(na, nb));
                }
            }
            segs = next;
            if (segs.isEmpty()) {
                // Net nam sat mep clip: duong tam roi ra ngoai nhung MOT NUA BE RONG net
                // van phai hien. Do khoang cach toi cac canh tam giac clip; trong pham vi
                // nua be rong thi giu nguyen doan, con hon vut sach ca net.
                const double margin = double(curWidth) * 0.5 + 0.05;
                if (margin > 0.05) {
                    double best = 1e30;
                    for (const QVector<QPointF>& tris2 : *curClipTris) {
                        for (int ti = 0; ti + 2 < tris2.size(); ti += 3) {
                            const QPointF& p0 = tris2[ti];
                            const QPointF& p1 = tris2[ti+1];
                            const QPointF& p2 = tris2[ti+2];
                            best = qMin(best, segSegDist(x0,y0,x1,y1, p0.x(),p0.y(), p1.x(),p1.y()));
                            best = qMin(best, segSegDist(x0,y0,x1,y1, p1.x(),p1.y(), p2.x(),p2.y()));
                            best = qMin(best, segSegDist(x0,y0,x1,y1, p2.x(),p2.y(), p0.x(),p0.y()));
                            if (best <= margin) break;
                        }
                        if (best <= margin) break;
                    }
                    if (best <= margin) { ++dbgSegRescued; emitSegRaw(x0, y0, x1, y1); return; }
                }
                ++dbgSegClipped;
                return;
            }
            if (segs.size() > 512) { emitSegRaw(x0, y0, x1, y1); return; }
        }
        const double dx = double(x1) - double(x0), dy = double(y1) - double(y0);
        for (const QPair<double, double>& sg : segs) {
            emitSegRaw(float(x0 + dx * sg.first),  float(y0 + dy * sg.first),
                       float(x0 + dx * sg.second), float(y0 + dy * sg.second));
        }
    };

    auto emitStroke = [&](float x0, float y0, float x1, float y1) {
        // ponytail: path chi-TO khong duoc ve duong bao. PDF spec: khong co toan tu stroke thi khong ve net.
        // Truoc day emitStroke goi vo dieu kien nen moi mang to bi vien mo bao quanh (Revit dat nen trang sau
        // moi nhan chu => moi text co 1 o vien).
        if (!curStroke) { ++dbgStrokeSkipped; return; }
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

    QVector<float>   tmpFillVertsA;
    QVector<uint8_t> tmpFillColorsA;
    QVector<float>   tmpFillDepthsA;
    QVector<float>   tmpFillClipIdxA;

    auto pushTri = [&](const QPointF& a, const QPointF& b, const QPointF& c,
                       uint8_t r, uint8_t g, uint8_t bl, uint8_t al) {
        if (m_fillVerts.size() > 6'000'000) return;
        const QPointF pts[3] = {a, b, c};
        // ponytail: fill trong suot di vao buffer tam, duoi cung moi ghep vao mang chinh
        QVector<float>& dstV = (al < 255) ? tmpFillVertsA : m_fillVerts;
        QVector<uint8_t>& dstC = (al < 255) ? tmpFillColorsA : m_fillColors;
        QVector<float>& dstD = (al < 255) ? tmpFillDepthsA : m_fillDepths;
        QVector<float>& dstI = (al < 255) ? tmpFillClipIdxA : m_fillClipIdx;
        for (const QPointF& q : pts) {
            dstV.append(float(q.x())); dstV.append(float(q.y()));
            dstC.append(r); dstC.append(g); dstC.append(bl); dstC.append(al);
            dstD.append(curDepth);
            dstI.append(curClip);
        }
    };
    // Phan ra vung clip thanh HINH THANG bang quet doc (scanline) theo luat EVEN-ODD, roi cat doi
    // moi hinh thang thanh 2 tam giac. Phai lam vay moi dung khi duong clip co LO: ear-clip tung
    // subpath roi HOP se lap day lo. PDFium khong co API doc luat winding cua clip; file CAD dung
    // eofill (733/737 clip cua trang mau) nen chon even-odd — clip chi co 1 contour thi 2 luat nhu nhau.
    auto buildTris = [](const QVector<QVector<QPointF>>& subs, bool evenOdd, QVector<QPointF>& out) {
        struct Edge { double x0, y0, x1, y1; int dir; };
        QVector<Edge> edges;
        QVector<double> ys;
        for (const QVector<QPointF>& sp : subs) {
            const int n = sp.size();
            if (n < 3) continue;
            for (int i = 0; i < n; ++i) {
                const QPointF& a = sp[i];
                const QPointF& b = sp[(i + 1) % n];
                if (std::fabs(a.y() - b.y()) < 1e-9) continue;
                edges.append(Edge{a.x(), a.y(), b.x(), b.y(), (b.y() > a.y()) ? 1 : -1});
                ys.append(a.y());
                ys.append(b.y());
            }
        }
        if (edges.isEmpty()) return;
        std::sort(ys.begin(), ys.end());
        ys.erase(std::unique(ys.begin(), ys.end(),
                             [](double p, double q){ return std::fabs(p - q) < 1e-9; }), ys.end());
        struct Cross { double xa, xb; int dir; };
        QVector<Cross> xs;
        for (int bi = 0; bi + 1 < ys.size(); ++bi) {
            const double ya = ys[bi], yb = ys[bi + 1];
            if (yb - ya < 1e-9) continue;
            xs.clear();
            for (const Edge& e : edges) {
                const double lo = qMin(e.y0, e.y1), hi = qMax(e.y0, e.y1);
                if (lo > ya + 1e-9 || hi < yb - 1e-9) continue;
                const double dy = e.y1 - e.y0;
                if (std::fabs(dy) < 1e-12) continue;
                const double t0 = (ya - e.y0) / dy;
                const double t1 = (yb - e.y0) / dy;
                xs.append(Cross{e.x0 + (e.x1 - e.x0) * t0,
                                e.x0 + (e.x1 - e.x0) * t1, e.dir});
            }
            if (xs.size() < 2) continue;
            std::sort(xs.begin(), xs.end(),
                      [](const Cross& p, const Cross& q) {
                          return (p.xa + p.xb) < (q.xa + q.xb);
                      });
            int wind = 0;
            for (int k = 0; k + 1 < xs.size(); ++k) {
                wind += evenOdd ? 1 : xs[k].dir;
                const bool inside = evenOdd ? ((k + 1) & 1) != 0 : (wind != 0);
                if (!inside) continue;
                const double la = xs[k].xa,     lb = xs[k].xb;
                const double ra = xs[k + 1].xa, rb = xs[k + 1].xb;
                if (ra - la < 1e-9 && rb - lb < 1e-9) continue;
                const QPointF p0(la, ya), p1(ra, ya), p2(rb, yb), p3(lb, yb);
                out.append(p0); out.append(p1); out.append(p2);
                out.append(p0); out.append(p2); out.append(p3);
            }
        }
    };

    // Sutherland-Hodgman: cat da giac `poly` bang TAM GIAC (luon loi) t[0..2]. Chinh xac tuyet doi.
    auto clipByTri = [](const QVector<QPointF>& poly, const QPointF* t) -> QVector<QPointF> {
        QVector<QPointF> out = poly;
        const double area2 = (t[1].x()-t[0].x())*(t[2].y()-t[0].y())
                           - (t[1].y()-t[0].y())*(t[2].x()-t[0].x());
        if (std::fabs(area2) < 1e-12) return QVector<QPointF>();
        const double s = (area2 > 0) ? 1.0 : -1.0;
        for (int e = 0; e < 3 && out.size() >= 3; ++e) {
            const QPointF& a = t[e];
            const QPointF& b = t[(e + 1) % 3];
            auto side = [&](const QPointF& p) {
                return s * ((b.x()-a.x())*(p.y()-a.y()) - (b.y()-a.y())*(p.x()-a.x()));
            };
            QVector<QPointF> in;
            in.reserve(out.size() + 4);
            for (int i = 0; i < out.size(); ++i) {
                const QPointF& p = out[i];
                const QPointF& q = out[(i + 1) % out.size()];
                const double dp = side(p), dq = side(q);
                const bool ip = (dp >= -1e-9), iq = (dq >= -1e-9);
                if (ip) in.append(p);
                if (ip != iq) {
                    const double den = dp - dq;
                    if (std::fabs(den) > 1e-12) {
                        const double tt = dp / den;
                        in.append(QPointF(p.x() + (q.x() - p.x()) * tt,
                                          p.y() + (q.y() - p.y()) * tt));
                    }
                }
            }
            out = in;
        }
        return (out.size() >= 3) ? out : QVector<QPointF>();
    };

    int nObj = FPDFPage_CountObjects(page);
    constexpr int kHeavyThreshold = 2000; // ponytail: nguong THAP (2000 object) — do thuc te: 308k object build het 332ms, 137k het ~150ms; trang duoi nguong nay dung raster nen la du.
    if (nObj <= kHeavyThreshold) {
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
    int imgsNative = 0, imgsFallback = 0, imgsMasked = 0;
    int dbgTextOpaque = 0;
    int dbgTextAlphaMax255 = 0;
    m_clips.clear();
    m_clips.append(QRectF());
    QHash<const void*, float> clipCache;
    QHash<const void*, QVector<QVector<QPointF>>> clipTriCache;
    clipTriCache.reserve(1024);   // bot rehash -> bot rui ro con tro treo
    int dbgClipPolyBuilt = 0, dbgFillClipped = 0, dbgFillClipBail = 0;
    int dbgClipPtr = 0, dbgClipNoGeom = 0, dbgClipTooBig = 0;
    for (int oi = 0; oi < nObj; ++oi) {
        curDepth = 1.0f - float(oi + 1) / float(nObj + 1);
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, oi);
        if (!obj) continue;

        curClip = 0.0f;
        curClipTris = nullptr;
        if (FPDF_CLIPPATH cp = FPDFPageObj_GetClipPath(obj)) {
            auto it = clipCache.constFind((const void*)cp);
            if (it != clipCache.constEnd()) {
                curClip = *it;
                auto tit = clipTriCache.constFind((const void*)cp);
                if (tit != clipTriCache.constEnd() && !tit->isEmpty()) curClipTris = &(*tit);
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
                    // Tam giac clip: dung ke ca khi m_clips DA DAY (shader chi cat duoc 64 clip,
                    // con cat hinh hoc o CPU thi khong bi gioi han do).
                    if (cover < 0.995) {
                        QVector<QVector<QPointF>> perPath;
                        bool clipUsable = true;
                        for (int pi = 0; pi < np && pi < 8 && clipUsable; ++pi) {
                            const int ns = FPDFClipPath_CountPathSegments(cp, pi);
                            if (ns <= 0) continue;
                            QVector<QVector<QPointF>> subs;
                            QVector<QPointF> sub;
                            for (int si2 = 0; si2 < ns; ++si2) {
                                FPDF_PATHSEGMENT s = FPDFClipPath_GetPathSegment(cp, pi, si2);
                                if (!s) continue;
                                const int st = FPDFPathSegment_GetType(s);
                                if (st == FPDF_SEGMENT_BEZIERTO) { clipUsable = false; break; }
                                float px = 0, py = 0;
                                if (!FPDFPathSegment_GetPoint(s, &px, &py)) continue;
                                const QPointF q(double(px) - originX, topY - double(py));
                                if (st == FPDF_SEGMENT_MOVETO) {
                                    if (sub.size() >= 3) subs.append(sub);
                                    sub.clear();
                                }
                                sub.append(q);
                            }
                            if (!clipUsable) break;
                            if (sub.size() >= 3) subs.append(sub);
                            QVector<QPointF> tris;
                            buildTris(subs, true, tris);
                            if (tris.size() >= 3) perPath.append(tris);
                        }
                        if (clipUsable && !perPath.isEmpty()) {
                            clipTriCache.insert((const void*)cp, perPath);
                            auto nit = clipTriCache.constFind((const void*)cp);
                            if (nit != clipTriCache.constEnd()) curClipTris = &(*nit);
                            ++dbgClipPolyBuilt;
                        }
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
            // Chu OCR vo hinh (render mode INVISIBLE hoac fill alpha=0) khong duoc
            // ve — lop OCR chi ton tai de search/tim kiem, ve ra thanh chu den
            // tren anh scan (loi da gap: OCR dat INVISIBLE + alpha=0 van hien).
            {
                unsigned int cR = 0, cG = 0, cB = 0, cA = 0;
                if (FPDFTextObj_GetTextRenderMode(obj) == FPDF_TEXTRENDERMODE_INVISIBLE) continue;
                if (FPDFPageObj_GetFillColor(obj, &cR, &cG, &cB, &cA) && cA == 0) continue;
            }
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
            // ponytail: GetBitmap tra pixel goc, bo qua /SMask -> KHOI DEN cho anh trong suot.
            //    Thu RenderedBitmap truoc; neu co alpha < 255 thi anh co mat na, dung no.
            //    Khong thi fallback pixel goc cho sac net.
            FPDF_BITMAP bmp = nullptr;
            bool fromNative = false;
            bool rbUsed = false;
            {
                // Do phan giai cua GetRenderedBitmap bam theo ma tran doi tuong (co tren trang).
                // Anh mat na bi thu nho nhieu lan => alpha loang => chu nhat. Tam phong ma tran
                // len cho gan do phan giai goc roi TRA LAI NGAY.
                FPDF_BITMAP rb = nullptr;
                {
                    FPDF_IMAGEOBJ_METADATA md{};
                    unsigned int natW = 0, natH = 0;
                    if (FPDFImageObj_GetImageMetadata(obj, page, &md)) {
                        natW = md.width; natH = md.height;
                    }
                    FS_MATRIX om{};
                    const bool haveM = FPDFPageObj_GetMatrix(obj, &om) != 0;
                    rb = FPDFImageObj_GetRenderedBitmap(doc, page, obj);
                    if (rb && haveM && natW > 0 && natH > 0) {
                        const int rw0 = FPDFBitmap_GetWidth(rb);
                        const int rh0 = FPDFBitmap_GetHeight(rb);
                        if (rw0 > 0 && rh0 > 0) {
                            double k = qMin(double(natW) / double(rw0), double(natH) / double(rh0));
                            const double kCap = std::sqrt(4000000.0 / double(qMax(1, rw0 * rh0)));
                            k = qMin(k, kCap);
                            if (k > 1.2) {
                                FS_MATRIX big{ float(om.a * k), float(om.b * k),
                                               float(om.c * k), float(om.d * k), om.e, om.f };
                                if (FPDFPageObj_SetMatrix(obj, &big)) {
                                    FPDF_BITMAP rb2 = FPDFImageObj_GetRenderedBitmap(doc, page, obj);
                                    FPDFPageObj_SetMatrix(obj, &om);
                                    if (rb2) { FPDFBitmap_Destroy(rb); rb = rb2; }
                                }
                            }
                        }
                    }
                }
                bool rbHasAlpha = false;
                if (rb && FPDFBitmap_GetFormat(rb) == FPDFBitmap_BGRA) {
                    const int rw = FPDFBitmap_GetWidth(rb), rh = FPDFBitmap_GetHeight(rb);
                    const int rs = FPDFBitmap_GetStride(rb);
                    const unsigned char* rp = (const unsigned char*)FPDFBitmap_GetBuffer(rb);
                    if (rp) {
                        for (int y = 0; y < rh && !rbHasAlpha; y += 3)
                            for (int x = 0; x < rw; x += 3)
                                if (rp[y * rs + x * 4 + 3] < 255) { rbHasAlpha = true; break; }
                    }
                }
                if (rbHasAlpha) {
                    bmp = rb;
                    rbUsed = true;
                    ++imgsMasked;
                } else {
                    if (rb) FPDFBitmap_Destroy(rb);
                    bmp = FPDFImageObj_GetBitmap(obj);
                    fromNative = (bmp != nullptr);
                    if (fromNative) ++imgsNative; else ++imgsFallback;
                    if (!bmp) bmp = FPDFImageObj_GetRenderedBitmap(doc, page, obj);
                }
            }
            if (!bmp) continue;
            int bw = FPDFBitmap_GetWidth(bmp), bh = FPDFBitmap_GetHeight(bmp);
            if (bw > 0 && bh > 0) {
                const int fmt = FPDFBitmap_GetFormat(bmp);
                // PDFium tra BGRA voi alpha NHAN SAN. Doc bang Format_ARGB32 (alpha roi)
                // se lam sai mau moi pixel co alpha < 255 -> vien lom dom, "nhieu mau".
                QImage::Format qfmt = QImage::Format_ARGB32_Premultiplied;
                if (fmt == FPDFBitmap_BGR)        qfmt = QImage::Format_BGR888;
                else if (fmt == FPDFBitmap_BGRx)  qfmt = QImage::Format_RGB32;
                else if (fmt == FPDFBitmap_Gray)  qfmt = QImage::Format_Grayscale8;
                QImage view((const uchar*)FPDFBitmap_GetBuffer(bmp), bw, bh,
                            FPDFBitmap_GetStride(bmp), qfmt);
                TextTile tile;
                tile.img = view.convertToFormat(QImage::Format_ARGB32);
                {   // DO: ghi 2 anh dau tien co mat na ra file de doi chieu mau
                    static int _nd = 0;
                    static const QByteArray _dd = qgetenv("TORREADER_FBDUMP");
                    if (rbUsed && _nd < 2 && !_dd.isEmpty()) {
                        tile.img.save(QString::fromUtf8(_dd) + "/img_masked_"
                                      + QString::number(_nd) + ".png");
                        qDebug().noquote() << "[imgdump] mat na #" << _nd
                                           << "size=" << bw << "x" << bh
                                           << "fmtPdfium=" << fmt;
                        ++_nd;
                    }
                }
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
        curStroke = hasStroke;

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
                QVector<QPointF> fillTris;
                buildTris(subpaths, (fillMode == FPDF_FILLMODE_ALTERNATE), fillTris);
                if (!curClipTris || curClipTris->isEmpty()) {
                    for (int ti = 0; ti + 2 < fillTris.size(); ti += 3)
                        pushTri(fillTris[ti], fillTris[ti + 1], fillTris[ti + 2],
                                fillR, fillG, fillB, fillA);
                } else {
                    for (int ti = 0; ti + 2 < fillTris.size(); ti += 3) {
                        const double fl = qMin(fillTris[ti].x(), qMin(fillTris[ti+1].x(), fillTris[ti+2].x()));
                        const double fr = qMax(fillTris[ti].x(), qMax(fillTris[ti+1].x(), fillTris[ti+2].x()));
                        const double ft = qMin(fillTris[ti].y(), qMin(fillTris[ti+1].y(), fillTris[ti+2].y()));
                        const double fb = qMax(fillTris[ti].y(), qMax(fillTris[ti+1].y(), fillTris[ti+2].y()));
                        QVector<QVector<QPointF>> polys;
                        polys.append(QVector<QPointF>{fillTris[ti], fillTris[ti + 1], fillTris[ti + 2]});
                        for (const QVector<QPointF>& ctris : *curClipTris) {
                            QVector<QVector<QPointF>> next;
                            for (const QVector<QPointF>& p : polys) {
                                for (int cj = 0; cj + 2 < ctris.size(); cj += 3) {
                                    const QPointF* t = ctris.constData() + cj;
                                    const double cl = qMin(t[0].x(), qMin(t[1].x(), t[2].x()));
                                    const double cr2 = qMax(t[0].x(), qMax(t[1].x(), t[2].x()));
                                    const double ct = qMin(t[0].y(), qMin(t[1].y(), t[2].y()));
                                    const double cb = qMax(t[0].y(), qMax(t[1].y(), t[2].y()));
                                    if (cr2 < fl || cl > fr || cb < ft || ct > fb) continue;
                                    QVector<QPointF> r = clipByTri(p, t);
                                    if (r.size() >= 3) next.append(r);
                                }
                            }
                            polys = next;
                            if (polys.isEmpty()) break;
                            if (polys.size() > 256) { ++dbgFillClipBail; break; }
                        }
                        if (!polys.isEmpty()) ++dbgFillClipped;
                        for (const QVector<QPointF>& p : polys)
                            for (int k = 1; k + 1 < p.size(); ++k)
                                pushTri(p[0], p[k], p[k + 1], fillR, fillG, fillB, fillA);
                    }
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

    // Trang la cua PageCache — khong FPDF_ClosePage o day.
    m_ready = true;
    m_fillOpaqueFloats = m_fillVerts.size();
    m_fillVerts   += tmpFillVertsA;
    m_fillColors  += tmpFillColorsA;
    m_fillDepths  += tmpFillDepthsA;
    m_fillClipIdx += tmpFillClipIdxA;

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
                        << "imgsMasked=" << imgsMasked
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
                          << "clipPoly=" << dbgClipPolyBuilt
                          << "fillClipped=" << dbgFillClipped
                           << "fillClipBail=" << dbgFillClipBail
                           << "segClipped=" << dbgSegClipped
                           << "segRescued=" << dbgSegRescued
                           << "strokeSkipped=" << dbgStrokeSkipped
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
    const int rotN = FPDFPage_GetRotation(page) & 3;
    float cbL = 0.f, cbB = 0.f, cbR = 0.f, cbT = 0.f;
    bool haveBox = FPDFPage_GetCropBox(page, &cbL, &cbB, &cbR, &cbT);
    if (!haveBox) haveBox = FPDFPage_GetMediaBox(page, &cbL, &cbB, &cbR, &cbT);
    if (!haveBox || cbR <= cbL || cbT <= cbB) {
        cbL = 0.f; cbB = 0.f;
        if (rotN & 1) { cbR = float(pageH); cbT = float(pageW); }
        else          { cbR = float(pageW); cbT = float(pageH); }
    }
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
