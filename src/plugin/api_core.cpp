// isekur.* for chat, settings, the plugin's own store, the network (http, embed, proxy) and what the client is doing.
#include "plugin/internal.h"

using namespace plug;

std::string json_str(const std::string& s);  // web.cpp

static int l_on(lua_State* L) {
    std::string ev = str_arg(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    self(L)->on[ev].push_back(luaL_ref(L, LUA_REGISTRYINDEX));
    return 0;
}
static int l_command(lua_State* L) {
    Plugin* p = self(L);
    std::string name = str_arg(L, 1), usage = opt_str(L, 2);
    to_lower(name);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    if (name.empty() || name.find(' ') != std::string::npos) luaL_error(L, "bad command name");
    if (find_command(name) || name == "plugin" || name == "plugins") luaL_error(L, "/%s is iSekur's own", name.c_str());
    for (auto& o : p->c->plugins)
        if (o.get() != p && o->cmds.count(name)) luaL_error(L, "/%s is taken by %s", name.c_str(), o->name.c_str());
    lua_pushvalue(L, 3);
    p->cmds[name] = {luaL_ref(L, LUA_REGISTRYINDEX), usage};
    return 0;
}
// setting(key, default, scope "g"|"n"|"c", help: what it is, its unit, choices: {"a", "b"} the only values it takes)
static int l_setting(lua_State* L) {
    std::string key = str_arg(L, 1), sc = luaL_optstring(L, 3, "c");
    int t = lua_type(L, 2);
    std::string def = t == LUA_TBOOLEAN ? (lua_toboolean(L, 2) ? "on" : "off") : t == LUA_TNIL ? "" : str_arg(L, 2);
    if (sc != "g" && sc != "n" && sc != "c") luaL_error(L, "scope is g, n or c");
    Plugin* p = self(L);
    if (!p->opts.count(key)) p->order.push_back(key);
    std::vector<std::string> ch;
    if (!lua_isnoneornil(L, 5)) {
        luaL_checktype(L, 5, LUA_TTABLE);
        for (lua_Integer i = 1; i <= 32 && lua_rawgeti(L, 5, i) == LUA_TSTRING; i++, lua_pop(L, 1))
            if (std::string v = one_line(lua_tostring(L, -1)).substr(0, 32); !v.empty() && v.find(' ') == std::string::npos) ch.push_back(v);
        lua_pop(L, 1);
    }
    p->opts[key] = {def, t, sc[0], one_line(opt_str(L, 4)).substr(0, 120), ch};
    return 0;
}
static int l_opt(lua_State* L) {  // opt(key, srv, target): channel, network, global, the default
    Plugin* p = self(L);
    std::string key = str_arg(L, 1);
    auto d = p->opts.find(key);
    Plugin::Opt o = d != p->opts.end() ? d->second : Plugin::Opt{"", LUA_TSTRING, 'c', ""};
    if (key == "enabled") o = {"off", LUA_TBOOLEAN, 'c', ""};  // iSekur's own: on there or not
    std::string v = p->c->plugin_opt(p->name, key, srv_arg(L, 2), opt_str(L, 3), o.def, o.scope);
    if (o.type == LUA_TBOOLEAN) lua_pushboolean(L, truthy(v));
    else if (o.type == LUA_TNUMBER) lua_pushnumber(L, atof(v.c_str()));
    else lua_pushlstring(L, v.data(), v.size());
    return 1;
}
// set(key, value, srv): one of the plugin's own settings, for everywhere or (scope n/c) that network; nil or "" clears it.
// Saved to the config like /plugin set. Never "enabled": a plugin doesn't turn itself on
static int l_set(lua_State* L) {
    Plugin* p = self(L);
    std::string key = str_arg(L, 1);
    auto o = p->opts.find(key);
    if (o == p->opts.end()) luaL_error(L, "%s isn't one of this plugin's settings", key.c_str());
    std::string val = lua_isnoneornil(L, 2) ? "" : one_line(lua_type(L, 2) == LUA_TBOOLEAN ? (lua_toboolean(L, 2) ? "on" : "off")
                                                                                              : luaL_tolstring(L, 2, nullptr));
    if (lua_type(L, 2) != LUA_TBOOLEAN && !lua_isnoneornil(L, 2)) lua_pop(L, 1);
    if (val.size() > 4096) luaL_error(L, "value too long");
    Server* s = o->second.scope == 'g' ? nullptr : srv_arg(L, 3);
    if (!s && o->second.scope != 'g' && !lua_isnoneornil(L, 3)) luaL_error(L, "no network %s", lua_tostring(L, 3));  // not everywhere's by mistake
    std::string sec = plugin_sec(p->name, s ? s->cfg.name : "");
    p->c->cfg[sec][key] = val;
    std::string err;
    if (!p->c->cfg_path.empty() && !ini_save(p->c->cfg_path, Ini{{sec, {{key, val}}}}, err)) luaL_error(L, "%s", err.c_str());
    return 0;
}
static int l_say(lua_State* L) {
    Server* s = live_srv(L, 1);
    std::string t = one_line(str_arg(L, 2));
    for (auto& line : split(str_arg(L, 3), '\n')) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (Buffer* b = self(L)->c->find(s, t)) self(L)->c->say(b, line);
        else s->send("PRIVMSG " + t + " :" + line);
    }
    return 0;
}
static int l_send(lua_State* L) { live_srv(L, 1)->send(one_line(str_arg(L, 2))); return 0; }
static int l_print(lua_State* L) {  // print(srv, target, text, note): note true adds the INFO badge
    Buffer* b = tab(L, 1, 2);
    self(L)->c->plug_print(b, LK::INFO, str_arg(L, 3), self(L)->name, lua_toboolean(L, 4) ? 'i' : 'p');
    return 0;
}
static int l_exec(lua_State* L) {  // exec(srv, target, "/command ...") as if typed in that tab
    Buffer* b = tab(L, 1, 2);
    if (b) self(L)->c->exec(b, one_line(str_arg(L, 3)));
    return 0;
}

