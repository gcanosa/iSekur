// isekur.* for what is drawn: tabs, the theme and nick colors, the folded join/part rows, the screensaver, the focus view and the
// plugin window. Their state lives in Core (core.h); the terminal and the webchat draw it.
#include "plugin/internal.h"

using namespace plug;

static int l_sort_tabs(lua_State* L) {  // sort_tabs(): each network's tabs together, status first, the rest by name; true if the order changed
    auto& bs = self(L)->c->bufs;
    std::map<const Server*, size_t> first;  // a network's group sits where its first tab is, so networks keep their order
    for (size_t i = 0; i < bs.size(); i++)
        if (bs[i]->srv) first.emplace(bs[i]->srv, i);
    auto grp = [&](size_t i) { return bs[i]->srv ? first[bs[i]->srv] : i; };
    std::vector<size_t> ord(bs.size());
    for (size_t i = 0; i < ord.size(); i++) ord[i] = i;
    std::stable_sort(ord.begin(), ord.end(), [&](size_t x, size_t y) { return grp(x) < grp(y); });
    // within a network: a tab the user /moved keeps its place in the group, the others fill the rest by name
    for (size_t s = 0, e; s < ord.size(); s = e) {
        for (e = s; e < ord.size() && grp(ord[e]) == grp(ord[s]); e++) {}
        std::vector<size_t> free;
        for (size_t k = s; k < e; k++) if (!bs[ord[k]]->placed) free.push_back(ord[k]);
        auto key = [&](size_t i) { std::string n = bs[i]->name; return std::make_tuple(bs[i]->type != BT::STATUS, to_lower(n)); };  // to_lower works in place
        std::stable_sort(free.begin(), free.end(), [&](size_t x, size_t y) { return key(x) < key(y); });
        for (size_t k = s, f = 0; k < e; k++) if (!bs[ord[k]]->placed) ord[k] = free[f++];
    }
    bool moved = !std::is_sorted(ord.begin(), ord.end());  // ord is a permutation: sorted means unchanged
    if (moved) {
        std::vector<std::unique_ptr<Buffer>> out;
        out.reserve(bs.size());
        for (size_t i : ord) out.push_back(std::move(bs[i]));
        bs = std::move(out);
        Core* c = self(L)->c;
        c->dirty = true;
        if (c->active) c->notify(Ev::BUFFER, c->active);  // the webchat's tab list follows
    }
    lua_pushboolean(L, moved);
    return 1;
}

// isekur.theme(): the loaded theme's colors, 0xRRGGBB (nil: the terminal's own)
static int l_theme(lua_State* L) {
    const Theme& t = self(L)->c->theme;  // the configured one, not a view's
    Style d = t.get("default");
    lua_createtable(L, 0, 6);
    lua_pushstring(L, t.name.c_str()), lua_setfield(L, -2, "name");
    auto col = [&](const char* k, RGB c) { if (c >= 0) lua_pushinteger(L, c), lua_setfield(L, -2, k); };
    col("fg", d.fg), col("bg", d.bg), col("hilight", t.get("hilight").fg), col("timestamp", t.get("timestamp").fg);
    if (self(L)->c->own_nick_on()) col("self", t.self_nick);  // your nick's own color (own_nick_color): none of `nicks`
    lua_createtable(L, (int)t.nicks.size(), 0);
    for (size_t i = 0; i < t.nicks.size(); i++) lua_pushinteger(L, t.nicks[i]), lua_rawseti(L, -2, (lua_Integer)i + 1);
    lua_setfield(L, -2, "nicks");
    return 1;
}
// isekur.nick_color(srv, nick): the color the chat gives that nick, 0xRRGGBB: yours is its own (the theme's `self`), anyone's else
// comes from a hash of the nick (one of the theme's `nicks`). nil when the theme has no nick colors
static int l_nick_color(lua_State* L) {
    Core& c = *self(L)->c;
    RGB r = c.nick_color(srv_arg(L, 1), str_arg(L, 2), c.theme);
    if (r < 0) lua_pushnil(L);
    else lua_pushinteger(L, r);
    return 1;
}
// the web gets each channel again (its "nc" says whether the list is painted); the terminal just draws
void plug::nicklist_by(Core& c, const std::string& by) {
    if (by == c.nicklist_by) return;
    c.nicklist_by = by, c.dirty = true;
    for (auto& b : c.bufs)
        if (b->type == BT::CHANNEL) c.notify(Ev::BUFFER, b.get());
}
// isekur.nicklist_colors(on): the user list shows each nick in its chat color, in the tabs where your plugin is on (display only).
// One plugin at a time; it ends with the plugin. isekur.nicklist_colors() says whether yours is on
static int l_nicklist_colors(lua_State* L) {
    Plugin* p = self(L);
    Core& c = *p->c;
    if (lua_isnone(L, 1)) return lua_pushboolean(L, c.nicklist_by == p->name), 1;
    if (lua_toboolean(L, 1)) nicklist_by(c, p->name);
    else if (c.nicklist_by == p->name) nicklist_by(c, "");
    return 0;
}
void plug::fold_by(Core& c, const std::string& by) {
    if (by == c.fold_by) return;
    c.fold_by = by, c.dirty = true;
    for (auto& b : c.bufs)
        if (b->type == BT::CHANNEL) c.notify(Ev::BUFFER, b.get());  // the webchat re-reads how to draw it
}
// isekur.fold(on): runs of joins, parts, quits and nick changes show as one row, a click opens them, in the tabs where your
// plugin is on (display only). It reads your settings names, nicks (booleans) and quiet ("hide" | "fold"). One plugin at a
// time; it ends with the plugin. isekur.fold() says whether yours is on
static int l_fold(lua_State* L) {
    Plugin* p = self(L);
    Core& c = *p->c;
    if (lua_isnone(L, 1)) return lua_pushboolean(L, c.fold_by == p->name), 1;
    if (lua_toboolean(L, 1)) fold_by(c, p->name);
    else if (c.fold_by == p->name) fold_by(c, "");
    return 0;
}

