// raster.hpp — 小さなソフトウェア描画（ヘッダーオンリー、依存は stb_truetype だけ）。metro_map_cpp のものに多角形の塗りと角丸の四角を足した
//
//   Canvas  : RGBA8 の画面。線・円・文字を「覆う割合（0..1）」で塗り重ねる（なめらか＝アンチエイリアス）
//   polyline: 太い折れ線。1 本ぶんの覆い方を先に最大値で集めてから 1 回だけ塗るので、継ぎ目が濃くならない
//   bloom   : 光る効果。明るいもの（線・列車）を別の層に描き、縮めてぼかして足す
//   Font    : stb_truetype で日本語を描く。字の周りに暗い縁取り（読みやすさのため）
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "stb_truetype.h"

namespace qm {

struct Rgb { float r, g, b; };
inline Rgb hex(uint32_t c) { return {((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f}; }
inline Rgb mix(Rgb a, Rgb b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }

struct V2 { float x, y; };

// 浮動小数の RGB 画面（合成は float、最後に RGBA8 にする）
struct Canvas {
    int w = 0, h = 0;
    std::vector<float> px;   // r, g, b
    void resize(int W, int H) { w = W; h = H; px.assign((size_t)W * H * 3, 0.f); }
    void clear(Rgb c) { for (size_t i = 0; i < px.size(); i += 3) { px[i] = c.r; px[i + 1] = c.g; px[i + 2] = c.b; } }
    void blend(int x, int y, Rgb c, float a) {
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
        float* p = &px[((size_t)y * w + x) * 3];
        p[0] += (c.r - p[0]) * a; p[1] += (c.g - p[1]) * a; p[2] += (c.b - p[2]) * a;
    }
    void add(int x, int y, Rgb c, float a) {
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
        float* p = &px[((size_t)y * w + x) * 3];
        p[0] += c.r * a; p[1] += c.g * a; p[2] += c.b * a;
    }
    void to_rgba(uint8_t* out) const {
        for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
            for (int k = 0; k < 3; ++k) {
                float v = px[i * 3 + k];
                v = v < 0 ? 0 : v > 1 ? 1 : v;
                out[i * 4 + k] = (uint8_t)(std::sqrt(v) * 255.f + 0.5f);   // 線形→おおよそ sRGB
            }
            out[i * 4 + 3] = 255;
        }
    }
};

// 色は sRGB で受け取り、内部は線形（二乗）で合成する（光を足したときに自然に見える）
inline Rgb lin(Rgb c) { return {c.r * c.r, c.g * c.g, c.b * c.b}; }

// 点 p と線分 ab の距離
inline float seg_dist(V2 p, V2 a, V2 b) {
    float dx = b.x - a.x, dy = b.y - a.y, L2 = dx * dx + dy * dy;
    float t = L2 > 0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / L2, 0.f, 1.f) : 0.f;
    float ex = a.x + dx * t - p.x, ey = a.y + dy * t - p.y;
    return std::sqrt(ex * ex + ey * ey);
}

// 太い折れ線（端と継ぎ目は丸）。width は全幅（px）。add=true なら足し算（光る層用）
inline void polyline(Canvas& cv, const std::vector<V2>& pts, float width, Rgb color, float alpha = 1.f, bool add = false) {
    if (pts.size() < 2) return;
    const float hw = width * 0.5f;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (auto& p : pts) { x0 = std::min(x0, p.x); y0 = std::min(y0, p.y); x1 = std::max(x1, p.x); y1 = std::max(y1, p.y); }
    int bx0 = std::max(0, (int)std::floor(x0 - hw - 1)), by0 = std::max(0, (int)std::floor(y0 - hw - 1));
    int bx1 = std::min(cv.w - 1, (int)std::ceil(x1 + hw + 1)), by1 = std::min(cv.h - 1, (int)std::ceil(y1 + hw + 1));
    if (bx0 > bx1 || by0 > by1) return;
    const int bw = bx1 - bx0 + 1, bh = by1 - by0 + 1;
    static std::vector<float> mask;
    mask.assign((size_t)bw * bh, 0.f);
    for (size_t i = 1; i < pts.size(); ++i) {
        V2 a = pts[i - 1], b = pts[i];
        int sx0 = std::max(bx0, (int)std::floor(std::min(a.x, b.x) - hw - 1)), sx1 = std::min(bx1, (int)std::ceil(std::max(a.x, b.x) + hw + 1));
        int sy0 = std::max(by0, (int)std::floor(std::min(a.y, b.y) - hw - 1)), sy1 = std::min(by1, (int)std::ceil(std::max(a.y, b.y) + hw + 1));
        for (int y = sy0; y <= sy1; ++y)
            for (int x = sx0; x <= sx1; ++x) {
                float d = seg_dist({x + 0.5f, y + 0.5f}, a, b);
                float cov = std::clamp(hw + 0.5f - d, 0.f, 1.f);
                float& m = mask[(size_t)(y - by0) * bw + (x - bx0)];
                if (cov > m) m = cov;
            }
    }
    Rgb c = lin(color);
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x) {
            float m = mask[(size_t)y * bw + x];
            if (m <= 0) continue;
            if (add) cv.add(bx0 + x, by0 + y, c, m * alpha);
            else cv.blend(bx0 + x, by0 + y, c, m * alpha);
        }
}