// the store: <name>.data next to the plugin, "key\tvalue" lines, \\ \t \n escaped
static std::string store_path(Plugin* p) { return p->c->plugin_dir() + "/" + p->name + ".data"; }
static std::string esc(const std::string& s) {
    std::string o;
    for (char ch : s) o += ch == '\\' ? "\\\\" : ch == '\t' ? "\\t" : ch == '\n' ? "\\n" : std::string(1, ch);
    return o;
}
static std::string unesc(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++)
        if (s[i] == '\\' && i + 1 < s.size()) i++, o += s[i] == 't' ? '\t' : s[i] == 'n' ? '\n' : s[i];
        else o += s[i];
    return o;
}
static void store_load(Plugin* p) {
    if (p->store_read) return;
    p->store_read = true;
    FILE* f = fopen(store_path(p).c_str(), "r");
    if (!f) return;
    std::string all;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) all.append(buf, n);
    fclose(f);
    for (auto& l : split(all, '\n'))
        if (size_t tb = l.find('\t'); tb != std::string::npos) p->store[unesc(l.substr(0, tb))] = unesc(l.substr(tb + 1));
}
static int l_store_get(lua_State* L) {
    Plugin* p = self(L);
    store_load(p);
    auto it = p->store.find(str_arg(L, 1));
    if (it == p->store.end()) lua_pushnil(L);
    else lua_pushlstring(L, it->second.data(), it->second.size());
    return 1;
}
// the whole file through a temporary one; false: it couldn't be written (it stays dirty, tried again later)
bool plug::store_flush(Plugin* p) {
    if (!p->store_dirty) return true;
    std::string path = store_path(p), tmp = path + ".tmp";
    mkdir_p(p->c->plugin_dir());
    mode_t old_mask = umask(077);  // private from its first byte, not after a chmod
    FILE* f = fopen(tmp.c_str(), "w");
    umask(old_mask);
    p->store_at = time(nullptr);
    if (!f) return false;
    for (auto& [kk, v] : p->store) fprintf(f, "%s\t%s\n", esc(kk).c_str(), esc(v).c_str());
    bool ok = fclose(f) == 0 && rename(tmp.c_str(), path.c_str()) == 0;
    if (ok) p->store_dirty = false;
    else unlink(tmp.c_str());
    return ok;
}
Plugin::~Plugin() {
    if (L) lua_close(L);
    store_flush(this);  // unloaded, removed, or iSekur exiting: nothing set is lost
}
static int l_store_set(lua_State* L) {  // store_set(key, value): nil removes it; on disk within STORE_EVERY seconds
    Plugin* p = self(L);
    store_load(p);
    std::string k = str_arg(L, 1);
    if (lua_isnoneornil(L, 2)) p->store.erase(k);
    else p->store[k] = luaL_tolstring(L, 2, nullptr), lua_pop(L, 1);
    p->store_dirty = true;
    return 0;
}
static int l_store_keys(lua_State* L) {  // every key in the store (unordered)
    Plugin* p = self(L);
    store_load(p);
    lua_createtable(L, (int)p->store.size(), 0);
    int i = 1;
    for (auto& [k, v] : p->store) lua_pushlstring(L, k.data(), k.size()), lua_rawseti(L, -2, i++);
    return 1;
}
static int l_active(lua_State* L) {  // the tab you're on: its network (nil: none) and name
    Core& c = *self(L)->c;
    if (!c.active) return 0;
    if (c.active->srv) lua_pushstring(L, c.active->srv->cfg.name.c_str());
    else lua_pushnil(L);
    lua_pushstring(L, c.active->name.c_str());
    return 2;
}
static int l_date(lua_State* L) {  // date(t, fmt): strftime in local time; fmt defaults to the [general] timestamp
    Core& c = *self(L)->c;
    time_t t = (time_t)luaL_optinteger(L, 1, time(nullptr));
    std::string fmt = luaL_optstring(L, 2, ini_get(c.cfg, "general", "timestamp", c.theme.lay("timestamp", "%H:%M")).c_str());
    std::string out = fmt_time(t, fmt.substr(0, 64));
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}
static int l_is_chan(lua_State* L) {  // is_chan(srv, name): a channel by that network's CHANTYPES
    Server* s = srv_arg(L, 1);
    lua_pushboolean(L, s ? s->is_chan(str_arg(L, 2)) : !str_arg(L, 2).empty() && (str_arg(L, 2)[0] == '#' || str_arg(L, 2)[0] == '&'));
    return 1;
}
static int l_servers(lua_State* L) {  // the connected ones' names
    lua_newtable(L);
    int i = 1;
    for (auto& s : self(L)->c->servers)
        if (s->state == SS::CONNECTED) lua_pushstring(L, s->cfg.name.c_str()), lua_rawseti(L, -2, i++);
    return 1;
}
static int l_channels(lua_State* L) {  // the joined channels on srv
    Server* s = srv_arg(L, 1);
    lua_newtable(L);
    int i = 1;
    for (auto& b : self(L)->c->bufs)
        if (s && b->srv == s && b->type == BT::CHANNEL && b->joined) lua_pushstring(L, b->name.c_str()), lua_rawseti(L, -2, i++);
    return 1;
}
// proxy(srv, url): in a "connect" handler, the connection being made goes through url (nil or "": direct)
static int l_proxy(lua_State* L) {
    Server* s = srv_arg(L, 1);
    std::string url = opt_str(L, 2), err;
    if (!s) luaL_error(L, "proxy: no such network");
    if (url.empty()) { s->conn.px = {}; return 0; }
    Proxy px;
    if (!parse_proxy(url, px, err)) luaL_error(L, "%s", err.c_str());
    s->conn.px = px;
    return 0;
}
static int l_away(lua_State* L) { Server* s = srv_arg(L, 1); lua_pushboolean(L, s && s->away && !s->pre_away); return 1; }  // our AWAY * isn't a message of its own: autoaway may still set one
static int l_attached(lua_State* L) { lua_pushboolean(L, self(L)->c->attached); return 1; }
static int l_idle(lua_State* L) { lua_pushinteger(L, time(nullptr) - self(L)->c->last_input); return 1; }
static int l_detached_for(lua_State* L) {
    Core& c = *self(L)->c;
    lua_pushinteger(L, c.attached ? 0 : time(nullptr) - c.detached_at);
    return 1;
}
static int l_time(lua_State* L) { lua_pushinteger(L, time(nullptr)); return 1; }
static int l_me(lua_State* L) {
    Server* s = srv_arg(L, 1);
    if (s && s->state == SS::CONNECTED) lua_pushstring(L, s->nick.c_str());
    else lua_pushnil(L);
    return 1;
}
static int l_nick(lua_State* L) {  // nick(srv): the nick the config asks for there (me() is the one you have)
    Server* s = srv_arg(L, 1);
    if (s) lua_pushstring(L, s->cfg.nick.c_str());
    else lua_pushnil(L);
    return 1;
}
static int l_rank(lua_State* L) {  // rank(srv, chan, nick): its prefix ("@", "+", ""), nil when not in there
    Server* s = srv_arg(L, 1);
    Buffer* b = s ? self(L)->c->find(s, str_arg(L, 2)) : nullptr;
    auto it = b ? b->nicks.find(s->lower(str_arg(L, 3))) : decltype(b->nicks.end()){};
    if (!b || it == b->nicks.end()) lua_pushnil(L);
    else lua_pushstring(L, it->second.prefix.c_str());
    return 1;
}

