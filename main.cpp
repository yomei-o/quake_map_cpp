// quake_map — ネイティブで地震モニターの画面を PNG に描く（WASM と同じ quake_map.hpp）
//   quake_map <data/basemap.txt> <fonts/NotoSansJP-Bold-jis.ttf> <out_prefix> <W> <H> <list.json> [<dir of detail json>] [<eew.json>]
//   → <out>_still.png（止まった絵）, <out>_replay.png（再生の途中）, <out>_eew.png（緊急地震速報の表示）
#define _CRT_SECURE_NO_WARNINGS
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "quake_map.hpp"

#define STB_TRUETYPE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_truetype.h"
#include "stb_image_write.h"

static std::string read_all(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }

int main(int argc, char** argv) {
    if (argc < 7) { fprintf(stderr, "usage: quake_map basemap font out W H list.json [detail_dir] [eew.json]\n"); return 1; }
    std::string out = argv[3];
    int W = atoi(argv[4]), H = atoi(argv[5]);
    std::string bm = read_all(argv[1]), font = read_all(argv[2]);
    qm::Map m;
    auto t0 = std::chrono::steady_clock::now();
    if (!m.load(bm, (const unsigned char*)font.data(), font.size())) { fprintf(stderr, "cannot load basemap\n"); return 1; }
    m.resize(W, H); m.fit();
    m.wall = 1.79e9;
    m.set_list(read_all(argv[6]));
    printf("quakes %zu, polys %zu (load %.0f ms)\n", m.quakes.size(), m.polys.size(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    std::string want = m.wanted();
    if (!want.empty() && argc > 7) {
        bool ok = m.set_detail(want, read_all(std::string(argv[7]) + "/" + want));
        printf("detail %s: %s, stations %zu\n", want.c_str(), ok ? "ok" : "fail", m.quakes[m.sel].st.size());
    }
    auto shot = [&](const char* name) {
        auto a = std::chrono::steady_clock::now();
        const uint8_t* px = m.render();
        auto b = std::chrono::steady_clock::now();
        m.render();
        auto c = std::chrono::steady_clock::now();
        std::string f = out + "_" + name + ".png";
        stbi_write_png(f.c_str(), W, H, 4, px, W * 4);
        printf("%s: first %.0f ms, next %.1f ms -> %s\n", name, std::chrono::duration<double, std::milli>(b - a).count(), std::chrono::duration<double, std::milli>(c - b).count(), f.c_str());
    };
    // 再生の途中（S 波が 12 秒ぶん進んだところ）
    m.wall = m.replay_start + 12;
    shot("replay");
    // 止まった絵（再生が終わったあと）
    m.wall += 600; m.render();
    m.hover(0, 0);
    shot("still");
    // 緊急地震速報（発生から 25 秒の時刻にして表示）
    if (argc > 8) {
        std::string e = read_all(argv[8]);
        qm::Eew ev = qm::parse_eew(e);
        m.wall = ev.origin + 25;
        m.set_eew(e);
        shot("eew");
    }
    return 0;
}
