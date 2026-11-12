// WASM の入口。ページは JSON（気象庁・Wolfx）を取ってきて渡すだけで、画面はすべてここ（quake_map.hpp）で描く。
//   build: sh wasm/build.sh → docs/qm.js, docs/qm.wasm
#include "quake_map.hpp"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include <emscripten/emscripten.h>
#include <string>

static qm::Map g;
static std::string g_str;

extern "C" {
EMSCRIPTEN_KEEPALIVE int qm_init(const char* basemap, const unsigned char* font, int font_n) { return g.load(basemap, font, (size_t)font_n) ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE void qm_resize(int w, int h, double ui) { g.ui = ui; g.resize(w, h); }
EMSCRIPTEN_KEEPALIVE void qm_fit() { g.fit(); }
EMSCRIPTEN_KEEPALIVE void qm_pan(double dx, double dy) { g.pan(dx, dy); }
EMSCRIPTEN_KEEPALIVE void qm_zoom(double f, double x, double y) { g.zoom(f, x, y); }
EMSCRIPTEN_KEEPALIVE void qm_tick(double dt, double now) { g.tick(dt, now); }
EMSCRIPTEN_KEEPALIVE const uint8_t* qm_render() { return g.render(); }
// 気象庁の list.json
EMSCRIPTEN_KEEPALIVE void qm_list(const char* json) { g.set_list(json); }
// 本文が要るならそのファイル名（https://www.jma.go.jp/bosai/quake/data/<名前>）。要らなければ ""
EMSCRIPTEN_KEEPALIVE const char* qm_wanted() { return (g_str = g.wanted()).c_str(); }
EMSCRIPTEN_KEEPALIVE int qm_detail(const char* file, const char* json) { return g.set_detail(file, json) ? 1 : 0; }
// 緊急地震速報（Wolfx の JSON）と、その受信の状態（0 = 未接続、1 = 接続中、2 = 接続できず）
EMSCRIPTEN_KEEPALIVE void qm_eew(const char* json) { g.set_eew(json); }
EMSCRIPTEN_KEEPALIVE void qm_eew_conn(int s) { if (g.eew_conn != s) { g.eew_conn = s; g.dirty_info = true; } }
// 操作。qm_hit はその点にボタンや一覧があるか（あればドラッグを始めない）
EMSCRIPTEN_KEEPALIVE void qm_hover(double x, double y) { g.hover(x, y); }
EMSCRIPTEN_KEEPALIVE int qm_hit(double x, double y) { return g.hit(x, y); }
EMSCRIPTEN_KEEPALIVE int qm_click(double x, double y) { return g.click(x, y); }
}