// isekur.http(url, fn): a GET on a thread; fn({status, type, body, url, error}) on a tick once it's done.
// ponytail: one job list per process (one Core); 4 at once per plugin, the rest refused
struct HttpJob {
    std::weak_ptr<Plugin> p;
    int fn = LUA_NOREF, status = 0;
    std::string url, ctype, body, err, post;
    std::atomic<bool> done{false};
};
static std::vector<std::shared_ptr<HttpJob>> http_jobs;
static const size_t HTTP_CAP = 512 << 10;
static const int HTTP_AT_ONCE = 4;
static int l_http(lua_State* L) {
    Plugin* p = self(L);
    std::string url = str_arg(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    std::string bad = https_fetch_err(url);
    if (!bad.empty()) return lua_pushboolean(L, 0), lua_pushstring(L, bad.c_str()), 2;
    int mine = 0;
    for (auto& j : http_jobs) mine += j->p.lock().get() == p;
    if (mine >= HTTP_AT_ONCE) return lua_pushboolean(L, 0), lua_pushstring(L, "busy: 4 fetches already running"), 2;
    auto j = std::make_shared<HttpJob>();
    j->p = p->me, j->url = url;
    if (!lua_isnoneornil(L, 3)) j->post = str_arg(L, 3);  // isekur.http(url, fn, body): a POST
    lua_pushvalue(L, 2), j->fn = luaL_ref(L, LUA_REGISTRYINDEX);
    http_jobs.push_back(j);
    std::thread([j] {
        if (!https_fetch(j->url, HTTP_CAP, j->status, j->ctype, j->body, j->err, j->post)) j->status = 0;
        j->done.store(true, std::memory_order_release);
    }).detach();
    lua_pushboolean(L, 1);
    return 1;
}
static void upload_poll(Core& c);
void plug::http_poll(Core& c) {
    auto jobs = http_jobs;
    for (auto& j : jobs) {
        if (!j->done.load(std::memory_order_acquire)) continue;
        http_jobs.erase(std::find(http_jobs.begin(), http_jobs.end(), j));
        auto p = j->p.lock();
        if (!p || p->dead) continue;  // unloaded: its Lua state (and the ref) went with it
        lua_State* L = p->L;
        lua_rawgeti(L, LUA_REGISTRYINDEX, j->fn);
        luaL_unref(L, LUA_REGISTRYINDEX, j->fn);
        lua_createtable(L, 0, 5);
        lua_pushinteger(L, j->status), lua_setfield(L, -2, "status");
        std::string t = j->ctype.substr(0, j->ctype.find(';'));
        while (!t.empty() && t.back() == ' ') t.pop_back();
        to_lower(t);
        lua_pushstring(L, t.c_str()), lua_setfield(L, -2, "type");
        lua_pushlstring(L, j->body.data(), j->body.size()), lua_setfield(L, -2, "body");
        lua_pushstring(L, j->url.c_str()), lua_setfield(L, -2, "url");
        if (!j->err.empty()) lua_pushstring(L, j->err.c_str()), lua_setfield(L, -2, "error");
        call(p.get(), 1, 0);
    }
    upload_poll(c);
}

// isekur.upload(id, {url, field, type, fields = {k = v}, headers = {k = v}}, fn): sends the file of a `drop` event (a multipart POST, https only, streamed from the
// disk, the private-address guard on) on a thread; fn({status, body, error}) on a tick once it's done. A 2xx forgets the file, anything else leaves it
// for another try (it is forgotten after 30 minutes). isekur.upload(id, false) forgets it now. true, or nil and why
// ponytail: 2 at once per plugin, no progress (the file's size is on the event: say "sending" and wait)
struct UpJob {
    std::weak_ptr<Plugin> p;
    int fn = LUA_NOREF, status = 0;
    std::string id, body, err;
    Upload up;
    std::atomic<bool> done{false};
};
static std::vector<std::shared_ptr<UpJob>> up_jobs;
static std::vector<std::pair<std::string, std::string>> kv_table(lua_State* L, int t, const char* key) {  // t[key]: strings and numbers, by name
    std::map<std::string, std::string> m;
    lua_getfield(L, t, key);
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            if (lua_type(L, -2) == LUA_TSTRING && (lua_type(L, -1) == LUA_TSTRING || lua_type(L, -1) == LUA_TNUMBER)) m[lua_tostring(L, -2)] = lua_tostring(L, -1);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return {m.begin(), m.end()};
}
static int l_upload(lua_State* L) {
    Plugin* p = self(L);
    Core& c = *p->c;
    std::string id = str_arg(L, 1);
    if (lua_isboolean(L, 2) && !lua_toboolean(L, 2)) return c.drop_forget(id), lua_pushboolean(L, 1), 1;
    luaL_checktype(L, 2, LUA_TTABLE);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    auto no = [&](const char* why) { return lua_pushnil(L), lua_pushstring(L, why), 2; };
    Core::Drop* d = c.drop_find(id);
    if (!d) return no("that file is gone");
    int mine = 0;
    for (auto& j : up_jobs) mine += j->p.lock().get() == p, mine += 100 * (j->id == id);
    if (mine >= 100) return no("that file is being sent");
    if (mine >= 2) return no("busy: 2 uploads already running");
    auto j = std::make_shared<UpJob>();
    lua_getfield(L, 2, "url");
    j->up.url = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    if (j->up.url.size() < 9 || strncasecmp(j->up.url.c_str(), "https://", 8) || !https_fetch_err(j->up.url).empty()) return no("the URL must be https://...");
    for (const char* k : {"field", "type"}) {
        lua_getfield(L, 2, k);
        if (lua_isstring(L, -1) && *lua_tostring(L, -1)) (k[0] == 'f' ? j->up.field : j->up.ctype) = lua_tostring(L, -1);
        lua_pop(L, 1);
    }
    if (j->up.ctype == "application/octet-stream") j->up.ctype = d->mime;
    j->up.fields = kv_table(L, 2, "fields"), j->up.headers = kv_table(L, 2, "headers");
    j->up.path = d->path, j->up.filename = d->name, j->id = id, j->p = p->me;
    lua_pushvalue(L, 3), j->fn = luaL_ref(L, LUA_REGISTRYINDEX);
    up_jobs.push_back(j);
    std::thread([j] {
        if (!https_upload(j->up, j->status, j->body, j->err)) j->status = 0;
        j->done.store(true, std::memory_order_release);
    }).detach();
    lua_pushboolean(L, 1);
    return 1;
}
static void upload_poll(Core& c) {
    auto jobs = up_jobs;
    for (auto& j : jobs) {
        if (!j->done.load(std::memory_order_acquire)) continue;
        up_jobs.erase(std::find(up_jobs.begin(), up_jobs.end(), j));
        if (j->status / 100 == 2) c.drop_forget(j->id);
        auto p = j->p.lock();
        if (!p || p->dead) continue;
        lua_State* L = p->L;
        lua_rawgeti(L, LUA_REGISTRYINDEX, j->fn);
        luaL_unref(L, LUA_REGISTRYINDEX, j->fn);
        lua_createtable(L, 0, 3);
        lua_pushinteger(L, j->status), lua_setfield(L, -2, "status");
        lua_pushlstring(L, j->body.data(), j->body.size()), lua_setfield(L, -2, "body");
        if (!j->err.empty()) lua_pushstring(L, j->err.c_str()), lua_setfield(L, -2, "error");
        call(p.get(), 1, 0);
    }
}

// isekur.embed(srv, target, id, {k = v, ...}): attaches a flat table (strings, numbers, booleans) to the message with
// that id (its event's e.id) for the webchat to show under it. Untrusted data: the page shows it as text only
static int l_embed(lua_State* L) {
    Core& c = *self(L)->c;
    Server* s = srv_arg(L, 1);
    Buffer* b = s ? c.find(s, str_arg(L, 2)) : nullptr;
    unsigned long long id = strtoull(luaL_checkstring(L, 3), nullptr, 10);
    luaL_checktype(L, 4, LUA_TTABLE);
    std::map<std::string, std::string> kv;  // keys sorted: the same table, the same JSON
    lua_pushnil(L);
    while (lua_next(L, 4)) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            int t = lua_type(L, -1);
            std::string v = t == LUA_TBOOLEAN ? (lua_toboolean(L, -1) ? "true" : "false")
                          : t == LUA_TNUMBER && lua_isinteger(L, -1) ? std::to_string(lua_tointeger(L, -1))
                          : t == LUA_TNUMBER && std::isfinite(lua_tonumber(L, -1)) ? std::to_string(lua_tonumber(L, -1))
                          : t == LUA_TSTRING ? json_str(lua_tostring(L, -1)) : "";
            if (!v.empty()) kv[lua_tostring(L, -2)] = v;
        }
        lua_pop(L, 1);
    }
    std::string o = "{";
    for (auto& [k, v] : kv) o += (o.size() > 1 ? "," : "") + json_str(k) + ":" + v;
    o += "}";
    Line* l = nullptr;
    if (b && id)
        for (auto it = b->lines.rbegin(); it != b->lines.rend() && !l; ++it) if (it->id == id) l = &*it;
    if (!l && b && id && id == c.line_seq && o.size() <= 4096) {  // from its message event: the line isn't added yet
        auto& [pid, pend] = c.embed_pend;
        if (pid != id) pid = id, pend.clear();
        if (pend.size() + o.size() > 16384) return lua_pushboolean(L, 0), 1;
        pend += (pend.empty() ? "" : ",") + o;
        return lua_pushboolean(L, 1), 1;
    }
    if (!l || o.size() > 4096 || l->embeds.size() + o.size() > 16384) return lua_pushboolean(L, 0), 1;  // gone, or too much
    l->embeds += (l->embeds.empty() ? "" : ",") + o;
    c.notify(Ev::EMBED, b, l);
    c.dirty = true;  // the terminal draws link previews too
    lua_pushboolean(L, 1);
    return 1;
}

// what the isekur table gets from this file (host.cpp's load registers it)
const luaL_Reg plug::api_core[] = {
    {"on", l_on}, {"command", l_command}, {"setting", l_setting}, {"opt", l_opt}, {"set", l_set},
    {"say", l_say}, {"send", l_send}, {"print", l_print}, {"exec", l_exec}, {"store_get", l_store_get},
    {"store_set", l_store_set}, {"store_keys", l_store_keys}, {"active", l_active}, {"date", l_date},
    {"is_chan", l_is_chan}, {"servers", l_servers}, {"channels", l_channels}, {"away", l_away},
    {"attached", l_attached}, {"idle", l_idle}, {"detached_for", l_detached_for}, {"time", l_time},
    {"me", l_me}, {"nick", l_nick}, {"rank", l_rank}, {"http", l_http}, {"upload", l_upload}, {"embed", l_embed}, {"proxy", l_proxy},
    {nullptr, nullptr}};
