// Terminal frontend: layout, rendering, keyboard.
#include "tui.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <random>
#include "core.h"
#include "termbox2.h"
#include "vterm.h"

static const int TB_MOD_SUPER = 16;  // vendor/termbox2.c: CSI u / modifyOtherKeys, Cmd
static bool g_true = true;  // truecolor output, else nearest xterm-256

static int nearest256(RGB c) {
    int r = c >> 16 & 255, g = c >> 8 & 255, b = c & 255;
    auto q = [](int v) { return v < 48 ? 0 : v < 115 ? 1 : (v - 35) / 40; };
    auto lv = [](int x) { return x ? 55 + x * 40 : 0; };
    int qr = q(r), qg = q(g), qb = q(b);
    int dr = lv(qr) - r, dg = lv(qg) - g, db = lv(qb) - b;
    int avg = (r + g + b) / 3, gi = avg > 238 ? 23 : avg < 8 ? 0 : (avg - 8) / 10, gv = 8 + gi * 10;
    int dgray = (gv - r) * (gv - r) + (gv - g) * (gv - g) + (gv - b) * (gv - b);
    return dgray < dr * dr + dg * dg + db * db ? 232 + gi : 16 + 36 * qr + 6 * qg + qb;
}

static uintattr_t tbc(RGB c) {
    if (c < 0) return TB_DEFAULT;
    if (g_true) return c == 0 ? TB_HI_BLACK : (uintattr_t)c;
    int i = nearest256(c);
    return i == 0 ? TB_HI_BLACK : (uintattr_t)i;
}

static void cell(int x, int y, uint32_t cp, const Style& s) {
    uintattr_t fg = tbc(s.fg), bg = tbc(s.bg);
    if (s.attr & A_BOLD) fg |= TB_BOLD;
    if (s.attr & A_ITALIC) fg |= TB_ITALIC;
    if (s.attr & A_UNDER) fg |= TB_UNDERLINE;
    if (s.attr & A_REV) fg |= TB_REVERSE;
    if (s.attr & A_DIM) fg |= TB_DIM;
    tb_set_cell(x, y, cp, fg, bg);
}

static int cwidth(uint32_t cp) {
    int w = wcwidth((wchar_t)cp);
    return w < 0 ? 1 : w;
}

struct Cell { uint32_t cp; Style st; int w; int link = 0; };  // link: frame_urls index + 1
using Row = std::vector<Cell>;

// clickable URLs (OSC 8, vendor/termbox2.c): the ones drawn this frame
extern "C" void isekur_links_begin(void);
extern "C" void isekur_links_urls(const char* const* urls, int n);
extern "C" void isekur_link(int x, int y, int id);
extern "C" int isekur_present(void);
static std::vector<std::string> frame_urls;

// unlocking: the session opens from the middle out. g_reveal 0..1, or -1: none
static double g_reveal = -1;
static Style g_cover, g_edge;

static void present() {
    if (g_reveal >= 0) {
        int W = tb_width(), H = tb_height();
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                double nx = (x - W / 2.0) / (W / 2.0), ny = (y - H / 2.0) / (H / 2.0);
                double d = std::sqrt(nx * nx + ny * ny) / 1.4143 + (unsigned)((x * 73856093) ^ (y * 19349663)) % 100 / 800.0;
                double r = g_reveal * 1.2;  // a ragged ring: the noise keeps it from looking drawn with a compass
                if (d > r + 0.05) cell(x, y, ' ', g_cover);
                else if (d > r) cell(x, y, U'░', g_edge);
            }
    }
    std::vector<const char*> u;
    for (auto& s : frame_urls) u.push_back(s.c_str());
    isekur_links_urls(u.data(), (int)u.size());
    isekur_present();
}

// http(s)://... in a row of cells, like the webchat: up to a space or < > " ', without the
// trailing punctuation of the sentence. Printable ASCII only, since it goes into an escape
// sequence (anything else ends the link).
static void linkify(Row& r) {
    auto at = [&](size_t i, const char* s) {
        for (size_t k = 0; s[k]; k++)
            if (i + k >= r.size() || r[i + k].cp != (uint32_t)s[k]) return false;
        return true;
    };
    for (size_t i = 0; i < r.size(); i++) {
        if (!at(i, "http://") && !at(i, "https://")) continue;
        if (i && r[i - 1].cp < 128 && isalnum((int)r[i - 1].cp)) continue;
        size_t j = i;
        while (j < r.size() && r[j].cp > 0x20 && r[j].cp < 0x7f && !strchr("<>\"'", (int)r[j].cp)) j++;
        while (j > i && strchr(".,;:!?)]", (int)r[j - 1].cp)) j--;
        std::string url;
        for (size_t k = i; k < j; k++) url += (char)r[k].cp;
        if (url.size() <= 8 || url.size() > 2000 || frame_urls.size() >= 65535) { i = j; continue; }
        frame_urls.push_back(url);
        for (size_t k = i; k < j; k++) r[k].link = (int)frame_urls.size();
        i = j;
    }
}

static void cells(Row& out, const std::string& s, Style st, bool mirc = true) {
    auto put = [&](uint32_t cp, const Style& cs) {
        int w = cwidth(cp);
        if (w) out.push_back({cp, cs, w});
    };
    if (mirc) mirc_walk(s, st, put);
    else
        for (size_t i = 0; i < s.size();) put(utf8_next(s, i), st);
}

// draw cells from x, clipped at xmax; returns the next x
static int draw_row(int x, int y, int xmax, const Row& r) {
    for (auto& c : r) {
        if (x + c.w > xmax) break;
        cell(x, y, c.cp, c.st);
        if (c.link) isekur_link(x, y, c.link);
        x += c.w;
    }
    return x;
}

static int text(int x, int y, int xmax, const std::string& s, Style st, bool mirc = true) {
    Row r;
    cells(r, s, st, mirc);
    return draw_row(x, y, xmax, r);
}

static int width(const Row& r) {
    int w = 0;
    for (auto& c : r) w += c.w;
    return w;
}

static void fill(int y, int x0, int x1, Style st) {
    for (int x = x0; x < x1; x++) cell(x, y, ' ', st);
}

// A box's top row (x0, x1: its corners): the title, and the theme's window buttons like the webchat's dialogs.
// window = mac: red, yellow and green dots before the title; win95 (mirc): a navy title bar, white bold title,
// gray _ □ × at the right. Other themes: the title on the border
// returns the close button's columns ({-1, -1}: none)
static std::pair<int, int> box_title(const Theme& T, int x0, int x1, int y, const std::string& title, Style hs) {
    std::string ws = T.lay("window", "");
    if (ws == "win95") {
        Style bar{0xffffff, 0x000080, A_BOLD}, btn{0x000000, 0xc0c0c0, A_BOLD};
        for (int x = x0 + 1; x < x1; x++) cell(x, y, U' ', bar);
        // Minimize and close only (maximize did nothing): two gray buttons, three cells each with the glyph in the
        // middle, a navy cell between them like Windows' gap before close. Solid faces: edge lines inside a cell read
        // as dividers, half-blocks made notches, and a maximize □ is two cells wide in fonts like Iosevka
        text(x0 + 2, y, x1 - 9, title, bar, false);
        int bx[] = {x1 - 8, x1 - 4};
        char32_t g[] = {U'_', U'\u00D7'};
        for (int i = 0; i < 2; i++) cell(bx[i], y, U' ', btn), cell(bx[i] + 1, y, g[i], btn), cell(bx[i] + 2, y, U' ', btn);
        return {bx[1], bx[1] + 2};
    }
    int tx = x0 + 2;
    std::pair<int, int> close{-1, -1};
    if (ws == "mac") {
        close = {tx, tx};  // the red one
        Style dot = hs;
        dot.attr = 0;
        for (RGB col : {0xff5f57, 0xfebc2e, 0x28c840}) dot.fg = col, cell(tx, y, U'\u25CF', dot), tx += 2;
    }
    text(tx, y, x1 - 1, " " + title + " ", hs, false);
    return close;
}