// ---- the screensaver (API 5) ------------------------------------------------------------
static std::vector<Core::SCell>* g_grid = nullptr;  // the frame being drawn
static int g_gw = 0, g_gh = 0;
// put(x, y, text, color 0xRRGGBB): only from a screen frame; off the grid is clipped. x and y may be fractions:
// the webchat draws between cells (smooth slow motion), the terminal in the cell
static int l_put(lua_State* L) {
    if (!g_grid) return luaL_error(L, "put: only while drawing a screen frame");
    double fx = luaL_checknumber(L, 1), fy = luaL_checknumber(L, 2);
    if (!std::isfinite(fx) || !std::isfinite(fy) || fx < -1e6 || fx > 1e6 || fy < -1e6 || fy > 1e6) return 0;
    int x = (int)std::floor(fx), y = (int)std::floor(fy);
    auto ox = (uint8_t)std::min(7, (int)((fx - x) * 8)), oy = (uint8_t)std::min(7, (int)((fy - y) * 8));
    std::string s = str_arg(L, 3);
    int fg = (int)(luaL_optinteger(L, 4, 0xffffff) & 0xffffff);
    if (y < 0 || y >= g_gh) return 0;
    for (size_t i = 0; i < s.size() && x < g_gw; x++) {
        uint32_t ch = utf8_next(s, i);
        if (x >= 0) (*g_grid)[(size_t)y * g_gw + x] = {ch == ' ' ? 0 : ch, fg, ox, oy};
    }
    return 0;
}
static int l_screen(lua_State* L) {  // screen(fn, fps): a screensaver until a key, true if it started (not with no terminal or webchat attached); screen(false) ends yours; screen(): is one up
    Plugin* p = self(L);
    Core& c = *p->c;
    if (lua_isnone(L, 1)) return lua_pushboolean(L, !c.screen_by.empty()), 1;
    if (!lua_isfunction(L, 1)) {
        if (c.screen_by == p->name) c.screen_end();
        return 0;
    }
    if (!c.ui_on) return lua_pushboolean(L, 0), 1;  // nobody at a terminal or a webchat to see it
    luaL_unref(L, LUA_REGISTRYINDEX, p->screen_fn);
    lua_pushvalue(L, 1);
    p->screen_fn = luaL_ref(L, LUA_REGISTRYINDEX);
    c.screen_fps = std::clamp((int)luaL_optinteger(L, 2, 15), 1, 30);
    bool was = !c.screen_by.empty();
    crumb("screen", p->name.c_str());
    c.screen_by = p->name, c.screen_t0 = now_ms(), c.dirty = true;
    if (!was) c.notify(Ev::SCREEN, nullptr);
    return lua_pushboolean(L, 1), 1;
}
// view{term, web, width, lines, theme, events}: the focus view (zen) on the terminal and/or the webchat: only the chat, in a
// centered column `width` wide, the lines before the last `lines` dimmed, join/part/quit lines hidden unless `events`,
// drawn in `theme` (nil: the configured one), which is never saved. true, or nil and why. view(false) ends yours, view(): is one up
static int l_view(lua_State* L) {
    Plugin* p = self(L);
    Core& c = *p->c;
    if (lua_isnone(L, 1)) return lua_pushboolean(L, !c.view.by.empty()), 1;
    if (!lua_istable(L, 1)) {
        if (c.view.by == p->name) c.view_end();
        return 0;
    }
    Core::View v;
    v.by = p->name;
    auto flag = [&](const char* k, bool def) { lua_getfield(L, 1, k); bool r = lua_isnil(L, -1) ? def : lua_toboolean(L, -1); lua_pop(L, 1); return r; };
    auto num = [&](const char* k, int def, int lo, int hi) { lua_getfield(L, 1, k); int r = lua_isnumber(L, -1) ? (int)std::clamp<lua_Number>(lua_tonumber(L, -1), lo, hi) : def; lua_pop(L, 1); return r; };
    v.term = flag("term", true), v.web = flag("web", true), v.events = flag("events", true);
    v.width = num("width", 80, 20, 400), v.lines = num("lines", 5, 1, 100);
    lua_getfield(L, 1, "theme");
    std::string tn = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    std::string err;
    if (!tn.empty() && !(v.own = load_theme(tn, v.theme, err))) return lua_pushnil(L), lua_pushstring(L, err.c_str()), 2;
    c.view = v, c.dirty = true;
    c.notify(Ev::THEME, nullptr);
    return lua_pushboolean(L, 1), 1;
}