// 塗りつぶしの円（r は半径 px）
inline void disc(Canvas& cv, V2 c, float r, Rgb color, float alpha = 1.f, bool add = false) {
    Rgb lc = lin(color);
    for (int y = (int)std::floor(c.y - r - 1); y <= (int)std::ceil(c.y + r + 1); ++y)
        for (int x = (int)std::floor(c.x - r - 1); x <= (int)std::ceil(c.x + r + 1); ++x) {
            float d = std::hypot(x + 0.5f - c.x, y + 0.5f - c.y);
            float cov = std::clamp(r + 0.5f - d, 0.f, 1.f);
            if (add) cv.add(x, y, lc, cov * alpha); else cv.blend(x, y, lc, cov * alpha);
        }
}

// ぼかした光（中心が明るく外へなめらかに消える）を足す。列車の光に使う
inline void glow_dot(Canvas& cv, V2 c, float r, Rgb color, float alpha) {
    Rgb lc = lin(color);
    for (int y = (int)std::floor(c.y - r); y <= (int)std::ceil(c.y + r); ++y)
        for (int x = (int)std::floor(c.x - r); x <= (int)std::ceil(c.x + r); ++x) {
            float d = std::hypot(x + 0.5f - c.x, y + 0.5f - c.y) / r;
            if (d >= 1) continue;
            float k = (1 - d) * (1 - d);
            cv.add(x, y, lc, k * alpha);
        }
}

// 箱型ぼかしを 3 回（ガウスに近い）。src を 1/4 に縮めてぼかし、dst に strength 倍で足す
inline void bloom(Canvas& dst, const Canvas& src, int radius, float strength) {
    const int S = 4, w = (src.w + S - 1) / S, h = (src.h + S - 1) / S;
    std::vector<float> a((size_t)w * h * 3, 0.f), b(a.size());
    for (int y = 0; y < src.h; ++y)
        for (int x = 0; x < src.w; ++x)
            for (int k = 0; k < 3; ++k) a[((size_t)(y / S) * w + x / S) * 3 + k] += src.px[((size_t)y * src.w + x) * 3 + k] / (S * S);
    auto box = [&](std::vector<float>& in, std::vector<float>& out, bool horiz) {
        int n = horiz ? w : h, m = horiz ? h : w;
        for (int j = 0; j < m; ++j) {
            float acc[3] = {0, 0, 0};
            auto idx = [&](int i) { int x = horiz ? i : j, y = horiz ? j : i; return ((size_t)y * w + x) * 3; };
            for (int i = -radius; i <= radius; ++i) { int c = std::clamp(i, 0, n - 1); for (int k = 0; k < 3; ++k) acc[k] += in[idx(c) + k]; }
            for (int i = 0; i < n; ++i) {
                for (int k = 0; k < 3; ++k) out[idx(i) + k] = acc[k] / (2 * radius + 1);
                int add = std::clamp(i + radius + 1, 0, n - 1), sub = std::clamp(i - radius, 0, n - 1);
                for (int k = 0; k < 3; ++k) acc[k] += in[idx(add) + k] - in[idx(sub) + k];
            }
        }
    };
    for (int pass = 0; pass < 3; ++pass) { box(a, b, true); box(b, a, false); }
    for (int y = 0; y < dst.h; ++y)
        for (int x = 0; x < dst.w; ++x) {
            // 縮めた画像を双線形で引き伸ばす
            float fx = (x + 0.5f) / S - 0.5f, fy = (y + 0.5f) / S - 0.5f;
            int x0 = std::clamp((int)std::floor(fx), 0, w - 1), y0 = std::clamp((int)std::floor(fy), 0, h - 1);
            int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
            float tx = std::clamp(fx - x0, 0.f, 1.f), ty = std::clamp(fy - y0, 0.f, 1.f);
            float* p = &dst.px[((size_t)y * dst.w + x) * 3];
            for (int k = 0; k < 3; ++k) {
                float v = (a[((size_t)y0 * w + x0) * 3 + k] * (1 - tx) + a[((size_t)y0 * w + x1) * 3 + k] * tx) * (1 - ty) +
                          (a[((size_t)y1 * w + x0) * 3 + k] * (1 - tx) + a[((size_t)y1 * w + x1) * 3 + k] * tx) * ty;
                p[k] += v * strength;
            }
        }
}

