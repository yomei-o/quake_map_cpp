// quake.hpp — 地震の情報（気象庁の JSON、緊急地震速報の JSON）を読む（ヘッダーオンリー、nlohmann/json）
//
// 読むもの（どれもブラウザから直接取れる。CORS が * ）:
//   気象庁 https://www.jma.go.jp/bosai/quake/data/list.json
//       発表の一覧。1 件 = 1 回の発表。同じ地震（eid）に「震度速報」「震源に関する情報」「震源・震度情報」などが並ぶ
//       at（発生時刻）, anm（震央地名）, cod（"+41.0+142.0-60000/" 緯度・経度・深さ m）, mag, maxi（最大震度）, json（本文のファイル名）
//   気象庁 https://www.jma.go.jp/bosai/quake/data/<json>
//       本文。Body.Intensity.Observation.Pref[].Area[].City[].IntensityStation[] に観測点ごとの震度と緯度経度（latlon）
//   Wolfx https://api.wolfx.jp/jma_eew.json（WebSocket は wss://ws-api.wolfx.jp/jma_eew）
//       緊急地震速報（気象庁の電文を JSON にしたもの）。OriginTime, Latitude, Longitude, Depth, Magnitude, MaxIntensity, isWarn, isCancel, ...
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "json.hpp"

namespace qm {

// ---- 時刻（UNIX 秒）----
inline long long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}
// "2026-11-12T02:15:00+09:00" または "2026/11/11 12:56:57"（日本時間）→ UNIX 秒。読めなければ 0
inline double parse_time(const std::string& s) {
    int Y, M, D, h, m, sec = 0, tzh = 9, tzm = 0;
    char sign = '+';
    if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d%c%d:%d", &Y, &M, &D, &h, &m, &sec, &sign, &tzh, &tzm) >= 6 ||
        sscanf(s.c_str(), "%d/%d/%d %d:%d:%d", &Y, &M, &D, &h, &m, &sec) >= 5) {
        double off = (sign == '-' ? -1 : 1) * (tzh * 3600.0 + tzm * 60.0);
        if (sign == 'Z') off = 0;
        return days_from_civil(Y, M, D) * 86400.0 + h * 3600.0 + m * 60.0 + sec - off;
    }
    return 0;
}
// UNIX 秒 → 日本時間の "11/12 02:15" など
inline std::string jst(double t, bool with_date = true, bool with_sec = false) {
    long long s = (long long)std::floor(t + 9 * 3600);
    long long days = s >= 0 ? s / 86400 : (s - 86399) / 86400, r = s - days * 86400;
    // civil_from_days
    long long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    char b[48];
    int hh = (int)(r / 3600), mm = (int)(r / 60 % 60), ss = (int)(r % 60);
    if (with_date && with_sec) snprintf(b, sizeof b, "%u/%u %02d:%02d:%02d", m, d, hh, mm, ss);
    else if (with_date) snprintf(b, sizeof b, "%u/%u %02d:%02d", m, d, hh, mm);
    else if (with_sec) snprintf(b, sizeof b, "%02d:%02d:%02d", hh, mm, ss);
    else snprintf(b, sizeof b, "%02d:%02d", hh, mm);
    return b;
}

// ---- 震度 ----
// 0 = なし、1〜4 = 震度 1〜4、5 = 5 弱、6 = 5 強、7 = 6 弱、8 = 6 強、9 = 7
inline int parse_int(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && !(s[i] >= '0' && s[i] <= '9')) ++i;   // "!5-"（5 弱以上未入電）などの前置きを飛ばす
    if (i >= s.size()) return 0;
    int v = s[i] - '0';
    std::string rest = s.substr(i + 1);
    bool lower = rest.rfind("-", 0) == 0 || rest.rfind("弱", 0) == 0, upper = rest.rfind("+", 0) == 0 || rest.rfind("強", 0) == 0;
    if (v <= 4) return v;
    if (v == 5) return upper ? 6 : 5;
    if (v == 6) return upper ? 8 : 7;
    (void)lower;
    return 9;
}
inline const char* int_label(int i) {
    static const char* L[] = {"-", "1", "2", "3", "4", "5弱", "5強", "6弱", "6強", "7"};
    return L[std::clamp(i, 0, 9)];
}
// 気象庁の震度の色（暗い背景に合わせて 1 だけ少し明るい灰色に）
inline uint32_t int_color(int i) {
    static const uint32_t C[] = {0x5A6478, 0xB8C4D6, 0x2EA8FF, 0x1F5BFF, 0xF5DC78, 0xFFE600, 0xFF9900, 0xFF2800, 0xC0003A, 0xD4007A};
    return C[std::clamp(i, 0, 9)];
}