// the strings of t[key] (a list), one line each, at most n of them and len bytes each (never cut inside a UTF-8 character)
static std::vector<std::string> str_list(lua_State* L, int t, const char* key, size_t n, size_t len) {
    std::vector<std::string> v;
    lua_getfield(L, t, key);
    if (lua_istable(L, -1))
        for (lua_Integer i = 1; (size_t)v.size() < n; i++) {
            lua_rawgeti(L, -1, i);
            if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
            std::string s = luaL_tolstring(L, -1, nullptr);
            lua_pop(L, 2);
            for (char& ch : s)
                if ((unsigned char)ch < 32 || ch == 127) ch = ' ';
            if (s.size() > len) {
                s.resize(len);
                while (!s.empty() && ((unsigned char)s.back() & 0xc0) == 0x80) s.pop_back();
                if (!s.empty() && (unsigned char)s.back() >= 0xc0) s.pop_back();
            }
            v.push_back(s);
        }
    lua_pop(L, 1);
    return v;
}
// isekur.window{title, display = {lines}, items = {rows}, sel, mark, star, buttons = {labels}, icons = {a symbol for each button}, reels, slider = {value, max, step, icon}}: opens or updates your window, in the
// terminal and the webchat. sel is the row the cursor starts on (it moves when this number changes), mark the row that is playing and
// star one drawn with a ★ (1-based, 0: none). reels: a cassette in the display, its reels turning when true, still when false (nothing if left out). A click, Enter or Esc comes back as the `window` event. isekur.window(false) closes it, isekur.window() says if it's open
static int l_window(lua_State* L) {
    Plugin* p = self(L);
    Core& c = *p->c;
    if (lua_isnone(L, 1)) return lua_pushboolean(L, c.pwin.open && c.pwin.by == p->name), 1;
    if (!lua_istable(L, 1)) {
        if (c.pwin.open && c.pwin.by == p->name) c.pwin = Core::PWin{}, c.dirty = true, c.notify(Ev::PWIN, nullptr);
        return 0;
    }
    Core::PWin w;
    w.by = p->name, w.open = true;
    lua_getfield(L, 1, "title");
    w.title = lua_isstring(L, -1) ? one_line(lua_tostring(L, -1)).substr(0, 60) : p->name;
    lua_pop(L, 1);
    w.display = str_list(L, 1, "display", 4, 200), w.items = str_list(L, 1, "items", 300, 120), w.buttons = str_list(L, 1, "buttons", 6, 16), w.icons = str_list(L, 1, "icons", 6, 8);
    auto row = [&](const char* k) {
        lua_getfield(L, 1, k);
        int r = lua_isnumber(L, -1) ? (int)std::clamp<lua_Number>(lua_tonumber(L, -1), 0, (lua_Number)w.items.size()) : 0;
        lua_pop(L, 1);
        return r;
    };
    w.sel = row("sel"), w.mark = row("mark"), w.star = row("star");
    lua_getfield(L, 1, "reels");
    w.reels = lua_isboolean(L, -1) ? lua_toboolean(L, -1) : -1;
    lua_pop(L, 1);
    lua_getfield(L, 1, "slider");  // {value, max, step, icon}: a bar under the list (the radio's volume)
    if (lua_istable(L, -1)) {
        auto num = [&](const char* k, int def, int lo, int hi) { lua_getfield(L, -1, k); int r = lua_isnumber(L, -1) ? (int)std::clamp<lua_Number>(lua_tonumber(L, -1), lo, hi) : def; lua_pop(L, 1); return r; };
        w.slider_max = num("max", 100, 1, 1000), w.slider_step = num("step", 5, 1, w.slider_max), w.slider = num("value", 0, 0, w.slider_max);
        lua_getfield(L, -1, "icon");
        if (lua_isstring(L, -1)) w.slider_icon = one_line(lua_tostring(L, -1)).substr(0, 8);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    if (!c.pwin.same(w)) c.pwin = w, c.dirty = true, c.notify(Ev::PWIN, nullptr);
    return 0;
}

// a click, Enter or Esc in the plugin's window: its `window` event (action "pick" with the row, "button" with the button and the row
// the cursor is on, "close"); only to the plugin that owns it
void Core::pwin_act(const std::string& a, int i, int bt) {
    if (!pwin.open) return;
    std::string by = pwin.by;
    PArgs args{{"action", a == "close" ? std::string("close") : a == "pick" ? std::string("pick") : a == "slider" ? std::string("slider") : std::string("button")}};
    if (a == "close") pwin = PWin{}, dirty = true, notify(Ev::PWIN, nullptr);
    else if (a == "slider") {  // its new value; the other side (terminal or webchat) follows
        if (pwin.slider < 0) return;
        i = std::clamp(i, 0, pwin.slider_max);
        pwin.slider = i, dirty = true;
        notify(Ev::PWIN, nullptr);
        args.push_back({"value", i});
    } else {
        i = std::clamp(i, 0, (int)pwin.items.size());
        if (i) pwin.sel = i;
        args.push_back({"index", i});
        if (a != "pick") {
            if (bt < 1 || bt > (int)pwin.buttons.size()) return;
            args.push_back({"button", pwin.buttons[(size_t)bt - 1]});
        }
    }
    emit_only = by;
    plugin_emit("window", nullptr, "", args);
    emit_only.clear();
}
static int l_inactive(lua_State* L) { lua_pushinteger(L, time(nullptr) - self(L)->c->active_at); return 1; }
static int l_lock(lua_State* L) {  // lock(): locks like /lock when a lock password is set; false: none is
    Core& c = *self(L)->c;
    if (c.lock_on() && !c.locked) c.set_lock(true);
    lua_pushboolean(L, c.locked);
    return 1;
}

bool Core::screen_frame(int w, int h, const char* ui, std::vector<SCell>& g) {
    g.assign((size_t)std::max(0, w) * std::max(0, h), SCell{});
    std::shared_ptr<Plugin> p;
    for (auto& x : plugins)
        if (x->name == screen_by && !x->dead && x->screen_fn != LUA_NOREF) p = x;
    if (!p || g.empty() || g_grid) {  // unloaded, or a frame inside a frame
        if (!p) screen_end();
        return false;
    }
    lua_State* L = p->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, p->screen_fn);
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, w), lua_setfield(L, -2, "w");
    lua_pushinteger(L, h), lua_setfield(L, -2, "h");
    lua_pushstring(L, ui), lua_setfield(L, -2, "ui");
    lua_pushinteger(L, now_ms() - screen_t0), lua_setfield(L, -2, "t");
    g_grid = &g, g_gw = w, g_gh = h;
    bool ok = call(p.get(), 1, 0);
    g_grid = nullptr;
    if (!ok) screen_end();  // one error, not one a frame
    reap(*this);
    return ok;
}

