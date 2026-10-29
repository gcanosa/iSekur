// Terminal frontend: layout, rendering, keyboard.
#include "tui.h"
#include <algorithm>
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

static void present() {
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
    return b->type == BT::STATUS && b->srv && !b->srv->network.empty() ? b->srv->network : b->name;
}

// ---- status_info: this machine's CPU, memory, load and battery -------------
// src/sysinfo.cpp, apart since macOS headers have their own Style
int cpu_pct();
bool mem_free(unsigned long long& avail, unsigned long long& total);
int battery_pct();

// 512M, 3.2G, 16G, 1.5T: the unit that keeps it under 1024, one decimal below 10
static std::string human_bytes(unsigned long long n) {
    const char* u = "BKMGTP";
    double v = (double)n;
    while (v >= 1024 && u[1]) v /= 1024, u++;
    char buf[32];
    snprintf(buf, sizeof buf, v < 10 && *u != 'B' ? "%.1f%c" : "%.0f%c", v, *u);
    std::string s = buf;
    if (s.size() > 3 && s.compare(s.size() - 3, 2, ".0") == 0) s.erase(s.size() - 3, 2);  // 16.0G -> 16G
    return s;
}

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
static std::vector<Row> status_info(const Core& c, Style st) {
    const Theme& T = c.theme;
    auto on = split(c.opt(nullptr, "status_info", "up"), ' ');
    auto has = [&](const char* k) { return std::find(on.begin(), on.end(), k) != on.end(); };
    std::vector<Row> out;
    auto add = [&](const std::string& s) { out.emplace_back(), cells(out.back(), s, st, false); };
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

// ---- line layout -----------------------------------------------------------
static std::vector<Row> wrap(const Core& c, const Line& l, const Buffer* b, int width_) {
    const Theme& T = c.theme;
    int nw = atoi(T.lay("nick_width", "12").c_str());
    std::string tsfmt = ini_get(c.cfg, "general", "timestamp", T.lay("timestamp", "%H:%M"));
    Row pre;
    cells(pre, fmt_time(l.ts, tsfmt) + " ", T.get("timestamp"), false);

    std::string marker;
    Style ms, body = T.get("default");
    std::string txt = l.text;
    switch (l.kind) {
    case LK::MSG: {
        std::string pfx;
        auto it = b->nicks.find(irc_lower(l.nick));
        if (it != b->nicks.end() && !it->second.prefix.empty()) pfx = it->second.prefix.substr(0, 1);
        marker = pfx + l.nick;
        ms = l.self ? T.get("msg_self") : nick_style(T, l.nick);
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
    default: marker = "--", ms = body = T.get("info"); break;
    }
    Row nick;
    cells(nick, marker, ms, false);
    if (width(nick) > nw) {
        while (!nick.empty() && width(nick) > nw - 1) nick.pop_back();
        nick.push_back({'+', T.get("separator"), 1});
    }
    for (int pad = nw - width(nick); pad > 0; pad--) pre.push_back({' ', T.get("default"), 1});
    pre.insert(pre.end(), nick.begin(), nick.end());
    cells(pre, " " + T.lay("separator", "│") + " ", T.get("separator"), false);

    int indent = width(pre), avail = std::max(10, width_ - indent);
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
    return rows;
}

// ---- drawing ---------------------------------------------------------------
int Tui::chat_height() const { return std::max(1, tb_height() - 2 - top_row()); }

void Tui::draw() {
    if (c.cfg_changed) c.cfg_changed = false, apply_cfg();
    if (c.settings_win) c.settings_win = false, sw = SetWin{}, sw.open = true;
    if (c.bell) c.bell = false, tb_send("\a", 1);
    const Theme& T = c.theme;
    Style def = T.get("default");
    tb_set_clear_attrs(tbc(def.fg), tbc(def.bg));
    tb_clear();
    isekur_links_begin(), frame_urls.clear();
    int W = tb_width(), H = tb_height();
    Buffer* b = c.active;
    c.dirty = false;
    hits.clear(), nl_x = -1;
    if (c.locked) {  // the lock screen: nothing of the session shows until the password
        std::vector<std::string> info;
        if (!c.lock_msg.empty()) info.push_back(c.lock_msg);
        draw_dialog(W, H, "iSekur is locked", info, "password", true, lock_in, lock_in.size(), "Enter: unlock · Alt+D: detach");
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
        else if (b->name == "*about" && !b->srv) topic = "About iSekur";
        else if (b->type == BT::QUERY) topic = "Query with " + b->name;
        else if (b->type == BT::SHELL) topic = b->term ? std::string("Shell: ") + (getenv("SHELL") ? getenv("SHELL") : "/bin/sh") : "Shell: /shell logs in and starts one";
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
    int ty = top_row() - 1;
    fill(ty, 0, W, tbs);
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
        Style s = t == b ? (t->quiet ? sty(T, "tab_quiet_act", "tab_active") : T.get("tab_active"))
                : act == 3 ? T.get("tab_hilight")
                : act == 2 ? T.get("tab_msg")
                : act == 1 ? T.get("tab_data")
                : t->quiet ? sty(T, "tab_quiet", "tabbar")
                : t->type == BT::SHELL ? sty(T, "tab_shell", "tab_home")
                : !t->srv ? sty(T, "tab_home", "tab_server")
                : t->type == BT::STATUS ? sty(T, "tab_server", "tabbar") : tbs;
        Row r;
        cells(r, " " + std::to_string(i + 1) + ":" + buf_label(t) + (e2e_on(t) ? " \xf0\x9f\x94\x92" : "") + (hid ? " +" + std::to_string(hid) : "") + " ", s, false);
        if (t == b) av = (int)labels.size();
        labels.push_back(r);
        vis.push_back((int)i);
    }
    // too many tabs: the active one stays in view, "‹N" / "N›" count the hidden ones on each
    // side (colored like the most urgent of them) and a click on it brings the next one in
    int nt = (int)labels.size(), total = 0, first = 0, lm = 0, rm = 0;
    for (auto& l : labels) total += width(l);
    auto pick = [&] {
        int span = 0;
        first = 0;
        for (int i = 0; i <= av; i++) span += width(labels[(size_t)i]);
        while (span > W - lm - rm && first < av) span -= width(labels[(size_t)first++]);
    };
    if (total > W) rm = 4, pick();
    if (first) lm = 4, pick();
    int last = first, tx = lm;
    for (int i = first; i < nt && (i == first || tx + width(labels[(size_t)i]) <= W - rm); i++) {
        int x0 = tx;
        tx = draw_row(tx, ty, W - rm, labels[(size_t)i]);
        hits.push_back({ty, x0, tx, vis[(size_t)i], ""});
        last = i;
    }
    auto more = [&](int from, int to, bool left) {
        int act = 0;
        for (int i = from; i < to; i++) act = std::max(act, c.bufs[(size_t)vis[(size_t)i]]->activity);
        Style ms = act == 3 ? T.get("tab_hilight") : act == 2 ? T.get("tab_msg") : act == 1 ? T.get("tab_data") : tbs;
        std::string num = std::to_string(to - from), m = left ? "‹" + num : num + "›";
        int x0 = left ? 1 : W - (int)num.size() - 2;
        text(x0, ty, W, m, ms, false);
        hits.push_back({ty, left ? 0 : x0, left ? lm : W, vis[(size_t)(left ? to - 1 : from)], ""});
    };
    if (first) more(0, first, true);
    if (last < nt - 1) more(last + 1, nt, false);

    // nick list
    int top = top_row(), ch = chat_height();
    int nlw = nicklist && b->type == BT::CHANNEL && W >= 60 ? atoi(T.lay("nicklist_width", "18").c_str()) : 0;
    int cw = W - (nlw ? nlw + 1 : 0);
    nl_x = nlw ? cw : -1;
    if (nlw) {
        Style bs = sty(T, "nicklist_border", "separator");
        for (int y = top; y < top + ch; y++) text(cw, y, W, T.lay("separator", "│"), bs, false);
        std::vector<const NickEntry*> v;
        for (auto& kv : b->nicks) v.push_back(&kv.second);
        Server* s = b->srv;
        std::sort(v.begin(), v.end(), [&](const NickEntry* x, const NickEntry* y) {
            int rx = s->prefix_rank(x->prefix), ry = s->prefix_rank(y->prefix);
            return rx != ry ? rx < ry : irc_lower(x->nick) < irc_lower(y->nick);
        });
        if (nl_buf != b) nl_buf = b, nl_scroll = 0;
        nl_scroll = std::max(0, std::min(nl_scroll, (int)v.size() - (ch - 1)));
        v.erase(v.begin(), v.begin() + nl_scroll);
        Style hs = sty(T, "nicklist_title", "nicklist");
        fill(top, cw + 1, W, hs);
        text(cw + 2, top, W, std::to_string(v.size() + (size_t)nl_scroll) + " users" + (nl_scroll ? " ↑" : ""), hs, false);
        for (size_t i = 0; i < v.size() && (int)i < ch - 1; i++) {
            const NickEntry* e = v[i];
            hits.push_back({top + 1 + (int)i, cw + 1, W, -1, e->nick});
            Style ns = e->prefix.empty() ? T.get("nicklist")
                     : e->prefix[0] == '+' ? sty(T, "nicklist_voice", "nicklist")
                                           : sty(T, "nicklist_op", "nicklist");
            if (e->away) ns.attr |= A_DIM;
            fill(top + 1 + (int)i, cw + 1, W, T.get("nicklist"));
            text(cw + 2, top + 1 + (int)i, W,
                 (e->prefix.empty() ? " " : e->prefix.substr(0, 1)) + e->nick, ns, false);
        }
        for (int y = top + 1 + (int)v.size(); y < top + ch; y++) fill(y, cw + 1, W, T.get("nicklist"));
    }

    // a shell tab: its terminal screen, unless scrolled back into what went off the top
    Term* term = b->term.get();
    if (term && b->scroll == 0) {
        c.term_resize(term, ch, cw);
        draw_term(term, top, ch, cw);
    } else {
    // chat
    std::vector<Row> rows;  // bottom-up
    int need = ch + b->scroll;
    size_t n = 0;
    for (auto it = b->lines.rbegin(); it != b->lines.rend() && (int)rows.size() < need; ++it, ++n) {
        if (b->unread && n == (size_t)b->unread) {  // read marker above the new lines
            Row mk;
            std::string bar = "── new ";
            for (int k = 7; k < cw; k++) bar += "─";
            cells(mk, bar, sty(T, "marker", "separator"), false);
            rows.push_back(mk);
        }
        if (b->quiet && noise(*it)) continue;
        auto r = wrap(c, *it, b, cw);
        rows.insert(rows.end(), r.rbegin(), r.rend());
    }
    if ((int)rows.size() < need) b->scroll = std::max(0, (int)rows.size() - ch);
    for (int k = 0; k < ch; k++) {
        size_t idx = (size_t)(b->scroll + k);
        if (idx >= rows.size()) break;
        draw_row(0, top + ch - 1 - k, cw, rows[idx]);
    }
    }
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
    if (b->scroll) seg("-- MORE --", T.get("hilight"));
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
    if (sw.open) {  // /set: the settings window has the keys
        draw_settings(W, H);
        present();
        return;
    }
    if (!confirm.empty()) {
        int px = text(0, y, W, confirm.substr(0, confirm.find('\n')) + " (y/n) ", T.get("hilight"), false);
        tb_set_cursor(px, y);
        present();
        return;
    }
    if (!paste.empty()) {
        size_t n = 0;
        for (auto& l : split(paste, '\n')) n += !l.empty();
        int px = text(0, y, W, "Send " + std::to_string(n) + " lines to " + buf_label(b) + "? (y/n) ",
                      T.get("hilight"), false);
        tb_set_cursor(px, y);
        present();
        return;
    }
    if (term && !c.asking(b)) {  // keys go to the shell: the input line only says so
        text(0, y, W, "[" + buf_label(b) + "] ", T.get("hilight"), false);
        text((int)buf_label(b).size() + 3, y, W, "keys go to the shell · Alt+←/→ or Alt+1-9: other tabs · wheel: scrollback · exit ends it", sty(T, "timestamp", "input"), false);
        VTermPos p;
        vterm_state_get_cursorpos(vterm_obtain_state(term->vt), &p);
        if (term->cursor && b->scroll == 0 && p.row < ch && p.col < cw) tb_set_cursor(p.col, top + p.row);
        else tb_hide_cursor();
        present();
        return;
    }
    if (c.asking(b)) {  // /shell login, /e2e and /oper passwords: a dialog box
        draw_dialog(W, H, c.pass_title.empty() ? "Password" : c.pass_title, c.pass_info, c.pass_label, c.pass_hidden, input, cur, "Enter: OK · Esc: cancel");
        present();
        return;
    }
    int px = text(0, y, W, "[" + buf_label(b) + "] ", sty(T, "input_prompt", "input"), false);
    int avail = std::max(1, W - px - 1);
    int curw = 0;
    for (size_t i = 0; i < cur; i++) curw += cwidth(input[i]);
    int off = std::max(0, curw - avail);
    int col = 0;
    for (char32_t ch32 : input) {
        int w = cwidth(ch32);
        if (col >= off && col - off + w <= avail) {
            if (ch32 < 0x20) {  // formatting codes show as reverse letters, like irssi
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
void Tui::draw_dialog(int W, int H, const std::string& title, const std::vector<std::string>& info, const std::string& label,
                      bool hidden, const std::u32string& in, size_t at, const std::string& hint) {
    const Theme& T = c.theme;
    Style bs = sty(T, "menu", "nicklist"), fs = T.get("input"), es = T.get("error"), hs = bs, ns = sty(T, "timestamp", "menu"), ls = sty(T, "menu_border", "separator");
    for (Style* st : {&es, &ns, &ls}) st->bg = bs.bg;
    hs.attr |= A_BOLD;
    int bw = std::min(W - 2, 48), x0 = (W - bw) / 2, inner = bw - 4;
    int bh = 5 + (int)info.size(), y0 = std::max(0, (H - bh) / 2 - 1);
    for (int yy = y0; yy < y0 + bh; yy++) fill(yy, x0, x0 + bw, bs);
    for (int xx = x0; xx < x0 + bw; xx++) cell(xx, y0, U'─', ls), cell(xx, y0 + bh - 1, U'─', ls);
    for (int yy = y0; yy < y0 + bh; yy++) cell(x0, yy, U'│', ls), cell(x0 + bw - 1, yy, U'│', ls);
    cell(x0, y0, U'┌', ls), cell(x0 + bw - 1, y0, U'┐', ls), cell(x0, y0 + bh - 1, U'└', ls), cell(x0 + bw - 1, y0 + bh - 1, U'┘', ls);
    text(x0 + 2, y0, x0 + bw - 2, " " + title + " ", hs, false);
    int yy = y0 + 1;
    for (auto& l : info)
        text(x0 + 2, yy++, x0 + 2 + inner, l[0] == '!' ? l.substr(1) : l, l[0] == '!' ? es : bs, false);
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
    tb_set_cursor(fx + curw - off, fy);
}

// ---- the settings window (/set alone) ----------------------------------------
//   ┌─ Settings ───────────────────────────────────────────────┐
//   │ / search: tls_                                           │
//   │ All          50 │ [server.libera]                        │
//   │ general      36 │  tls              true                 │
//   │ ⇢ libera      6 │  tls_verify       default: true        │
//   │ ↑↓ move · e edit · Space on/off · d default · s save · q │
//   └──────────────────────────────────────────────────────────┘
static std::string lower(std::string s) { for (auto& ch : s) ch = (char)tolower((unsigned char)ch); return s; }
static std::string full_key(const Core::SetRow& r) { return (r.sec == "general" ? "" : r.sec + ".") + r.key; }

// the categories with how many keys match the search, and the rows of the chosen one
static void set_view(const std::vector<Core::SetRow>& all, const std::string& q, int& cat,
                     std::vector<std::string>& cats, std::vector<int>& counts, std::vector<const Core::SetRow*>& rows) {
    auto match = [&](const Core::SetRow& r) { return q.empty() || lower(full_key(r)).find(q) != std::string::npos || (!r.secret && lower(r.val).find(q) != std::string::npos); };
    cats = {""};
    for (auto& r : all)
        if (std::find(cats.begin(), cats.end(), r.sec) == cats.end()) cats.push_back(r.sec);
    counts.assign(cats.size(), 0);
    for (auto& r : all)
        if (match(r)) counts[0]++, counts[(size_t)(std::find(cats.begin(), cats.end(), r.sec) - cats.begin())]++;
    if (cat >= (int)cats.size() || (cat > 0 && !counts[(size_t)cat])) cat = 0;  // a search with no hits here: All
    rows.clear();
    for (auto& r : all)
        if (match(r) && (cat == 0 || r.sec == cats[(size_t)cat])) rows.push_back(&r);
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
    text(x0 + 2, y0, x1, " Settings ", hs, false);

    auto all = c.setting_rows();
    std::vector<std::string> cats;
    std::vector<int> counts;
    std::vector<const Core::SetRow*> rows;
    set_view(all, lower(to_utf8(sw.q)), sw.cat, cats, counts, rows);
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
    for (int i = 0; i < (int)cats.size() && i < n; i++) {
        std::string name = cats[(size_t)i].empty() ? "All" : cats[(size_t)i].rfind("server.", 0) == 0 ? "\u21E2 " + cats[(size_t)i].substr(7) : cats[(size_t)i];
        bool on = i == sw.cat, uns = false;
        for (auto& r : all) uns |= r.unsaved && (i == 0 || r.sec == cats[(size_t)i]);
        Style st = on ? sel : counts[(size_t)i] ? bs : dim;
        if (on && sw.focus != 0) st = keyst, st.attr |= A_BOLD;  // chosen, but the keys have the focus
        fill(ly + i, x0 + 2, x0 + 2 + nw - 1, on && sw.focus == 0 ? sel : bs);
        text(x0 + 3, ly + i, x0 + nw - 5, name, st, false);
        std::string cnt = (uns ? "\u25CF " : "") + std::to_string(counts[(size_t)i]);
        int cl = (int)cnt.size() - (uns ? 2 : 0);
        text(x0 + 2 + nw - 2 - cl, ly + i, x0 + 2 + nw, cnt, on && sw.focus == 0 ? sel : dim, false);
    }
    for (int yy = ly - 1; yy < ly + n; yy++) cell(x0 + 2 + nw, yy, U'│', ls);

    // keys: with section headers when showing All (or a search across sections)
    struct DL { const Core::SetRow* r; std::string head; };
    std::vector<DL> lines;
    std::string last;
    int sel_line = 0;
    for (size_t i = 0; i < rows.size(); i++) {
        if (rows[i]->sec != last) lines.push_back({nullptr, "[" + rows[i]->sec + "]"}), last = rows[i]->sec;
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
        if (L.r->choices.size() == 2 && L.r->key != "theme") {  // on/off: ● on, ○ off, (default) when not set
            bool yes = eff == L.r->choices[0];
            Style ts = on ? sel : yes ? val : dim;
            int ex = text(vx, yy, vmax, std::string(yes ? "\u25CF " : "\u25CB ") + eff, ts, false);
            if (L.r->val.empty()) ex = text(ex, yy, vmax, "  (default)", on ? sel : dim, false);
            if (on) text(ex, yy, vmax, "   Space: switch", sel, false);
            continue;
        }
        std::string v = L.r->secret ? (L.r->val.empty() ? "(not set)" : "********") : L.r->val.empty() ? (L.r->def.empty() ? "(not set)" : "default: " + L.r->def) : L.r->val;
        int ex = text(vx, yy, vmax, v, on ? sel : L.r->val.empty() ? dim : val, false);
        if (on && !L.r->choices.empty()) {  // the themes: where this one is among them
            std::string list;
            for (auto& ch : L.r->choices) list += (list.empty() ? "" : " ") + (ch == eff ? "[" + ch + "]" : ch);
            text(ex, yy, vmax, "   Space: next  " + list, sel, false);
        }
    }
    // what happened, or what the keys do
    std::string hint = sw.editing     ? "Enter: set it · Esc: cancel · empty = the default"
                       : sw.searching ? "type to search · Enter or \u2193: to the list · Esc: clear it"
                       : sw.focus == 0 ? "\u2191\u2193: category · \u2192 Tab Enter: its keys · /: search · q Esc: close"
                                       : "\u2191\u2193: move · e Enter: edit · Space: on/off, next theme · d: default · /: search · s: save · q: close";
    text(x0 + 2, y1 - 1, x1 - 1, sw.msg.empty() ? hint : sw.msg, sw.msg.empty() ? dim : mark, false);
}

void Tui::settings_key(int mod, int k, uint32_t ch) {
    auto all = c.setting_rows();
    std::vector<std::string> cats;
    std::vector<int> counts;
    std::vector<const Core::SetRow*> rows;
    set_view(all, lower(to_utf8(sw.q)), sw.cat, cats, counts, rows);
    const Core::SetRow* r = rows.empty() ? nullptr : rows[(size_t)std::max(0, std::min(sw.sel, (int)rows.size() - 1))];
    bool bs = k == TB_KEY_BACKSPACE || k == TB_KEY_BACKSPACE2;
    if (sw.editing) {
        if (k == TB_KEY_ESC) { sw.editing = false; return; }
        if (k == TB_KEY_ENTER && r) {
            std::string v = to_utf8(sw.ed);
            c.exec(c.active, "/set " + full_key(*r) + " " + (v.empty() ? "\"\"" : v));
            sw.editing = false;
            sw.msg = full_key(*r) + (v.empty() ? " is back to its default" : " = " + (r->secret ? std::string("********") : v)) + "  (not saved yet: s)";
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
        for (int i = sw.cat + d; d && i >= 0 && i < nc; i += d)
            if (counts[(size_t)i]) { sw.cat = i, sw.sel = sw.top = 0; break; }
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
    else if (k == TB_KEY_DELETE && r) {
        c.exec(c.active, "/set " + full_key(*r) + " \"\"");
        sw.msg = full_key(*r) + " is back to its default  (not saved yet: s)";
    }
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
        if (i < (int)cats.size() && counts[(size_t)i]) sw.cat = i, sw.sel = sw.top = 0, sw.focus = 0;
        return;
    }
    // the row under the pointer, counting the section headers the way draw_settings lays them out
    std::string last;
    int line = 0, want = sw.top + i;
    for (int j = 0; j < (int)rows.size(); j++) {
        if (rows[(size_t)j]->sec != last) last = rows[(size_t)j]->sec, line++;
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
void Tui::draw_term(Term* t, int top, int rows, int cols) {
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
            cell(x, top + r, cp < 0x20 || cp == 0x7f ? ' ' : cp, st);  // never a control byte to the real terminal
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
    keymap["M-d"] = keymap["∂"] = "detach";  // ∂: Option+D on a Mac without "Option as Meta"
    // [keys] action = spec [spec...]
    auto ks = c.cfg.find("keys");
    if (ks != c.cfg.end())
        for (auto& [act, specs] : ks->second)
            for (auto& sp : split(specs, ' ')) keymap[sp] = act;
}

void Tui::shutdown() { tb_shutdown(); }

void Tui::fds(int& tty, int& resize) { tb_get_fds(&tty, &resize); }

void Tui::on_readable() {
    tb_event ev;
    // An Enter followed by more input in the same read is a paste: it becomes
    // a newline in the input line instead of sending each line as it comes.
    bool enter = false;
    while (tb_peek_event(&ev, 0) == TB_OK) {
        if (enter) input.insert(cur++, 1, U'\n'), enter = false;
        if ((c.locked || sw.open || shell_keys()) && ev.type == TB_EVENT_KEY) { key(ev.mod, ev.key, ev.ch); c.dirty = true; continue; }  // a paste is keys too (the settings window: Enter edits)
        bool nl = ev.type == TB_EVENT_KEY && !(ev.mod & TB_MOD_ALT) && (ev.key == TB_KEY_ENTER || ev.key == TB_KEY_CTRL_J);
        if (nl && paste.empty() && confirm.empty() && menu.items.empty() && sugg_sel < 0) { enter = true; continue; }  // a highlighted suggestion: key() takes it
        if (ev.type == TB_EVENT_KEY) key(ev.mod, ev.key, ev.ch);
        else if (ev.type == TB_EVENT_MOUSE) mouse(ev.mod, ev.key, ev.x, ev.y);
        c.dirty = true;
    }
    if (enter) submit(), c.dirty = true;
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
    else if (a == "detach") c.detach_req = true;
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
    if (!menu.items.empty()) {  // the nick menu has the keys until it closes
        int n = (int)menu.items.size(), d = k == TB_KEY_ARROW_DOWN ? 1 : k == TB_KEY_ARROW_UP ? -1 : 0;
        if (d) {
            do menu.sel = (menu.sel + d + n) % n;
            while (menu.items[(size_t)menu.sel].first.empty());
            return;
        }
        std::string cmd = k == TB_KEY_ENTER ? menu.items[(size_t)menu.sel].second : "";  // other keys close it
        menu.items.clear();
        if (!cmd.empty() && cmd[0] == '?') confirm = cmd.substr(1);  // asks first
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
    if (ch || k == TB_KEY_SPACE) { input.insert(cur++, 1, ch ? (char32_t)ch : U' '); return; }
    switch (k) {
    case TB_KEY_ENTER: submit(); break;
    case TB_KEY_BACKSPACE: case TB_KEY_BACKSPACE2: if (cur) input.erase(--cur, 1); break;
    case TB_KEY_DELETE: if (cur < input.size()) input.erase(cur, 1); break;
    case TB_KEY_ARROW_LEFT: if (cur) cur--; break;
    case TB_KEY_ARROW_RIGHT: if (cur < input.size()) cur++; break;
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
    case TB_KEY_CTRL_B: case TB_KEY_CTRL_C: case TB_KEY_CTRL_O: case TB_KEY_CTRL_V:
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
// left click picks a tab or an activity number, opens a query with a nick, or
// unfolds the topic (double click: edit it); a drag in the chat copies text.
void Tui::mouse(int mod, int k, int x, int y) {
    if (c.locked) return;
    if (sw.open) { if (!(mod & TB_MOD_MOTION)) settings_mouse(k, x, y); return; }
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
    if (mod & TB_MOD_MOTION) return;
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
            if (!h.nick.empty()) { open_menu(h.nick, x, y); continue; }
            if (y != top_row() - 1 || h.tab < 0 || h.tab >= (int)c.bufs.size()) continue;
            Buffer* tb = c.bufs[(size_t)h.tab].get();
            if (tb->type == BT::STATUS && tb->srv) c.fold(tb);  // a server tab: fold or unfold its channels
            else if (tb->type == BT::CHANNEL) c.select(tb), menu = Menu{x, y, 0, tb->name, c.tab_menu(tb)}, c.dirty = true;  // its commands run there
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
        auto me = b->nicks.find(irc_lower(b->srv->nick));
        std::string my = me == b->nicks.end() ? "" : me->second.prefix;
        if (b->modes.find('t') != std::string::npos && !rank_ok(b->srv, my, '%')) {
            c.print(b, LK::ERROR, b->name + " is +t: only channel operators can change the topic");
            return;
        }
        input = from_utf8("/topic " + b->topic), cur = input.size(), completing = false;
        return;
    }
    for (auto& h : hits) {
        if (h.y != y || x < h.x0 || x >= h.x1) continue;
        if (h.tab >= 0 && h.tab < (int)c.bufs.size()) c.select(c.bufs[(size_t)h.tab].get());
        else if (!h.nick.empty()) c.exec(c.active, "/query " + h.nick);
        return;
    }
    if (over_chat) {
        Buffer* b = c.active;
        std::string w = word_at(x, y);
        // *channels: a click on the Channel or Users header sorts by it
        if (b->srv && b->name == "*channels" && (w == "Channel" || w == "Users")) { c.list_sort(b, w == "Channel" ? "name" : "users"); return; }
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

void Tui::open_menu(const std::string& nick, int x, int y) {
    if (!c.active->srv) return;
    menu = Menu{x, y, 0, nick, c.nick_menu(c.active, nick)};
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
    if (hist.empty() || hist.back() != line) hist.push_back(line);
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
