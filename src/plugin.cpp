// Lua plugins: <config dir>/plugins/<name>.lua, one sandboxed Lua 5.4 state each, the isekur.* API.
// On when a [plugin.<name>] section (or a @net / @net/#chan override) says enabled = on.
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>
#include <tuple>
#include <openssl/evp.h>
#include "core.h"
#include "lua/lua.hpp"
#include "net.h"

std::string json_str(const std::string& s);  // web.cpp

static const size_t MEM_CAP = 32 << 20;  // bytes per plugin
static const int HOOK_EVERY = 10000, STEP_CAP = 1000;  // ~10M instructions per call, then it's stopped
static const int MAX_ERRORS = 5;  // in a row: the plugin is unloaded

struct Plugin {
    Core* c = nullptr;
    std::string name;
    lua_State* L = nullptr;
    size_t mem = 0;
    int steps = 0, depth = 0, errors = 0;
    bool dead = false;  // too many errors: dropped once nothing of it runs
    std::map<std::string, std::vector<int>> on;  // event -> handler refs
    struct Cmd { int fn; std::string usage; };
    std::map<std::string, Cmd> cmds;
    struct Opt { std::string def; int type; char scope; std::string help; std::vector<std::string> choices = {}; };  // type: the default's Lua type, so opt() gives that type back
    std::map<std::string, Opt> opts;
    std::vector<std::string> order;  // opts in the order they were declared
    std::map<std::string, std::string> store;
    bool store_read = false, store_dirty = false;  // dirty: written by store_flush within STORE_EVERY seconds
    time_t store_at = 0;                           // when it was last written
    int screen_fn = LUA_NOREF;  // isekur.screen's frame function
    pid_t speaker = -1;         // isekur.run: the speech engine still running, -1 = none
    std::weak_ptr<Plugin> me;  // isekur.http: its answer comes back only to a plugin still loaded
    ~Plugin();
};

static bool truthy(const std::string& v) { return v == "on" || v == "true" || v == "yes" || v == "1"; }
bool plugin_name_ok(const std::string& n) {
    if (n.empty() || n.size() > 32) return false;
    for (char ch : n)
        if (!isalnum((unsigned char)ch) && ch != '_' && ch != '-') return false;
    return true;
}

static Plugin* self(lua_State* L) { return *(Plugin**)lua_getextraspace(L); }

static void* palloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    Plugin* p = (Plugin*)ud;
    if (!ptr) osize = 0;  // then osize is the object's type, not a size
    if (!nsize) { free(ptr); p->mem -= osize; return nullptr; }
    if (nsize > osize && p->mem + (nsize - osize) > MEM_CAP) return nullptr;
    void* n = realloc(ptr, nsize);
    if (n) p->mem = p->mem - osize + nsize;
    return n;
}

static void hook(lua_State* L, lua_Debug*) {
    if (++self(L)->steps > STEP_CAP) luaL_error(L, "took too long, stopped");
}

static void say_err(Plugin* p, const std::string& what) {
    p->c->plug_print(nullptr, LK::ERROR, what, p->name);
    if (++p->errors >= MAX_ERRORS && !p->dead) {
        p->dead = true;
        p->c->plug_print(nullptr, LK::ERROR, "unloaded after " + std::to_string(MAX_ERRORS) +
                                            " errors in a row: /plugin reload " + p->name + " once it's fixed", p->name);
    }
}

// runs the function under its nargs arguments on top of the stack; false: it failed (and said so)
static bool call(Plugin* p, int nargs, int nres) {
    if (!p->depth++) p->steps = 0;
    p->c->plugin_depth++;
    int r = lua_pcall(p->L, nargs, nres, 0);
    p->c->plugin_depth--, p->depth--;
    if (r != LUA_OK) {
        say_err(p, std::string("error: ") + (lua_tostring(p->L, -1) ? lua_tostring(p->L, -1) : "?"));
        lua_pop(p->L, 1);
        return false;
    }
    p->errors = 0;
    return true;
}