// the user list of b is painted: a plugin asked (isekur.nicklist_colors) and is on in that channel
bool Core::nicklist_colored(const Buffer* b) const {
    return !nicklist_by.empty() && b->srv && b->type == BT::CHANNEL && truthy(plugin_opt(nicklist_by, "enabled", b->srv, b->name, "off", 'c'));
}

Core::FoldCfg Core::fold_cfg(const Buffer* b) const {
    FoldCfg f;
    if (fold_by.empty() || !b->srv || b->type != BT::CHANNEL || !truthy(plugin_opt(fold_by, "enabled", b->srv, b->name, "off", 'c'))) return f;
    f.on = true;
    f.names = truthy(plugin_opt(fold_by, "names", b->srv, b->name, "off", 'c'));
    f.nicks = truthy(plugin_opt(fold_by, "nicks", b->srv, b->name, "on", 'c'));
    f.quiet = plugin_opt(fold_by, "quiet", b->srv, b->name, "hide", 'c') == "fold";
    return f;
}

// what the isekur table gets from this file (host.cpp's load registers it)
const luaL_Reg plug::api_ui[] = {
    {"window", l_window}, {"screen", l_screen}, {"put", l_put}, {"inactive", l_inactive}, {"view", l_view},
    {"lock", l_lock}, {"sort_tabs", l_sort_tabs}, {"theme", l_theme}, {"nick_color", l_nick_color},
    {"nicklist_colors", l_nicklist_colors}, {"fold", l_fold},
    {nullptr, nullptr}};