// 多角形の塗り（偶奇規則）。縦に 4 段サンプルし、横は端の割合まで数えて、縁をなめらかにする
// rings は輪の集まり（穴も同じ規則で抜ける）。辺を上端の y で並べ、走査線ごとに「いま掛かっている辺」だけを見る
inline void fill_rings(Canvas& cv, const std::vector<std::vector<V2>>& rings, Rgb color, float alpha = 1.f) {
    struct E { float x0, y0, x1, y1; };
    std::vector<E> es;
    float ymin = 1e30f, ymax = -1e30f, xmin = 1e30f, xmax = -1e30f;
    for (auto& r : rings) {
        size_t n = r.size();
        for (size_t i = 0; i < n; ++i) {
            V2 a = r[i], b = r[(i + 1) % n];
            if (a.y == b.y) continue;
            if (a.y > b.y) std::swap(a, b);
            es.push_back({a.x, a.y, b.x, b.y});
            ymin = std::min(ymin, a.y); ymax = std::max(ymax, b.y);
            xmin = std::min({xmin, a.x, b.x}); xmax = std::max({xmax, a.x, b.x});
        }
    }
    if (es.empty() || ymax < 0 || ymin >= cv.h || xmax < 0 || xmin >= cv.w) return;
    std::sort(es.begin(), es.end(), [](const E& a, const E& b) { return a.y0 < b.y0; });
    const int SS = 4;
    const Rgb lc = lin(color);
    int py0 = std::max(0, (int)std::floor(ymin)), py1 = std::min(cv.h - 1, (int)std::ceil(ymax));
    int cx0 = std::max(0, (int)std::floor(xmin)), cx1 = std::min(cv.w - 1, (int)std::ceil(xmax));
    std::vector<float> cov(cv.w + 1, 0.f);
    std::vector<int> active;
    std::vector<float> xs;
    size_t next = 0;
    auto span = [&](float xa, float xb, float wgt) {
        xa = std::clamp(xa, 0.f, (float)cv.w); xb = std::clamp(xb, 0.f, (float)cv.w);
        if (xb <= xa) return;
        int ia = (int)xa, ib = (int)xb;
        if (ia == ib) { cov[ia] += (xb - xa) * wgt; return; }
        cov[ia] += (ia + 1 - xa) * wgt;
        for (int i = ia + 1; i < ib; ++i) cov[i] += wgt;
        if (ib < cv.w) cov[ib] += (xb - ib) * wgt;
    };
    for (int py = py0; py <= py1; ++py) {
        std::fill(cov.begin() + cx0, cov.begin() + cx1 + 1, 0.f);
        for (int s = 0; s < SS; ++s) {
            float y = py + (s + 0.5f) / SS;
            while (next < es.size() && es[next].y0 <= y) active.push_back((int)next++);
            xs.clear();
            for (size_t k = 0; k < active.size();) {
                const E& e = es[active[k]];
                if (e.y1 <= y) { active[k] = active.back(); active.pop_back(); continue; }
                if (e.y0 <= y) xs.push_back(e.x0 + (y - e.y0) * (e.x1 - e.x0) / (e.y1 - e.y0));
                ++k;
            }
            std::sort(xs.begin(), xs.end());
            for (size_t k = 0; k + 1 < xs.size(); k += 2) span(xs[k], xs[k + 1], 1.f / SS);
        }
        for (int x = cx0; x <= cx1; ++x) if (cov[x] > 0) cv.blend(x, py, lc, std::min(1.f, cov[x]) * alpha);
    }
}