// ---- 地震 ----
struct Station { std::string name; double lat = 0, lon = 0; int ints = 0; };

struct Quake {
    std::string eid;              // 地震の ID（発生時刻から作られた番号）
    std::string ctt;              // いちばん新しい発表の時刻（並べ替えと更新の判定）
    std::string name;             // 震央地名
    std::string title;            // いちばん新しい発表の題名
    std::string json;             // 観測点まで入った本文のファイル名（震源・震度情報があればそれ）
    double origin = 0;            // 発生時刻（UNIX 秒）
    double lat = 0, lon = 0, depth = -1;   // 深さ km（わからなければ -1）
    bool has_hypo = false;
    double mag = -1;
    int maxi = 0;
    bool cancelled = false;
    bool detail = false;          // 本文を読んだか
    std::vector<Station> st;
};

// cod "+41.0+142.0-60000/" → 緯度・経度・深さ（km）
inline bool parse_cod(const std::string& c, double& lat, double& lon, double& depth) {
    if (c.size() < 4) return false;
    const char* p = c.c_str();
    char* e;
    lat = strtod(p, &e); if (e == p) return false; p = e;
    lon = strtod(p, &e); if (e == p) return false; p = e;
    depth = -1;
    if (*p == '+' || *p == '-') { double d = strtod(p, &e); if (e != p) depth = std::fabs(d) / 1000.0; }
    return true;
}

inline std::string jstr(const nlohmann::json& o, const char* k) {
    auto it = o.find(k);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}
inline double jnum(const nlohmann::json& o, const char* k, double dflt) {
    auto it = o.find(k);
    if (it == o.end()) return dflt;
    if (it->is_number()) return it->get<double>();
    if (it->is_string()) { const std::string s = it->get<std::string>(); char* e; double v = strtod(s.c_str(), &e); return e != s.c_str() ? v : dflt; }
    return dflt;
}

// list.json → 地震ごとにまとめ、新しい順に（max 件）。すでに読んだ本文（old）は引き継ぐ
inline std::vector<Quake> parse_list(const std::string& text, const std::vector<Quake>& old, size_t max = 60) {
    std::vector<Quake> out;
    auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_array()) return old;
    std::map<std::string, Quake> by;
    std::map<std::string, std::string> best_json_ctt;
    for (auto& o : j) {
        if (!o.is_object()) continue;
        std::string eid = jstr(o, "eid");
        if (eid.empty()) continue;
        Quake& q = by[eid];
        q.eid = eid;
        std::string ctt = jstr(o, "ctt"), ttl = jstr(o, "ttl"), file = jstr(o, "json"), ift = jstr(o, "ift");
        bool newer = ctt > q.ctt;
        if (newer) { q.ctt = ctt; q.title = ttl; q.cancelled = ift == "取消"; }
        std::string anm = jstr(o, "anm");
        if (!anm.empty() && (newer || q.name.empty())) q.name = anm;
        double t = parse_time(jstr(o, "at"));
        if (t > 0 && (newer || q.origin == 0)) q.origin = t;
        double la, lo, de;
        if (parse_cod(jstr(o, "cod"), la, lo, de) && (newer || !q.has_hypo)) { q.lat = la; q.lon = lo; q.depth = de; q.has_hypo = true; }
        double m = jnum(o, "mag", -1);
        if (m >= 0 && (newer || q.mag < 0)) q.mag = m;
        int mi = parse_int(jstr(o, "maxi"));
        if (mi > 0 && (newer || q.maxi == 0)) q.maxi = mi;
        // 観測点の入った本文は「震源・震度情報」（VXSE53）。いちばん新しいものを使う
        if (file.find("VXSE53") != std::string::npos || file.find("VXSE5k") != std::string::npos) {
            if (ctt > best_json_ctt[eid]) { best_json_ctt[eid] = ctt; q.json = file; }
        }
    }
    // 震源も震度も無いもの（「南海トラフ地震関連解説情報」などのお知らせ）は一覧に出さない
    for (auto& kv : by) if (kv.second.has_hypo || kv.second.maxi > 0) out.push_back(kv.second);
    std::sort(out.begin(), out.end(), [](const Quake& a, const Quake& b) { return a.origin != b.origin ? a.origin > b.origin : a.eid > b.eid; });
    if (out.size() > max) out.resize(max);
    for (auto& q : out)
        for (auto& o : old)
            if (o.eid == q.eid && o.detail && o.json == q.json) { q.detail = true; q.st = o.st; if (o.has_hypo && !q.has_hypo) { q.lat = o.lat; q.lon = o.lon; q.depth = o.depth; q.has_hypo = true; } }
    return out;
}

