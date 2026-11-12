// quake_map.hpp — 地震モニターの画面をまるごと描く（地図・震度・揺れの広がり・緊急地震速報・一覧・ボタン）
//
// 使い方（ネイティブも WASM も同じ）:
//   qm::Map m; m.load(basemap_text, font_bytes, n);
//   m.resize(1280, 800); m.fit();
//   m.set_list(list_json);                      // 気象庁の list.json を受け取るたびに
//   std::string f = m.wanted();                 // 本文が要るならファイル名（JS が取ってきて set_detail）
//   m.set_detail(f, detail_json);
//   m.set_eew(eew_json);                        // 緊急地震速報（Wolfx）を受け取るたびに
//   m.tick(dt, unix_seconds); const uint8_t* rgba = m.render();
//   m.hover(x, y); m.click(x, y); m.pan(dx, dy); m.zoom(f, x, y);
//
// 層: base（背景・陸・県境。表示範囲が変わったときだけ）→ info（過去の震源・観測点・パネル。データが変わったときだけ）
//     → frame（毎フレーム: 揺れの広がり・震源の点滅・緊急地震速報・吹き出し）
#pragma once
#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

#include "quake.hpp"
#include "raster.hpp"

namespace qm {

struct P2 { double x = 0, y = 0; };
inline P2 operator+(P2 a, P2 b) { return {a.x + b.x, a.y + b.y}; }
inline P2 operator-(P2 a, P2 b) { return {a.x - b.x, a.y - b.y}; }
inline P2 operator*(P2 a, double s) { return {a.x * s, a.y * s}; }

constexpr double PI = 3.14159265358979323846, D2R = PI / 180.0, R_EARTH = 6371.0;
// メルカトル図法（km）。y は北が負（画面の下向きが正）
inline P2 merc(double lon, double lat) { const double R = 6378.137; return {R * lon * D2R, -R * std::log(std::tan(PI / 4 + lat * D2R / 2))}; }

// 中心から方位 brg（ラジアン）に地表で d km 進んだ点（大円）
inline void destination(double lat, double lon, double d, double brg, double& olat, double& olon) {
    double p1 = lat * D2R, l1 = lon * D2R, dr = d / R_EARTH;
    double p2 = std::asin(std::sin(p1) * std::cos(dr) + std::cos(p1) * std::sin(dr) * std::cos(brg));
    double l2 = l1 + std::atan2(std::sin(brg) * std::sin(dr) * std::cos(p1), std::cos(dr) - std::sin(p1) * std::sin(p2));
    olat = p2 / D2R; olon = l2 / D2R;
}
inline double surface_km(double la1, double lo1, double la2, double lo2) {
    double p1 = la1 * D2R, p2 = la2 * D2R, dp = p2 - p1, dl = (lo2 - lo1) * D2R;
    double a = std::sin(dp / 2) * std::sin(dp / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 2 * R_EARTH * std::asin(std::min(1.0, std::sqrt(a)));
}

// 揺れの速さ（一定と仮定した近似）。P 波 7 km/s、S 波 4 km/s
constexpr double VP = 7.0, VS = 4.0;

struct Poly { std::string name; bool japan = false; std::vector<std::vector<P2>> rings; P2 lo{1e18, 1e18}, hi{-1e18, -1e18}; };

struct Button { float x0, y0, x1, y1; int id; };
enum { BTN_REPLAY = 1, BTN_FIT = 2, BTN_TEST = 3, ROW0 = 100 };

struct Map {
    std::vector<Poly> polys;
    Font font;
    int W = 0, H = 0;
    double ui = 1;                    // 描画 px と画面の px の比（文字・線の太さに掛ける）
    P2 center{0, 0};
    double scale = 1;                 // 1 km あたりの px
    double wall = 0;                  // いまの時刻（UNIX 秒）
    Canvas base, info, frame, glow;
    std::vector<uint8_t> rgba;
    bool dirty_base = true, dirty_info = true;

    std::vector<Quake> quakes;        // 新しい順
    int sel = -1;                     // 選んでいる地震（quakes の添字）
    std::string sel_eid;
    double replay_start = -1;         // 再生を始めた時刻（-1 = 再生していない）
    bool first_list = true;
    double list_time = 0;             // list.json を最後に受け取った時刻
    int eew_conn = 0;                 // 緊急地震速報の受信: 0 = 未接続、1 = 接続中、2 = 取れていない
    Eew eew;                          // いちばん新しい緊急地震速報
    double eew_recv = 0;              // それを受け取った時刻
    bool eew_test = false;            // 表示テスト（本物ではない）
    double hover_x = -1, hover_y = -1;
    std::vector<Button> buttons;
    int list_rows = 0;

    // ---- 読み込み ----
    bool load(const std::string& basemap, const unsigned char* font_bytes, size_t font_n) {
        std::istringstream in(basemap);
        std::string ln;
        while (std::getline(in, ln)) {
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
            if (ln.rfind("pref ", 0) == 0) { polys.push_back({}); polys.back().name = ln.substr(5); polys.back().japan = true; }
            else if (ln.rfind("land ", 0) == 0) { polys.push_back({}); polys.back().name = ln.substr(5); }
            else if (ln.rfind("ring ", 0) == 0 && !polys.empty()) {
                std::vector<P2> r;
                const char* p = ln.c_str() + 5;
                char* e;
                while (*p) {
                    double lon = strtod(p, &e); if (e == p) break; p = e; if (*p == ',') ++p;
                    double lat = strtod(p, &e); if (e == p) break; p = e;
                    while (*p == ' ') ++p;
                    P2 q = merc(lon, lat);
                    r.push_back(q);
                    Poly& P = polys.back();
                    P.lo.x = std::min(P.lo.x, q.x); P.lo.y = std::min(P.lo.y, q.y); P.hi.x = std::max(P.hi.x, q.x); P.hi.y = std::max(P.hi.y, q.y);
                }
                if (r.size() >= 3) polys.back().rings.push_back(std::move(r));
            }
        }
        if (font_bytes && font_n) font.load(font_bytes, font_n);
        return !polys.empty();
    }
    void resize(int w, int h) {
        W = w; H = h;
        base.resize(w, h); info.resize(w, h); frame.resize(w, h); glow.resize(w, h);
        rgba.assign((size_t)w * h * 4, 255);
        dirty_base = dirty_info = true;
    }
    // 日本全体（沖縄から北海道まで）が入るように
    void fit() {
        P2 a = merc(127.0, 45.8), b = merc(146.5, 25.8);
        center = (a + b) * 0.5;
        double pad_r = side_panel() ? 300 * ui : 0;   // 右の一覧のぶんを空ける
        scale = std::min((W - pad_r) / (b.x - a.x), (double)H / (b.y - a.y)) * 0.94;
        center.x += pad_r / 2 / scale;
        dirty_base = dirty_info = true;
    }
    void pan(double dx, double dy) { center.x -= dx / scale; center.y -= dy / scale; dirty_base = dirty_info = true; }
    void zoom(double f, double sx, double sy) {
        P2 w = to_world(sx, sy);
        scale = std::clamp(scale * f, 0.05, 50.0);
        P2 w2 = to_world(sx, sy);
        center = center + (w - w2);
        dirty_base = dirty_info = true;
    }
    P2 to_world(double sx, double sy) const { return {center.x + (sx - W / 2.0) / scale, center.y + (sy - H / 2.0) / scale}; }
    V2 to_screen(P2 p) const { return {(float)((p.x - center.x) * scale + W / 2.0), (float)((p.y - center.y) * scale + H / 2.0)}; }
    V2 ll(double lat, double lon) const { return to_screen(merc(lon, lat)); }
    bool side_panel() const { return W / ui >= 760; }

    // ---- データ ----
    void set_list(const std::string& text) {
        std::string top = quakes.empty() ? "" : quakes[0].eid;
        auto nq = parse_list(text, quakes);
        if (nq.empty()) return;
        bool fresh = !first_list && !nq.empty() && nq[0].eid != top;
        quakes.swap(nq);
        list_time = wall;
        // 選んでいた地震を探し直す。新しい地震が来たら、それを選んで再生する（最初に開いたときも最新を再生）
        sel = -1;
        for (size_t i = 0; i < quakes.size(); ++i) if (quakes[i].eid == sel_eid) sel = (int)i;
        if (first_list || fresh || sel < 0) { select(0, true); }
        first_list = false;
        dirty_info = true;
    }
    void select(int i, bool replay) {
        if (i < 0 || i >= (int)quakes.size()) return;
        sel = i; sel_eid = quakes[i].eid;
        replay_start = replay ? wall + 0.6 : -1;
        dirty_info = true;
    }
    // 選んでいる地震の本文がまだなら、そのファイル名
    std::string wanted() const {
        if (sel < 0 || sel >= (int)quakes.size()) return "";
        const Quake& q = quakes[sel];
        return !q.detail && !q.json.empty() ? q.json : "";
    }
    bool set_detail(const std::string& file, const std::string& text) {
        for (auto& q : quakes)
            if (q.json == file) {
                bool ok = parse_detail(text, q);
                if (&q == &quakes[std::max(0, sel)] && replay_start > 0) replay_start = wall + 0.3;   // 観測点が来たので、そこから再生し直す
                dirty_info = true;
                return ok;
            }
        return false;
    }
    void set_eew(const std::string& text) {
        Eew e = parse_eew(text);
        if (!e.valid) return;
        // 古い報（同じ地震の前の番号）は捨てる
        if (eew.valid && e.id == eew.id && e.serial < eew.serial) return;
        eew = e; eew_recv = wall; eew_test = false;
        dirty_info = true;
    }
    // 表示テスト：選んでいる地震の震源で、いま起きたことにした緊急地震速報（本物ではない）
    void test_eew() {
        if (sel < 0 || !quakes[sel].has_hypo) return;
        const Quake& q = quakes[sel];
        Eew e;
        e.valid = true; e.id = "test"; e.title = "緊急地震速報（警報）"; e.place = q.name; e.serial = 1;
        e.origin = wall - 4; e.announced = wall; e.lat = q.lat; e.lon = q.lon; e.depth = std::max(0.0, q.depth); e.mag = q.mag;
        e.maxi = std::max(5, q.maxi); e.warn = true; e.training = true;
        eew = e; eew_recv = wall; eew_test = true;
    }
    bool eew_active() const {
        if (!eew.valid) return false;
        double age = wall - (eew.origin > 0 ? eew.origin : eew_recv);
        return eew.cancel ? wall - eew_recv < 20 : age < 180;
    }

    void tick(double /*dt*/, double now) { wall = now; }

    // ---- 描画 ----
    float px(float v) const { return v * (float)ui; }
    int fpx(float v) const { return std::max(8, (int)std::lround(v * ui)); }

    void draw_base() {
        Rgb top = lin({0.025f, 0.035f, 0.06f}), bot = lin({0.01f, 0.015f, 0.03f});
        for (int y = 0; y < H; ++y) {
            Rgb c = mix(top, bot, (float)y / std::max(1, H - 1));
            for (int x = 0; x < W; ++x) { float* p = &base.px[((size_t)y * W + x) * 3]; p[0] = c.r; p[1] = c.g; p[2] = c.b; }
        }
        // 経緯線（2 度おき）
        for (int lon = 118; lon <= 156; lon += 2) polyline(base, {ll(20, lon), ll(50, lon)}, 1, {0.2f, 0.3f, 0.45f}, 0.10f);
        for (int lat = 20; lat <= 50; lat += 2) {
            std::vector<V2> l;
            for (int lon = 118; lon <= 156; lon += 2) l.push_back(ll(lat, lon));
            polyline(base, l, 1, {0.2f, 0.3f, 0.45f}, 0.10f);
        }
        // 陸：日本の外は暗め、日本は少し明るく。縁を光らせる
        P2 wlo = to_world(-50, -50), whi = to_world(W + 50, H + 50);
        std::fill(glow.px.begin(), glow.px.end(), 0.f);
        for (auto& P : polys) {
            if (P.hi.x < wlo.x || P.lo.x > whi.x || P.hi.y < wlo.y || P.lo.y > whi.y) continue;
            std::vector<std::vector<V2>> rs;
            for (auto& r : P.rings) rs.push_back(screen_ring(r));
            fill_rings(base, rs, P.japan ? Rgb{0.105f, 0.13f, 0.17f} : Rgb{0.065f, 0.075f, 0.095f});
            for (auto& r : rs) {
                if (r.size() < 2) continue;
                auto c = r; c.push_back(r[0]);
                if (P.japan) {
                    polyline(base, c, std::max(0.6f, px(0.7f)), {0.32f, 0.42f, 0.55f}, 0.55f);
                    polyline(glow, c, px(2.0f), {0.25f, 0.5f, 0.8f}, 0.35f, true);
                } else polyline(base, c, std::max(0.6f, px(0.6f)), {0.22f, 0.26f, 0.32f}, 0.5f);
            }
        }
        bloom(base, glow, std::max(2, (int)px(3)), 0.6f);
    }
    // 画面の座標に直し、0.6 px より近い点は飛ばす（遠目では点がとても多いので）
    std::vector<V2> screen_ring(const std::vector<P2>& r) const {
        std::vector<V2> out;
        out.reserve(r.size());
        for (auto& p : r) {
            V2 s = to_screen(p);
            if (!out.empty() && std::fabs(s.x - out.back().x) + std::fabs(s.y - out.back().y) < 0.6f) continue;
            out.push_back(s);
        }
        return out;
    }

    // 選んでいる地震の、再生中の経過秒（再生していなければ -1）
    double replay_t() const { return replay_start > 0 ? wall - replay_start : -1; }
    // 観測点に S 波が着くまでの秒
    static double s_arrival(const Quake& q, const Station& s) {
        double d = surface_km(q.lat, q.lon, s.lat, s.lon), z = std::max(0.0, q.depth);
        return std::sqrt(d * d + z * z) / VS;
    }

    void draw_info() {
        info.px = base.px;
        // 過去の地震（一覧のもの）の震源：大きさは M、色は最大震度、古いほど薄く
        for (int i = (int)quakes.size() - 1; i >= 0; --i) {
            const Quake& q = quakes[i];
            if (!q.has_hypo || i == sel) continue;
            V2 p = ll(q.lat, q.lon);
            float r = px(2.0f + 1.6f * (float)std::max(0.0, q.mag));
            float a = 0.25f + 0.5f * (1.f - (float)i / std::max<size_t>(1, quakes.size()));
            disc(info, p, r, hex(int_color(q.maxi)), a * 0.35f);
            ring(info, p, r, px(1.2f), hex(int_color(q.maxi)), a);
        }
        // 選んでいる地震の観測点（再生中は毎フレーム描くので、ここでは止まっているときだけ）
        if (sel >= 0 && replay_t() < 0) draw_stations(info, quakes[sel], 1e9);
        draw_panels(info);
    }

    // 輪（線だけの円）
    void ring(Canvas& cv, V2 c, float r, float w, Rgb col, float a, bool add = false) {
        std::vector<V2> pts;
        int n = std::clamp((int)(r * 0.8f), 24, 256);
        for (int k = 0; k <= n; ++k) { float t = 2 * (float)PI * k / n; pts.push_back({c.x + r * std::cos(t), c.y + r * std::sin(t)}); }
        polyline(cv, pts, w, col, a, add);
    }
    // 地表の距離 d km の輪（震源のまわり）を画面の点の列に
    std::vector<V2> geo_ring(double lat, double lon, double d) const {
        std::vector<V2> pts;
        const int n = 160;
        for (int k = 0; k <= n; ++k) { double la, lo; destination(lat, lon, d, 2 * PI * k / n, la, lo); pts.push_back(ll(la, lo)); }
        return pts;
    }

    // 観測点：震度の色の点。t（再生の経過秒）までに S 波が着いた点だけ。着いたばかりの点は光る
    void draw_stations(Canvas& cv, const Quake& q, double t) {
        float base_r = std::clamp((float)(scale * 2.2), px(2.6f), px(5.0f));
        for (auto& s : q.st) {
            double ta = q.has_hypo ? s_arrival(q, s) : 0;
            if (ta > t) continue;
            V2 p = ll(s.lat, s.lon);
            if (p.x < -10 || p.y < -10 || p.x > W + 10 || p.y > H + 10) continue;
            float r = base_r * (0.8f + 0.18f * s.ints);
            Rgb c = hex(int_color(s.ints));
            double age = t - ta;
            if (age < 2.0) glow_dot(cv, p, r * (4.f - (float)age), c, 0.9f * (float)(1 - age / 2));
            if (s.ints >= 4) glow_dot(cv, p, r * 3.f, c, 0.35f);
            disc(cv, p, r + px(1.0f), {0.02f, 0.03f, 0.05f}, 0.9f);
            disc(cv, p, r, c);
        }
    }

    // 震源の印（×）と点滅
    void epicenter(Canvas& cv, double lat, double lon, Rgb c, float size) {
        V2 p = ll(lat, lon);
        float s = px(size), pulse = 0.5f + 0.5f * (float)std::sin(wall * 4);
        glow_dot(cv, p, s * (2.2f + pulse), c, 0.8f);
        polyline(cv, {{p.x - s, p.y - s}, {p.x + s, p.y + s}}, px(4.5f), {0.02f, 0.02f, 0.03f}, 0.9f);
        polyline(cv, {{p.x - s, p.y + s}, {p.x + s, p.y - s}}, px(4.5f), {0.02f, 0.02f, 0.03f}, 0.9f);
        polyline(cv, {{p.x - s, p.y - s}, {p.x + s, p.y + s}}, px(2.6f), c);
        polyline(cv, {{p.x - s, p.y + s}, {p.x + s, p.y - s}}, px(2.6f), c);
    }

    // P 波・S 波の輪。t は発生からの秒
    void waves(Canvas& cv, double lat, double lon, double depth, double t, float strength) {
        double z = std::max(0.0, depth);
        auto surf = [&](double v) { double h = v * t; return h > z ? std::sqrt(h * h - z * z) : -1.0; };
        double rp = surf(VP), rs = surf(VS);
        if (rp > 0 && rp < 2500) {
            auto pts = geo_ring(lat, lon, rp);
            polyline(cv, pts, px(6), {0.35f, 0.65f, 1.0f}, 0.12f * strength, true);
            polyline(cv, pts, px(1.6f), {0.55f, 0.8f, 1.0f}, 0.85f * strength);
        }
        if (rs > 0 && rs < 2500) {
            auto pts = geo_ring(lat, lon, rs);
            polyline(cv, pts, px(14), {1.0f, 0.35f, 0.15f}, 0.10f * strength, true);
            polyline(cv, pts, px(5), {1.0f, 0.45f, 0.2f}, 0.35f * strength, true);
            polyline(cv, pts, px(2.2f), {1.0f, 0.62f, 0.35f}, 0.95f * strength);
        }
    }

    // ---- パネル（すべてここで描く）----
    void text(Canvas& cv, float x, float y, const std::string& s, float size, Rgb c) { font.draw(cv, x, y, s, fpx(size), c, {0, 0, 0}, 0); }
    float tw(const std::string& s, float size) { return font.width(s, fpx(size)); }
    void panel(Canvas& cv, float x0, float y0, float x1, float y1) {
        round_rect(cv, x0, y0, x1, y1, px(12), {0.055f, 0.075f, 0.115f}, 0.86f);
        // 細い縁
        round_rect(cv, x0, y0, x1, y0 + 1, 0, {0.3f, 0.4f, 0.55f}, 0.25f);
    }
    void badge(Canvas& cv, float x, float y, float w, float h, int ints) {
        Rgb c = hex(int_color(ints));
        round_rect(cv, x, y, x + w, y + h, px(6), c);
        std::string s = int_label(ints);
        float sz = h / (float)ui * 0.62f;
        if (s.size() > 1 && ints >= 5) sz *= 0.8f;
        bool dark = ints == 1 || ints == 4 || ints == 5;
        text(cv, x + (w - tw(s, sz)) / 2, y + h * 0.5f + px(sz) * 0.36f, s, sz, dark ? Rgb{0.05f, 0.06f, 0.1f} : Rgb{1, 1, 1});
    }
    void button(Canvas& cv, float x, float y, const std::string& s, int id, bool on = false) {
        float w = tw(s, 13) + px(22), h = px(30);
        round_rect(cv, x, y, x + w, y + h, px(8), on ? Rgb{0.35f, 0.69f, 1.0f} : Rgb{0.16f, 0.2f, 0.28f}, on ? 1.f : 0.9f);
        text(cv, x + px(11), y + h / 2 + px(13) * 0.36f, s, 13, on ? Rgb{0.02f, 0.07f, 0.12f} : Rgb{0.9f, 0.93f, 0.97f});
        buttons.push_back({x, y, x + w, y + h, id});
    }
    std::string mag_s(double m) const { if (m < 0) return "M不明"; char b[16]; snprintf(b, sizeof b, "M%.1f", m); return b; }
    std::string depth_s(double d) const { if (d < 0) return "深さ不明"; if (d < 1) return "ごく浅い"; char b[24]; snprintf(b, sizeof b, "深さ %.0f km", d); return b; }

    void draw_panels(Canvas& cv) {
        buttons.clear();
        const Rgb fg{0.9f, 0.93f, 0.97f}, muted{0.55f, 0.6f, 0.68f};
        // 左上：題名・状態・選んでいる地震
        float x0 = px(16), y0 = px(16), wpan = std::min((float)W - px(32), px(340));
        float y = y0 + px(30);
        float hpan = px(sel >= 0 ? 196 : 76);
        panel(cv, x0, y0, x0 + wpan, y0 + hpan);
        text(cv, x0 + px(14), y, "地震モニター", 18, fg);
        y += px(20);
        std::string st = list_time > 0 ? "気象庁の発表を " + jst(list_time, false, true) + " に確認" : "気象庁の発表を読み込み中…";
        st += eew_conn == 1 ? "・緊急地震速報 受信中" : eew_conn == 2 ? "・緊急地震速報 接続できず" : "";
        text(cv, x0 + px(14), y, st, 11.5f, muted);
        if (sel >= 0) {
            const Quake& q = quakes[sel];
            y += px(16);
            float bw = px(64), bh = px(64);
            badge(cv, x0 + px(14), y, bw, bh, q.maxi);
            float tx = x0 + px(14) + bw + px(12);
            text(cv, tx, y + px(14), jst(q.origin) + " ごろ", 12.5f, muted);
            text(cv, tx, y + px(36), q.name.empty() ? "（震源は調査中）" : q.name, 17, fg);
            text(cv, tx, y + px(57), mag_s(q.mag) + "・" + depth_s(q.depth), 13, fg);
            y += bh + px(14);
            text(cv, x0 + px(14), y + px(2), "最大震度 " + std::string(int_label(q.maxi)) + "・観測点 " + std::to_string(q.st.size()) + (q.detail || q.json.empty() ? "" : "（読み込み中）"), 11.5f, muted);
            y += px(12);
            float bx = x0 + px(14);
            button(cv, bx, y, "揺れの広がりを再生", BTN_REPLAY, replay_start > 0);
            bx = buttons.back().x1 + px(8);
            button(cv, bx, y, "全体", BTN_FIT);
            if (!side_panel()) { bx = buttons.back().x1 + px(8); button(cv, bx, y, "表示テスト", BTN_TEST); }
        }
        // 一覧：広い画面は右、せまい画面は下
        list_rows = 0;
        float lx0, ly0, lx1, ly1;
        float rowh = px(40);
        if (side_panel()) { lx1 = W - px(16); lx0 = lx1 - px(290); ly0 = px(16); ly1 = H - px(56); }
        else {
            lx0 = px(16); lx1 = W - px(16); ly1 = H - px(56);
            int rows = std::clamp((int)((ly1 - (y0 + hpan + px(12)) - px(42)) / rowh), 1, 4);
            ly0 = ly1 - px(42) - rows * rowh;
        }
        if (ly1 - ly0 > rowh + px(40) && !quakes.empty()) {
            panel(cv, lx0, ly0, lx1, ly1);
            text(cv, lx0 + px(14), ly0 + px(24), "最近の地震（気象庁）", 13, fg);
            float ry = ly0 + px(36);
            for (size_t i = 0; i < quakes.size() && ry + rowh <= ly1 - px(6); ++i, ry += rowh) {
                const Quake& q = quakes[i];
                if ((int)i == sel) round_rect(cv, lx0 + px(6), ry, lx1 - px(6), ry + rowh - px(2), px(8), {0.2f, 0.32f, 0.5f}, 0.55f);
                badge(cv, lx0 + px(12), ry + px(5), px(30), px(30), q.maxi);
                float tx = lx0 + px(52);
                text(cv, tx, ry + px(16), jst(q.origin) + "　" + mag_s(q.mag), 11, muted);
                std::string nm = q.name.empty() ? q.title : q.name;
                if (q.cancelled) nm += "（取消）";
                // 長い名前は欄に収まるように切る
                float maxw = lx1 - px(12) - tx;
                while (!nm.empty() && tw(nm, 13.5f) > maxw) { size_t k = nm.size() - 1; while (k > 0 && ((unsigned char)nm[k] & 0xC0) == 0x80) --k; nm.erase(k); }
                text(cv, tx, ry + px(33), nm, 13.5f, fg);
                buttons.push_back({lx0 + px(6), ry, lx1 - px(6), ry + rowh, ROW0 + (int)i});
                ++list_rows;
            }
        }
        // 下：震度の色の見本と出典
        float by = H - px(16), bx = px(16);
        const float lh = px(24);
        bool wide = side_panel();
        float step = std::min(31.f, ((float)(W / ui) - 32 - 52) / 9);   // バッジの間隔（画面の幅に合わせる）
        float pw = 42 + 9 * step + (wide ? 116 : 4);
        panel(cv, bx, by - lh - px(10), bx + px(pw), by);
        text(cv, bx + px(10), by - px(12), "震度", 11, muted);
        for (int i = 1; i <= 9; ++i) badge(cv, bx + px(42 + (i - 1) * step), by - lh - px(4), px(step - 3), lh - px(2), i);
        if (wide) button(cv, bx + px(42 + 9 * step + 6), by - lh - px(8), "表示テスト", BTN_TEST);
        std::string src = "震度・震源：気象庁　緊急地震速報：Wolfx（気象庁の電文）　地図：Natural Earth　文字：Noto Sans JP";
        if (side_panel()) text(cv, W - px(16) - tw(src, 10), H - px(30), src, 10, {0.45f, 0.5f, 0.58f});
    }

    // 緊急地震速報の帯（毎フレーム。秒読みがあるので）
    void draw_eew(Canvas& cv) {
        if (!eew_active()) return;
        const Eew& e = eew;
        bool cancel = e.cancel;
        Rgb bg = cancel ? Rgb{0.3f, 0.32f, 0.36f} : e.warn ? Rgb{0.78f, 0.06f, 0.1f} : Rgb{0.85f, 0.45f, 0.05f};
        float w = std::min((float)W - px(32), px(620)), x0 = (W - w) / 2, y0 = side_panel() ? px(16) : px(220);
        float h = px(e.warn_areas.empty() ? 92 : 116);
        float blink = e.warn && !cancel ? 0.85f + 0.15f * (float)std::sin(wall * 6) : 1.f;
        round_rect(cv, x0, y0, x0 + w, y0 + h, px(12), bg, 0.92f * blink);
        std::string head = e.title.empty() ? "緊急地震速報" : e.title;
        if (cancel) head += "（取消）";
        if (e.serial > 0) head += "　第" + std::to_string(e.serial) + "報" + (e.final_ ? "（最終）" : "");
        if (eew_test) head = "表示テスト（本物の緊急地震速報ではありません）";
        else if (e.training) head += "（訓練）";
        text(cv, x0 + px(16), y0 + px(28), head, 17, {1, 1, 1});
        std::string l2 = (e.place.empty() ? std::string("震源不明") : e.place) + "　" + mag_s(e.mag) + "　" + depth_s(e.depth) + "　最大予測震度 " + int_label(e.maxi);
        text(cv, x0 + px(16), y0 + px(54), l2, 14, {1, 1, 1});
        double t = wall - e.origin;
        char b[96];
        snprintf(b, sizeof b, "発生から %.0f 秒（%s 発生）", std::max(0.0, t), jst(e.origin, false, true).c_str());
        text(cv, x0 + px(16), y0 + px(78), b, 12, {1, 0.92f, 0.85f});
        if (!e.warn_areas.empty()) {
            std::string a = "強い揺れに警戒：";
            for (size_t i = 0; i < e.warn_areas.size() && i < 6; ++i) a += (i ? "、" : "") + e.warn_areas[i];
            if (e.warn_areas.size() > 6) a += " ほか";
            text(cv, x0 + px(16), y0 + px(102), a, 12, {1, 1, 1});
        }
    }

    // 吹き出し（マウスの下の観測点）
    void draw_tip(Canvas& cv) {
        if (hover_x < 0 || sel < 0) return;
        const Quake& q = quakes[sel];
        double t = replay_t() >= 0 ? replay_t() : 1e9;
        const Station* best = nullptr; double bd = px(10);
        for (auto& s : q.st) {
            if (q.has_hypo && s_arrival(q, s) > t) continue;
            V2 p = ll(s.lat, s.lon);
            double d = std::hypot(p.x - hover_x, p.y - hover_y);
            if (d < bd) { bd = d; best = &s; }
        }
        if (!best) return;
        std::string l1 = best->name, l2 = std::string("震度 ") + int_label(best->ints);
        if (q.has_hypo) { char b[48]; snprintf(b, sizeof b, "・震源から %.0f km", surface_km(q.lat, q.lon, best->lat, best->lon)); l2 += b; }
        float w = std::max(tw(l1, 13), tw(l2, 11.5f)) + px(20), h = px(44);
        float x = std::min((float)hover_x + px(14), W - w - px(4)), y = std::min((float)hover_y + px(14), H - h - px(4));
        round_rect(cv, x, y, x + w, y + h, px(8), {0.06f, 0.08f, 0.12f}, 0.92f);
        text(cv, x + px(10), y + px(18), l1, 13, {1, 1, 1});
        text(cv, x + px(10), y + px(36), l2, 11.5f, hex(int_color(best->ints)));
    }

    const uint8_t* render() {
        if (dirty_base) { draw_base(); dirty_base = false; dirty_info = true; }
        if (dirty_info) { draw_info(); dirty_info = false; }
        frame.px = info.px;
        // 選んでいる地震：再生中なら揺れの輪と、着いた観測点
        if (sel >= 0) {
            const Quake& q = quakes[sel];
            double t = replay_t();
            if (t >= 0) {
                draw_stations(frame, q, t);
                if (q.has_hypo) waves(frame, q.lat, q.lon, q.depth, t, (float)std::clamp(1.0 - (t - 60) / 60, 0.0, 1.0));
                double last = 0;
                for (auto& s : q.st) last = std::max(last, s_arrival(q, s));
                if (t > std::max(last + 3, 20.0)) { replay_start = -1; dirty_info = true; }   // 全部着いたら止まった絵へ
            }
            if (q.has_hypo) epicenter(frame, q.lat, q.lon, {1.0f, 0.25f, 0.25f}, 7);
        }
        // 緊急地震速報：震源と、発生からの P 波・S 波
        if (eew_active() && !eew.cancel && eew.lat != 0) {
            double t = wall - eew.origin;
            waves(frame, eew.lat, eew.lon, eew.depth, t, 1.f);
            epicenter(frame, eew.lat, eew.lon, {1.0f, 0.85f, 0.2f}, 9);
        }
        draw_eew(frame);
        draw_tip(frame);
        frame.to_rgba(rgba.data());
        return rgba.data();
    }

    // ---- 操作 ----
    void hover(double x, double y) { hover_x = x; hover_y = y; }
    // 押されたものがあれば 1（地図のドラッグにしない）
    int hit(double x, double y) const {
        for (auto& b : buttons) if (x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1) return b.id;
        return 0;
    }
    int click(double x, double y) {
        int id = hit(x, y);
        if (id == BTN_REPLAY) select(sel, true);
        else if (id == BTN_FIT) fit();
        else if (id == BTN_TEST) test_eew();
        else if (id >= ROW0) select(id - ROW0, true);
        return id != 0;
    }
};

}  // namespace qm