// ---- isekur.* -------------------------------------------------------------------------
static Server* srv_arg(lua_State* L, int i) {
    if (lua_isnoneornil(L, i)) return nullptr;
    std::string n = luaL_checkstring(L, i);
    for (auto& s : self(L)->c->servers)
        if (s->cfg.name == n) return s.get();
    return nullptr;
}
static Server* live_srv(lua_State* L, int i) {
    Server* s = srv_arg(L, i);
    if (!s || s->state != SS::CONNECTED) luaL_error(L, "not connected: %s", luaL_optstring(L, i, "(no server)"));
    return s;
}
static std::string str_arg(lua_State* L, int i) { size_t n; const char* s = luaL_checklstring(L, i, &n); return {s, n}; }
static std::string opt_str(lua_State* L, int i) { return lua_isnoneornil(L, i) ? "" : str_arg(L, i); }
// srv's tab for target, else srv's server tab, else the main window
static Buffer* tab(lua_State* L, int si, int ti) {
    Core& c = *self(L)->c;
    Server* s = srv_arg(L, si);
    if (!s) return c.bufs.empty() ? nullptr : c.bufs.front().get();
    std::string t = opt_str(L, ti);
    Buffer* b = t.empty() ? nullptr : c.find(s, t);
    return b ? b : s->status;
}
static std::string one_line(std::string s) { return s.substr(0, s.find_first_of("\r\n")); }

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
static int l_sort_tabs(lua_State* L) {  // sort_tabs(): each network's tabs together, status first, the rest by name; true if the order changed
    auto& bs = self(L)->c->bufs;
    std::map<const Server*, size_t> first;  // a network's group sits where its first tab is, so networks keep their order
    for (size_t i = 0; i < bs.size(); i++)
        if (bs[i]->srv) first.emplace(bs[i]->srv, i);
    auto key = [&](size_t i) {
        Buffer* b = bs[i].get();
        std::string name = b->name;
        return std::make_tuple(b->srv ? first[b->srv] : i, b->type != BT::STATUS, to_lower(name));
    };
    std::vector<size_t> ord(bs.size());
    for (size_t i = 0; i < ord.size(); i++) ord[i] = i;
    std::stable_sort(ord.begin(), ord.end(), [&](size_t x, size_t y) { return key(x) < key(y); });
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
// isekur.platform(): the system's name in lowercase: "darwin", "linux", "freebsd", "openbsd", "netbsd", "dragonfly"
static int l_platform(lua_State* L) {
    struct utsname u;
    std::string s = uname(&u) == 0 ? u.sysname : "";
    lua_pushstring(L, to_lower(s).c_str());
    return 1;
}
// isekur.theme(): the loaded theme's colors, 0xRRGGBB (nil: the terminal's own)
static int l_theme(lua_State* L) {
    const Theme& t = self(L)->c->theme;
    Style d = t.get("default");
    lua_createtable(L, 0, 6);
    lua_pushstring(L, t.name.c_str()), lua_setfield(L, -2, "name");
    auto col = [&](const char* k, RGB c) { if (c >= 0) lua_pushinteger(L, c), lua_setfield(L, -2, k); };
    col("fg", d.fg), col("bg", d.bg), col("hilight", t.get("hilight").fg), col("timestamp", t.get("timestamp").fg);
    lua_createtable(L, (int)t.nicks.size(), 0);
    for (size_t i = 0; i < t.nicks.size(); i++) lua_pushinteger(L, t.nicks[i]), lua_rawseti(L, -2, (lua_Integer)i + 1);
    lua_setfield(L, -2, "nicks");
    return 1;
}
// isekur.which(name): the program's full path on PATH, nil when it isn't there
static int l_which(lua_State* L) {
    std::string n = str_arg(L, 1);
    const char* path = getenv("PATH");
    if (n.empty() || n.find('/') != std::string::npos || !path) { lua_pushnil(L); return 1; }
    std::string p = path;
    for (size_t i = 0; i <= p.size();) {
        size_t j = p.find(':', i);
        if (j == std::string::npos) j = p.size();
        std::string f = (j > i ? p.substr(i, j - i) : std::string(".")) + "/" + n;
        if (access(f.c_str(), X_OK) == 0) { lua_pushstring(L, f.c_str()); return 1; }
        i = j + 1;
    }
    lua_pushnil(L);
    return 1;
}
// the speech engines isekur.run starts: nothing else, and never through a shell
static const char* const SPEAK_PROGS[] = {"say", "spd-say", "espeak-ng", "espeak", "flite", "festival"};
// isekur.run(argv, text): starts argv (a speech engine, by its name) with text on its stdin, its output dropped.
// false while the last one still runs (one at a time per plugin): the plugin queues the rest.
// < 0: the error is on the Lua stack. l_run raises it, once av and text are gone: a Lua error jumps past destructors
static int run_speaker(lua_State* L) {
    Plugin* p = self(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    std::string text = opt_str(L, 2);
    std::vector<std::string> av;
    for (lua_Integer i = 1;; i++) {
        lua_rawgeti(L, 1, i);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
        if (!lua_isstring(L, -1)) { lua_pushliteral(L, "run: argv holds strings only"); return -1; }
        av.push_back(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    if (av.empty() || !std::any_of(std::begin(SPEAK_PROGS), std::end(SPEAK_PROGS), [&](const char* s) { return av[0] == s; })) {
        lua_pushfstring(L, "%s isn't a speech engine", av.empty() ? "(nothing)" : av[0].c_str());
        return -1;
    }
    // without --tts festival reads its stdin as Scheme, which can run programs: the text is whatever someone said on IRC
    if (av[0] == "festival" && (av.size() != 2 || av[1] != "--tts")) { lua_pushliteral(L, "festival runs only as {\"festival\", \"--tts\"}"); return -1; }
    if (text.size() > 4096) { lua_pushliteral(L, "text too long for an engine"); return -1; }
    if (p->speaker > 0) {
        if (waitpid(p->speaker, nullptr, WNOHANG) == 0) { lua_pushboolean(L, 0); return 1; }
        p->speaker = -1;
    }
    int in[2];
    if (pipe(in) != 0) { lua_pushboolean(L, 0); return 1; }
    fcntl(in[1], F_SETFD, FD_CLOEXEC);  // only the child's stdin is the read end
    std::vector<char*> argv;
    for (auto& a : av) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = fork();
    if (pid < 0) { close(in[0]), close(in[1]); lua_pushboolean(L, 0); return 1; }
    if (pid == 0) {  // child: the text on stdin, nothing on stdout or stderr (they'd draw over the screen)
        int nul = open("/dev/null", O_WRONLY);
        dup2(in[0], 0), dup2(nul, 1), dup2(nul, 2);
        for (int fd = 3; fd < 1024; fd++) close(fd);
        signal(SIGPIPE, SIG_DFL);
        execvp(argv[0], argv.data());
        _exit(127);  // not installed: the plugin checked with isekur.which before
    }
    close(in[0]);
    for (size_t off = 0; off < text.size();) {
        ssize_t n = write(in[1], text.data() + off, text.size() - off);
        if (n <= 0) break;  // the engine went away: nothing more to say
        off += (size_t)n;
    }
    close(in[1]);
    p->speaker = pid;
    lua_pushboolean(L, 1);
    return 1;
}
static int l_run(lua_State* L) {
    int n = run_speaker(L);
    return n < 0 ? lua_error(L) : n;
}
// isekur.speak_web(text, lang): the webchat says text aloud in the browser (speechSynthesis), in language lang (a code
// such as "es", "" = the browser's); nothing in the terminal
static int l_speak_web(lua_State* L) {
    Line l{time(nullptr), LK::INFO, opt_str(L, 2), str_arg(L, 1)};  // nick carries the language for SPEAK
    self(L)->c->notify(Ev::SPEAK, nullptr, &l);
    return 0;
}
static int l_lua_print(lua_State* L) {  // print(...): the main window, with the plugin's badge
    std::string out;
    for (int i = 1; i <= lua_gettop(L); i++) out += (i > 1 ? " " : "") + std::string(luaL_tolstring(L, i, nullptr)), lua_pop(L, 1);
    self(L)->c->plug_print(nullptr, LK::INFO, out, self(L)->name);
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
static const time_t STORE_EVERY = 5;  // seconds: a busy channel's seen updates are one write, not one per line
static bool store_flush(Plugin* p) {
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
static int l_away(lua_State* L) { Server* s = srv_arg(L, 1); lua_pushboolean(L, s && s->away); return 1; }
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
static void http_poll() {
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

static void reap(Core& c);
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

static std::shared_ptr<Plugin> load(Core& c, const std::string& name, std::string& err) {
    auto p = std::make_shared<Plugin>();
    p->c = &c, p->name = name, p->me = p;
    lua_State* L = p->L = lua_newstate(palloc, p.get());
    if (!L) { err = "out of memory"; return nullptr; }
    *(Plugin**)lua_getextraspace(L) = p.get();
    lua_sethook(L, hook, LUA_MASKCOUNT, HOOK_EVERY);
    // the sandbox: no io, os, package, debug; no loading of files or bytecode
    static const luaL_Reg libs[] = {{LUA_GNAME, luaopen_base}, {LUA_STRLIBNAME, luaopen_string}, {LUA_TABLIBNAME, luaopen_table},
                                    {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8}};
    for (auto& l : libs) luaL_requiref(L, l.name, l.func, 1), lua_pop(L, 1);
    for (const char* g : {"dofile", "loadfile", "load"}) lua_pushnil(L), lua_setglobal(L, g);  // ponytail: no load() at all, a text-only one if a plugin needs it
    lua_pushcfunction(L, l_lua_print), lua_setglobal(L, "print");
    static const luaL_Reg api[] = {
        {"on", l_on}, {"command", l_command}, {"setting", l_setting}, {"opt", l_opt},
        {"say", l_say}, {"send", l_send}, {"print", l_print}, {"exec", l_exec},
        {"store_get", l_store_get}, {"store_set", l_store_set}, {"store_keys", l_store_keys},
        {"active", l_active}, {"date", l_date}, {"is_chan", l_is_chan},
        {"servers", l_servers}, {"channels", l_channels}, {"away", l_away}, {"attached", l_attached},
        {"idle", l_idle}, {"detached_for", l_detached_for}, {"time", l_time}, {"me", l_me}, {"nick", l_nick}, {"rank", l_rank},
        {"http", l_http}, {"embed", l_embed}, {"proxy", l_proxy},
        {"screen", l_screen}, {"put", l_put}, {"inactive", l_inactive}, {"lock", l_lock}, {"sort_tabs", l_sort_tabs},
        {"platform", l_platform}, {"which", l_which}, {"run", l_run}, {"speak_web", l_speak_web}, {"theme", l_theme},
        {nullptr, nullptr}};
    luaL_newlib(L, api);
    lua_pushstring(L, name.c_str()), lua_setfield(L, -2, "name");
    lua_pushinteger(L, PLUGIN_API), lua_setfield(L, -2, "api");
    lua_setglobal(L, "isekur");
    std::string path = c.plugin_dir() + "/" + name + ".lua";
    if (luaL_loadfilex(L, path.c_str(), "t") != LUA_OK) {
        err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "can't load";
        return nullptr;
    }
    p->errors = MAX_ERRORS;  // a failed load: one message, no "unloaded" on top
    bool ok = call(p.get(), 0, 0);
    p->errors = 0;
    if (!ok) { err = "didn't load"; return nullptr; }
    return p;
}

// ---- Core side ------------------------------------------------------------------------
std::string Core::plugin_dir() const { return cfg_path.substr(0, cfg_path.rfind('/') + 1) + "plugins"; }

static std::vector<std::string> plugin_files(const std::string& dir) {
    std::vector<std::string> out;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string f = e->d_name;
            if (f.size() > 4 && f.compare(f.size() - 4, 4, ".lua") == 0 && plugin_name_ok(f.substr(0, f.size() - 4)))
                out.push_back(f.substr(0, f.size() - 4));
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// on anywhere: [plugin.x], [plugin.x@net] or [plugin.x@net/#chan] says enabled = on
static bool wanted(const Core& c, const std::string& name) {
    std::string sec = "plugin." + name;
    for (auto& [k, kv] : c.cfg)
        if ((k == sec || k.rfind(sec + "@", 0) == 0) && truthy(ini_get(c.cfg, k, "enabled"))) return true;
    return false;
}

std::string Core::plugin_opt(const std::string& name, const std::string& key, const Server* s, const std::string& target,
                             const std::string& def, char scope) const {
    std::string sec = "plugin." + name;
    if (s && scope != 'g') {
        std::string net = sec + "@" + s->cfg.name;
        if (scope == 'c' && !target.empty())
            if (std::string v = ini_get(cfg, net + "/" + s->lower(target), key); !v.empty()) return v;
        if (std::string v = ini_get(cfg, net, key); !v.empty()) return v;
    }
    return ini_get(cfg, sec, key, def);
}

static void reap(Core& c) {
    if (c.plugin_depth) return;
    c.plugins.erase(std::remove_if(c.plugins.begin(), c.plugins.end(), [](auto& p) { return p->dead; }), c.plugins.end());
}

static void load_one(Core& c, const std::string& name) {
    std::string err;
    if (auto p = load(c, name, err)) c.plugins.push_back(p);
    else if (err != "didn't load") c.plug_print(nullptr, LK::ERROR, "not loaded: " + err, name);
}

void Core::plugins_sync() {
    if (plugin_depth || cfg_path.empty()) return;
    reap(*this);
    catalog_load();
    auto files = plugin_files(plugin_dir());
    plugins.erase(std::remove_if(plugins.begin(), plugins.end(), [&](auto& p) {
                      return !wanted(*this, p->name) || std::find(files.begin(), files.end(), p->name) == files.end();
                  }), plugins.end());
    for (auto& n : files)
        if (wanted(*this, n) && std::none_of(plugins.begin(), plugins.end(), [&](auto& p) { return p->name == n; })) load_one(*this, n);
    plugins_reshow();
}

// a nickmenu handler's {{label, "/command"}, ...}: one-line labels, /commands only, none of those a plugin can't run
static void menu_items(lua_State* L, std::vector<std::pair<std::string, std::string>>& out) {
    for (lua_Integer i = 1; i <= 8; i++) {  // ponytail: 8 items per plugin, a menu not a page
        bool more = lua_rawgeti(L, -1, i) == LUA_TTABLE;
        if (!more) { lua_pop(L, 1); break; }
        lua_rawgeti(L, -1, 1), lua_rawgeti(L, -2, 2);
        std::string l = lua_type(L, -2) == LUA_TSTRING ? one_line(lua_tostring(L, -2)).substr(0, 40) : "";
        std::string cmd = lua_type(L, -1) == LUA_TSTRING ? one_line(lua_tostring(L, -1)) : "";
        lua_pop(L, 3);
        std::string w = cmd.substr(1, cmd.find(' ') - 1);
        to_lower(w);
        if (!l.empty() && cmd.size() > 1 && cmd[0] == '/' &&
            w != "plugin" && w != "exec" && w != "sh" && w != "shell" && w != "attach")
            out.push_back({l, cmd});
    }
}

bool Core::plugin_hooks(const char* ev, const Server* s, const std::string& target) {
    for (auto& p : plugins)
        if (!p->dead && p->on.count(ev) && (!s || truthy(plugin_opt(p->name, "enabled", s, target, "off", 'c')))) return true;
    return false;
}

bool Core::plugin_emit(const char* ev, const Server* s, const std::string& target, const PArgs& args, std::string* text,
                       std::vector<std::pair<std::string, std::string>>* menu) {
    if (plugins.empty()) return false;
    bool hide = false;
    auto list = plugins;  // kept alive, whatever a handler does
    for (auto& p : list) {
        auto h = p->on.find(ev);
        if (p->dead || h == p->on.end()) continue;
        if (s && !truthy(plugin_opt(p->name, "enabled", s, target, "off", 'c'))) continue;  // off in this network or channel
        for (int ref : std::vector<int>(h->second)) {
            if (p->dead) break;
            lua_State* L = p->L;
            lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
            lua_createtable(L, 0, (int)args.size() + 2);
            if (s) lua_pushstring(L, s->cfg.name.c_str()), lua_setfield(L, -2, "srv");
            if (!target.empty()) lua_pushstring(L, target.c_str()), lua_setfield(L, -2, "target");
            for (auto& [k, v] : args) {
                if (auto* str = std::get_if<std::string>(&v)) lua_pushlstring(L, str->data(), str->size());
                else lua_pushboolean(L, std::get<bool>(v));
                lua_setfield(L, -2, k);
            }
            if (call(p.get(), 1, 1)) {
                if (text && lua_type(L, -1) == LUA_TSTRING) *text = lua_tostring(L, -1);
                else if (menu && lua_type(L, -1) == LUA_TTABLE) menu_items(L, *menu);
                else hide |= (bool)lua_toboolean(L, -1);
                lua_pop(L, 1);
            }
        }
    }
    reap(*this);
    return hide;
}

bool Core::plugin_run(Buffer* b, const std::string& cmd, const std::string& args) {
    for (auto p : plugins) {
        auto it = p->cmds.find(cmd);
        if (p->dead || it == p->cmds.end()) continue;
        lua_State* L = p->L;
        lua_rawgeti(L, LUA_REGISTRYINDEX, it->second.fn);
        lua_pushstring(L, args.c_str());
        if (b->srv) lua_pushstring(L, b->srv->cfg.name.c_str());
        else lua_pushnil(L);
        lua_pushstring(L, b->name.c_str());
        if (call(p.get(), 3, 1)) {
            if (lua_isstring(L, -1)) plug_print(b, LK::ERROR, lua_tostring(L, -1));  // a returned string: an error to show (usage...)
            lua_pop(L, 1);
        }
        reap(*this);
        return true;
    }
    return false;
}


// ---- the catalog: <base>index.json and <base><name>/<name>.lua ---------------------------
// base: $ISEKUR_PLUGIN_URL (https://... or file:///dir/ for testing), else the repo's plugins/
static std::string catalog_base() {
    const char* e = getenv("ISEKUR_PLUGIN_URL");
    std::string b = e && *e ? e : "https://raw.githubusercontent.com/gcanosa/isekur-plugins/main/";
    return b.back() == '/' ? b : b + "/";
}
static const size_t FETCH_CAP = 1 << 20;

static bool fetch(const std::string& base, const std::string& rel, std::string& out) {
    if (base.rfind("file://", 0) == 0) {
        std::ifstream f(base.substr(7) + rel, std::ios::binary);
        if (!f) { out = "can't read " + base.substr(7) + rel; return false; }
        std::stringstream ss;
        ss << f.rdbuf();
        out = ss.str();
        return true;
    }
    if (base.rfind("https://", 0) != 0) { out = "ISEKUR_PLUGIN_URL must be https:// or file://"; return false; }
    size_t sl = base.find('/', 8);
    return https_get(base.substr(8, sl - 8), base.substr(sl) + rel, FETCH_CAP, out);
}

std::string sha256_of(const std::string& s) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    EVP_Digest(s.data(), s.size(), md, &n, EVP_sha256(), nullptr);
    std::string o;
    char b[3];
    for (unsigned i = 0; i < n; i++) snprintf(b, sizeof b, "%02x", md[i]), o += b;
    return o;
}

// index.json: an array of flat objects, string/number/true/false/null values; ok false on bad JSON
std::vector<std::map<std::string, std::string>> plugin_index(const std::string& j, bool* ok) {
    std::vector<std::map<std::string, std::string>> out;
    size_t i = 0;
    auto ws = [&] { while (i < j.size() && isspace((unsigned char)j[i])) i++; };
    auto eat = [&](char ch) { ws(); if (i < j.size() && j[i] == ch) { i++; return true; } return false; };
    auto str = [&](std::string& v) {
        if (!eat('"')) return false;
        v.clear();
        while (i < j.size() && j[i] != '"') {
            char ch = j[i++];
            if (ch != '\\') { v += ch; continue; }
            if (i >= j.size()) return false;
            ch = j[i++];
            if (ch == 'u' && i + 4 <= j.size()) utf8_put(v, (uint32_t)strtoul(j.substr(i, 4).c_str(), nullptr, 16)), i += 4;
            else v += ch == 'n' ? '\n' : ch == 't' ? '\t' : ch;  // \" \\ \/
        }
        return eat('"');
    };
    auto fail = [&] { if (ok) *ok = false; return std::vector<std::map<std::string, std::string>>{}; };
    if (ok) *ok = true;
    if (!eat('[')) return fail();
    if (eat(']')) return out;
    do {
        if (!eat('{')) return fail();
        std::map<std::string, std::string> o;
        if (!eat('}')) {
            do {
                std::string k, v;
                if (!str(k) || !eat(':')) return fail();
                ws();
                if (i < j.size() && j[i] == '"') { if (!str(v)) return fail(); }
                else {
                    size_t st = i;
                    while (i < j.size() && (isalnum((unsigned char)j[i]) || j[i] == '.' || j[i] == '-' || j[i] == '+')) i++;
                    if (st == i) return fail();
                    v = j.substr(st, i - st);
                }
                o[k] = v;
            } while (eat(','));
            if (!eat('}')) return fail();
        }
        out.push_back(o);
    } while (eat(','));
    if (!eat(']')) return fail();
    return out;
}

struct CatalogJob {
    std::string sub, base, index, err;     // sub: available, install, update
    std::vector<std::string> want;         // the plugin files to fetch with the index
    std::map<std::string, std::string> files, errs;
    int buf = 0;                           // the tab /plugin was typed in
    bool all = false;                      // update with no name: only newer versions, edited files left alone
    std::atomic<bool> done{false};
};

static void catalog_start(Core& c, Buffer* b, const std::string& sub, std::vector<std::string> want) {
    if (c.catalog) { if (sub != "refresh") c.plug_print(b, LK::ERROR, "Still fetching the plugin catalog: try again in a moment"); return; }
    auto j = std::make_shared<CatalogJob>();
    j->sub = sub, j->base = catalog_base(), j->want = std::move(want), j->buf = b->id;
    c.catalog = j;
    if (sub != "refresh") c.plug_print(b, LK::INFO, "Fetching the plugin catalog from " + j->base + "...");
    std::thread([j] {
        if (fetch(j->base, "index.json", j->index)) {
            for (auto& n : j->want) {
                std::string body;
                if (fetch(j->base, n + "/" + n + ".lua", body)) j->files[n] = body;
                else j->errs[n] = body;
            }
        } else j->err = j->index;
        j->done.store(true, std::memory_order_release);
    }).detach();
}

static std::string read_all(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    if (f) ss << f.rdbuf();
    return ss.str();
}

static bool write_plugin(Core& c, const std::string& name, const std::string& body, std::string& err) {
    std::string dir = c.plugin_dir(), path = dir + "/" + name + ".lua", tmp = path + ".tmp";
    mkdir_p(dir);
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) { err = "can't write " + path; return false; }
    bool ok = fwrite(body.data(), 1, body.size(), f) == body.size();
    ok = fclose(f) == 0 && ok && rename(tmp.c_str(), path.c_str()) == 0;
    if (!ok) err = "can't write " + path, unlink(tmp.c_str());
    return ok;
}

// ponytail: one shared cache file per config; ETag/If-None-Match if a 12h fetch per user ever counts on GitHub's side
static const time_t CATALOG_TTL = 12 * 3600, CATALOG_RETRY = 600, CATALOG_FORCE = 60;
static std::string catalog_file(const Core& c) { return c.plugin_dir() + "/.catalog.json"; }

// "-- version: 1.2", "-- author: ...", "-- contact: ..." in the first 20 lines
std::string plugin_header(const std::string& lua, const std::string& key) {
    size_t at = 0;
    std::string tag = "-- " + key + ":";
    for (int i = 0; i < 20 && at < lua.size(); i++) {
        size_t nl = lua.find('\n', at);
        std::string l = lua.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = nl == std::string::npos ? lua.size() : nl + 1;
        if (l.rfind(tag, 0) != 0) continue;
        std::string v = l.substr(tag.size());
        v.erase(0, v.find_first_not_of(" \t"));
        v.erase(v.find_last_not_of(" \t\r") + 1);
        return v;
    }
    return "";
}
// what the windows compare with the catalog's
std::string plugin_version(const std::string& lua) { return plugin_header(lua, "version"); }

// the installed file's sha256 and version, read again only when its size or time changed
// its header comment's first sentence ("-- name: what it does. More..."): the description until the catalog's comes
static std::string plugin_blurb(const std::string& lua, const std::string& name) {
    std::string out;
    bool in = false;
    for (auto& l : split(lua, '\n')) {
        if (l.rfind("--", 0) != 0) { if (in) break; continue; }
        std::string t = l.substr(2);
        t.erase(0, t.find_first_not_of(" \t")), t.erase(t.find_last_not_of(" \t\r") + 1);
        if (!in) { if (t.rfind(name + ":", 0) != 0) continue; in = true, t = t.substr(name.size() + 1); t.erase(0, t.find_first_not_of(' ')); }
        if (t.empty()) break;
        out += (out.empty() ? "" : " ") + t;
        if (size_t e = out.find(". "); e != std::string::npos) { out.resize(e + 1); break; }
    }
    if (!out.empty() && islower((unsigned char)out[0])) out[0] = (char)toupper((unsigned char)out[0]);
    return out;
}

struct FileInfo { std::string sha, version, blurb, author, contact; };
static FileInfo file_info(const std::string& path, const std::string& name) {  // name: for the blurb (cached by path)
    static std::map<std::string, std::pair<std::string, FileInfo>> cache;
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return {};
    std::string key = std::to_string(st.st_size) + ":" + std::to_string((long long)st.st_mtime);
    auto& e = cache[path];
    if (e.first != key) {
        std::string t = read_all(path);
        e = {key, {sha256_of(t), plugin_version(t), plugin_blurb(t, name), plugin_header(t, "author"), plugin_header(t, "contact")}};
    }
    return e.second;
}
// newer: the catalog's version is above the installed one (or the file says none and isn't the catalog's)
static bool newer_in_catalog(const std::string& cat, const std::string& local, bool same) {
    return local.empty() ? !same : version_newer(cat, local);
}

// auto-away was in core: its [general] and [server.X] keys become [plugin.autoaway] and
// [plugin.autoaway@X], and leave the config. false: the old auto_away = off said it was off
static const std::multimap<std::string, std::string> AWAY_KEYS = {
    {"auto_away", "enabled"}, {"auto_away_idle", "idle"}, {"away_message", "message"},
    {"away_announce", "announce_away"}, {"away_announce", "announce_back"}};  // it did both
bool old_away_keys(const Core& c) {
    for (auto& [sec, kv] : c.cfg)
        if (sec == "general" || sec.rfind("server.", 0) == 0)
            for (auto& [k, v] : AWAY_KEYS) if (kv.count(k)) return true;
    return false;
}
static void migrate_autoaway(Core& c, Buffer* b) {
    Ini moved;
    int n = 0;
    for (auto& [sec, kv] : c.cfg) {
        std::string to = sec == "general" ? "plugin.autoaway" : sec.rfind("server.", 0) == 0 ? "plugin.autoaway@" + sec.substr(7) : "";
        if (to.empty()) continue;
        for (auto& [k, nk] : AWAY_KEYS) {
            auto it = kv.find(k);
            if (it == kv.end()) continue;
            if (!it->second.empty() && ini_get(c.cfg, to, nk).empty()) moved[to][nk] = it->second, n++;
        }
    }
    for (auto& [sec, kv] : c.cfg)
        for (auto& [k, nk] : AWAY_KEYS) kv.erase(k), c.unsaved[sec].erase(k);
    for (auto& [sec, kv] : moved)
        for (auto& [k, v] : kv) c.cfg[sec][k] = v;
    std::string err;
    std::set<std::string> old;
    for (auto& [k, nk] : AWAY_KEYS) old.insert(k);
    if (!c.cfg_path.empty() && ((!moved.empty() && !ini_save(c.cfg_path, moved, err)) || !ini_drop_keys_file(c.cfg_path, old, err))) {
        c.plug_print(b, LK::ERROR, "autoaway: " + err);
        return;
    }
    if (n) c.plug_print(b, LK::INFO, "autoaway: moved " + std::to_string(n) + " auto-away setting" + (n > 1 ? "s" : "") +
                                    " into [plugin.autoaway] sections; the old keys are gone from the config");
}

// /plugin available: the cached catalog, what's installed marked
static void list_available(Core& c, Buffer* b) {
    int newer = 0, shown = 0;
    for (auto& r : c.catalog_rows()) {
        if (r.too_new) { newer++; continue; }
        std::string st = !r.installed ? "" : r.update ? "  [update: " + (r.local.empty() ? "?" : r.local) + " → " + r.version + ", /plugin update " + r.name + "]"
                                       : r.edited ? "  [installed, edited here: /plugin update " + r.name + " restores it]" : "  [installed]";
        c.plug_print(b, LK::INFO, "\x02" + r.name + "\x02 " + r.version + "  " + r.desc + st);
        shown++;
    }
    if (!shown) c.plug_print(b, LK::INFO, "The catalog has no plugins for this iSekur yet");
    else c.plug_print(b, LK::INFO, "/plugin install name installs one · catalog from " + fmt_dur(time(nullptr) - c.catalog_at) + " ago, /plugin refresh fetches it again");
    if (newer) c.plug_print(b, LK::INFO, std::to_string(newer) + " more need a newer iSekur");
}

void Core::plugins_poll() {
    http_poll();
    time_t now = time(nullptr);
    for (auto& p : plugins)
        if (p->store_dirty && now - p->store_at >= STORE_EVERY) store_flush(p.get());
    if (!catalog || !catalog->done.load(std::memory_order_acquire)) return;
    auto j = catalog;
    catalog.reset();
    Buffer* b = nullptr;
    for (auto& x : bufs) if (x->id == j->buf) b = x.get();
    bool quiet = j->sub == "refresh";
    dirty = true;
    if (j->err.size()) { catalog_err = j->err; if (!quiet) plug_print(b, LK::ERROR, "Plugin catalog: " + j->err); return; }
    bool ok;
    auto idx = plugin_index(j->index, &ok);
    if (!ok) { catalog_err = "index.json isn't valid"; if (!quiet) plug_print(b, LK::ERROR, "Plugin catalog: index.json isn't valid"); return; }
    catalog_cache = idx, catalog_have = true, catalog_err.clear(), catalog_at = time(nullptr);
    if (!cfg_path.empty()) {  // kept for the next start and the windows: no fetch while it's fresh
        std::string path = catalog_file(*this), tmp = path + ".tmp";
        mkdir_p(plugin_dir());
        if (FILE* f = fopen(tmp.c_str(), "w")) {
            bool okw = fwrite(j->index.data(), 1, j->index.size(), f) == j->index.size();
            if (fclose(f) == 0 && okw) rename(tmp.c_str(), path.c_str());
            else unlink(tmp.c_str());
        }
    }
    if (quiet) return;
    if (j->sub == "fetch") {
        plug_print(b, LK::INFO, "Catalog fetched: " + std::to_string(catalog_rows().size()) + " plugin" + (catalog_rows().size() == 1 ? "" : "s") + " (/plugin available)");
        return;
    }
    std::map<std::string, std::map<std::string, std::string>> by;
    for (auto& e : idx) if (plugin_name_ok(e["name"])) by[e["name"]] = e;
    auto files = plugin_files(plugin_dir());
    auto have = [&](const std::string& n) { return std::find(files.begin(), files.end(), n) != files.end(); };
    auto local_sha = [&](const std::string& n) { return sha256_of(read_all(plugin_dir() + "/" + n + ".lua")); };
    if (j->sub == "available") { list_available(*this, b); return; }
    // install and update: checked against the index, written, (re)loaded
    int done = 0;
    for (auto& n : j->want) {
        auto e = by.find(n);
        if (e == by.end()) { if (j->sub == "install") plug_print(b, LK::ERROR, "No plugin named " + n + " in the catalog (/plugin available)"); continue; }
        auto& en = e->second;
        if (atoi(en["api"].c_str()) > PLUGIN_API) { plug_print(b, LK::ERROR, n + " " + en["version"] + " needs a newer iSekur"); continue; }
        std::string lv = have(n) ? plugin_version(read_all(plugin_dir() + "/" + n + ".lua")) : "";
        if (j->sub == "update" && have(n)) {
            bool same = local_sha(n) == en["sha256"];
            if (same) continue;  // already the catalog's
            if (j->all && !newer_in_catalog(en["version"], lv, same)) continue;  // edited here, same version: only by name
        }
        auto f = j->files.find(n);
        if (f == j->files.end()) { plug_print(b, LK::ERROR, n + ": " + (j->errs.count(n) ? j->errs[n] : "not fetched")); continue; }
        if (sha256_of(f->second) != en["sha256"]) { plug_print(b, LK::ERROR, n + ": refused, its checksum doesn't match the catalog's"); continue; }
        std::string err;
        if (!write_plugin(*this, n, f->second, err)) { plug_print(b, LK::ERROR, n + ": " + err); continue; }
        done++;
        plugins.erase(std::remove_if(plugins.begin(), plugins.end(), [&](auto& p) { return p->name == n; }), plugins.end());
        bool on = true;
        if (j->sub == "install") {
            if (n == "autoaway") migrate_autoaway(*this, b), on = !(cfg.count("plugin.autoaway") && cfg["plugin.autoaway"].count("enabled"));
            else on = !wanted(*this, n) && !(cfg.count("plugin." + n) && cfg["plugin." + n].count("enabled"));
            std::string err2;
            if (on) {  // installed means on, unless the config already says otherwise
                cfg["plugin." + n]["enabled"] = "on";
                if (!cfg_path.empty() && !ini_save(cfg_path, Ini{{"plugin." + n, {{"enabled", "on"}}}}, err2)) plug_print(b, LK::ERROR, err2);
            }
        }
        plugins_sync();
        bool loaded = std::any_of(plugins.begin(), plugins.end(), [&](auto& p) { return p->name == n; });
        plug_print(b, LK::INFO, n + " " + en["version"] + (j->sub == "install" ? " installed" : lv.empty() || lv == en["version"] ? " restored" : " updated from " + lv) +
                               (loaded ? " and on" : wanted(*this, n) ? " (it didn't load, see above)" : ": off in the config, /plugin enable " + n));
    }
    if (j->sub == "update" && !done) plug_print(b, LK::INFO, j->all ? "Your plugins are up to date with the catalog" : "Already the catalog's version");
}

static void remove_plugin(Core& c, Buffer* b, const std::string& name) {
    c.plugins.erase(std::remove_if(c.plugins.begin(), c.plugins.end(), [&](auto& p) { return p->name == name; }), c.plugins.end());
    std::string dir = c.plugin_dir(), err;
    unlink((dir + "/" + name + ".lua").c_str()), unlink((dir + "/" + name + ".data").c_str());
    std::vector<std::string> secs;
    for (auto& [k, kv] : c.cfg)
        if (k == "plugin." + name || k.rfind("plugin." + name + "@", 0) == 0) secs.push_back(k);
    for (auto& k : secs) {
        c.cfg.erase(k), c.unsaved.erase(k);
        if (!c.cfg_path.empty() && !ini_drop_section(c.cfg_path, k, err)) c.plug_print(b, LK::ERROR, err);
    }
    c.plug_print(b, LK::INFO, name + " removed, with its settings and saved data");
}


// ---- what the plugin windows show -------------------------------------------------------
void Core::catalog_load() {
    if (catalog_have || cfg_path.empty()) return;
    struct stat st;
    std::string path = catalog_file(*this), text = read_all(path);
    bool ok;
    auto idx = plugin_index(text, &ok);
    if (text.empty() || !ok || stat(path.c_str(), &st) != 0) return;
    catalog_cache = idx, catalog_have = true, catalog_at = st.st_mtime;
}

void Core::plugin_refresh(Buffer* b, bool force) {
    if (!b || catalog) return;
    catalog_load();
    time_t now = time(nullptr);
    if (!force && catalog_have && now - catalog_at < CATALOG_TTL) return;  // fresh enough
    if (now - catalog_tried < (force ? CATALOG_FORCE : CATALOG_RETRY)) {
        if (force) plug_print(b, LK::INFO, "The catalog was last asked for " + fmt_dur(now - catalog_tried) + " ago: try again in a minute");
        return;
    }
    catalog_tried = now;
    catalog_start(*this, b, force ? "fetch" : "refresh", {});
}

std::string plugin_sec(const std::string& name, const std::string& scope) { return "plugin." + name + (scope.empty() ? "" : "@" + scope); }

std::string Core::plugin_at(const std::string& name, const std::string& key, const std::string& scope) const {
    return ini_get(cfg, plugin_sec(name, scope), key);
}

std::string Core::plugin_eff(const std::string& name, const std::string& key, const std::string& scope, const std::string& def, char sc) const {
    std::string net = scope.substr(0, scope.find('/'));
    if (sc == 'c' && scope.find('/') != std::string::npos)
        if (std::string v = plugin_at(name, key, scope); !v.empty()) return v;
    if (sc != 'g' && !net.empty())
        if (std::string v = plugin_at(name, key, net); !v.empty()) return v;
    return ini_get(cfg, plugin_sec(name, ""), key, def);
}

// the windows' scopes: everywhere and each network. A channel only when the config already has one for it
// (made with /plugin set name@net/#chan): enough for a window, and a channel's own can still be changed there
std::vector<std::string> Core::plugin_scopes() const {
    std::set<std::string> out{""};
    for (auto& s : servers) out.insert(s->cfg.name);
    for (auto& [k, kv] : cfg)
        if (size_t at = k.find('@'); k.rfind("plugin.", 0) == 0 && at != std::string::npos) out.insert(k.substr(at + 1));
    return {out.begin(), out.end()};
}

std::vector<PluginInfo> Core::plugin_info() const {
    std::map<std::string, const std::map<std::string, std::string>*> cat;
    for (auto& e : catalog_cache) if (e.count("name")) cat[e.at("name")] = &e;
    std::vector<PluginInfo> out;
    for (auto& n : plugin_files(plugin_dir())) {
        PluginInfo pi;
        pi.name = n;
        const Plugin* p = nullptr;
        for (auto& x : plugins) if (x->name == n && !x->dead) p = x.get();
        pi.state = p ? "on" : wanted(*this, n) ? "failed" : "off";
        for (auto& [k, kv] : cfg)
            if ((k == "plugin." + n || k.rfind("plugin." + n + "@", 0) == 0) && truthy(ini_get(cfg, k, "enabled")))
                pi.on_in.push_back(k.size() > n.size() + 7 ? k.substr(n.size() + 8) : "");
        if (p) {
            for (auto& [cmd, i] : p->cmds) pi.cmds.push_back(cmd);
            for (auto& k : p->order) {
                auto& o = p->opts.at(k);
                pi.sets.push_back({k, o.def, o.scope, o.type == LUA_TBOOLEAN, o.help, o.choices});
            }
        }
        auto fi = file_info(plugin_dir() + "/" + n + ".lua", n);
        const std::string& sha = fi.sha;
        std::string local = fi.version;
        pi.local = local, pi.desc = fi.blurb, pi.author = fi.author, pi.contact = fi.contact;
        if (auto e = cat.find(n); e != cat.end()) {
            auto get = [&](const char* k) { auto it = e->second->find(k); return it == e->second->end() ? std::string() : it->second; };
            pi.version = get("version"), pi.in_catalog = true, pi.official = get("official") == "true";
            if (!get("description").empty()) pi.desc = get("description");
            if (!get("author").empty()) pi.author = get("author");
            if (!get("contact").empty()) pi.contact = get("contact");
            bool same = sha == get("sha256");
            pi.update = newer_in_catalog(pi.version, local, same);
            pi.edited = !same && !pi.update;
        }
        out.push_back(pi);
    }
    return out;
}

std::vector<CatalogRow> Core::catalog_rows() const {
    auto files = plugin_files(plugin_dir());
    std::vector<CatalogRow> out;
    for (auto& e : catalog_cache) {
        auto get = [&](const char* k) { auto it = e.find(k); return it == e.end() ? std::string() : it->second; };
        std::string n = get("name");
        if (!plugin_name_ok(n)) continue;
        bool inst = std::find(files.begin(), files.end(), n) != files.end();
        FileInfo fi = inst ? file_info(plugin_dir() + "/" + n + ".lua", n) : FileInfo{};
        const std::string &sha = fi.sha, &local = fi.version;
        bool same = sha == get("sha256"), up = inst && newer_in_catalog(get("version"), local, same);
        out.push_back({n, get("version"), local, get("description"), inst, up, inst && !same && !up, atoi(get("api").c_str()) > PLUGIN_API,
                       get("author"), get("contact"), get("official") == "true"});
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.name < b.name; });
    return out;
}

// /plugin [list] | enable|disable name[@net[/#chan]] | set name[@net[/#chan]] key value | reload name
void Core::plugin_cmd(Buffer* b, const std::string& args) {
    if (plugin_depth) { plug_print(b, LK::ERROR, "/plugin can't be run by a plugin"); return; }
    auto argv = split(args, ' ');
    argv.erase(std::remove(argv.begin(), argv.end(), std::string()), argv.end());
    if (argv.empty() && tty && !from_web) { plugin_win = dirty = true; return; }  // the terminal's window (the webchat opens its own)
    std::string sub = argv.empty() ? "list" : argv[0];
    std::string dir = plugin_dir();
    auto files = plugin_files(dir);
    auto loaded = [&](const std::string& n) -> Plugin* {
        for (auto& p : plugins)
            if (p->name == n && !p->dead) return p.get();
        return nullptr;
    };
    if (sub == "list") {
        if (files.empty()) { plug_print(b, LK::INFO, "No plugins in " + dir + ": /plugin available lists the catalog's"); return; }
        for (auto& n : files) {
            std::string scopes;
            for (auto& [k, kv] : cfg)
                if (k.rfind("plugin." + n + "@", 0) == 0 && truthy(ini_get(cfg, k, "enabled"))) scopes += " " + k.substr(n.size() + 8);
            Plugin* p = loaded(n);
            std::string cmds;
            if (p) for (auto& [c, i] : p->cmds) cmds += " /" + c;
            plug_print(b, LK::INFO, n + "  " + (p ? "on" : wanted(*this, n) ? "failed (see above, /plugin reload " + n + ")" : "off") +
                                   (scopes.empty() ? "" : "  in:" + scopes) + (cmds.empty() ? "" : "  commands:" + cmds));
        }
        return;
    }
    if (sub == "available") {  // the cached catalog while it's fresh, else fetched (at most every 10 minutes after a failure)
        catalog_load();
        time_t now = time(nullptr);
        if (catalog_have && (now - catalog_at < CATALOG_TTL || now - catalog_tried < CATALOG_RETRY)) { list_available(*this, b); return; }
        if (now - catalog_tried < CATALOG_RETRY) { plug_print(b, LK::ERROR, "Plugin catalog: " + (catalog_err.empty() ? "fetched a moment ago" : catalog_err) + " (/plugin refresh tries again)"); return; }
        catalog_tried = now;
        catalog_start(*this, b, sub, {});
        return;
    }
    if (sub == "refresh") { plugin_refresh(b, true); return; }
    if (sub == "update") {  // update [name]: the installed ones whose file differs from the catalog's
        std::vector<std::string> want = argv.size() > 1 ? std::vector<std::string>{argv[1]} : files;
        if (want.empty()) { plug_print(b, LK::INFO, "No plugins installed: /plugin available"); return; }
        catalog_start(*this, b, sub, want);
        if (catalog && argv.size() == 1) catalog->all = true;
        return;
    }
    if (sub == "install") {
        if (argv.size() != 2 || !plugin_name_ok(argv[1])) { plug_print(b, LK::ERROR, "Usage: /plugin install name (/plugin available lists them)"); return; }
        catalog_start(*this, b, sub, {argv[1]});
        return;
    }
    const char* usage = "Usage: /plugin [list] | available | refresh | install|update|remove name | enable|disable name[@network[/#channel]] | set name[@network[/#channel]] key value | reload name";
    if (argv.size() < 2) { plug_print(b, LK::ERROR, usage); return; }
    // name@net or name@net/#chan: that scope; enable and disable also take it as a word after the name
    std::string name = argv[1].substr(0, argv[1].find('@'));
    std::string scope = argv[1].size() > name.size() ? argv[1].substr(name.size() + 1) : "";
    if (scope.empty() && argv.size() == 3 && (sub == "enable" || sub == "disable")) scope = argv[2];
    if (!plugin_name_ok(name) || std::find(files.begin(), files.end(), name) == files.end()) {
        plug_print(b, LK::ERROR, "No plugin named " + name + " in " + dir);
        return;
    }
    // [plugin.name], [plugin.name@net] or [plugin.name@net/#chan] (the channel lowercased): written to the config at once
    std::string sec = "plugin." + name;
    if (!scope.empty()) {
        size_t sl = scope.find('/');
        std::string net = scope.substr(0, sl);
        if (!server_name_ok(net)) { plug_print(b, LK::ERROR, "No network named " + net); return; }
        sec += "@" + net;
        if (sl != std::string::npos) {
            Server* sv = nullptr;
            for (auto& s : servers) if (s->cfg.name == net) sv = s.get();
            std::string ch = scope.substr(sl + 1);
            if (ch.empty()) { plug_print(b, LK::ERROR, usage); return; }
            sec += "/" + (sv ? sv->lower(ch) : irc_lower(ch));
        }
    }
    std::string where = scope.empty() ? "" : " in " + scope;
    auto write = [&](const std::string& key, const std::string& val) {
        cfg[sec][key] = val;
        std::string err;
        if (!cfg_path.empty() && !ini_save(cfg_path, Ini{{sec, {{key, val}}}}, err)) { plug_print(b, LK::ERROR, err); return false; }
        return true;
    };
    if (sub == "enable" || sub == "disable") {
        if (!write("enabled", sub == "enable" ? "on" : "off")) return;
        plugins_sync();
        plug_print(b, LK::INFO, name + (sub == "enable" ? " enabled" : " disabled") + where +
                               (loaded(name) ? "" : wanted(*this, name) ? " (it didn't load, see above)" : ""));
        return;
    }
    if (sub == "set") {
        if (argv.size() < 3) { plug_print(b, LK::ERROR, usage); return; }
        std::string val = after_words(args, 3);
        for (auto& p : plugins)  // a setting with choices takes only those (in any case)
            if (auto o = p->name == name ? p->opts.find(argv[2]) : p->opts.end(); o != p->opts.end() && !o->second.choices.empty() && !val.empty()) {
                auto& cs = o->second.choices;
                auto it = std::find_if(cs.begin(), cs.end(), [&](auto& v) { return strcasecmp(v.c_str(), val.c_str()) == 0; });
                if (it == cs.end()) {
                    std::string all;
                    for (auto& v : cs) all += (all.empty() ? "" : ", ") + v;
                    plug_print(b, LK::ERROR, name + " " + argv[2] + " is one of: " + all);
                    return;
                }
                val = *it;
            }
        if (!write(argv[2], val)) return;
        if (argv[2] == "enabled") plugins_sync();
        else plugins_reshow();  // lolcat's palette or lines: the backlog too
        plug_print(b, LK::INFO, name + " " + argv[2] + " = " + (val.empty() ? "(default)" : val) + where);
        return;
    }
    if (sub == "remove") { remove_plugin(*this, b, name), plugins_reshow(); return; }
    if (sub == "reload") {
        plugins.erase(std::remove_if(plugins.begin(), plugins.end(), [&](auto& p) { return p->name == name; }), plugins.end());
        if (!wanted(*this, name)) { plugins_reshow(), plug_print(b, LK::INFO, name + " is off: /plugin enable " + name); return; }
        load_one(*this, name);
        plugins_reshow();
        if (loaded(name)) plug_print(b, LK::INFO, name + " reloaded");
        return;
    }
    plug_print(b, LK::ERROR, "Unknown /plugin " + sub + ": list, available, refresh, install, update, remove, enable, disable, set, reload");
}