// 本文（震源・震度情報）→ 観測点と震源を q に入れる
inline bool parse_detail(const std::string& text, Quake& q) {
    auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    auto body = j.find("Body");
    if (body == j.end()) return false;
    if (auto eq = body->find("Earthquake"); eq != body->end()) {
        double t = parse_time(jstr(*eq, "OriginTime"));
        if (t > 0) q.origin = t;
        double m = jnum(*eq, "Magnitude", -1);
        if (m >= 0) q.mag = m;
        if (auto hy = eq->find("Hypocenter"); hy != eq->end())
            if (auto ar = hy->find("Area"); ar != hy->end()) {
                double la, lo, de;
                if (parse_cod(jstr(*ar, "Coordinate"), la, lo, de)) { q.lat = la; q.lon = lo; q.depth = de; q.has_hypo = true; }
                std::string nm = jstr(*ar, "Name");
                if (!nm.empty()) q.name = nm;
            }
    }
    q.st.clear();
    auto in = body->find("Intensity");
    if (in != body->end()) {
        auto ob = in->find("Observation");
        if (ob != in->end()) {
            int mx = parse_int(jstr(*ob, "MaxInt"));
            if (mx > 0) q.maxi = mx;
            auto each = [](const nlohmann::json& o, const char* k) { auto it = o.find(k); return it != o.end() && it->is_array() ? *it : nlohmann::json::array(); };
            for (auto& pf : each(*ob, "Pref"))
                for (auto& ar : each(pf, "Area"))
                    for (auto& ci : each(ar, "City"))
                        for (auto& s : each(ci, "IntensityStation")) {
                            auto ll = s.find("latlon");
                            if (ll == s.end() || !ll->is_object()) continue;
                            Station st;
                            st.name = jstr(s, "Name");
                            st.lat = jnum(*ll, "lat", 0); st.lon = jnum(*ll, "lon", 0);
                            st.ints = parse_int(jstr(s, "Int"));
                            if (st.lat == 0 && st.lon == 0) continue;
                            q.st.push_back(st);
                        }
        }
    }
    std::stable_sort(q.st.begin(), q.st.end(), [](const Station& a, const Station& b) { return a.ints < b.ints; });   // 強い震度を上に描く
    q.detail = true;
    return true;
}

// ---- 緊急地震速報 ----
struct Eew {
    bool valid = false;
    std::string id, title, place;
    int serial = 0;
    double origin = 0, announced = 0;
    double lat = 0, lon = 0, depth = 0, mag = -1;
    int maxi = 0;
    bool warn = false, final_ = false, cancel = false, training = false, assumption = false;
    std::vector<std::string> warn_areas;
};

// Wolfx の JSON（jma_eew.json か WebSocket の 1 通）。緊急地震速報でなければ valid = false
inline Eew parse_eew(const std::string& text) {
    Eew e;
    auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return e;
    std::string type = jstr(j, "type");
    if (!type.empty() && type != "jma_eew") return e;   // heartbeat など
    e.id = jstr(j, "EventID");
    if (e.id.empty()) return e;
    e.title = jstr(j, "Title");
    e.place = jstr(j, "Hypocenter");
    e.serial = (int)jnum(j, "Serial", 0);
    e.origin = parse_time(jstr(j, "OriginTime"));
    e.announced = parse_time(jstr(j, "AnnouncedTime"));
    e.lat = jnum(j, "Latitude", 0); e.lon = jnum(j, "Longitude", 0);
    e.depth = jnum(j, "Depth", 0);
    e.mag = jnum(j, "Magnitude", jnum(j, "Magunitude", -1));
    e.maxi = parse_int(jstr(j, "MaxIntensity"));
    auto b = [&](const char* k) { auto it = j.find(k); return it != j.end() && it->is_boolean() && it->get<bool>(); };
    e.warn = b("isWarn"); e.final_ = b("isFinal"); e.cancel = b("isCancel"); e.training = b("isTraining"); e.assumption = b("isAssumption");
    if (auto wa = j.find("WarnArea"); wa != j.end() && wa->is_array())
        for (auto& a : *wa) { std::string c = jstr(a, "Chiiki"); if (!c.empty()) e.warn_areas.push_back(c); }
    e.valid = e.origin > 0;
    return e;
}

}  // namespace qm
