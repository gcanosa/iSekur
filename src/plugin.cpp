// Lua plugins: <config dir>/plugins/<name>.lua, one sandboxed Lua 5.4 state each, the isekur.* API.
// On when a [plugin.<name>] section (or a @net / @net/#chan override) says enabled = on.
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include "core.h"
#include "lua/lua.hpp"

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
    struct Opt { std::string def; int type; char scope; };  // type: the default's Lua type, so opt() gives that type back
    std::map<std::string, Opt> opts;
    std::map<std::string, std::string> store;
    bool store_read = false;
    ~Plugin() { if (L) lua_close(L); }
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
    p->c->print(nullptr, LK::ERROR, "[" + p->name + "] " + what);
    if (++p->errors >= MAX_ERRORS && !p->dead) {
        p->dead = true;
        p->c->print(nullptr, LK::ERROR, "[" + p->name + "] unloaded after " + std::to_string(MAX_ERRORS) +
                                            " errors in a row: /plugin reload " + p->name + " once it's fixed");
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
    for (auto& ch : name) ch = (char)tolower((unsigned char)ch);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    if (name.empty() || name.find(' ') != std::string::npos) luaL_error(L, "bad command name");
    if (find_command(name) || name == "plugin" || name == "plugins") luaL_error(L, "/%s is iSekur's own", name.c_str());
    for (auto& o : p->c->plugins)
        if (o.get() != p && o->cmds.count(name)) luaL_error(L, "/%s is taken by %s", name.c_str(), o->name.c_str());
    lua_pushvalue(L, 3);
    p->cmds[name] = {luaL_ref(L, LUA_REGISTRYINDEX), usage};
    return 0;
}
static int l_setting(lua_State* L) {  // setting(key, default, scope "g"|"n"|"c")
    std::string key = str_arg(L, 1), sc = luaL_optstring(L, 3, "c");
    int t = lua_type(L, 2);
    std::string def = t == LUA_TBOOLEAN ? (lua_toboolean(L, 2) ? "on" : "off") : t == LUA_TNIL ? "" : str_arg(L, 2);
    if (sc != "g" && sc != "n" && sc != "c") luaL_error(L, "scope is g, n or c");
    self(L)->opts[key] = {def, t, sc[0]};
    return 0;
}
static int l_opt(lua_State* L) {  // opt(key, srv, target): channel, network, global, the default
    Plugin* p = self(L);
    std::string key = str_arg(L, 1);
    auto d = p->opts.find(key);
    Plugin::Opt o = d != p->opts.end() ? d->second : Plugin::Opt{"", LUA_TSTRING, 'c'};
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
static int l_print(lua_State* L) { self(L)->c->print(tab(L, 1, 2), LK::INFO, str_arg(L, 3)); return 0; }
static int l_exec(lua_State* L) {  // exec(srv, target, "/command ...") as if typed in that tab
    Buffer* b = tab(L, 1, 2);
    if (b) self(L)->c->exec(b, one_line(str_arg(L, 3)));
    return 0;
}
static int l_lua_print(lua_State* L) {  // print(...): the main window, [name] first
    std::string out = "[" + self(L)->name + "]";
    for (int i = 1; i <= lua_gettop(L); i++) out += " " + std::string(luaL_tolstring(L, i, nullptr)), lua_pop(L, 1);
    self(L)->c->print(nullptr, LK::INFO, out);
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
static int l_store_set(lua_State* L) {  // store_set(key, value): nil removes it; written at once
    Plugin* p = self(L);
    store_load(p);
    std::string k = str_arg(L, 1);
    if (lua_isnoneornil(L, 2)) p->store.erase(k);
    else p->store[k] = luaL_tolstring(L, 2, nullptr), lua_pop(L, 1);
    // ponytail: the whole file on every set; an in-memory batch with a flush on tick if a plugin writes a lot
    std::string path = store_path(p), tmp = path + ".tmp";
    mkdir_p(p->c->plugin_dir());
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return luaL_error(L, "can't write %s", path.c_str());
    chmod(tmp.c_str(), 0600);
    for (auto& [kk, v] : p->store) fprintf(f, "%s\t%s\n", esc(kk).c_str(), esc(v).c_str());
    bool ok = fclose(f) == 0 && rename(tmp.c_str(), path.c_str()) == 0;
    if (!ok) return luaL_error(L, "can't write %s", path.c_str());
    return 0;
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
static int l_rank(lua_State* L) {  // rank(srv, chan, nick): its prefix ("@", "+", ""), nil when not in there
    Server* s = srv_arg(L, 1);
    Buffer* b = s ? self(L)->c->find(s, str_arg(L, 2)) : nullptr;
    auto it = b ? b->nicks.find(s->lower(str_arg(L, 3))) : decltype(b->nicks.end()){};
    if (!b || it == b->nicks.end()) lua_pushnil(L);
    else lua_pushstring(L, it->second.prefix.c_str());
    return 1;
}

static std::shared_ptr<Plugin> load(Core& c, const std::string& name, std::string& err) {
    auto p = std::make_shared<Plugin>();
    p->c = &c, p->name = name;
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
        {"store_get", l_store_get}, {"store_set", l_store_set},
        {"servers", l_servers}, {"channels", l_channels}, {"away", l_away}, {"attached", l_attached},
        {"idle", l_idle}, {"detached_for", l_detached_for}, {"time", l_time}, {"me", l_me}, {"rank", l_rank},
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
    else if (err != "didn't load") c.print(nullptr, LK::ERROR, "[" + name + "] not loaded: " + err);
}

void Core::plugins_sync() {
    if (plugin_depth || cfg_path.empty()) return;
    reap(*this);
    auto files = plugin_files(plugin_dir());
    plugins.erase(std::remove_if(plugins.begin(), plugins.end(), [&](auto& p) {
                      return !wanted(*this, p->name) || std::find(files.begin(), files.end(), p->name) == files.end();
                  }), plugins.end());
    for (auto& n : files)
        if (wanted(*this, n) && std::none_of(plugins.begin(), plugins.end(), [&](auto& p) { return p->name == n; })) load_one(*this, n);
}

bool Core::plugin_emit(const char* ev, const Server* s, const std::string& target, const PArgs& args) {
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
            if (call(p.get(), 1, 1)) hide |= (bool)lua_toboolean(L, -1), lua_pop(L, 1);
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
            if (lua_isstring(L, -1)) print(b, LK::ERROR, lua_tostring(L, -1));  // a returned string: an error to show (usage...)
            lua_pop(L, 1);
        }
        reap(*this);
        return true;
    }
    return false;
}

// /plugin [list] | enable|disable name[@net[/#chan]] | set name[@net[/#chan]] key value | reload name
void Core::plugin_cmd(Buffer* b, const std::string& args) {
    if (plugin_depth) { print(b, LK::ERROR, "/plugin can't be run by a plugin"); return; }
    auto argv = split(args, ' ');
    argv.erase(std::remove(argv.begin(), argv.end(), std::string()), argv.end());
    std::string sub = argv.empty() ? "list" : argv[0];
    std::string dir = plugin_dir();
    auto files = plugin_files(dir);
    auto loaded = [&](const std::string& n) -> Plugin* {
        for (auto& p : plugins)
            if (p->name == n && !p->dead) return p.get();
        return nullptr;
    };
    if (sub == "list") {
        if (files.empty()) { print(b, LK::INFO, "No plugins in " + dir); return; }
        for (auto& n : files) {
            std::string scopes;
            for (auto& [k, kv] : cfg)
                if (k.rfind("plugin." + n + "@", 0) == 0 && truthy(ini_get(cfg, k, "enabled"))) scopes += " " + k.substr(n.size() + 8);
            Plugin* p = loaded(n);
            std::string cmds;
            if (p) for (auto& [c, i] : p->cmds) cmds += " /" + c;
            print(b, LK::INFO, n + "  " + (p ? "on" : wanted(*this, n) ? "failed (see above, /plugin reload " + n + ")" : "off") +
                                   (scopes.empty() ? "" : "  in:" + scopes) + (cmds.empty() ? "" : "  commands:" + cmds));
        }
        return;
    }
    const char* usage = "Usage: /plugin [list] | enable|disable name[@network[/#channel]] | set name[@network[/#channel]] key value | reload name";
    if (argv.size() < 2) { print(b, LK::ERROR, usage); return; }
    // name@net or name@net/#chan: that scope; enable and disable also take it as a word after the name
    std::string name = argv[1].substr(0, argv[1].find('@'));
    std::string scope = argv[1].size() > name.size() ? argv[1].substr(name.size() + 1) : "";
    if (scope.empty() && argv.size() == 3 && (sub == "enable" || sub == "disable")) scope = argv[2];
    if (!plugin_name_ok(name) || std::find(files.begin(), files.end(), name) == files.end()) {
        print(b, LK::ERROR, "No plugin named " + name + " in " + dir);
        return;
    }
    // [plugin.name], [plugin.name@net] or [plugin.name@net/#chan] (the channel lowercased): written to the config at once
    std::string sec = "plugin." + name;
    if (!scope.empty()) {
        size_t sl = scope.find('/');
        std::string net = scope.substr(0, sl);
        if (!server_name_ok(net)) { print(b, LK::ERROR, "No network named " + net); return; }
        sec += "@" + net;
        if (sl != std::string::npos) {
            Server* sv = nullptr;
            for (auto& s : servers) if (s->cfg.name == net) sv = s.get();
            std::string ch = scope.substr(sl + 1);
            if (ch.empty()) { print(b, LK::ERROR, usage); return; }
            sec += "/" + (sv ? sv->lower(ch) : irc_lower(ch));
        }
    }
    std::string where = scope.empty() ? "" : " in " + scope;
    auto write = [&](const std::string& key, const std::string& val) {
        cfg[sec][key] = val;
        std::string err;
        if (!cfg_path.empty() && !ini_save(cfg_path, Ini{{sec, {{key, val}}}}, err)) { print(b, LK::ERROR, err); return false; }
        return true;
    };
    if (sub == "enable" || sub == "disable") {
        if (!write("enabled", sub == "enable" ? "on" : "off")) return;
        plugins_sync();
        print(b, LK::INFO, name + (sub == "enable" ? " enabled" : " disabled") + where +
                               (loaded(name) ? "" : wanted(*this, name) ? " (it didn't load, see above)" : ""));
        return;
    }
    if (sub == "set") {
        if (argv.size() < 3) { print(b, LK::ERROR, usage); return; }
        std::string val = after_words(args, 3);
        if (write(argv[2], val)) print(b, LK::INFO, name + " " + argv[2] + " = " + (val.empty() ? "(default)" : val) + where);
        return;
    }
    if (sub == "reload") {
        plugins.erase(std::remove_if(plugins.begin(), plugins.end(), [&](auto& p) { return p->name == name; }), plugins.end());
        if (!wanted(*this, name)) { print(b, LK::INFO, name + " is off: /plugin enable " + name); return; }
        load_one(*this, name);
        if (loaded(name)) print(b, LK::INFO, name + " reloaded");
        return;
    }
    print(b, LK::ERROR, "Unknown /plugin " + sub + ": list, enable, disable, set, reload");
}