// WCAG contrast of two colors (1..21), like the webchat's: picks colors that read whatever the theme
static double contrast(RGB a, RGB b) {
    auto lum = [](RGB c) {
        auto ch = [&](int sh) { double v = (c >> sh & 255) / 255.0; return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
        return 0.2126 * ch(16) + 0.7152 * ch(8) + 0.0722 * ch(0);
    };
    double x = lum(a), y = lum(b);
    return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
}

// c, or if it doesn't read on bg (4.5:1), c toward black or white until it does (the webchat's readOn)
static RGB readable(RGB c, RGB bg) {
    if (c < 0 || bg < 0 || contrast(c, bg) >= 4.5) return c;
    bool dark = contrast(0x000000, bg) > contrast(0xffffff, bg);
    RGB to = dark ? 0x000000 : 0xffffff, out = c;
    for (double k = 0.05; contrast(out, bg) < 4.5 && k <= 1.0; k += 0.05) {
        auto mix = [&](int sh) { int a = c >> sh & 255, b = to >> sh & 255; return (int)std::lround(a + (b - a) * k) << sh; };
        out = mix(16) | mix(8) | mix(0);
    }
    return out;
}

// a word-wrapped question for a yes/no box
static std::vector<std::string> wrap_words(const std::string& s, int w) {
    std::vector<std::string> out;
    std::string line;
    for (auto& word : split(s, ' ')) {
        if (!line.empty() && (int)(line.size() + 1 + word.size()) > w) out.push_back(line), line.clear();
        line += (line.empty() ? "" : " ") + word;
    }
    if (!line.empty()) out.push_back(line);
    return out;
}


static std::string to_utf8(const std::u32string& s) {
    std::string o;
    for (char32_t c : s) utf8_put(o, c);
    return o;
}

static std::u32string from_utf8(const std::string& s) {
    std::u32string o;
    for (size_t i = 0; i < s.size();) o += (char32_t)utf8_next(s, i);
    return o;
}

// theme element with a fallback element instead of "default"
static Style sty(const Theme& t, const std::string& k, const std::string& fb) {
    return t.el.count(k) ? t.get(k) : t.get(fb);
}

static Style nick_style(const Theme& t, const std::string& nick) {
    unsigned h = 5381;
    for (char ch : irc_lower(nick)) h = h * 33 + (unsigned char)ch;
    Style s = t.get("default");
    if (!t.nicks.empty()) s.fg = t.nicks[h % t.nicks.size()];  // no theme file found: no nick colors
    return s;
}

// a status tab shows the network (NETWORK= from 005, e.g. OFTC), the config name until then
static std::string buf_label(const Buffer* b) {
    if (b->type == BT::STATUS && !b->srv) return "CMD";  // the home tab: capitals, unlike any channel
    return b->type == BT::STATUS && b->srv && !b->srv->network.empty() ? b->srv->network : b->name;
}

// ---- status_info: this machine's CPU, memory, load and battery -------------
// src/sysinfo.cpp, apart since macOS headers have their own Style
int cpu_pct();
bool mem_free(unsigned long long& avail, unsigned long long& total);
int battery_pct();

// uptime as 3d4h22m, seconds below a day
static std::string fmt_uptime(long up) {
    std::string u;
    const std::pair<long, const char*> units[] = {{2592000, "mo"}, {86400, "d"}, {3600, "h"}, {60, "m"}, {1, "s"}};
    bool days = up >= 86400;
    for (auto& [n, name] : units)
        if ((!u.empty() || up >= n || n == 1) && !(n == 1 && days)) u += std::to_string(up / n) + name, up %= n;
    return u;
}

// cpu_levels / mem_levels: "min avg max" percents; at min a value turns status_warn, at avg
// status_high, at max status_crit (0 = none of them). Anything but three rising numbers
// 0-100 means the defaults.
static int level(const Core& c, const std::string& key, const char* def, int pct) {
    int l[3];
    auto parse = [&](const std::string& v) {
        return sscanf(v.c_str(), "%d %d %d", &l[0], &l[1], &l[2]) == 3 && 0 <= l[0] && l[0] <= l[1] && l[1] <= l[2] && l[2] <= 100;
    };
    if (!parse(c.opt(nullptr, key, def))) parse(def);
    return pct >= l[2] ? 3 : pct >= l[1] ? 2 : pct >= l[0] ? 1 : 0;
}

// a status_info color from the theme, on base; a theme without it gets fg (and bg)
static Style info_style(const Theme& T, const std::string& k, Style base, RGB fg, RGB bg = -1) {
    Style t = T.el.count(k) ? T.get(k) : Style{fg, bg, 0};
    if (t.fg >= 0) base.fg = t.fg;
    if (t.bg >= 0) base.bg = t.bg;
    base.attr |= t.attr;
    return base;
}
// status_ok, status_warn, status_high, status_crit: green, yellow, orange, red by default
static Style level_style(const Theme& T, int lv, Style base) {
    static const char* keys[] = {"status_ok", "status_warn", "status_high", "status_crit"};
    static const RGB fgs[] = {0x5fd75f, 0xffd75f, 0xffaf00, 0xff5f5f};
    return info_style(T, keys[lv], base, fgs[lv]);
}

// the status_info items that are on, in a fixed order, drawn in st
static std::vector<Row> status_info(const Core& c, Style st, bool dcc = true) {
    const Theme& T = c.theme;
    auto on = split(c.opt(nullptr, "status_info", "up"), ' ');
    auto has = [&](const char* k) { return std::find(on.begin(), on.end(), k) != on.end(); };
    std::vector<Row> out;
    auto add = [&](const std::string& s) { out.emplace_back(), cells(out.back(), s, st, false); };
    // /dcc transfers, always and first (the last items drop): ⇣name ▮▮▮▯▯▯▯▯ 42% 1.2M/s +1 (⇡ sending)
    std::vector<const Dcc*> rx;
    for (auto& d : c.dccs)
        if (d->st == Dcc::RUN) rx.push_back(d.get());
    if (!dcc) {  // the webchat draws its own
    } else if (!rx.empty()) {
        const Dcc* d = rx[0];
        Style es = info_style(T, "status_bat", st, 0x6c6c6c), bs = level_style(T, 0, st);
        es.bg = bs.bg = st.bg;
        Row r;
        cells(r, (d->send ? "\u21E1" : "\u21E3") + (mirc_strip(d->file).size() > 16 ? mirc_prefix(d->file, 15) + "\u2026" : d->file) + " ", st, false);
        if (d->size) {
            int pct = (int)(d->got * 100 / d->size), full = pct * 8 / 100;
            for (int i = 0; i < 8; i++) cells(r, i < full ? "\u25AE" : "\u25AF", i < full ? bs : es, false);
            cells(r, " " + std::to_string(pct) + "%", st, false);
        } else {
            cells(r, human_bytes(d->got), st, false);
        }
        cells(r, " " + human_bytes(d->rate) + "/s" + (rx.size() > 1 ? " +" + std::to_string(rx.size() - 1) : ""), st, false);
        out.push_back(r);
    } else {  // nothing moving: the last one that ended, for 10s (✓ green, ✗ red), else the offers waiting
        const Dcc* last = nullptr;
        size_t offers = 0;
        for (auto& d : c.dccs) {
            if (d->ended && time(nullptr) - d->ended < 10 && (!last || d->ended >= last->ended)) last = d.get();
            offers += d->st == Dcc::OFFER;
        }
        auto nm = [](const std::string& f) { return mirc_strip(f).size() > 16 ? mirc_prefix(f, 15) + "\u2026" : f; };
        if (last) {
            bool ok = last->st == Dcc::DONE;
            Row r;
            cells(r, std::string(ok ? "\u2713" : "\u2717") + nm(last->file), level_style(T, ok ? 0 : 3, st), false);
            cells(r, ok ? " done" : last->err == "cancelled" ? " stopped" : " failed", st, false);
            out.push_back(r);
        } else if (offers) {
            out.emplace_back();
            cells(out.back(), "\u21E3" + std::to_string(offers) + " offered (/dcc)", sty(T, "hilight", "status_hi"), false);
        }
    }
    if (has("up")) add("u:" + fmt_uptime((long)(time(nullptr) - c.started)));
    // label in st, the value in its level's color
    auto leveled = [&](const std::string& label, const std::string& val, const std::string& key, const char* def, int pct) {
        int lv = pct >= 0 ? level(c, key, def, pct) : 0;
        Style vs = lv ? level_style(T, lv, st) : st;
        out.emplace_back();
        cells(out.back(), label, st, false);
        cells(out.back(), val, vs, false);
    };
    if (has("cpu")) {
        int p = cpu_pct();
        leveled("cpu:", (p < 0 ? std::string("--") : std::to_string(p)) + "%", "cpu_levels", "50 75 90", p);
    }
    unsigned long long a, t;
    if (has("mem") && mem_free(a, t))  // colored by how much is used
        leveled("free:", human_bytes(a) + "/" + human_bytes(t), "mem_levels", "70 85 95", t ? (int)((t - a) * 100 / t) : -1);
    double ld[3];
    if (has("load") && getloadavg(ld, 3) == 3) {
        char buf[48];
        snprintf(buf, sizeof buf, "load:%.2f %.2f %.2f", ld[0], ld[1], ld[2]);
        add(buf);
    }
    int bp = has("bat") ? battery_pct() : -1;
    if (bp >= 0) {  // ▮▮▯╸64%: the bars on a status_bat body, 3 status_ok, 2 warn, 1 high, 1 crit below 15%
        int bars = bp > 66 ? 3 : bp > 33 ? 2 : 1;
        Style es = info_style(T, "status_bat", st, 0x6c6c6c, 0x3a3a3a);  // fg: empty bars and the tip, bg: the body
        Style bs = level_style(T, bp > 66 ? 0 : bp > 33 ? 1 : bp >= 15 ? 2 : 3, st), tip = es;
        bs.bg = es.bg, tip.bg = st.bg;  // no edge lines: fonts put ▕ ▏ in different places
        Row r;
        for (int i = 0; i < 3; i++) cells(r, i < bars ? "\u25AE" : "\u25AF", i < bars ? bs : es, false);
        cells(r, "\u2578", tip, false);
        cells(r, std::to_string(bp) + "%", bp < 15 ? Style{bs.fg, st.bg, st.attr} : st, false);
        out.push_back(r);
    }
    return out;
}

std::string json_str(const std::string& s);  // web.cpp
std::string status_info_json(const Core& c) {
    std::string o = "[";
    for (auto& r : status_info(c, sty(c.theme, "status_info", "status_hi"), false)) {
        o += o.size() > 1 ? ",[" : "[";
        std::string run;
        const Style* at = nullptr;  // runs of one color: [text, fg, bg]
        auto put = [&] { if (at) o += std::string(o.back() == '[' ? "" : ",") + "[" + json_str(run) + "," + json_str(rgb_hex(at->fg)) + "," + json_str(rgb_hex(at->bg)) + "]"; };
        for (auto& cl : r) {
            if (!at || cl.st.fg != at->fg || cl.st.bg != at->bg) put(), run.clear(), at = &cl.st;
            run += to_utf8(std::u32string(1, (char32_t)cl.cp));
        }
        put();
        o += "]";
    }
    return o + "]";
}

// ---- line layout -----------------------------------------------------------
static std::vector<Row> wrap(const Core& c, const Line& l, const Buffer* b, int width_) {
    const Theme& T = c.theme;
    int nw = atoi(T.lay("nick_width", "12").c_str());
    std::string tsfmt = ini_get(c.cfg, "general", "timestamp", T.lay("timestamp", "%H:%M"));
    // timestamp_align = right: the time at the right edge of the first row, the text wraps short of it
    bool ts_right = ini_get(c.cfg, "general", "timestamp_align") == "right";
    Row pre, tsr;
    cells(ts_right ? tsr : pre, fmt_time(l.ts, tsfmt) + (ts_right ? "" : " "), T.get("timestamp"), false);

    std::string marker;
    Style ms, body = T.get("default");
    std::string txt = l.text;
    switch (l.kind) {
    case LK::MSG: {
        std::string pfx;
        if (b->srv) {  // the cmd tab has no server (and no nicks), its lines still can be msgs
            auto it = b->nicks.find(b->srv->lower(l.nick));
            if (it != b->nicks.end() && !it->second.prefix.empty()) pfx = it->second.prefix.substr(0, 1);
        }
        marker = pfx + l.nick;
        if (auto br = T.lay("nick_brackets", ""); br.size() == 2) marker = br[0] + marker + br[1];  // mirc: <nick>
        ms = l.self ? T.get("msg_self") : nick_style(T, l.nick);
        if (!l.self && l.color >= 0) ms.fg = l.color;  // Twitch: the color they picked
        if (l.hilight) ms = T.get("hilight"), body = sty(T, "hilight_text", "default");
        break;
    }
    case LK::ACTION:
        marker = "*", ms = body = T.get("action");
        txt = l.nick + " " + l.text;
        if (l.hilight) ms = T.get("hilight");
        break;
    case LK::NOTICE: marker = "-" + l.nick + "-", ms = T.get("notice"); break;
    case LK::JOIN: marker = "-->", ms = body = T.get("join"); break;
    case LK::PART: marker = "<--", ms = body = T.get("part"); break;
    case LK::QUIT: marker = "<--", ms = body = sty(T, "quit", "part"); break;
    case LK::ERROR: marker = "!!", ms = body = T.get("error"); break;
    case LK::NICK: case LK::MODE: case LK::TOPIC: marker = "--", ms = body = sty(T, "event", "info"); break;  // mirc: green, like joins
    default: marker = "--", ms = body = T.get("info"); break;
    }
    if (auto em = T.lay("event_marker", ""); !em.empty() && l.kind != LK::MSG && l.kind != LK::NOTICE)  // mirc: "*"
        marker = l.kind == LK::INFO && !b->srv && b->type != BT::SHELL ? "" : em;  // home tab's banner and welcome: bare
    Row nick;
    cells(nick, marker, ms, false);
    if (nw > 0 && width(nick) > nw) {  // nick_width 0: no column, the nick as long as it is
        while (!nick.empty() && width(nick) > nw - 1) nick.pop_back();
        nick.push_back({'+', T.get("separator"), 1});
    }
    for (int pad = nw - width(nick); pad > 0; pad--) pre.push_back({' ', T.get("default"), 1});
    pre.insert(pre.end(), nick.begin(), nick.end());
    std::string sep = T.lay("separator", "│");
    if (!marker.empty() || !sep.empty()) cells(pre, sep.empty() ? " " : " " + sep + " ", T.get("separator"), false);

    int tsw = tsr.empty() ? 0 : width(tsr) + 1, indent = width(pre), avail = std::max(10, width_ - indent - tsw);
    if (l.typed_at) {  // /typing: the part typed so far, and the cursor
        size_t len = 0, n;
        for (unsigned char ch : mirc_strip(txt)) len += (ch & 0xc0) != 0x80;  // characters, not bytes
        n = typed_chars(l, len, now_ms());
        if (n < len) txt = mirc_prefix(txt, n) + "\x0f\u258C";
    }
    Row body_cells;
    cells(body_cells, txt, body);
    linkify(body_cells);

    std::vector<Row> rows;
    Row cur = pre;
    size_t i = 0;
    while (i < body_cells.size()) {
        // take as many cells as fit, break at the last space if there is one
        int w = 0;
        size_t j = i, last_space = std::string::npos;
        while (j < body_cells.size() && w + body_cells[j].w <= avail) {
            if (body_cells[j].cp == ' ') last_space = j;
            w += body_cells[j++].w;
        }
        if (j < body_cells.size() && last_space != std::string::npos && last_space > i) j = last_space + 1;
        cur.insert(cur.end(), body_cells.begin() + (long)i, body_cells.begin() + (long)j);
        rows.push_back(cur);
        cur.assign((size_t)indent, Cell{' ', T.get("default"), 1});
        i = j;
    }
    if (rows.empty()) rows.push_back(cur);
    if (!tsr.empty()) {
        Row& r0 = rows[0];
        for (int w = width(r0); w < width_ - (tsw - 1); w++) r0.push_back({' ', T.get("default"), 1});
        r0.insert(r0.end(), tsr.begin(), tsr.end());
    }
    return rows;
}

// ---- drawing ---------------------------------------------------------------
// a channel's nicklist: the separator at column sx, the list right of it up to xend. Only the
// focused pane's scrolls (nl_scroll); a double click on a nick opens a query
void Tui::draw_nicks(Buffer* b, int sx, int top, int xend, int ch, bool focus) {
    const Theme& T = c.theme;
    Style bs = sty(T, "nicklist_border", "separator");
    for (int y = top; y < top + ch; y++) text(sx, y, xend, T.lay("separator", "│"), bs, false);
    std::vector<const NickEntry*> v;
    for (auto& kv : b->nicks) v.push_back(&kv.second);
    Server* s = b->srv;
    // the map is keyed by irc_lower(nick), so it's already in name order: only rank moves them
    std::stable_sort(v.begin(), v.end(), [&](const NickEntry* x, const NickEntry* y) {
        return s->prefix_rank(x->prefix) < s->prefix_rank(y->prefix);
    });
    int sc = 0;
    if (focus) {
        if (nl_buf != b) nl_buf = b, nl_scroll = 0, nick_sel.clear();  // the selection is this list's
        sc = nl_scroll = std::max(0, std::min(nl_scroll, (int)v.size() - (ch - 1)));
    }
    v.erase(v.begin(), v.begin() + sc);
    Style hs = sty(T, "nicklist_title", "nicklist");
    fill(top, sx + 1, xend, hs);
    text(sx + 2, top, xend, std::to_string(v.size() + (size_t)sc) + " users" + (sc ? " ↑" : ""), hs, false);
    for (size_t i = 0; i < v.size() && (int)i < ch - 1; i++) {
        const NickEntry* e = v[i];
        if (focus) hits.push_back({top + 1 + (int)i, sx + 1, xend, -1, e->nick});
        Style ns = e->prefix.empty() ? T.get("nicklist")
                 : e->prefix[0] == '+' ? sty(T, "nicklist_voice", "nicklist")
                                       : sty(T, "nicklist_op", "nicklist");
        if (e->away) ns.attr |= A_DIM;
        if (e->bot) ns.attr |= A_ITALIC;  // the 005 BOT mode
        bool sel = focus && std::find(nick_sel.begin(), nick_sel.end(), e->nick) != nick_sel.end();
        if (sel) ns = sty(T, "menu_sel", "tab_active");
        fill(top + 1 + (int)i, sx + 1, xend, sel ? ns : T.get("nicklist"));
        text(sx + 2, top + 1 + (int)i, xend, (e->prefix.empty() ? " " : e->prefix.substr(0, 1)) + e->nick, ns, false);
    }
    for (int y = top + 1 + (int)v.size(); y < top + ch; y++) fill(y, sx + 1, xend, T.get("nicklist"));
}

void Tui::draw_chat(Buffer* b, int x, int y, int w, int h) {
    const Theme& T = c.theme;
    // a shell tab: its terminal screen, unless scrolled back into what went off the top
    Term* term = b->term.get();
    if (term && b->scroll == 0) {
        c.term_resize(term, h, w);
        draw_term(term, y, h, w, x);
        return;
    }
    std::vector<Row> rows;  // bottom-up
    int need = h + b->scroll;
    size_t n = 0;
    for (auto it = b->lines.rbegin(); it != b->lines.rend() && (int)rows.size() < need; ++it, ++n) {
        if (b->unread && n == (size_t)b->unread) {  // read marker above the new lines
            Row mk;
            std::string bar = "── new ";
            for (int k = 7; k < w; k++) bar += "─";
            cells(mk, bar, sty(T, "marker", "separator"), false);
            rows.push_back(mk);
        }
        if (b->quiet && noise(*it)) continue;
        auto r = wrap(c, *it, b, w);
        rows.insert(rows.end(), r.rbegin(), r.rend());
    }
    if ((int)rows.size() < need) b->scroll = std::max(0, (int)rows.size() - h);
    for (int k = 0; k < h; k++) {
        size_t idx = (size_t)(b->scroll + k);
        if (idx >= rows.size()) break;
        draw_row(x, y + h - 1 - k, x + w, rows[idx]);
    }
}

int Tui::chat_height() const { return std::max(1, tb_height() - 2 - top_row()); }

void Tui::draw() {
    xbtn = XBtn{};
    if (c.cfg_changed) c.cfg_changed = false, apply_cfg();
    if (c.settings_win) c.settings_win = false, sw = SetWin{}, sw.open = true, sw.to_servers = c.settings_servers, c.settings_servers = false;
    if (c.theme_win) {
        c.theme_win = false, tw = ThemeWin{}, tw.open = true, tw.orig = c.theme.name;
        auto l = theme_list();
        tw.sel = std::max(0, (int)(std::find(l.begin(), l.end(), tw.orig) - l.begin()) % std::max(1, (int)l.size()));
    }
    if (c.keys_win) c.keys_win = false, kw = KeysWin{}, kw.open = true;
    if (c.bell) c.bell = false, tb_send("\a", 1);
    const Theme& T = c.theme;
    Style def = T.get("default");
    tb_set_clear_attrs(tbc(def.fg), tbc(def.bg));
    tb_clear();
    isekur_links_begin(), frame_urls.clear();
    int W = tb_width(), H = tb_height();
    sync_input();  // a tab switch from outside a key (a command, the webchat, a closed tab)
    Buffer* b = c.active;
    c.dirty = false;
    hits.clear(), nl_x = -1;
    long long la = c.lock_anim ? now_ms() - c.lock_anim_at : 0;
    g_reveal = c.lock_anim == 'u' && la >= 350 ? std::min(1.0, (la - 350) / 550.0) : -1;
    g_cover = def, g_edge = sty(T, "menu_border", "separator"), g_edge.bg = def.bg;
    if (c.locked || (c.lock_anim == 'u' && la < 350)) {  // the lock screen: nothing of the session shows until the password
        draw_lock(W, H, la);
        present();
        return;
    }
    if (H < 6 || W < 30 || !b) {
        text(0, 0, W, "terminal too small", def, false);
        present();
        return;
    }

    // topic bar
    if (topicbar) {
        Style ts = T.get("topic");
        fill(0, 0, W, ts);
        std::string topic;
        if (b->type == BT::CHANNEL) topic = b->topic;
        else if (b->name == "*channels") topic = "Channel list: /list again refreshes it · /join #channel";
        else if (b->name == "*raw" && b->srv) topic = "Raw log: \xe2\x86\x90 from the server, \xe2\x86\x92 to it · /rawlog off or Ctrl+W stops it";
        else if (b->name == "*about" && !b->srv) topic = "About iSekur";
        else if (b->name == "*guide" && !b->srv) topic = "First steps guide: ← → turn the pages · /guide N jumps · q, Esc or Ctrl+W closes";
        else if (b->type == BT::QUERY) topic = "Query with " + b->name;
        else if (b->type == BT::SHELL) topic = b->term && !b->term->host.empty() ? "Remote iSekur: ssh " + b->term->host + (b->term->rcfg.empty() ? "" : ", -c " + b->term->rcfg) + " (/detach inside leaves it running)"
                    : b->term ? std::string("Shell: ") + (getenv("SHELL") ? getenv("SHELL") : "/bin/sh") : "Shell: /shell logs in and starts one";
        else if (b->srv) {
            static const char* st[] = {"disconnected", "connecting", "registering", "connected"};
            topic = b->srv->cfg.host + ":" + std::to_string(b->srv->cfg.port) +
                    (b->srv->cfg.tls ? " (TLS)" : "") + " - " + st[(int)b->srv->state];
        } else topic = "iSekur - multi-server IRC with a built-in webchat";
        if (topic_buf != b) topic_buf = b, topic_off = 0, topic_open = false;
        Row r;
        cells(r, topic, ts);
        linkify(r);
        int tw = W - 2, maxh = std::max(1, (H - 4) / 2);
        if (topic_open && width(r) > tw) {  // unfolded: wrapped over as many rows as it needs
            std::vector<Row> rows(1);
            for (auto& cl : r) {
                if (width(rows.back()) + cl.w > tw) {
                    if ((int)rows.size() == maxh) break;
                    rows.emplace_back();
                }
                rows.back().push_back(cl);
            }
            topic_h = (int)rows.size();
            for (int i = 0; i < topic_h; i++) fill(i, 0, W, ts), draw_row(1, i, W - 1, rows[(size_t)i]);
        } else {
            topic_h = 1, topic_open = false;
            topic_off = std::max(0, std::min(topic_off, width(r) - tw));
            int skip = topic_off;
            size_t i = 0;
            while (i < r.size() && skip > 0) skip -= r[i++].w;
            r.erase(r.begin(), r.begin() + (long)i);
            fill(0, 0, W, ts);
            draw_row(1, 0, W - 1, r);
            if (topic_off) cell(0, 0, U'‹', ts);
            if (width(r) > tw) cell(W - 1, 0, U'›', ts);
        }
    } else topic_h = 1;

    // tab bar
    Style tbs = T.get("tabbar");
    // a collapsed server tab hides its channels and queries (the active one stays), shows "+N"
    // and takes their activity color; Ctrl+N/P, Ctrl+A and /win still go to them
    std::vector<Row> labels;
    std::vector<int> vis;  // c.bufs index of each label
    int ai = c.index_of(b), av = 0;  // av: the active tab among the labels
    for (size_t i = 0; i < c.bufs.size(); i++) {
        Buffer* t = c.bufs[i].get();
        if (t != b && t->type != BT::STATUS && t->srv && t->srv->status->collapsed) continue;
        int act = t->activity, hid = 0;
        if (t->type == BT::STATUS && t->collapsed)
            for (auto& o : c.bufs)
                if (o->srv == t->srv && o.get() != t && o.get() != b) hid++, act = std::max(act, o->activity);
        Style s = t->id == tab_drop ? T.get("tab_active")  // where a dragged tab would land: reversed below
                : t == b ? (t->quiet ? sty(T, "tab_quiet_act", "tab_active") : T.get("tab_active"))
                : act == 3 ? T.get("tab_hilight")
                : act == 2 ? T.get("tab_msg")
                : act == 1 ? T.get("tab_data")
                : t->quiet ? sty(T, "tab_quiet", "tabbar")
                : t->type == BT::SHELL ? sty(T, "tab_shell", "tab_home")
                : !t->srv ? sty(T, "tab_home", "tab_server")
                : t->type == BT::STATUS ? sty(T, "tab_server", "tabbar") : tbs;
        if (t->id == tab_drop) s.attr ^= A_REV;
        Row r;
        cells(r, " " + std::to_string(i + 1) + ":" + buf_label(t) + (c.muted(t) ? " \xf0\x9f\x94\x87" : "") + (e2e_on(t) ? " \xf0\x9f\x94\x92" : "") + (hid ? " +" + std::to_string(hid) : "") + " ", s, false);
        if (t == b) av = (int)labels.size();
        labels.push_back(r);
        vis.push_back((int)i);
    }
    // tabs wrap onto more rows, whole labels only, filled left to right. Past TAB_ROWS the last row
    // scrolls: the active tab stays in view, "‹N" / "N›" count the hidden ones on each side (colored
    // like the most urgent of them) and a click on it brings the next one in
    const int TAB_ROWS = 3;  // ponytail: fixed cap, a setting if anyone wants more
    int nt = (int)labels.size();
    std::vector<int> starts{0};  // the first label of each row
    for (int i = 0, x = 0; i < nt; i++) {
        int w = width(labels[(size_t)i]);
        if (x && x + w > W) starts.push_back(i), x = 0;
        x += w;
    }
    if ((int)starts.size() > TAB_ROWS) starts.resize(TAB_ROWS);
    tab_h = (int)starts.size();
    int ty0 = top_row() - tab_h;
    auto more = [&](int ty, int from, int to, bool left, int lm) {
        int act = 0;
        for (int i = from; i < to; i++) act = std::max(act, c.bufs[(size_t)vis[(size_t)i]]->activity);
        Style ms = act == 3 ? T.get("tab_hilight") : act == 2 ? T.get("tab_msg") : act == 1 ? T.get("tab_data") : tbs;
        std::string num = std::to_string(to - from), m = left ? "‹" + num : num + "›";
        int x0 = left ? 1 : W - (int)num.size() - 2;
        text(x0, ty, W, m, ms, false);
        hits.push_back({ty, left ? 0 : x0, left ? lm : W, vis[(size_t)(left ? to - 1 : from)], ""});
    };
    for (int r = 0; r < tab_h; r++) {
        int ty = ty0 + r, from = starts[(size_t)r], to = r + 1 < tab_h ? starts[(size_t)r + 1] : nt;
        fill(ty, 0, W, tbs);
        if (from >= to) continue;
        int a = std::clamp(av, from, to - 1), total = 0, first = from, lm = 0, rm = 0;
        for (int i = from; i < to; i++) total += width(labels[(size_t)i]);
        auto pick = [&] {
            int span = 0;
            first = from;
            for (int i = from; i <= a; i++) span += width(labels[(size_t)i]);
            while (span > W - lm - rm && first < a) span -= width(labels[(size_t)first++]);
        };
        if (total > W) rm = 4, pick();
        if (first > from) lm = 4, pick();
        int last = first, tx = lm;
        for (int i = first; i < to && (i == first || tx + width(labels[(size_t)i]) <= W - rm); i++) {
            int x0 = tx;
            tx = draw_row(tx, ty, W - rm, labels[(size_t)i]);
            hits.push_back({ty, x0, tx, vis[(size_t)i], ""});
            last = i;
        }
        if (first > from) more(ty, from, first, true, lm);
        if (last < to - 1) more(ty, last + 1, to, false, lm);
    }

    // the chat and its nicklist, or /split: two panes, each with its own title row (the focused one in
    // tab_active) and, for a channel with room, its own nicklist. Side by side when the window is wide
    int top = top_row(), ch = chat_height();
    int nlw0 = atoi(T.lay("nicklist_width", "18").c_str());
    Buffer* sb = c.split_buf && c.split_buf != b ? c.split_buf : nullptr;
    bool two = sb && ch >= 7;
    nl_x = -1, other = Box{}, div_x = div_y = -1;
    Box pane{0, top, W, ch};  // where the focused tab's chat went (a shell's cursor goes in it)
    auto draw_pane = [&](Buffer* pb, Box bx, bool focus) {
        if (two) {
            Style hs = focus ? T.get("tab_active") : T.get("tabbar");
            fill(bx.y, bx.x, bx.x + bx.w, hs);
            text(bx.x + 1, bx.y, bx.x + bx.w, std::to_string(c.index_of(pb) + 1) + ":" + buf_label(pb) + (focus ? "" : "  (F6 / Alt+O / click: focus)"), hs, false);
            bx.y++, bx.h--;
        }
        int nw = nicklist && pb->type == BT::CHANNEL && bx.w >= 60 ? nlw0 : 0, cwp = bx.w - (nw ? nw + 1 : 0);
        if (nw) draw_nicks(pb, bx.x + cwp, bx.y, bx.x + bx.w, bx.h, focus);
        if (nw && focus) nl_x = bx.x + cwp;
        draw_chat(pb, bx.x, bx.y, cwp, bx.h);
        if (focus) pane = Box{bx.x, bx.y, cwp, bx.h};
    };
    if (!two) draw_pane(b, pane, true);
    else {
        Box p1{0, top, W, ch}, p2 = p1;
        if (c.split_mode == 1 || (c.split_mode == 0 && W >= 140)) {  // side by side, a separator column between
            p1.w = std::max(10, std::min(W - 11, (W - 1) * c.split_pm / 1000)), p2.x = p1.w + 1, p2.w = W - p2.x;
            for (int y = top; y < top + ch; y++) text(p1.w, y, W, "│", T.get("separator"), false);
            div_x = p1.w;
        } else p1.h = std::max(3, std::min(ch - 3, ch * c.split_pm / 1000)), p2.y = top + p1.h, p2.h = ch - p1.h, div_y = p2.y;
        other = c.split_first ? p1 : p2;
        draw_pane(b, c.split_first ? p2 : p1, true);
        draw_pane(sb, other, false);
    }
    int cw = !two && nl_x >= 0 ? nl_x : W, nlw = !two && nl_x >= 0;  // the status line lines up with a lone nicklist
    if (sel_x0 >= 0) {  // the selection, reversed in place
        tb_cell* cb = tb_cell_buffer();
        for (int yy = top; yy < top + ch; yy++)
            for (int xx = 0; xx < cw; xx++) {
                tb_cell& cl = cb[yy * W + xx];
                if (cl.ch && in_sel(xx, yy)) tb_set_cell(xx, yy, cl.ch, cl.fg | TB_REVERSE, cl.bg);
            }
    }

    // live suggestions ("/set key", "/jo", "/help k"), Tab completes
    std::vector<Sugg> sk;
    if (suggest(sk) < 0 || sugg_sel >= (int)sk.size()) sugg_sel = -1;
    if (!sk.empty()) {
        int sz = (int)sk.size(), n = std::min(sz, std::max(1, ch / 2));
        // scrolled so the highlight stays above the "... more" row
        int off = sugg_sel >= n - 1 ? sugg_sel - n + 2 : 0;
        if (off + n >= sz) off = sz - n;
        Style ks = T.get("input"), kk = sty(T, "set_key", "info"), kv = sty(T, "set_value", "input"), ke = T.get("separator");
        for (Style* st : {&kk, &kv, &ke})
            if (st->bg < 0) st->bg = ks.bg;
        for (int i = 0; i < n; i++) {
            int yy = top + ch - n + i, idx = off + i;
            fill(yy, 0, W, ks);
            if (i == n - 1 && idx < sz - 1) {
                text(1, yy, W, "... " + std::to_string(sz - idx) + " more", ks, false);
                break;
            }
            const Sugg& g = sk[(size_t)idx];
            Style rk = kk, re = ke, rv = kv;
            if (idx == sugg_sel) {
                for (Style* st : {&rk, &re, &rv}) st->attr |= A_REV;
                Style rs = ks;
                rs.attr |= A_REV;
                fill(yy, 0, W, rs);
            }
            int x2 = text(1, yy, W, g.word, rk, false);
            x2 = text(x2, yy, W, g.sep, re, false);
            text(x2, yy, W, g.val, rv, false);
        }
    }

    // status bar
    int y = H - 2, x = 0;
    Style ss = T.get("status"), sh = sty(T, "status_hi", "status");
    sh.fg = readable(sh.fg, sh.bg >= 0 ? sh.bg : ss.bg);  // the bar's bright text reads whatever the theme (tokyonight-day: 4.0:1)
    fill(y, 0, W, ss);
    auto seg = [&](const std::string& inner, Style is) {
        x = text(x, y, W, "[", ss, false);
        x = text(x, y, W, inner, is, false);
        x = text(x, y, W, "] ", ss, false);
    };
    x = text(x, y, W, " ", ss, false);
    seg(fmt_time(time(nullptr), "%H:%M"), sh);
    Server* s = b->srv;
    if (s) seg(s->nick + (s->umodes.empty() ? "" : "(+" + s->umodes + ")"), sh);
    seg(std::to_string(ai + 1) + ":" + (s && b->type != BT::STATUS ? (s->network.empty() ? s->cfg.name : s->network) + "/" : "") +
            buf_label(b) + (b->modes.empty() ? "" : "(+" + b->modes + ")"),
        sh);
    if (s && s->state == SS::CONNECTED && s->lag >= 1) {
        char lag[32];
        snprintf(lag, sizeof lag, "Lag %.1fs", s->lag);
        seg(lag, sh);
    } else if (s && s->state != SS::CONNECTED) {
        seg(s->state == SS::DISCONNECTED ? "disconnected" : "connecting", T.get("error"));
    }
    if (!b->typing.empty()) {  // +typing
        std::string who;
        for (auto& kv : b->typing) {
            auto it = b->nicks.find(kv.first);
            who += (who.empty() ? "" : ", ") + (it != b->nicks.end() ? it->second.nick : b->type == BT::QUERY ? b->name : kv.first);
        }
        seg(who + (b->typing.size() == 1 ? " is typing\u2026" : " are typing\u2026"), ss);
    }
    if (!c.update_ver.empty()) {  // check_updates found a newer release; a click opens /about
        Style us = sh;
        us.attr |= A_BOLD;
        int x0 = x;
        seg("\u2191v" + c.update_ver, us);
        hits.push_back({y, x0, x - 1, -2, ""});
    }
    std::string acts;
    std::vector<std::pair<int, int>> act;
    for (size_t i = 0; i < c.bufs.size(); i++)
        if (c.bufs[i]->activity) act.push_back({(int)i + 1, c.bufs[i]->activity});
    if (!act.empty()) {
        x = text(x, y, W, "[Act: ", ss, false);
        for (size_t i = 0; i < act.size(); i++) {
            Style as = act[i].second == 3 ? T.get("tab_hilight") : act[i].second == 2 ? T.get("tab_msg") : T.get("tab_data");
            as.bg = ss.bg;
            if (i) x = text(x, y, W, ",", as, false);
            int x0 = x;
            x = text(x, y, W, std::to_string(act[i].first), as, false);
            hits.push_back({y, x0, x, act[i].first - 1, ""});
        }
        x = text(x, y, W, "] ", ss, false);
    }
    if (b->scroll) {  // -- MORE --: the hilight's colors where they read on the status bar (github-dark's hilight text
                      // is near-black, made for its pink background), else the bar's bright text; a default bar: hilight
        Style hl = T.get("hilight"), ms = sh;
        ms.bg = ss.bg, ms.attr |= A_BOLD;
        if (ss.bg < 0) ms = hl;
        else {
            ms.fg = -1;
            for (RGB cand : {hl.bg, hl.fg, sh.fg})
                if (cand >= 0 && contrast(cand, ss.bg) >= 4.5) { ms.fg = cand; break; }
            if (ms.fg < 0) ms.fg = readable(sh.fg >= 0 ? sh.fg : hl.fg, ss.bg);
        }
        seg("-- MORE --", ms);
    }
    {  // status_info (uptime, cpu, ...), under the nick list (right edge without one); the last ones drop when it won't fit
        Style is = sty(T, "status_info", "status_hi");
        auto parts = status_info(c, is);
        for (; !parts.empty(); parts.pop_back()) {
            Row u;
            for (auto& p2 : parts) {
                if (!u.empty()) cells(u, " ", is, false);
                u.insert(u.end(), p2.begin(), p2.end());
            }
            int uw = width(u), ux = nlw && cw + 2 + uw <= W ? cw + 2 : W - uw - 1;
            if (ux > x) { draw_row(ux, y, W, u); break; }
        }
    }

    // nick menu, over everything but the input line
    if (!menu.items.empty()) {
        Style ms = sty(T, "menu", "nicklist"), ss2 = sty(T, "menu_sel", "tab_active"), sep = sty(T, "menu_border", "separator");
        if (sep.bg < 0) sep.bg = ms.bg;
        int mw = 0;
        for (auto& it : menu.items) mw = std::max(mw, (int)it.first.size() + 2);
        mw = std::max(mw, (int)menu.nick.size() + 2);
        int mh = (int)menu.items.size() + 1;
        menu.x = std::max(0, std::min(menu.x, W - mw)), menu.y = std::max(0, std::min(menu.y, H - 2 - mh));
        Style hs = ms;
        hs.attr |= A_BOLD;
        fill(menu.y, menu.x, menu.x + mw, hs);
        text(menu.x + 1, menu.y, menu.x + mw, menu.nick, hs, false);
        for (int i = 0; i < (int)menu.items.size(); i++) {
            int yy = menu.y + 1 + i;
            auto& it = menu.items[(size_t)i];
            Style st = i == menu.sel ? ss2 : ms;
            fill(yy, menu.x, menu.x + mw, st);
            if (it.first.empty()) for (int xx = menu.x; xx < menu.x + mw; xx++) cell(xx, yy, U'─', sep);
            else text(menu.x + 1, yy, menu.x + mw, it.first, st, false);
        }
    }

    // input line
    y = H - 1;
    Style is = T.get("input");
    fill(y, 0, W, is);
    if (tw.open) {  // /theme: the picker has the keys
        draw_themes(W, H);
        present();
        return;
    }
    if (fw.open) {  // Ctrl+G: the tab finder has the keys
        draw_find(W, H);
        present();
        return;
    }
    if (kw.open) {  // /keys, F1: the key list
        draw_keys(W, H);
        present();
        return;
    }
    if (sw.open) {  // /set: the settings window has the keys
        draw_settings(W, H);
        present();
        return;
    }
    if (!confirm.empty()) {  // a question, in the centered box like every prompt (the webchat's is the same)
        draw_dialog(W, H, confirm_title.empty() ? "iSekur" : confirm_title, wrap_words(confirm.substr(0, confirm.find('\n')), 44), "",
                    false, U"", 0, "y: yes · n or Esc: no", 0, 0, 'c');
        present();
        return;
    }
    if (!paste.empty()) {
        size_t n = 0;
        for (auto& l : split(paste, '\n')) n += !l.empty();
        draw_dialog(W, H, "Paste", wrap_words("Send " + std::to_string(n) + " lines to " + buf_label(b) + "?", 44), "", false, U"", 0,
                    "y: send · n or Esc: back to edit it", 0, 0, 'p');
        present();
        return;
    }
    if (Term* term = b->term.get(); term && !c.asking(b)) {  // keys go to the shell: the input line only says so
        text(0, y, W, "[" + buf_label(b) + "] ", T.get("hilight"), false);
        text((int)buf_label(b).size() + 3, y, W, term->host.empty() ? "keys go to the shell · Alt+←/→ or Alt+1-9: other tabs · wheel: scrollback · exit ends it"
                                                          : "keys go to the remote iSekur · Alt+←/→ or Alt+1-9: other tabs · Alt+D in it detaches it", sty(T, "timestamp", "input"), false);
        VTermPos p;
        vterm_state_get_cursorpos(vterm_obtain_state(term->vt), &p);
        if (term->cursor && b->scroll == 0 && p.row < pane.h && p.col < pane.w) tb_set_cursor(pane.x + p.col, pane.y + p.row);
        else tb_hide_cursor();
        present();
        return;
    }
    if (c.asking(b)) {  // /shell login, /e2e and /oper passwords: a dialog box
        draw_dialog(W, H, c.pass_title.empty() ? "Password" : c.pass_title, c.pass_info, c.pass_label, c.pass_hidden, input, cur, "Enter: OK · Esc: cancel", 0, 0, 'a');
        present();
        return;
    }
    int px = text(0, y, W, "[" + buf_label(b) + "] ", sty(T, "input_prompt", "input"), false);
    int avail = std::max(1, W - px - 1);
    std::u32string in = input;  // a NickServ password being typed: *s from where it starts
    {
        std::string u = to_utf8(input);
        size_t e, p = input_secret(b->name, u, &e);
        if (p != std::string::npos)
            for (size_t i = from_utf8(u.substr(0, p)).size(), n = e == std::string::npos ? in.size() : from_utf8(u.substr(0, e)).size(); i < n; i++) in[i] = U'*';
    }
    int curw = 0;
    for (size_t i = 0; i < cur; i++) curw += cwidth(in[i]);
    int off = std::max(0, curw - avail);
    int col = 0;
    for (size_t i = 0; i < in.size(); i++) {
        char32_t ch32 = in[i];
        int w = cwidth(ch32);
        if (col >= off && col - off + w <= avail) {
            if (ch32 == 3) {  // a color code: ◆ in the color its digits pick (dim until there are some)
                std::string code = "\x03";
                for (size_t j = i + 1; j < in.size() && j < i + 3 && in[j] >= U'0' && in[j] <= U'9'; j++) code += (char)in[j];
                Style d = sty(T, "timestamp", "input");
                if (code.size() > 1) mirc_walk(code + "x", is, [&](uint32_t, const Style& st) { d = is, d.fg = st.fg; });
                cell(px + col - off, y, U'◆', d);
            } else if (ch32 < 0x20) {  // other formatting codes show as reverse letters, like irssi
                Style r = is;
                r.attr |= A_REV;
                cell(px + col - off, y, ch32 == U'\n' ? U'↵' : '@' + ch32, r);
            } else cell(px + col - off, y, ch32, is);
        }
        col += w;
    }
    tb_set_cursor(px + curw - off, y);
    present();
}

// the password prompt over the middle of the screen:
//   ┌─ Shell login ──────────────────┐
//   │  login: ethernet               │
//   │  password: ********            │
//   │  Enter: OK · Esc: cancel       │
//   └────────────────────────────────┘
// label "": a yes/no box, the info lines and the hint, no field
void Tui::draw_dialog(int W, int H, const std::string& title, const std::vector<std::string>& info, const std::string& label,
                      bool hidden, const std::u32string& in, size_t at, const std::string& hint, int dx, int dy, char kind) {
    const Theme& T = c.theme;
    Style bs = sty(T, "menu", "nicklist"), fs = T.get("input"), es = T.get("error"), hs = bs, ns = sty(T, "timestamp", "menu"), ls = sty(T, "menu_border", "separator");
    for (Style* st : {&es, &ns, &ls}) st->bg = bs.bg;
    hs.attr |= A_BOLD;
    int bw = std::min(W - 2, 48), x0 = (W - bw) / 2, inner = bw - 4;
    bool field = !label.empty();
    int bh = (field ? 5 : 4) + (int)info.size(), y0 = std::max(0, (H - bh) / 2 - 1);
    x0 += dx, y0 += dy;  // the lock screen's animations move it
    for (int yy = y0; yy < y0 + bh; yy++) fill(yy, x0, x0 + bw, bs);
    for (int xx = x0; xx < x0 + bw; xx++) cell(xx, y0, U'─', ls), cell(xx, y0 + bh - 1, U'─', ls);
    for (int yy = y0; yy < y0 + bh; yy++) cell(x0, yy, U'│', ls), cell(x0 + bw - 1, yy, U'│', ls);
    cell(x0, y0, U'┌', ls), cell(x0 + bw - 1, y0, U'┐', ls), cell(x0, y0 + bh - 1, U'└', ls), cell(x0 + bw - 1, y0 + bh - 1, U'┘', ls);
    if (auto cb = box_title(T, x0, x0 + bw - 1, y0, title, hs); cb.first >= 0 && kind) xbtn = {y0, cb.first, cb.second, kind};
    int yy = y0 + 1;
    for (auto& l : info)
        text(x0 + 2, yy++, x0 + 2 + inner, !l.empty() && l[0] == '!' ? l.substr(1) : l, !l.empty() && l[0] == '!' ? es : bs, false);
    if (!field) {  // a yes/no: the hint under a blank line
        text(x0 + 2, yy + 1, x0 + 2 + inner, hint, ns, false);
        tb_hide_cursor();
        return;
    }
    int fy = yy, fx = text(x0 + 2, fy, x0 + 2 + inner, label + ": ", hs, false);
    int avail = std::max(1, x0 + 2 + inner - fx);
    fill(fy, fx, fx + avail, fs);
    // a password shows 1-3 dots per character, picked as it's typed: you see each key
    // land, someone watching can't count the length
    static std::minstd_rand rng{std::random_device{}()};
    while (pass_dots.size() < in.size()) pass_dots.push_back((uint8_t)(1 + rng() % 3));
    pass_dots.resize(in.size());
    std::u32string shown;
    if (!hidden) shown = in;
    else for (uint8_t d : pass_dots) shown.append(d, U'•');
    int curw = 0;
    if (hidden) curw = (int)shown.size();  // the cursor stays at the end
    else for (size_t i = 0; i < at; i++) curw += cwidth(in[i]);
    int off = std::max(0, curw - avail + 1), col = 0;
    for (char32_t ch32 : shown) {
        int w = cwidth(ch32);
        if (col >= off && col - off + w <= avail) cell(fx + col - off, fy, ch32 < 0x20 ? U' ' : ch32, fs);
        col += w;
    }
    text(x0 + 2, fy + 2, x0 + 2 + inner, hint, ns, false);
    if (fy >= 0) tb_set_cursor(fx + curw - off, fy);
    else tb_hide_cursor();
}

// the lock screen: a padlock over the password box. Core::lock_anim: 'l' both drop in from the top
// and the shackle snaps shut, 'f' (a wrong password) they shake and flash red, 'u' the shackle
// springs open (then present() opens the session from the middle out)
void Tui::draw_lock(int W, int H, long long ms) {
    const Theme& T = c.theme;
    std::vector<std::string> info;
    if (!c.lock_msg.empty()) info.push_back(c.lock_msg);
    int bh = 5 + (int)info.size(), y0 = std::max(0, (H - bh) / 2 - 1);
    int dx = 0, dy = 0, lift = 0;
    bool open = c.lock_anim == 'u', red = false;
    if (c.lock_anim == 'l' && ms < 650) {
        double t = ms / 650.0, u = t - 1;
        double e = 1 + 2.70158 * u * u * u + 1.70158 * u * u;  // ease out back: past the spot a little, then back
        dy = (int)std::lround((e - 1) * (y0 + bh + 6));
        lift = t < 0.75 ? 2 : t < 0.9 ? 1 : 0;  // the shackle closes as it lands
    } else if (c.lock_anim == 'f' && ms < 450) {
        double t = ms / 450.0;
        dx = (int)std::lround(3 * std::sin(t * 6 * M_PI) * (1 - t)), red = true;
    } else if (open) lift = ms < 120 ? 1 : 2;
    draw_dialog(W, H, "iSekur is locked", info, "password", true, lock_in, lock_in.size(), "Enter: unlock · Alt+D: detach", dx, dy);
    if (y0 < 7) return;  // no room for the padlock
    Style def = T.get("default"), ps = red ? T.get("error") : open ? T.get("join") : sty(T, "menu_border", "separator");
    ps.bg = def.bg, ps.attr |= A_BOLD;
    int px = (W - 9) / 2 + dx, py = y0 + dy - 6;
    text(px + 2, py - lift, px + 7, "╭───╮", ps, false);  // the shackle, its legs down to the body
    for (int y = py - lift + 1; y <= py + 1; y++) text(px + 2, y, px + 7, open && y == py + 1 ? "│    " : "│   │", ps, false);
    text(px, py + 2, px + 9, open ? "╭─┴─────╮" : "╭─┴───┴─╮", ps, false);
    text(px, py + 3, px + 9, red ? "│   ✕   │" : open ? "│   ○   │" : "│   ●   │", ps, false);
    text(px, py + 4, px + 9, "╰───────╯", ps, false);
}

// ---- the settings window (/set alone) ----------------------------------------
//   ┌─ Settings ────────────────────────────────────────────────┐
//   │ / search: log                                             │
//   │ All          12 │ ⇢ libera · Logs                         │
//   │ Global        5 │  log          ● on  ← overrides Global  │
//   │   Logs        5 │  log_history  inherits: 200             │
//   │ ⇢ libera      5 │  log_rotate   ● on  (Global)            │
//   │     Logs      5 │                                         │
//   │ ↑↓ move · e edit · Space on/off · d default · s save · q  │
//   └───────────────────────────────────────────────────────────┘
static std::string lower(std::string s) { for (auto& ch : s) ch = (char)tolower((unsigned char)ch); return s; }
static std::string full_key(const Core::SetRow& r) { return (r.sec == "general" ? "" : r.sec + ".") + r.key; }

// the categories with how many keys match the search, and the rows of the chosen one. Two levels:
// "general" (Global) over "general/Look"..., "server.x" over "server.x/Login"...; a network's topics show
// while it (or one of them) is chosen. cat: the chosen one's name ("" All), kept when the list folds
static std::string cat_parent(const std::string& c) { size_t sl = c.find('/'); return sl == std::string::npos ? "" : c.substr(0, sl); }
static int set_view(const std::vector<Core::SetRow>& all, const std::string& q, std::string& cat,
                    std::vector<std::string>& cats, std::vector<int>& counts, std::vector<const Core::SetRow*>& rows) {
    auto match = [&](const Core::SetRow& r) { return q.empty() || lower(full_key(r)).find(q) != std::string::npos || (!r.secret && lower(r.val).find(q) != std::string::npos); };
    auto in = [](const Core::SetRow& r, const std::string& c) { return c.empty() || r.cat == c || r.cat.rfind(c + "/", 0) == 0; };
    std::vector<std::string> full = {""};
    for (auto& r : all) {
        std::string p = cat_parent(r.cat);
        if (!p.empty() && std::find(full.begin(), full.end(), p) == full.end()) full.push_back(p);
        if (std::find(full.begin(), full.end(), r.cat) == full.end()) full.push_back(r.cat);
    }
    std::string open = cat_parent(cat).empty() ? cat : cat_parent(cat);  // the network whose topics show
    cats.clear(), counts.clear();
    for (auto& c : full) {
        std::string p = cat_parent(c);
        if (p.rfind("server.", 0) == 0 && p != open) continue;
        int n = 0;
        for (auto& r : all) n += match(r) && in(r, c);
        cats.push_back(c), counts.push_back(n);
    }
    int at = (int)(std::find(cats.begin(), cats.end(), cat) - cats.begin());
    if (at >= (int)cats.size() || !counts[(size_t)at]) cat.clear(), at = 0;  // gone, or a search with no hits here: All
    rows.clear();
    for (auto& r : all)
        if (match(r) && in(r, cat)) rows.push_back(&r);
    return at;
}
static std::string cat_label(const std::string& c) {
    if (c.empty()) return "All";
    if (c == "general") return "Global";
    if (c.rfind("server.", 0) == 0 && cat_parent(c).empty()) return "⇢ " + c.substr(7);
    if (!cat_parent(c).empty()) return (cat_parent(c) == "general" ? "  " : "    ") + c.substr(c.find('/') + 1);
    return c == "web" ? "Web" : c == "keys" ? "Keys" : c;
}

// the server the selected key or category is about ("" for none): its keys, or a "server.x" category
static std::string set_server(const std::string& cat, const Core::SetRow* r) {
    if (r && r->sec.rfind("server.", 0) == 0) return r->sec.substr(7);
    std::string p = cat_parent(cat).empty() ? cat : cat_parent(cat);
    return p.rfind("server.", 0) == 0 ? p.substr(7) : "";
}

void Tui::draw_settings(int W, int H) {
    const Theme& T = c.theme;
    Style bs = sty(T, "menu", "nicklist"), ls = sty(T, "menu_border", "separator"), hs = bs, dim = sty(T, "timestamp", "menu"),
          sel = sty(T, "menu_sel", "tab_active"), fs = T.get("input"), keyst = sty(T, "set_key", "info"), val = sty(T, "set_value", "input"),
          secst = sty(T, "info", "menu"), mark = sty(T, "hilight", "menu");
    for (Style* st : {&ls, &dim, &keyst, &val, &secst}) st->bg = bs.bg;
    mark.fg = sty(T, "input_prompt", "hilight").fg, mark.bg = bs.bg;
    hs.attr |= A_BOLD;
    int bw = std::min(W - 2, 110), bh = std::min(H - 2, 34);
    if (bw < 40 || bh < 10) { text(0, H - 1, W, "The terminal is too small for the settings window (Esc closes it)", T.get("error"), false); return; }
    int x0 = (W - bw) / 2, y0 = (H - bh) / 2, x1 = x0 + bw - 1, y1 = y0 + bh - 1;
    for (int yy = y0; yy <= y1; yy++) fill(yy, x0, x1 + 1, bs);
    for (int xx = x0; xx <= x1; xx++) cell(xx, y0, U'─', ls), cell(xx, y1, U'─', ls);
    for (int yy = y0; yy <= y1; yy++) cell(x0, yy, U'│', ls), cell(x1, yy, U'│', ls);
    cell(x0, y0, U'┌', ls), cell(x1, y0, U'┐', ls), cell(x0, y1, U'└', ls), cell(x1, y1, U'┘', ls);
    if (auto cb = box_title(T, x0, x1, y0, "Settings", hs); cb.first >= 0) xbtn = {y0, cb.first, cb.second, 's'};

    auto all = c.setting_rows();
    std::vector<std::string> cats;
    std::vector<int> counts;
    std::vector<const Core::SetRow*> rows;
    if (sw.to_servers) {
        sw.to_servers = false;
        for (auto& r : all)
            if (r.sec.rfind("server.", 0) == 0) { sw.cat = r.sec, sw.focus = 0, sw.sel = sw.top = 0; break; }
        if (sw.cat.empty()) sw.msg = "No servers yet: n adds one";
    }
    int ci = set_view(all, lower(to_utf8(sw.q)), sw.cat, cats, counts, rows);
    sw.sel = std::max(0, std::min(sw.sel, (int)rows.size() - 1));

    // search line: "/" starts it, letters are commands otherwise
    int sx = text(x0 + 2, y0 + 1, x1, "/ search: ", sw.searching ? keyst : dim, false);
    if (sw.searching || !sw.q.empty()) fill(y0 + 1, sx, x1 - 1, sw.searching ? fs : bs);
    text(sx, y0 + 1, x1 - 1, sw.q.empty() && !sw.searching ? "type / to search every key and value" : to_utf8(sw.q), sw.q.empty() && !sw.searching ? dim : sw.searching ? fs : val, false);
    if (sw.searching) tb_set_cursor(sx + (int)sw.q.size(), y0 + 1);
    else if (!sw.editing) tb_hide_cursor();

    // categories
    int nw = 22, ly = y0 + 3, n = bh - 6;  // n rows for the lists
    sw.x0 = x0, sw.y0 = y0, sw.w = bw, sw.h = bh, sw.nav_w = nw, sw.list_y = ly;
    if (ci < sw.ctop) sw.ctop = ci;  // the chosen category in sight
    if (ci >= sw.ctop + n) sw.ctop = ci - n + 1;
    sw.ctop = std::max(0, std::min(sw.ctop, (int)cats.size() - n));
    for (int j = 0; j < n && sw.ctop + j < (int)cats.size(); j++) {
        int i = sw.ctop + j;
        const std::string& cn = cats[(size_t)i];
        std::string name = cat_label(cn);
        bool on = i == ci, uns = false;
        for (auto& r : all) uns |= r.unsaved && (cn.empty() || r.cat == cn || r.cat.rfind(cn + "/", 0) == 0);
        Style st = on ? sel : counts[(size_t)i] ? bs : dim;
        if (on && sw.focus != 0) st = keyst, st.attr |= A_BOLD;  // chosen, but the keys have the focus
        if (cn == "general" || (cn.rfind("server.", 0) == 0 && cat_parent(cn).empty())) st.attr |= A_BOLD;  // a level's top
        fill(ly + j, x0 + 2, x0 + 2 + nw - 1, on && sw.focus == 0 ? sel : bs);
        text(x0 + 3, ly + j, x0 + nw - 5, name, st, false);
        std::string cnt = (uns ? "\u25CF " : "") + std::to_string(counts[(size_t)i]);
        int cl = (int)cnt.size() - (uns ? 2 : 0);
        text(x0 + 2 + nw - 2 - cl, ly + j, x0 + 2 + nw, cnt, on && sw.focus == 0 ? sel : dim, false);
    }
    for (int yy = ly - 1; yy < ly + n; yy++) cell(x0 + 2 + nw, yy, U'│', ls);

    // keys: with section headers when showing All (or a search across sections)
    struct DL { const Core::SetRow* r; std::string head; };
    std::vector<DL> lines;
    std::string last;
    int sel_line = 0;
    for (size_t i = 0; i < rows.size(); i++) {
        if (rows[i]->head != last) lines.push_back({nullptr, rows[i]->head}), last = rows[i]->head;
        if ((int)i == sw.sel) sel_line = (int)lines.size();
        lines.push_back({rows[i], ""});
    }
    if (sel_line < sw.top + 1) sw.top = std::max(0, sel_line - 1);  // keep its header in sight
    if (sel_line >= sw.top + n) sw.top = sel_line - n + 1;
    int kx = x0 + nw + 4, kw = 0;
    for (auto* r : rows) kw = std::max(kw, (int)r->key.size());
    kw = std::min(kw + 3, 26);
    int vx = kx + kw, vmax = x1 - 1;
    if (rows.empty()) text(kx, ly, vmax, "No key matches \u201c" + to_utf8(sw.q) + "\u201d (Esc clears the search)", dim, false);
    for (int i = 0; i < n && sw.top + i < (int)lines.size(); i++) {
        auto& L = lines[(size_t)(sw.top + i)];
        int yy = ly + i;
        if (!L.r) { text(kx, yy, vmax, L.head, secst, false); continue; }
        bool on = L.r == (rows.empty() ? nullptr : rows[(size_t)sw.sel]) && sw.focus == 1;
        Style ks = on ? sel : keyst;
        if (on) fill(yy, kx - 1, vmax + 1, sel);
        if (L.r->unsaved) text(kx - 1, yy, kx, "\u25CF", on ? sel : mark, false);
        text(kx + 1, yy, vx - 1, L.r->key, ks, false);
        if (on && sw.editing) {  // the value being edited, with its cursor
            fill(yy, vx, vmax, fs);
            std::u32string shown = L.r->secret ? std::u32string(sw.ed.size(), U'\u2022') : sw.ed;
            int avail = vmax - vx - 1, off = std::max(0, (int)sw.ecur - avail);
            text(vx, yy, vmax, to_utf8(shown.substr((size_t)off)), fs, false);
            tb_set_cursor(vx + (int)sw.ecur - off, yy);
            continue;
        }
        std::string eff = L.r->val.empty() ? L.r->def : L.r->val;
        // where the value comes from: a network's own (overrides Global), Global's (inherited), the default
        bool inherits = L.r->shared && L.r->val.empty(), overrides = L.r->shared && !L.r->val.empty();
        std::string from = inherits ? "  (Global)" : overrides ? "  \u2190 overrides Global" : L.r->val.empty() ? "  (default)" : "";
        if (!L.r->over.empty()) from += "  (" + L.r->over + ")";
        if (L.r->choices.size() == 2 && (L.r->choices[0] == "on" || L.r->choices[0] == "true" || L.r->choices[0] == "yes")) {  // on/off: ● on, ○ off, and where it comes from
            bool yes = eff == L.r->choices[0];
            Style ts = on ? sel : yes && !inherits ? val : dim;
            int ex = text(vx, yy, vmax, std::string(yes ? "\u25CF " : "\u25CB ") + eff, ts, false);
            ex = text(ex, yy, vmax, from, on ? sel : dim, false);
            if (on) text(ex, yy, vmax, inherits ? "   Space: override" : overrides ? "   Space: switch · d: Global's" : "   Space: switch", sel, false);
            continue;
        }
        std::string over = L.r->over.empty() ? "" : "  (" + L.r->over + ")";
        std::string v = !L.r->val.empty() ? (L.r->secret ? "********" : L.r->val) + (overrides ? "  \u2190 overrides Global" : "") + over
                        : L.r->secret || L.r->def.empty() ? "(not set)" + over
                                                          : (inherits ? "inherits: " : "default: ") + L.r->def + over;
        int ex = text(vx, yy, vmax, v, on ? sel : L.r->val.empty() ? dim : val, false);
        if (on && !L.r->choices.empty()) {  // the themes: where this one is among them
            std::string list;
            for (auto& ch : L.r->choices) list += (list.empty() ? "" : " ") + (ch == eff ? "[" + ch + "]" : ch);
            text(ex, yy, vmax, "   Space: next  " + list, sel, false);
        }
    }
    // what happened, or what the keys do
    const Core::SetRow* hr = rows.empty() ? nullptr : rows[(size_t)std::max(0, std::min(sw.sel, (int)rows.size() - 1))];
    bool on_srv = !set_server(sw.cat, sw.focus == 1 ? hr : nullptr).empty();
    std::string hint = sw.editing     ? "Enter: set it · Esc: cancel · empty = the default"
                       : sw.searching ? "type to search · Enter or \u2193: to the list · Esc: clear it"
                       : on_srv ? "c: connect · r: rename · x: delete server · n: new server · e Enter: edit · s: save · q: close"
                       : sw.focus == 0 ? "\u2191\u2193: category · \u2192 Tab Enter: its keys · n: new server · /: search · q Esc: close"
                                       : "\u2191\u2193: move · e Enter: edit · Space: on/off, next choice · d: default · /: search · s: save · q: close";
    text(x0 + 2, y1 - 1, x1 - 1, sw.msg.empty() ? hint : sw.msg, sw.msg.empty() ? dim : mark, false);
}

void Tui::settings_key(int mod, int k, uint32_t ch) {
    auto all = c.setting_rows();
    std::vector<std::string> cats;
    std::vector<int> counts;
    std::vector<const Core::SetRow*> rows;
    int ci = set_view(all, lower(to_utf8(sw.q)), sw.cat, cats, counts, rows);
    const Core::SetRow* r = rows.empty() ? nullptr : rows[(size_t)std::max(0, std::min(sw.sel, (int)rows.size() - 1))];
    bool bs = k == TB_KEY_BACKSPACE || k == TB_KEY_BACKSPACE2;
    if (sw.editing) {
        if (k == TB_KEY_ESC) { sw.editing = false; return; }
        if (k == TB_KEY_ENTER && r) {
            std::string v = to_utf8(sw.ed);
            c.exec(c.active, "/set " + full_key(*r) + " " + (v.empty() ? "\"\"" : v));
            sw.editing = false;
            sw.msg = r->key == "name" && r->sec.rfind("server.", 0) == 0 ? (v.empty() ? "A server needs a name" : "Renamed to " + v + " (saved)")
                     : full_key(*r) + (v.empty() ? (r->shared ? " uses Global's again" : " is back to its default") : " = " + (r->secret ? std::string("********") : v)) + "  (not saved yet: s)";
            return;
        }
        if (ch || k == TB_KEY_SPACE) sw.ed.insert(sw.ecur++, 1, ch ? (char32_t)ch : U' ');
        else if (bs && sw.ecur) sw.ed.erase(--sw.ecur, 1);
        else if (k == TB_KEY_DELETE && sw.ecur < sw.ed.size()) sw.ed.erase(sw.ecur, 1);
        else if (k == TB_KEY_ARROW_LEFT && sw.ecur) sw.ecur--;
        else if (k == TB_KEY_ARROW_RIGHT && sw.ecur < sw.ed.size()) sw.ecur++;
        else if (k == TB_KEY_HOME) sw.ecur = 0;
        else if (k == TB_KEY_END || k == TB_KEY_CTRL_E) sw.ecur = sw.ed.size();
        else if (k == TB_KEY_CTRL_U) sw.ed.erase(0, sw.ecur), sw.ecur = 0;
        return;
    }
    sw.msg.clear();
    int page = std::max(1, sw.h - 7);
    if (mod & (TB_MOD_ALT | TB_MOD_SUPER)) return;
    if (sw.searching) {  // after "/": the keys go to the search
        if (k == TB_KEY_ESC) sw.q.clear(), sw.searching = false;
        else if (k == TB_KEY_ENTER || k == TB_KEY_ARROW_DOWN || k == TB_KEY_TAB) sw.searching = false, sw.focus = 1;
        else if (bs) { if (!sw.q.empty()) sw.q.pop_back(); else sw.searching = false; }
        else if (ch || k == TB_KEY_SPACE) sw.q += ch ? (char32_t)ch : U' ';
        sw.sel = sw.top = 0;
        return;
    }
    if (k == TB_KEY_ESC) { if (!sw.q.empty()) sw.q.clear(); else sw.open = false; return; }
    if (ch == 'q') { sw.open = false; return; }
    if (ch == '/') { sw.searching = true; return; }
    if (k == TB_KEY_CTRL_S || ch == 's') {
        c.exec(c.active, "/save");
        sw.msg = "Saved to " + c.cfg_path;
        return;
    }
    // servers, like the webchat's Servers window: c connects the selected one, x deletes it (asks first), n adds one
    std::string srv = set_server(sw.cat, r);
    if (ch == 'c' && !srv.empty()) { c.exec(c.active, "/connect " + srv); sw.msg = "Connecting " + srv; return; }
    if (ch == 'r' && !srv.empty()) { sw.open = false; c.exec(c.active, "/server rename " + srv); return; }
    if (ch == 'x' && !srv.empty()) {
        sw.open = false;
        confirm = "Delete " + srv + ": disconnect, close its tabs and remove it from the config?\n/server del " + srv, confirm_title = "Delete server";
        return;
    }
    if (ch == 'n') {
        sw.open = false;
        c.ask_pass(c.active, "server", [this](const std::string& v) { if (!v.empty()) c.exec(c.active, "/server " + v); }, false, "New server",
                   {"host [port] [-tls] [-noverify] [-name name] [-nick nick]", "e.g. irc.sekurnet.org 6697 -tls -name sekurnet"});
        return;
    }
    if (ch == 'e') k = TB_KEY_ENTER, ch = 0;
    if (ch == 'd') k = TB_KEY_DELETE, ch = 0;
    if ((k == TB_KEY_SPACE || ch == ' ') && sw.focus == 1 && r && !r->choices.empty()) {  // on <-> off, the next theme: right away
        std::string eff = r->val.empty() ? r->def : r->val;
        auto it = std::find(r->choices.begin(), r->choices.end(), eff);
        std::string next = it == r->choices.end() || it + 1 == r->choices.end() ? r->choices[0] : *(it + 1);
        c.exec(c.active, "/set " + full_key(*r) + " " + next);
        sw.msg = full_key(*r) + " = " + next + "  (not saved yet: s)";
        return;
    }
    if (ch || k == TB_KEY_SPACE) return;  // other letters do nothing here
    if (bs) return;
    if (k == TB_KEY_TAB) { sw.focus = !sw.focus; return; }
    if (k == TB_KEY_ARROW_LEFT) { sw.focus = 0; return; }
    if (k == TB_KEY_ARROW_RIGHT) { sw.focus = 1; return; }
    if (sw.focus == 0) {  // categories: the empty ones are skipped
        int d = k == TB_KEY_ARROW_DOWN ? 1 : k == TB_KEY_ARROW_UP ? -1 : 0, nc = (int)cats.size();
        for (int i = ci + d; d && i >= 0 && i < nc; i += d)
            if (counts[(size_t)i]) { sw.cat = cats[(size_t)i], sw.sel = sw.top = 0; break; }
        if (k == TB_KEY_ENTER) sw.focus = 1;
        return;
    }
    int n = (int)rows.size();
    if (k == TB_KEY_ARROW_DOWN) sw.sel = std::min(n - 1, sw.sel + 1);
    else if (k == TB_KEY_ARROW_UP) sw.sel = std::max(0, sw.sel - 1);
    else if (k == TB_KEY_PGDN) sw.sel = std::min(n - 1, sw.sel + page);
    else if (k == TB_KEY_PGUP) sw.sel = std::max(0, sw.sel - page);
    else if (k == TB_KEY_HOME) sw.sel = 0;
    else if (k == TB_KEY_END) sw.sel = std::max(0, n - 1);
    else if (k == TB_KEY_ENTER && r) sw.editing = true, sw.ed = r->secret ? U"" : from_utf8(r->val), sw.ecur = sw.ed.size();
    else if (k == TB_KEY_DELETE && r && r->key == "name" && r->sec.rfind("server.", 0) == 0) sw.msg = "A server needs a name: r renames it, x deletes it";
    else if (k == TB_KEY_DELETE && r) {
        c.exec(c.active, "/set " + full_key(*r) + " \"\"");
        sw.msg = full_key(*r) + (r->shared ? " uses Global's again" : " is back to its default") + "  (not saved yet: s)";
    }
}

// ---- the theme picker (/theme alone) -------------------------------------------
//   ┌─ Theme ────────────────────────────────┐
//   │ filter: mi                             │
//   │ ▸ mirc                                 │
//   │   mirc-dark                            │
//   │ ↑↓ preview · Enter keep · Esc back     │
//   └────────────────────────────────────────┘
std::vector<std::string> Tui::theme_list() const {
    std::string q = lower(to_utf8(tw.q));
    std::vector<std::string> out;
    for (auto& n : theme_names())
        if (lower(n).find(q) != std::string::npos) out.push_back(n);
    return out;
}

void Tui::draw_themes(int W, int H) {
    const Theme& T = c.theme;
    Style bs = sty(T, "menu", "nicklist"), ls = sty(T, "menu_border", "separator"), hs = bs, dim = sty(T, "timestamp", "menu"),
          sel = sty(T, "menu_sel", "tab_active"), fs = T.get("input");
    for (Style* st : {&ls, &dim}) st->bg = bs.bg;
    hs.attr |= A_BOLD;
    auto l = theme_list();
    int n = (int)l.size();
    tw.rows = std::max(1, std::min({std::max(n, 1), H - 6, 14}));
    tw.w = std::min(W - 2, 56), tw.x0 = (W - tw.w) / 2;
    int bh = tw.rows + 4, y0 = std::max(0, (H - bh) / 2 - 1), x1 = tw.x0 + tw.w;
    tw.list_y = y0 + 2;
    if (tw.sel >= n) tw.sel = std::max(0, n - 1);
    if (tw.sel < tw.top) tw.top = tw.sel;
    if (tw.sel >= tw.top + tw.rows) tw.top = tw.sel - tw.rows + 1;
    for (int yy = y0; yy < y0 + bh; yy++) fill(yy, tw.x0, x1, bs);
    for (int xx = tw.x0; xx < x1; xx++) cell(xx, y0, U'─', ls), cell(xx, y0 + bh - 1, U'─', ls);
    for (int yy = y0; yy < y0 + bh; yy++) cell(tw.x0, yy, U'│', ls), cell(x1 - 1, yy, U'│', ls);
    cell(tw.x0, y0, U'┌', ls), cell(x1 - 1, y0, U'┐', ls), cell(tw.x0, y0 + bh - 1, U'└', ls), cell(x1 - 1, y0 + bh - 1, U'┘', ls);
    if (auto cb = box_title(T, tw.x0, x1 - 1, y0, "Theme", hs); cb.first >= 0) xbtn = {y0, cb.first, cb.second, 't'};
    int fx = text(tw.x0 + 2, y0 + 1, x1 - 2, "filter: ", hs, false);
    fill(y0 + 1, fx, x1 - 2, fs);
    int ex = text(fx, y0 + 1, x1 - 2, to_utf8(tw.q), fs, false);
    tb_set_cursor(ex, y0 + 1);
    if (!n) text(tw.x0 + 2, tw.list_y, x1 - 2, "no theme matches", dim, false);
    for (int i = 0; i < tw.rows && tw.top + i < n; i++) {
        const std::string& name = l[(size_t)(tw.top + i)];
        bool on = tw.top + i == tw.sel;
        Style st = on ? sel : bs;
        fill(tw.list_y + i, tw.x0 + 1, x1 - 1, st);
        int nx = text(tw.x0 + 2, tw.list_y + i, x1 - 2, (on ? "▸ " : "  ") + name, st, false);
        if (name == tw.orig) { Style d = dim; d.bg = st.bg; text(nx + 1, tw.list_y + i, x1 - 2, "(was)", d, false); }
    }
    text(tw.x0 + 2, y0 + bh - 2, x1 - 2, "↑↓ preview · type: filter · Enter keep · Esc back", dim, false);
}

void Tui::theme_key(int k, uint32_t ch) {
    auto l = theme_list();
    int n = (int)l.size();
    bool bs = k == TB_KEY_BACKSPACE || k == TB_KEY_BACKSPACE2;
    if (k == TB_KEY_ESC) {  // the filter first, then back to the theme it had
        if (!tw.q.empty()) { tw.q.clear(); l = theme_list(); tw.sel = std::max(0, (int)(std::find(l.begin(), l.end(), c.theme.name) - l.begin())); return; }
        std::string err;
        if (c.theme.name != tw.orig) c.use_theme(tw.orig, err);
        tw.open = false;
        return;
    }
    if (k == TB_KEY_ENTER) {
        tw.open = false;
        if (c.theme.name != tw.orig) c.exec(c.active, "/set theme " + c.theme.name);  // /save keeps it
        return;
    }
    if (ch || k == TB_KEY_SPACE) tw.q += ch ? (char32_t)ch : U' ', tw.sel = tw.top = 0;
    else if (bs) { if (tw.q.empty()) return; tw.q.pop_back(), tw.sel = tw.top = 0; }
    else if (k == TB_KEY_ARROW_DOWN) tw.sel = std::min(n - 1, tw.sel + 1);
    else if (k == TB_KEY_ARROW_UP) tw.sel = std::max(0, tw.sel - 1);
    else if (k == TB_KEY_PGDN) tw.sel = std::min(n - 1, tw.sel + tw.rows);
    else if (k == TB_KEY_PGUP) tw.sel = std::max(0, tw.sel - tw.rows);
    else if (k == TB_KEY_HOME) tw.sel = 0;
    else if (k == TB_KEY_END) tw.sel = n - 1;
    else return;
    l = theme_list(), n = (int)l.size();
    tw.sel = std::max(0, std::min(tw.sel, n - 1));
    std::string err;
    if (n && l[(size_t)tw.sel] != c.theme.name) c.use_theme(l[(size_t)tw.sel], err);  // the live preview
}

// /keys: the key list. A row is a group header (desc empty) or the keys of one action ("M-Right C-n") and what it does
struct KeyRow { std::string head, specs, desc; };
static std::vector<KeyRow> key_rows(const std::map<std::string, std::string>& keymap) {
    static const std::vector<std::pair<const char*, std::vector<std::pair<const char*, const char*>>>> GROUPS = {
        {"Tabs", {{"next_tab", "next tab"}, {"prev_tab", "previous tab"}, {"next_active", "the busiest tab"},
                  {"next_server", "next server tab"}, {"find_tab", "find a tab by name"},
                  {"other_pane", "the other /split pane"}, {"close_tab", "close the tab"}}},
        {"View", {{"scroll_up", "scroll up half a page"}, {"scroll_down", "scroll down half a page"}, {"toggle_nicklist", "show or hide the nick list"},
                  {"toggle_topic", "show or hide the topic bar"}, {"toggle_mouse", "mouse on / off"}, {"keys", "this list"}}},
        {"Session", {{"detach", "detach (stays connected)"}, {"quit", "quit (asks first)"}}},
    };
    std::map<std::string, std::string> by;  // action -> its keys; Option-symbol twins (¬ ∆...) left out
    for (auto& [sp, act] : keymap)
        if ((unsigned char)sp[0] < 0x80) by[act] += (by[act].empty() ? "" : " ") + sp;
    std::vector<KeyRow> out;
    for (auto& [g, acts] : GROUPS) {
        out.push_back({g, "", ""});
        if (std::string(g) == "Tabs") {  // Alt+1..0 in one row; a tab_N bound elsewhere gets its own
            out.push_back({"", "M-1…0", "go to tab 1 to 10"});
            for (int i = 1; i <= 10; i++) {
                std::string a = "tab_" + std::to_string(i), own;
                for (auto& sp : split(by[a], ' ')) if (sp != "M-" + std::to_string(i % 10)) own += (own.empty() ? "" : " ") + sp;
                if (!own.empty()) out.push_back({"", own, "go to tab " + std::to_string(i)});
            }
        }
        for (auto& [a, d] : acts)
            if (!by[a].empty()) out.push_back({"", by[a], d});
    }
    out.push_back({"Typing", "", ""});
    for (auto& r : std::vector<KeyRow>{
             {"", "Enter", "send the line"}, {"", "Tab", "complete nick or command"}, {"", "Up Down", "earlier lines you typed"},
             {"", "Home End", "start / end of the line"}, {"", "C-u C-k", "delete to start / end"}, {"", "M-BS", "delete the previous word"},
             {"", "C-b C-c C-o", "bold, color, reset"}, {"", "C-v C-] C-_", "reverse, italic, underline"}})
        out.push_back(r);
    return out;
}

// "M-Right" -> {"Alt", "→"}: the keycaps of one key
static std::vector<std::string> key_caps(const std::string& sp) {
    static const std::map<std::string, std::string> NAMES = {{"Right", "→"}, {"Left", "←"}, {"Up", "↑"}, {"Down", "↓"}, {"BS", "⌫"}, {"Esc", "Esc"}};
    std::vector<std::string> caps;
    size_t i = 0;
    for (; i + 1 < sp.size() && sp[i + 1] == '-' && sp.size() > i + 2; i += 2)
        caps.push_back(sp[i] == 'M' ? "Alt" : sp[i] == 'C' ? "Ctrl" : sp[i] == 'S' ? "Shift" : sp[i] == 'D' ? "Cmd" : std::string(1, sp[i]));
    std::string k = sp.substr(i);
    auto n = NAMES.find(k);
    caps.push_back(n != NAMES.end() ? n->second : k.size() == 1 ? std::string(1, (char)toupper((unsigned char)k[0])) : k);
    return caps;
}

void Tui::draw_keys(int W, int H) {
    const Theme& T = c.theme;
    Style bs = sty(T, "menu", "nicklist"), ls = sty(T, "menu_border", "separator"), hs = bs, dim = sty(T, "timestamp", "menu"),
          cap = sty(T, "menu_sel", "tab_active"), grp = sty(T, "hilight", "menu");
    for (Style* st : {&ls, &dim, &grp}) st->bg = bs.bg;
    grp.fg = readable(grp.fg, bs.bg), dim.fg = readable(dim.fg, bs.bg);
    hs.attr |= A_BOLD, grp.attr |= A_BOLD, cap.attr |= A_BOLD;
    auto rows = key_rows(keymap);
    kw.n = (int)rows.size();
    kw.rows = std::max(1, std::min(kw.n, H - 5));
    auto caps_w = [&](const std::string& sp, bool first) {  // " Alt "+" → ", " / " before an alternative
        int w = first ? -1 : 2;
        for (auto& k : key_caps(sp)) { Row rr; cells(rr, k, cap, false); w += width(rr) + 3; }
        return w;
    };
    int need = 0, dw = 0;  // the widest keycaps and description, for the box's width
    for (auto& r : rows) {
        int w = 0;
        bool first = true;
        for (auto& sp : split(r.specs, ' ')) w += caps_w(sp, first), first = false;
        need = std::max(need, w), dw = std::max(dw, (int)from_utf8(r.desc).size());
    }
    kw.w = std::min(W - 2, need + dw + 7), kw.x0 = (W - kw.w) / 2;
    kw.top = std::max(0, std::min(kw.top, kw.n - kw.rows));
    int bh = kw.rows + 3, y0 = std::max(0, (H - bh) / 2 - 1), x1 = kw.x0 + kw.w, capx = kw.x0 + 3,
        descx = capx + std::min(need, std::max(12, kw.w - 5 - dw)) + 2;
    kw.list_y = y0 + 1;
    for (int yy = y0; yy < y0 + bh; yy++) fill(yy, kw.x0, x1, bs);
    for (int xx = kw.x0; xx < x1; xx++) cell(xx, y0, U'─', ls), cell(xx, y0 + bh - 1, U'─', ls);
    for (int yy = y0; yy < y0 + bh; yy++) cell(kw.x0, yy, U'│', ls), cell(x1 - 1, yy, U'│', ls);
    cell(kw.x0, y0, U'┌', ls), cell(x1 - 1, y0, U'┐', ls), cell(kw.x0, y0 + bh - 1, U'└', ls), cell(x1 - 1, y0 + bh - 1, U'┘', ls);
    if (auto cb = box_title(T, kw.x0, x1 - 1, y0, "Keys", hs); cb.first >= 0) xbtn = {y0, cb.first, cb.second, 'k'};
    for (int i = 0; i < kw.rows && kw.top + i < kw.n; i++) {
        auto& r = rows[(size_t)(kw.top + i)];
        int y = kw.list_y + i;
        if (r.specs.empty()) {  // a group: its name and a rule to the edge
            int ex = text(kw.x0 + 2, y, x1 - 2, r.head, grp, false);
            for (int xx = ex + 1; xx < x1 - 2; xx++) cell(xx, y, U'─', ls);
            continue;
        }
        // the keycaps: " Alt "+" → ", alternatives apart; what doesn't fit before the description is left out
        int x = capx;
        bool first = true;
        for (auto& sp : split(r.specs, ' ')) {
            auto caps = key_caps(sp);
            if (x + caps_w(sp, first) + (first ? 1 : 0) > descx - 1) break;
            if (!first) x = text(x, y, descx, " / ", dim, false);
            for (size_t j = 0; j < caps.size(); j++) {
                if (j) x = text(x, y, descx, "+", dim, false);
                x = text(x, y, descx, " " + caps[j] + " ", cap, false);
            }
            first = false;
        }
        text(descx, y, x1 - 2, r.desc, bs, false);
    }
    std::string more = kw.n > kw.rows ? "↑↓ scroll · " : "";
    text(kw.x0 + 2, y0 + bh - 2, x1 - 2, more + "Esc close · /help keys", dim, false);
    tb_hide_cursor();
}

void Tui::keys_key(int k, uint32_t ch) {
    int last = std::max(0, kw.n - kw.rows);
    if (k == TB_KEY_ARROW_DOWN) kw.top = std::min(last, kw.top + 1);
    else if (k == TB_KEY_ARROW_UP) kw.top = std::max(0, kw.top - 1);
    else if (k == TB_KEY_PGDN || k == TB_KEY_SPACE) kw.top = std::min(last, kw.top + kw.rows);
    else if (k == TB_KEY_PGUP) kw.top = std::max(0, kw.top - kw.rows);
    else if (k == TB_KEY_HOME) kw.top = 0;
    else if (k == TB_KEY_END) kw.top = last;
    else kw.open = false;  // Esc, q, Enter, F1 or Alt+K again: anything else closes
}

// every word typed must be in the tab's name, its network, or its kind ("server", "query"...)
std::vector<Buffer*> Tui::find_list() const {
    auto words = split(irc_lower(to_utf8(fw.q)), ' ');
    std::vector<Buffer*> out;
    for (auto& bp : c.bufs) {
        Buffer* b = bp.get();
        static const char* KIND[] = {"server", "channel", "query", "shell"};
        std::string hay = irc_lower(buf_label(b) + " " + b->name + " " + (b->srv ? b->srv->cfg.name + " " + b->srv->network : "") +
                                    " " + (b->type == BT::STATUS && !b->srv ? "cmd console" : KIND[(int)b->type]));
        bool ok = true;
        for (auto& w : words) ok = ok && (w.empty() || hay.find(w) != std::string::npos);
        if (ok) out.push_back(b);
    }
    std::stable_partition(out.begin(), out.end(), [&](Buffer* b) {  // the tab's own name first, then network-only matches
        std::string n = irc_lower(buf_label(b));
        for (auto& w : words) if (n.find(w) == std::string::npos) return false;
        return true;
    });
    return out;
}

void Tui::draw_find(int W, int H) {
    const Theme& T = c.theme;
    Style bs = sty(T, "menu", "nicklist"), ls = sty(T, "menu_border", "separator"), hs = bs, dim = sty(T, "timestamp", "menu"),
          sel = sty(T, "menu_sel", "tab_active"), fs = T.get("input");
    for (Style* st : {&ls, &dim}) st->bg = bs.bg;
    hs.attr |= A_BOLD;
    auto l = find_list();
    int n = (int)l.size();
    fw.rows = std::max(1, std::min({std::max(n, 1), H - 6, 14}));
    fw.w = std::min(W - 2, 56), fw.x0 = (W - fw.w) / 2;
    int bh = fw.rows + 4, y0 = std::max(0, (H - bh) / 2 - 1), x1 = fw.x0 + fw.w;
    fw.list_y = y0 + 2;
    if (fw.sel >= n) fw.sel = std::max(0, n - 1);
    if (fw.sel < fw.top) fw.top = fw.sel;
    if (fw.sel >= fw.top + fw.rows) fw.top = fw.sel - fw.rows + 1;
    for (int yy = y0; yy < y0 + bh; yy++) fill(yy, fw.x0, x1, bs);
    for (int xx = fw.x0; xx < x1; xx++) cell(xx, y0, U'─', ls), cell(xx, y0 + bh - 1, U'─', ls);
    for (int yy = y0; yy < y0 + bh; yy++) cell(fw.x0, yy, U'│', ls), cell(x1 - 1, yy, U'│', ls);
    cell(fw.x0, y0, U'┌', ls), cell(x1 - 1, y0, U'┐', ls), cell(fw.x0, y0 + bh - 1, U'└', ls), cell(x1 - 1, y0 + bh - 1, U'┘', ls);
    if (auto cb = box_title(T, fw.x0, x1 - 1, y0, "Go to tab", hs); cb.first >= 0) xbtn = {y0, cb.first, cb.second, 'f'};
    int fx = text(fw.x0 + 2, y0 + 1, x1 - 2, "find: ", hs, false);
    fill(y0 + 1, fx, x1 - 2, fs);
    int ex = text(fx, y0 + 1, x1 - 2, to_utf8(fw.q), fs, false);
    tb_set_cursor(ex, y0 + 1);
    if (!n) text(fw.x0 + 2, fw.list_y, x1 - 2, "no tab matches", dim, false);
    for (int i = 0; i < fw.rows && fw.top + i < n; i++) {
        Buffer* b = l[(size_t)(fw.top + i)];
        bool on = fw.top + i == fw.sel;
        Style st = on ? sel : b->activity == 3 ? T.get("tab_hilight") : b->activity == 2 ? T.get("tab_msg") : bs;
        if (!on) st.bg = bs.bg;
        fill(fw.list_y + i, fw.x0 + 1, x1 - 1, on ? sel : bs);
        std::string where = !b->srv ? "" : b->type == BT::STATUS ? "server" : b->srv->network.empty() ? b->srv->cfg.name : b->srv->network;
        int wx = x1 - 2 - (int)where.size();
        text(fw.x0 + 2, fw.list_y + i, wx - 1, (on ? "▸ " : "  ") + std::to_string(c.index_of(b) + 1) + ":" + buf_label(b), st, false);
        Style d = dim;
        d.bg = on ? sel.bg : bs.bg;
        text(wx, fw.list_y + i, x1 - 2, where, d, false);
    }
    text(fw.x0 + 2, y0 + bh - 2, x1 - 2, "type: filter · ↑↓ · Enter go · Esc close", dim, false);
}

void Tui::find_key(int k, uint32_t ch) {
    auto l = find_list();
    int n = (int)l.size();
    if (k == TB_KEY_ESC) { if (!fw.q.empty()) fw.q.clear(), fw.sel = fw.top = 0; else fw.open = false; return; }
    if (k == TB_KEY_ENTER) { fw.open = false; if (n) c.select(l[(size_t)std::min(fw.sel, n - 1)]); return; }
    if (ch || k == TB_KEY_SPACE) fw.q += ch ? (char32_t)ch : U' ', fw.sel = fw.top = 0;
    else if (k == TB_KEY_BACKSPACE || k == TB_KEY_BACKSPACE2) { if (!fw.q.empty()) fw.q.pop_back(), fw.sel = fw.top = 0; }
    else if (k == TB_KEY_CTRL_U) fw.q.clear(), fw.sel = fw.top = 0;
    else if (k == TB_KEY_ARROW_DOWN || k == TB_KEY_TAB) fw.sel = n ? (fw.sel + 1) % n : 0;
    else if (k == TB_KEY_ARROW_UP) fw.sel = n ? (fw.sel - 1 + n) % n : 0;
    else if (k == TB_KEY_PGDN) fw.sel = std::min(n - 1, fw.sel + fw.rows);
    else if (k == TB_KEY_PGUP) fw.sel = std::max(0, fw.sel - fw.rows);
    else if (k == TB_KEY_CTRL_G) fw.open = false;  // the same key closes it
}

// a click goes there; the wheel moves
void Tui::find_mouse(int k, int x, int y) {
    if (k == TB_KEY_MOUSE_WHEEL_UP || k == TB_KEY_MOUSE_WHEEL_DOWN) { find_key(k == TB_KEY_MOUSE_WHEEL_UP ? TB_KEY_ARROW_UP : TB_KEY_ARROW_DOWN, 0); return; }
    int i = y - fw.list_y;
    if (k != TB_KEY_MOUSE_LEFT) return;
    if (i < 0 || i >= fw.rows || x <= fw.x0 || x >= fw.x0 + fw.w - 1) { fw.open = false; return; }  // outside: close
    if (fw.top + i >= (int)find_list().size()) return;
    fw.sel = fw.top + i;
    find_key(TB_KEY_ENTER, 0);
}

// a click previews a theme (again, or a double click: keeps it); the wheel moves
void Tui::theme_mouse(int k, int x, int y) {
    if (k == TB_KEY_MOUSE_WHEEL_UP || k == TB_KEY_MOUSE_WHEEL_DOWN) { theme_key(k == TB_KEY_MOUSE_WHEEL_UP ? TB_KEY_ARROW_UP : TB_KEY_ARROW_DOWN, 0); return; }
    int i = y - tw.list_y;
    if (k != TB_KEY_MOUSE_LEFT || i < 0 || i >= tw.rows || x <= tw.x0 || x >= tw.x0 + tw.w - 1) return;
    int j = tw.top + i;
    if (j >= (int)theme_list().size()) return;
    long long now = now_ms();
    bool again = tw.sel == j && now - tw.click_ms < 500;
    tw.click_ms = now;
    if (again) { theme_key(TB_KEY_ENTER, 0); return; }
    int d = j - tw.sel;
    tw.sel = j - (d > 0 ? 1 : d < 0 ? -1 : 0);  // one step from it, so theme_key's move lands there and previews
    if (d) theme_key(d > 0 ? TB_KEY_ARROW_DOWN : TB_KEY_ARROW_UP, 0);
}

// a click picks a category or a key (again, or a double click: edits it); the wheel moves
void Tui::settings_mouse(int k, int x, int y) {
    if (k == TB_KEY_MOUSE_WHEEL_UP || k == TB_KEY_MOUSE_WHEEL_DOWN) {
        if (!sw.editing) settings_key(0, k == TB_KEY_MOUSE_WHEEL_UP ? TB_KEY_ARROW_UP : TB_KEY_ARROW_DOWN, 0), settings_key(0, k == TB_KEY_MOUSE_WHEEL_UP ? TB_KEY_ARROW_UP : TB_KEY_ARROW_DOWN, 0);
        return;
    }
    if (k != TB_KEY_MOUSE_LEFT || sw.editing) return;
    if (y == sw.y0 + 1) { sw.searching = true; return; }  // the search line
    int i = y - sw.list_y;
    if (i < 0 || i >= sw.h - 6 || x <= sw.x0 || x >= sw.x0 + sw.w - 1) return;
    auto all = c.setting_rows();
    std::vector<std::string> cats;
    std::vector<int> counts;
    std::vector<const Core::SetRow*> rows;
    set_view(all, lower(to_utf8(sw.q)), sw.cat, cats, counts, rows);
    if (x < sw.x0 + 2 + sw.nav_w) {  // a category
        if (sw.ctop + i < (int)cats.size() && counts[(size_t)(sw.ctop + i)]) sw.cat = cats[(size_t)(sw.ctop + i)], sw.sel = sw.top = 0, sw.focus = 0;
        return;
    }
    // the row under the pointer, counting the section headers the way draw_settings lays them out
    std::string last;
    int line = 0, want = sw.top + i;
    for (int j = 0; j < (int)rows.size(); j++) {
        if (rows[(size_t)j]->head != last) last = rows[(size_t)j]->head, line++;
        if (line == want) {
            long long now = now_ms();
            bool again = sw.focus == 1 && sw.sel == j && now - sw.click_ms < 500;
            sw.sel = j, sw.focus = 1, sw.click_ms = now;
            if (again) settings_key(0, TB_KEY_ENTER, 0);
            return;
        }
        line++;
    }
}

// ---- keyboard --------------------------------------------------------------
// vendor/termbox2.c: CSI u / modifyOtherKeys decoding, TB_MOD_SUPER = Cmd
extern "C" int isekur_extract_pre(struct tb_event*, size_t*);
extern "C" int isekur_extract_post(struct tb_event*, size_t*);

static std::string keyspec(int mod, int key, uint32_t ch) {
    std::string s = std::string(mod & TB_MOD_SUPER ? "D-" : "") + (mod & TB_MOD_ALT ? "M-" : "");
    if (ch) { utf8_put(s, ch); return s; }
    if (key >= TB_KEY_ARROW_RIGHT && key <= TB_KEY_ARROW_UP) {
        if (mod & TB_MOD_CTRL) s += "C-";
        if (mod & TB_MOD_SHIFT) s += "S-";
    }
    switch (key) {
    case TB_KEY_ARROW_UP: return s + "Up";
    case TB_KEY_ARROW_DOWN: return s + "Down";
    case TB_KEY_ARROW_LEFT: return s + "Left";
    case TB_KEY_ARROW_RIGHT: return s + "Right";
    case TB_KEY_PGUP: return s + "PgUp";
    case TB_KEY_PGDN: return s + "PgDn";
    case TB_KEY_HOME: return s + "Home";
    case TB_KEY_END: return s + "End";
    case TB_KEY_INSERT: return s + "Insert";
    case TB_KEY_DELETE: return s + "Delete";
    case TB_KEY_ENTER: return s + "Enter";
    case TB_KEY_TAB: return s + "Tab";
    case TB_KEY_BACKSPACE: case TB_KEY_BACKSPACE2: return s + "Backspace";
    case TB_KEY_ESC: return s + "Esc";
    case TB_KEY_SPACE: return s + " ";
    }
    if (key >= TB_KEY_F12 && key <= TB_KEY_F1) return s + "F" + std::to_string(TB_KEY_F1 - key + 1);
    if (key >= 1 && key <= 26) return s + "C-" + (char)('a' + key - 1);
    return s + "?";
}

// ---- shell tabs (/shell) ----------------------------------------------------
bool Tui::guide_tab() const { return c.active && !c.active->srv && c.active->name == "*guide"; }

// the active tab has a running shell and nothing (menu, y/n question, login) is asking
bool Tui::shell_keys() const {
    Buffer* b = c.active;
    return b && b->term && b->term->fd >= 0 && !c.asking(b) && menu.items.empty() && confirm.empty();
}

// every key to the shell, but Alt/Cmd ones bound to moving between tabs
void Tui::shell_key(int mod, int k, uint32_t ch) {
    Buffer* b = c.active;
    if (mod & (TB_MOD_ALT | TB_MOD_SUPER)) {
        auto it = keymap.find(keyspec(mod, k, ch));
        if (it != keymap.end()) {
            const std::string& a = it->second;
            if (a == "next_tab" || a == "prev_tab" || a == "next_active" || a == "next_server" || a.rfind("tab_", 0) == 0) { action(a); return; }
        }
    }
    VTerm* vt = b->term->vt;
    b->scroll = 0;  // typing goes back to the live screen
    int m = (mod & TB_MOD_ALT ? VTERM_MOD_ALT : 0) | (mod & TB_MOD_CTRL ? VTERM_MOD_CTRL : 0) | (mod & TB_MOD_SHIFT ? VTERM_MOD_SHIFT : 0);
    if (ch) { vterm_keyboard_unichar(vt, ch, (VTermModifier)m); return; }
    VTermKey vk = VTERM_KEY_NONE;
    switch (k) {
    case TB_KEY_ENTER: vk = VTERM_KEY_ENTER; break;
    case TB_KEY_TAB: vk = VTERM_KEY_TAB; break;
    case TB_KEY_BACKSPACE2: vk = VTERM_KEY_BACKSPACE; break;
    case TB_KEY_ESC: vk = VTERM_KEY_ESCAPE; break;
    case TB_KEY_ARROW_UP: vk = VTERM_KEY_UP; break;
    case TB_KEY_ARROW_DOWN: vk = VTERM_KEY_DOWN; break;
    case TB_KEY_ARROW_LEFT: vk = VTERM_KEY_LEFT; break;
    case TB_KEY_ARROW_RIGHT: vk = VTERM_KEY_RIGHT; break;
    case TB_KEY_INSERT: vk = VTERM_KEY_INS; break;
    case TB_KEY_DELETE: vk = VTERM_KEY_DEL; break;
    case TB_KEY_HOME: vk = VTERM_KEY_HOME; break;
    case TB_KEY_END: vk = VTERM_KEY_END; break;
    case TB_KEY_PGUP: vk = VTERM_KEY_PAGEUP; break;
    case TB_KEY_PGDN: vk = VTERM_KEY_PAGEDOWN; break;
    case TB_KEY_SPACE: vterm_keyboard_unichar(vt, ' ', (VTermModifier)m); return;
    }
    if (k >= TB_KEY_F12 && k <= TB_KEY_F1) vk = (VTermKey)VTERM_KEY_FUNCTION(TB_KEY_F1 - k + 1);
    if (k < 0x80) m &= ~VTERM_MOD_CTRL;  // termbox calls Enter, Tab, Esc, Backspace Ctrl+M, I, [, ?
    if (vk != VTERM_KEY_NONE) vterm_keyboard_key(vt, vk, (VTermModifier)m);
    else if (k >= 0 && k < 0x20) vterm_keyboard_unichar(vt, (uint32_t)k, (VTermModifier)(m & VTERM_MOD_ALT));  // Ctrl+C, Ctrl+W...: the byte
}

// the shell's screen, cell by cell; its default colors are the theme's
void Tui::draw_term(Term* t, int top, int rows, int cols, int x0) {
    VTermScreen* scr = vterm_obtain_screen(t->vt);
    Style def = c.theme.get("default");
    for (int r = 0; r < rows && r < t->rows; r++)
        for (int x = 0; x < cols && x < t->cols; x++) {
            VTermScreenCell cl;
            if (!vterm_screen_get_cell(scr, VTermPos{r, x}, &cl) || cl.chars[0] == (uint32_t)-1) continue;  // -1: a wide char's right half
            Style st = def;
            auto rgb = [&](VTermColor col, RGB& out) {
                vterm_screen_convert_color_to_rgb(scr, &col);
                out = col.rgb.red << 16 | col.rgb.green << 8 | col.rgb.blue;
            };
            if (!VTERM_COLOR_IS_DEFAULT_FG(&cl.fg)) rgb(cl.fg, st.fg);
            if (!VTERM_COLOR_IS_DEFAULT_BG(&cl.bg)) rgb(cl.bg, st.bg);
            st.attr = (cl.attrs.bold ? A_BOLD : 0) | (cl.attrs.italic ? A_ITALIC : 0) | (cl.attrs.underline ? A_UNDER : 0) | (cl.attrs.reverse ? A_REV : 0);
            uint32_t cp = cl.chars[0];
            cell(x0 + x, top + r, cp < 0x20 || cp == 0x7f ? ' ' : cp, st);  // never a control byte to the real terminal
        }
}

bool Tui::init(std::string& err, int fd) {
    int r = fd < 0 ? tb_init() : tb_init_fd(fd);
    if (r != TB_OK) { err = tb_strerror(r) + std::string(" (") + std::to_string(r) + ")"; return false; }
    tb_set_func(TB_FUNC_EXTRACT_PRE, isekur_extract_pre);
    tb_set_func(TB_FUNC_EXTRACT_POST, isekur_extract_post);
    nicklist = c.theme.lay("nicklist", "on") != "off";
    apply_cfg();
    return true;
}

// mouse, colors and keys from the config: at start, and after /set keys.* or /reload
void Tui::apply_cfg() {
    mouse_on = ini_get(c.cfg, "general", "mouse", "on") != "off";
    tb_set_input_mode(TB_INPUT_ALT | (mouse_on ? TB_INPUT_MOUSE : 0));
    std::string mode = ini_get(c.cfg, "general", "colors", "auto");
    const char* ct = getenv("COLORTERM");
    truecolor = mode == "truecolor" ||
                (mode == "auto" && ct && (std::string(ct) == "truecolor" || std::string(ct) == "24bit"));
    g_true = truecolor;
    tb_set_output_mode(truecolor ? TB_OUTPUT_TRUECOLOR : TB_OUTPUT_256);
    load_keys();
}

void Tui::load_keys() {
    keymap.clear();
    for (int i = 1; i <= 10; i++) keymap["M-" + std::to_string(i % 10)] = "tab_" + std::to_string(i);
    keymap["M-Right"] = keymap["C-n"] = "next_tab";
    keymap["M-Left"] = keymap["C-p"] = "prev_tab";
    keymap["C-a"] = keymap["M-a"] = "next_active";
    keymap["C-x"] = "next_server";
    keymap["C-w"] = "close_tab";
    keymap["PgUp"] = "scroll_up";
    keymap["PgDn"] = "scroll_down";
    keymap["C-l"] = keymap["M-l"] = keymap["¬"] = keymap["F2"] = "toggle_nicklist";  // ¬: Option+L on a Mac
    keymap["C-t"] = keymap["M-t"] = keymap["†"] = "toggle_topic";  // †: Option+T on a Mac without "Option as Meta"
    keymap["F3"] = "toggle_mouse";
    keymap["C-g"] = keymap["M-j"] = keymap["∆"] = "find_tab";  // ∆: Option+J on a Mac
    keymap["F6"] = keymap["M-o"] = keymap["ø"] = "other_pane";  // /split; ø: Option+O on a Mac
    keymap["M-q"] = keymap["œ"] = "quit";  // œ: Option+Q on a Mac without "Option as Meta"; it asks first
    keymap["F1"] = keymap["M-k"] = keymap["˚"] = "keys";  // ˚: Option+K on a Mac
    keymap["M-d"] = keymap["∂"] = keymap["C-z"] = "detach";  // ∂: Option+D on a Mac without "Option as Meta"; C-z: detach (still connected) where a shell would suspend
    // [keys] action = spec [spec...]
    auto ks = c.cfg.find("keys");
    if (ks != c.cfg.end())
        for (auto& [act, specs] : ks->second)
            for (auto& sp : split(specs, ' ')) keymap[sp] = act;
}

void Tui::shutdown() { tb_shutdown(); }

void Tui::fds(int& tty, int& resize) { tb_get_fds(&tty, &resize); }

void Tui::sync_input() {
    int id = c.active ? c.active->id : 0;
    if (id == input_id) return;
    if (!input.empty()) drafts[input_id] = {input, cur};
    else drafts.erase(input_id);
    auto it = drafts.find(id);
    if (it != drafts.end()) input = it->second.first, cur = it->second.second, drafts.erase(it);
    else input.clear(), cur = 0;
    if (!hist.empty()) hists[input_id] = std::move(hist);
    auto h = hists.find(id);
    if (h != hists.end()) hist = std::move(h->second), hists.erase(h);
    else hist.clear();
    hpos = hist.size(), input_id = id;
}

void Tui::on_readable() {
    tb_event ev;
    // An Enter followed by more input in the same read is a paste: it becomes
    // a newline in the input line instead of sending each line as it comes.
    bool enter = false;
    while (tb_peek_event(&ev, 0) == TB_OK) {
        sync_input();  // a key may have switched tabs: the next one types into that tab's line
        if (enter) input.insert(cur++, 1, U'\n'), enter = false;
        if ((c.locked || sw.open || tw.open || fw.open || kw.open || shell_keys()) && ev.type == TB_EVENT_KEY) { key(ev.mod, ev.key, ev.ch); c.dirty = true; continue; }  // a paste is keys too (the settings window: Enter edits)
        bool nl = ev.type == TB_EVENT_KEY && !(ev.mod & TB_MOD_ALT) && (ev.key == TB_KEY_ENTER || ev.key == TB_KEY_CTRL_J);
        if (nl && paste.empty() && confirm.empty() && menu.items.empty() && sugg_sel < 0) { enter = true; continue; }  // a highlighted suggestion: key() takes it
        if (ev.type == TB_EVENT_KEY) key(ev.mod, ev.key, ev.ch);
        else if (ev.type == TB_EVENT_MOUSE) mouse(ev.mod, ev.key, ev.x, ev.y);
        c.dirty = true;
    }
    if (enter) submit(), c.dirty = true;
    sync_input();
    // +typing: the input line has text for the tab on screen; a tab left behind is told "done"
    int id = c.active ? c.active->id : 0;
    if (id != typed_id)
        for (auto& b : c.bufs)
            if (b->id == typed_id) c.typed(b.get(), "");
    typed_id = id;
    if (c.active) c.typed(c.active, c.locked || sw.open || tw.open || fw.open ? "" : to_utf8(input));
}

bool Tui::action(const std::string& a) {
    int n = c.index_of(c.active), sz = (int)c.bufs.size();
    auto at = [&](int i) { return c.bufs[(size_t)((i % sz + sz) % sz)].get(); };
    if (a == "next_tab") c.select(at(n + 1));
    else if (a == "prev_tab") c.select(at(n - 1));
    else if (a.rfind("tab_", 0) == 0) {
        int t = atoi(a.c_str() + 4) - 1;
        if (t >= 0 && t < sz) c.select(at(t));
    } else if (a == "next_active") {
        Buffer* best = nullptr;
        for (int i = 1; i < sz; i++)
            if (at(n + i)->activity > (best ? best->activity : 0)) best = at(n + i);
        if (best) c.select(best);
    } else if (a == "next_server") {
        for (int i = 1; i < sz; i++)
            if (at(n + i)->type == BT::STATUS) { c.select(at(n + i)); break; }
    } else if (a == "close_tab") c.close(c.active);
    else if (a == "scroll_up") c.active->scroll += chat_height() / 2;
    else if (a == "scroll_down") c.active->scroll = std::max(0, c.active->scroll - chat_height() / 2);
    else if (a == "toggle_nicklist") nicklist = !nicklist;
    else if (a == "toggle_topic") topicbar = !topicbar;
    else if (a == "other_pane") { if (c.split_buf) c.select(c.split_buf); }
    else if (a == "detach") c.detach_req = true;
    else if (a == "quit") {  // asks first, in the yes/no box (y quits; n, Esc or × don't)
        confirm = "Quit iSekur? You leave every network and the webchat closes. Alt+D detaches instead: iSekur stays connected.\n/quit";
        confirm_title = "Quit";
    }
    else if (a == "keys") kw = KeysWin{}, kw.open = true;
    else if (a == "find_tab") fw = FindWin{}, fw.open = true, fw.sel = std::max(0, c.index_of(c.active));
    else if (a == "toggle_mouse") {  // off = the terminal's own text selection works again
        mouse_on = !mouse_on;
        tb_set_input_mode(TB_INPUT_ALT | (mouse_on ? TB_INPUT_MOUSE : 0));
        c.print(c.active, LK::INFO, std::string("Mouse ") + (mouse_on ? "on (drag copies, Shift+drag is the terminal's selection)" : "off"));
    }
    else return false;
    return true;
}

void Tui::key(int mod, int k, uint32_t ch) {
    if (c.locked) {  // only the password; Alt+D still detaches
        auto it = keymap.find(keyspec(mod, k, ch));
        if (it != keymap.end() && it->second == "detach") { action("detach"); return; }
        if (mod & (TB_MOD_ALT | TB_MOD_SUPER)) return;
        if (ch) lock_in += (char32_t)ch;
        else if (k == TB_KEY_SPACE) lock_in += U' ';
        else if ((k == TB_KEY_BACKSPACE || k == TB_KEY_BACKSPACE2) && !lock_in.empty()) lock_in.pop_back();
        else if (k == TB_KEY_ESC || k == TB_KEY_CTRL_U) lock_in.clear();
        else if (k == TB_KEY_ENTER) c.unlock(to_utf8(lock_in)), lock_in.clear();
        return;
    }
    if (sw.open) { settings_key(mod, k, ch); return; }
    if (tw.open) { if (!(mod & (TB_MOD_ALT | TB_MOD_SUPER))) theme_key(k, ch); return; }
    if (fw.open) { if (!(mod & (TB_MOD_ALT | TB_MOD_SUPER))) find_key(k, ch); return; }
    if (kw.open) { keys_key(k, ch); return; }
    if (!menu.items.empty()) {  // the nick menu has the keys until it closes
        int n = (int)menu.items.size(), d = k == TB_KEY_ARROW_DOWN ? 1 : k == TB_KEY_ARROW_UP ? -1 : 0;
        if (d) {
            do menu.sel = (menu.sel + d + n) % n;
            while (menu.items[(size_t)menu.sel].first.empty());
            return;
        }
        std::string cmd = k == TB_KEY_ENTER ? menu.items[(size_t)menu.sel].second : "";  // other keys close it
        std::string it = menu.items[(size_t)menu.sel].first;
        menu.items.clear();
        if (!cmd.empty() && cmd[0] == '?') {  // asks first, titled like the item ("Delete server...": "Delete server")
            confirm = cmd.substr(1), confirm_title = it;
            while (!confirm_title.empty() && confirm_title.back() == '.') confirm_title.pop_back();
        }
        else if (!cmd.empty() && (cmd.back() == ' ' || cmd.back() == ':')) input = from_utf8(cmd), cur = input.size();  // ends in a space: you finish it
        else if (!cmd.empty()) c.exec(c.active, cmd);
        return;
    }
    sel_x0 = -1;  // typing drops the highlight of a copied selection
    if (!confirm.empty()) {  // a menu's question: y runs it, anything else drops it
        std::string q;
        q.swap(confirm);
        if (ch == 'y' || ch == 'Y') c.exec(c.active, q.substr(q.find('\n') + 1));
        return;
    }
    if (!paste.empty()) {  // "Send N lines?" - y sends, anything else puts it back to edit
        std::string p;
        p.swap(paste);
        if (ch == 'y' || ch == 'Y') c.exec(c.active, p);
        else input = from_utf8(p), cur = input.size();
        return;
    }
    if (shell_keys()) { shell_key(mod, k, ch); return; }
    if (c.asking(c.active)) {  // the password dialog
        if ((k == TB_KEY_ESC || k == TB_KEY_CTRL_C) && !ch && !(mod & TB_MOD_ALT)) {  // Esc (or Ctrl+C) cancels
            input.clear(), cur = 0;
            c.exec(c.active, "");
            return;
        }
        if (c.pass_hidden) cur = input.size();  // no moving inside what you can't see
        if (k == TB_KEY_ARROW_UP || k == TB_KEY_ARROW_DOWN || k == TB_KEY_TAB) return;  // no history or completion in it
    }
    // the suggestion list under the input line takes Up/Down while it shows;
    // Enter or Tab on the highlight puts it in the input line without sending
    // (only Alt/Cmd are excluded: termbox reports Enter and Tab as Ctrl+M / Ctrl+I)
    if (!(mod & (TB_MOD_ALT | TB_MOD_SUPER)) && (k == TB_KEY_ARROW_UP || k == TB_KEY_ARROW_DOWN || (sugg_sel >= 0 && (k == TB_KEY_ENTER || k == TB_KEY_TAB)))) {
        std::vector<Sugg> sg;
        int from = suggest(sg), n = (int)sg.size();
        if (from >= 0 && n > 0) {
            if (k == TB_KEY_ARROW_DOWN) sugg_sel = std::min(sugg_sel + 1, n - 1);
            else if (k == TB_KEY_ARROW_UP) sugg_sel = sugg_sel < 0 ? n - 1 : sugg_sel - 1;  // above the first: none
            else if (sugg_sel < n) {
                std::u32string w = from_utf8(sg[(size_t)sugg_sel].word + " ");
                input.replace((size_t)from, cur - (size_t)from, w);
                cur = (size_t)from + w.size(), sugg_sel = -1, completing = false;
            }
            return;
        }
    }
    sugg_sel = -1;
    auto it = keymap.find(keyspec(mod, k, ch));
    if (it != keymap.end() && action(it->second)) return;
    if (k != TB_KEY_TAB) completing = false;
    bool bs = k == TB_KEY_BACKSPACE || k == TB_KEY_BACKSPACE2;
    if (mod & TB_MOD_SUPER) {  // Cmd, when the terminal reports it (CSI u)
        if (bs) input.erase(0, cur), cur = 0;
        else if (k == TB_KEY_ARROW_LEFT) cur = 0;
        else if (k == TB_KEY_ARROW_RIGHT) cur = input.size();
        return;
    }
    if ((mod & TB_MOD_ALT) && bs) {  // Option/Alt+Backspace: previous word
        size_t st = cur;
        while (st > 0 && input[st - 1] == U' ') st--;
        while (st > 0 && input[st - 1] != U' ') st--;
        input.erase(st, cur - st), cur = st;
        return;
    }
    if (mod & TB_MOD_ALT) return;  // unbound Alt combos do nothing
    if (input.empty() && guide_tab() && ((ch == 'q' || ch == 'Q') || (k == TB_KEY_ESC && !ch))) { c.guide(c.active, "close"); return; }  // q or Esc closes the guide
    if (ch || k == TB_KEY_SPACE) { input.insert(cur++, 1, ch ? (char32_t)ch : U' '); return; }
    switch (k) {
    case TB_KEY_ENTER: submit(); break;
    case TB_KEY_BACKSPACE: case TB_KEY_BACKSPACE2: if (cur) input.erase(--cur, 1); break;
    case TB_KEY_DELETE: if (cur < input.size()) input.erase(cur, 1); break;
    case TB_KEY_ARROW_LEFT: if (input.empty() && guide_tab()) c.guide(c.active, "prev"); else if (cur) cur--; break;
    case TB_KEY_ARROW_RIGHT: if (input.empty() && guide_tab()) c.guide(c.active, "next"); else if (cur < input.size()) cur++; break;
    case TB_KEY_HOME: cur = 0; break;  // Ctrl+A is next_active
    // Ctrl+U, and Cmd+Backspace (Ghostty and others send it as ^U)
    case TB_KEY_CTRL_U: input.erase(0, cur); cur = 0; break;
    case TB_KEY_END: case TB_KEY_CTRL_E: cur = input.size(); break;
    case TB_KEY_CTRL_K: input.erase(cur); break;
    case TB_KEY_ARROW_UP:
        if (hpos > 0) input = from_utf8(hist[--hpos]), cur = input.size();
        break;
    case TB_KEY_ARROW_DOWN:
        if (hpos < hist.size()) {
            ++hpos;
            input = hpos == hist.size() ? U"" : from_utf8(hist[hpos]);
            cur = input.size();
        }
        break;
    case TB_KEY_TAB: complete(); break;
    // mIRC formatting: bold, color, reset, reverse, italic, underline
    case TB_KEY_CTRL_C:  // on an empty line it's the "how do I quit?" key: say so, once in a row
        if (input.empty()) {
            const std::string hint = "Ctrl+C doesn't quit iSekur (it's the color key while typing): /quit exits, Ctrl+Z, Alt+D or /detach leave it running";
            if (c.active->lines.empty() || c.active->lines.back().text != hint) {
                Line l{time(nullptr), LK::INFO, "", hint};
                l.nolog = true;
                c.add(c.active, l);
            }
            break;
        }
        input.insert(cur++, 1, (char32_t)k);
        break;
    case TB_KEY_CTRL_B: case TB_KEY_CTRL_O: case TB_KEY_CTRL_V:
    case TB_KEY_CTRL_RSQ_BRACKET: case TB_KEY_CTRL_UNDERSCORE:
        input.insert(cur++, 1, (char32_t)k);
        break;
    }
}

// vendor/termbox2.c: horizontal wheel
static const int TB_KEY_MOUSE_WHEEL_LEFT = 0xffff - 29, TB_KEY_MOUSE_WHEEL_RIGHT = 0xffff - 30;

bool Tui::in_sel(int x, int y) const {
    int a = sel_y0 * 10000 + sel_x0, z = sel_y1 * 10000 + sel_x1, p = y * 10000 + x;
    return p >= std::min(a, z) && p <= std::max(a, z);
}

// the selected cells as text, one line per row, to the terminal's clipboard
void Tui::copy_sel() {
    tb_cell* cb = tb_cell_buffer();
    int W = tb_width(), cw = nl_x >= 0 ? nl_x : W;
    std::string out;
    for (int y = std::min(sel_y0, sel_y1); y <= std::max(sel_y0, sel_y1); y++) {
        std::string row;
        for (int x = 0; x < cw; x++)
            if (cb[y * W + x].ch && in_sel(x, y)) utf8_put(row, cb[y * W + x].ch);
        while (!row.empty() && row.back() == ' ') row.pop_back();
        out += (out.empty() && y == std::min(sel_y0, sel_y1) ? "" : "\n") + row;
    }
    if (out.empty()) return;
    std::string osc = "\x1b]52;c;" + b64(out) + "\x07";
    tb_send(osc.data(), osc.size());
}

// the word drawn under x,y in the chat, without the punctuation around it
std::string Tui::word_at(int x, int y) const {
    tb_cell* cb = tb_cell_buffer();
    int W = tb_width(), cw = nl_x >= 0 ? nl_x : W;
    if (y < 0 || y >= tb_height() || x < 0 || x >= cw) return "";
    auto ch = [&](int xx) { return cb[y * W + xx].ch; };
    if (!ch(x) || ch(x) == ' ') return "";
    int a = x, z = x;
    while (a > 0 && ch(a - 1) && ch(a - 1) != ' ') a--;
    while (z + 1 < cw && ch(z + 1) && ch(z + 1) != ' ') z++;
    std::string w;
    for (int xx = a; xx <= z; xx++) utf8_put(w, ch(xx));
    while (!w.empty() && strchr(",.;:!?)]}'\"", w.back())) w.pop_back();
    while (!w.empty() && strchr("([{'\"", w.front())) w.erase(0, 1);
    return w;
}

// Wheel scrolls the chat (or the nicklist, or the topic, under the pointer);
// left click picks a tab or an activity number (double click on a nick: a query), or
// unfolds the topic (double click: edit it); a drag in the chat copies text.
// the × (or the red dot) of the box on screen: closes it like Esc does
void Tui::close_box() {
    switch (xbtn.kind) {
    case 's': sw.open = false; break;
    case 't': tw.q.clear(), theme_key(TB_KEY_ESC, 0); break;  // back to the theme it had
    case 'f': fw.open = false; break;
    case 'k': kw.open = false; break;
    case 'a': input.clear(), cur = 0, c.exec(c.active, ""); break;  // a question: cancelled
    case 'c': confirm.clear(); break;
    case 'p': input = from_utf8(paste), cur = input.size(), paste.clear(); break;  // back to edit it
    }
    xbtn = XBtn{};
    c.dirty = true;
}

void Tui::mouse(int mod, int k, int x, int y) {
    if (c.locked) return;
    if (k == TB_KEY_MOUSE_LEFT && !(mod & TB_MOD_MOTION) && xbtn.kind && y == xbtn.y && x >= xbtn.x0 && x <= xbtn.x1) { close_box(); return; }
    if (sw.open) { if (!(mod & TB_MOD_MOTION)) settings_mouse(k, x, y); return; }
    if (tw.open) { if (!(mod & TB_MOD_MOTION)) theme_mouse(k, x, y); return; }
    if (fw.open) { if (!(mod & TB_MOD_MOTION)) find_mouse(k, x, y); return; }
    if (kw.open) {  // the wheel scrolls, a click outside closes
        if (k == TB_KEY_MOUSE_WHEEL_UP || k == TB_KEY_MOUSE_WHEEL_DOWN) keys_key(k == TB_KEY_MOUSE_WHEEL_UP ? TB_KEY_ARROW_UP : TB_KEY_ARROW_DOWN, 0);
        else if (k == TB_KEY_MOUSE_LEFT && !(mod & TB_MOD_MOTION) && (x < kw.x0 || x >= kw.x0 + kw.w || y < kw.list_y - 1 || y > kw.list_y + kw.rows + 1)) kw.open = false;
        return;
    }
    int top = top_row(), ch = chat_height();
    bool over_topic = topicbar && y < topic_h, over_chat = y >= top && y < top + ch && (nl_x < 0 || x < nl_x);
    if (selecting && (k == TB_KEY_MOUSE_LEFT || k == TB_KEY_MOUSE_RELEASE)) {
        sel_x1 = std::max(0, std::min(x, (nl_x >= 0 ? nl_x : tb_width()) - 1));
        sel_y1 = std::max(top, std::min(y, top + ch - 1));
        if (k == TB_KEY_MOUSE_RELEASE) {
            selecting = false;
            if (sel_x1 == sel_x0 && sel_y1 == sel_y0) sel_x0 = -1;  // a plain click
            else copy_sel();
        }
        c.dirty = true;
        return;
    }
    if (div_drag && (k == TB_KEY_MOUSE_LEFT || k == TB_KEY_MOUSE_RELEASE)) {  // /split: moving the divider
        int W = tb_width(), at = div_x >= 0 ? std::max(10, std::min(W - 11, x)) * 1000 / std::max(1, W - 1)
                                            : std::max(3, std::min(ch - 3, y - top)) * 1000 / std::max(1, ch);
        if (at != c.split_pm) c.split_pm = at, div_moved = true, c.dirty = true;
        if (k == TB_KEY_MOUSE_RELEASE) {
            div_drag = false;
            if (!div_moved && other.in(x, y)) c.select(c.split_buf);  // a plain click on the other pane's title
        }
        return;
    }
    if (tab_drag >= 0 && (mod & TB_MOD_MOTION)) {  // dragging a tab: the one it would land on, reversed
        auto [moved, at] = drop_at(x, y);
        int over = -1;
        for (auto& h : hits)
            if (moved && h.y == y && x >= h.x0 && x < h.x1 && h.tab >= 0 && h.tab < (int)c.bufs.size()) over = c.bufs[(size_t)h.tab]->id;
        if (over != tab_drop) tab_drop = over, c.dirty = true;
        return;
    }
    if (mod & TB_MOD_MOTION) return;
    if (k == TB_KEY_MOUSE_LEFT && menu.items.empty() && !nick_sel.empty()) {  // a click off the nicklist unselects
        bool on = false;
        for (auto& h : hits) on |= h.y == y && x >= h.x0 && x < h.x1 && !h.nick.empty();
        if (!on) nick_sel.clear(), c.dirty = true;
    }
    if (k == TB_KEY_MOUSE_LEFT && menu.items.empty() && ((div_x >= 0 && x == div_x && y >= top && y < top + ch) || (div_y >= 0 && y == div_y))) {
        div_drag = true, div_moved = false;
        return;
    }
    if (menu.items.empty() && c.split_buf && other.in(x, y)) {  // /split: the other pane
        if (k == TB_KEY_MOUSE_WHEEL_UP) c.split_buf->scroll += 3;
        else if (k == TB_KEY_MOUSE_WHEEL_DOWN) c.split_buf->scroll = std::max(0, c.split_buf->scroll - 3);
        else if (k == TB_KEY_MOUSE_LEFT) c.select(c.split_buf);
        c.dirty = true;
        return;
    }
    if (!menu.items.empty() && (k == TB_KEY_MOUSE_LEFT || k == TB_KEY_MOUSE_RIGHT)) {  // pick, or close it
        int i = y - menu.y - 1;
        int mw = 0;
        for (auto& it : menu.items) mw = std::max(mw, (int)it.first.size() + 2);
        mw = std::max(mw, (int)menu.nick.size() + 2);
        bool in = x >= menu.x && x < menu.x + mw && i >= 0 && i < (int)menu.items.size() && !menu.items[(size_t)i].first.empty();
        if (in) menu.sel = i, key(0, TB_KEY_ENTER, 0);
        else menu.items.clear();
        c.dirty = true;
        return;
    }
    if (k == TB_KEY_MOUSE_RIGHT) {
        for (auto& h : hits) {
            if (h.y != y || x < h.x0 || x >= h.x1) continue;
            if (!h.nick.empty()) {  // on the selection: a menu for all of it; else just this one
                if (std::find(nick_sel.begin(), nick_sel.end(), h.nick) == nick_sel.end()) nick_sel = {h.nick};
                std::string who;
                for (auto& n : nick_sel) who += (who.empty() ? "" : " ") + n;
                nick_click_ms = 0, open_menu(who, x, y);
                continue;
            }
            if (y < top_row() - tab_h || y >= top_row() || h.tab < 0 || h.tab >= (int)c.bufs.size()) continue;
            Buffer* tb = c.bufs[(size_t)h.tab].get();
            if ((tb->type == BT::STATUS && tb->srv) || tb->type == BT::CHANNEL || tb->type == BT::QUERY)  // its commands run there
                c.select(tb), menu = Menu{x, y, 0, tb->type == BT::STATUS ? tb->srv->cfg.name : tb->name, c.tab_menu(tb)}, c.dirty = true;
        }
        return;
    }
    if (over_topic && k >= TB_KEY_MOUSE_WHEEL_RIGHT && k <= TB_KEY_MOUSE_WHEEL_UP) {
        bool back = k == TB_KEY_MOUSE_WHEEL_LEFT || k == TB_KEY_MOUSE_WHEEL_UP;
        topic_off = std::max(0, topic_off + (back ? -8 : 8));
        c.dirty = true;
        return;
    }
    if (k == TB_KEY_MOUSE_WHEEL_UP || k == TB_KEY_MOUSE_WHEEL_DOWN) {
        int d = k == TB_KEY_MOUSE_WHEEL_UP ? 3 : -3;
        bool over_nicks = nl_x >= 0 && x > nl_x && y >= top_row() && y < top_row() + chat_height();
        if (over_nicks) nl_scroll = std::max(0, nl_scroll - d);
        else c.active->scroll = std::max(0, c.active->scroll + d);
        c.dirty = true;
        return;
    }
    if (k == TB_KEY_MOUSE_RELEASE && tab_drag >= 0) {  // a dragged tab let go: it takes the place of the one under it
        auto [moved, at] = drop_at(x, y);
        tab_drag = tab_drop = -1, c.dirty = true;
        if (moved) c.exec(moved, "/move " + std::to_string(at));
        return;
    }
    if (k != TB_KEY_MOUSE_LEFT) return;
    if (sel_x0 >= 0) sel_x0 = -1, c.dirty = true;
    if (over_topic) {
        Buffer* b = c.active;
        long long now = now_ms();
        bool dbl = now - click_ms < 400;
        click_ms = dbl ? 0 : now;
        if (!dbl) { topic_open = !topic_open; return; }
        // double click: the topic into the input line, if we may set it (+t needs ops)
        topic_open = false;
        if (b->type != BT::CHANNEL || !b->srv) return;
        auto me = b->nicks.find(b->srv->lower(b->srv->nick));
        std::string my = me == b->nicks.end() ? "" : me->second.prefix;
        if (b->modes.find('t') != std::string::npos && !rank_ok(b->srv, my, '%')) {
            c.print(b, LK::ERROR, b->name + " is +t: only channel operators can change the topic");
            return;
        }
        // a dialog with the topic in it to edit; Enter sets it, Esc (or emptying it) leaves it be
        // (closing the tab drops the dialog and this with it, so b is still there when it runs)
        c.ask_pass(b, "topic", [this, b](const std::string& t) { c.exec(b, "/topic " + t); },
                   false, "Topic of " + b->name, {"Enter sets it · Esc leaves it as it is"});
        input = from_utf8(b->topic), cur = input.size(), completing = false;
        return;
    }
    for (auto& h : hits) {
        if (h.y != y || x < h.x0 || x >= h.x1) continue;
        if (h.tab == -2) c.about(c.active);
        else if (h.tab >= 0 && h.tab < (int)c.bufs.size()) c.select(c.bufs[(size_t)h.tab].get()), tab_drag = c.bufs[(size_t)h.tab]->id;
        else if (!h.nick.empty()) {
            long long now = now_ms();
            bool dbl = h.nick == nick_click && now - nick_click_ms < 400;
            nick_click = h.nick, nick_click_ms = dbl ? 0 : now, c.dirty = true;
            auto at = std::find(nick_sel.begin(), nick_sel.end(), h.nick);
            if (dbl) nick_sel = {h.nick}, c.exec(c.active, "/query " + h.nick);
            else if (mod & (TB_MOD_SHIFT | TB_MOD_CTRL)) { if (at == nick_sel.end()) nick_sel.push_back(h.nick); else nick_sel.erase(at); }
            else if (at != nick_sel.end()) nick_sel.clear();  // a selected one: unselects
            else nick_sel = {h.nick};
        }
        return;
    }
    if (over_chat) {
        Buffer* b = c.active;
        std::string w = word_at(x, y);
        // *channels: a click on the Channel or Users header sorts by it
        if (b->srv && b->name == "*channels" && (w == "Channel" || w == "Users")) { c.list_sort(b, w == "Channel" ? "name" : "users"); return; }
        if (guide_tab()) {  // its buttons: ◀ Prev, Next ▶ / ✔ Done, and the box from its ☐ on
            tb_cell* cb = tb_cell_buffer();
            int box = -1;
            for (int xx = 0; xx < tb_width() && box < 0; xx++)
                if (cb[y * tb_width() + xx].ch == 0x2610 || cb[y * tb_width() + xx].ch == 0x2611) box = xx;
            if (box >= 0 && x >= box) { c.guide(b, c.opt_on(nullptr, "show_guide", false) ? "off" : "on"); return; }
            if (w == "◀" || w == "Prev") { c.guide(b, "prev"); return; }
            if (w == "Next" || w == "▶") { c.guide(b, "next"); return; }
            if (w == "Done" || w == "✔") { c.guide(b, "close"); return; }
        }
        // double click a #channel (any tab of a server) joins it; in *channels anywhere on its row
        long long now = now_ms();
        bool dbl = now - chat_click_ms < 400 && chat_click_y == y;
        chat_click_ms = dbl ? 0 : now, chat_click_y = y;
        if (dbl && b->srv) {
            if (!b->srv->is_chan(w) && b->name == "*channels")
                for (int xx = 0; xx < x && !b->srv->is_chan(w); xx++) w = word_at(xx, y);  // the row's first #word: its name
            if (b->srv->is_chan(w)) { c.exec(b, "/join " + w); return; }
        }
        selecting = true, sel_x0 = sel_x1 = x, sel_y0 = sel_y1 = y;
    }
}

// a tab dropped on another of its server's (a server tab: on any tab of another server) takes that place;
// a channel on its server tab goes first
std::pair<Buffer*, int> Tui::drop_at(int x, int y) {
    Buffer* b = nullptr;
    for (auto& p : c.bufs) if (p->id == tab_drag) b = p.get();
    for (auto& h : hits) {
        if (h.y != y || x < h.x0 || x >= h.x1 || h.tab < 0 || h.tab >= (int)c.bufs.size() || !h.nick.empty()) continue;
        Buffer* to = c.bufs[(size_t)h.tab].get();
        if (!b || b == to || !b->srv || !to->srv) return {nullptr, 0};
        bool srv = b->type == BT::STATUS;  // a server tab: among the servers
        if (srv == (to->srv == b->srv)) return {nullptr, 0};
        int n = 0, at = 1;
        for (auto& p : c.bufs)
            if (srv ? p->srv && p->type == BT::STATUS : p->srv == to->srv && p->type != BT::STATUS) {
                n++;
                if (srv ? p->srv == to->srv : p.get() == to) at = n;
            }
        return {b, at};
    }
    return {nullptr, 0};
}

void Tui::open_menu(const std::string& nick, int x, int y) {
    if (!c.active->srv) return;
    size_t n = std::count(nick.begin(), nick.end(), ' ') + 1;
    menu = Menu{x, y, 0, n > 1 ? std::to_string(n) + " users" : nick, c.nick_menu(c.active, nick)};
    c.dirty = true;
}

void Tui::submit() {
    std::string line = to_utf8(input);
    input.clear(), cur = 0;
    if (c.asking(c.active)) { c.exec(c.active, line); return; }  // a password: no history
    if (line.empty()) return;
    // paste_confirm = N (default 5, 0 = off): ask before sending more lines than that
    size_t lines = 0;
    for (auto& l : split(line, '\n')) lines += !l.empty();
    int max = atoi(c.opt(c.active->srv, "paste_confirm", "5").c_str());
    if (max > 0 && lines > (size_t)max) {
        paste = line;
        return;
    }
    bool secret = input_secret(c.active->name, line) != std::string::npos;  // a password: not kept for Up
    if (!secret && (hist.empty() || hist.back() != line)) hist.push_back(line);
    if (hist.size() > 500) hist.erase(hist.begin());
    hpos = hist.size();
    c.active->scroll = 0;
    c.exec(c.active, line);
}

int Tui::suggest(std::vector<Sugg>& out) const {
    std::string s = to_utf8(input.substr(0, cur));
    if (c.asking(c.active)) return -1;  // a password is not a command
    if (s.empty() || s[0] != '/' || s.rfind("//", 0) == 0) return -1;
    size_t sp = s.find(' ');
    auto cmds = [&](const std::string& pfx, bool slash) {
        for (auto& k : commands())
            if (std::string(k.name).rfind(irc_lower(pfx), 0) == 0)
                out.push_back({(slash ? "/" : "") + std::string(k.name), (*k.usage ? " " + std::string(k.usage) : "") + "  ", k.summary});
    };
    if (sp == std::string::npos) { cmds(s.substr(1), true); return 0; }  // "/jo"
    std::string cmd = irc_lower(s.substr(1, sp - 1)), arg = s.substr(sp + 1);
    if (arg.find(' ') == std::string::npos) {
        if (cmd == "set") {  // config keys containing what's typed
            for (auto& st : c.settings())
                if (irc_lower(st.key).find(irc_lower(arg)) != std::string::npos) out.push_back({st.key, " = ", st.val});
            return (int)sp + 1;
        }
        if (cmd == "theme") {  // installed themes containing what's typed
            for (auto& n : theme_names())
                if (irc_lower(n).find(irc_lower(arg)) != std::string::npos) out.push_back({n, "  ", n == c.theme.name ? "(current)" : ""});
            return (int)sp + 1;
        }
        if (cmd == "help") { cmds(arg, false); return (int)sp + 1; }
        if (const CmdInfo* k = find_command(cmd)) {  // "/kickban al": the arguments, until the 2nd word
            out.push_back({"/" + std::string(k->name), (*k->usage ? " " + std::string(k->usage) : "") + "  ", k->summary});
            return -2;
        }
    }
    return -1;
}

void Tui::complete() {
    Buffer* b = c.active;
    if (!completing) {
        size_t st = cur;
        while (st > 0 && input[st - 1] != U' ') st--;
        comp.clear();
        std::vector<Sugg> sg;
        int from = suggest(sg);
        if (from >= 0) {  // a command or config key, counted in UTF-8 bytes: these are ASCII
            st = (size_t)from;
            for (auto& g : sg) comp.push_back(g.word + " ");
        } else {
            std::string pfx = irc_lower(to_utf8(input.substr(st, cur - st)));
            if (pfx.empty()) return;
            for (auto& kv : b->nicks)
                if (kv.first.rfind(pfx, 0) == 0) comp.push_back(kv.second.nick);
            if (b->type == BT::QUERY && irc_lower(b->name).rfind(pfx, 0) == 0) comp.push_back(b->name);
            std::sort(comp.begin(), comp.end(),
                      [](auto& x, auto& y) { return irc_lower(x) < irc_lower(y); });
            for (auto& n : comp) n += st == 0 ? ": " : " ";
        }
        if (comp.empty()) return;
        completing = true, comp_i = 0, comp_start = st;
    } else {
        comp_i = (comp_i + 1) % comp.size();
    }
    std::u32string w = from_utf8(comp[comp_i]);
    input.replace(comp_start, cur - comp_start, w);
    cur = comp_start + w.size();
}