// 角を丸めた四角の塗り（パネルの下地）
inline void round_rect(Canvas& cv, float x0, float y0, float x1, float y1, float r, Rgb color, float alpha = 1.f) {
    Rgb lc = lin(color);
    for (int y = std::max(0, (int)std::floor(y0)); y < std::min(cv.h, (int)std::ceil(y1)); ++y)
        for (int x = std::max(0, (int)std::floor(x0)); x < std::min(cv.w, (int)std::ceil(x1)); ++x) {
            float px = x + 0.5f, py = y + 0.5f;
            float qx = std::max({x0 + r - px, px - (x1 - r), 0.f}), qy = std::max({y0 + r - py, py - (y1 - r), 0.f});
            float d = std::sqrt(qx * qx + qy * qy) - r;   // 外側で正
            float c = std::clamp(0.5f - d, 0.f, 1.f);
            c *= std::clamp(std::min({px - x0, x1 - px, py - y0, y1 - py}) + 0.5f, 0.f, 1.f);
            cv.blend(x, y, lc, c * alpha);
        }
}

// ---- 文字 ----
inline uint32_t next_cp(const std::string& s, size_t& i) {
    unsigned char c = s[i];
    uint32_t cp; int n;
    if (c < 0x80) { cp = c; n = 1; } else if ((c >> 5) == 6) { cp = c & 31; n = 2; } else if ((c >> 4) == 14) { cp = c & 15; n = 3; } else { cp = c & 7; n = 4; }
    for (int k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (s[i + k] & 63);
    i += n;
    return cp;
}

struct Font {
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
    bool ok = false;
    struct Glyph { int w = 0, h = 0, xoff = 0, yoff = 0; float adv = 0; std::vector<unsigned char> bmp; };
    std::map<std::pair<uint32_t, int>, Glyph> cache;

    bool load(const unsigned char* p, size_t n) {
        data.assign(p, p + n);
        ok = stbtt_InitFont(&info, data.data(), stbtt_GetFontOffsetForIndex(data.data(), 0)) != 0;
        return ok;
    }
    const Glyph& glyph(uint32_t cp, int px) {
        auto k = std::make_pair(cp, px);
        auto it = cache.find(k);
        if (it != cache.end()) return it->second;
        Glyph g;
        float sc = stbtt_ScaleForPixelHeight(&info, (float)px);
        int adv, lsb; stbtt_GetCodepointHMetrics(&info, (int)cp, &adv, &lsb);
        g.adv = adv * sc;
        unsigned char* b = stbtt_GetCodepointBitmap(&info, sc, sc, (int)cp, &g.w, &g.h, &g.xoff, &g.yoff);
        if (b) { g.bmp.assign(b, b + (size_t)g.w * g.h); stbtt_FreeBitmap(b, nullptr); }
        return cache[k] = g;
    }
    float width(const std::string& s, int px) {
        float w = 0;
        for (size_t i = 0; i < s.size();) w += glyph(next_cp(s, i), px).adv;
        return w;
    }
    // (x, y) は文字列の左端・ベースライン。halo > 0 なら暗い縁取りを先に描く
    void draw(Canvas& cv, float x, float y, const std::string& s, int px, Rgb color, Rgb halo_col, float halo = 2.f) {
        Rgb lc = lin(color), lh = lin(halo_col);
        for (int pass = halo > 0 ? 0 : 1; pass < 2; ++pass) {
            float pen = x;
            for (size_t i = 0; i < s.size();) {
                const Glyph& g = glyph(next_cp(s, i), px);
                int gx = (int)std::lround(pen) + g.xoff, gy = (int)std::lround(y) + g.yoff;
                if (pass == 0) {
                    int r = (int)std::ceil(halo);
                    for (int yy = -r; yy < g.h + r; ++yy)
                        for (int xx = -r; xx < g.w + r; ++xx) {
                            float m = 0;
                            for (int dy = -r; dy <= r; ++dy)
                                for (int dx = -r; dx <= r; ++dx) {
                                    if (dx * dx + dy * dy > halo * halo + 0.5f) continue;
                                    int sx = xx + dx, sy = yy + dy;
                                    if (sx < 0 || sy < 0 || sx >= g.w || sy >= g.h) continue;
                                    m = std::max(m, g.bmp[(size_t)sy * g.w + sx] / 255.f);
                                }
                            cv.blend(gx + xx, gy + yy, lh, m * 0.85f);
                        }
                } else {
                    for (int yy = 0; yy < g.h; ++yy)
                        for (int xx = 0; xx < g.w; ++xx) cv.blend(gx + xx, gy + yy, lc, g.bmp[(size_t)yy * g.w + xx] / 255.f);
                }
                pen += g.adv;
            }
        }
    }
};

}  // namespace qm
