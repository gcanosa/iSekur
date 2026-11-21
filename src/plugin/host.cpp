// Lua plugins: <config dir>/plugins/<name>.lua, one sandboxed Lua 5.4 state each, the isekur.* API.
// On when a [plugin.<name>] section (or a @net / @net/#chan override) says enabled = on.
// This file is the engine: the state's limits, loading, the events plugins get, their commands and settings lookup.
// The isekur.* calls are in api_*.cpp, the catalog and /plugin in catalog.cpp; internal.h says what they share.
#include "plugin/internal.h"

using namespace plug;

static const size_t MEM_CAP = 32 << 20;  // bytes per plugin
static const int HOOK_EVERY = 10000, STEP_CAP = 1000;  // ~10M instructions per call, then it's stopped
static const int MAX_ERRORS = 5;  // in a row: the plugin is unloaded

bool plugin_name_ok(const std::string& n) {
    if (n.empty() || n.size() > 32) return false;
    for (char ch : n)
        if (!isalnum((unsigned char)ch) && ch != '_' && ch != '-') return false;
    return true;
}

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
bool plug::call(Plugin* p, int nargs, int nres) {
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

static int l_lua_print(lua_State* L) {  // print(...): the main window, with the plugin's badge
    std::string out;
    for (int i = 1; i <= lua_gettop(L); i++) out += (i > 1 ? " " : "") + std::string(luaL_tolstring(L, i, nullptr)), lua_pop(L, 1);
    self(L)->c->plug_print(nullptr, LK::INFO, out, self(L)->name);
    return 0;
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
    lua_createtable(L, 0, 64);  // the isekur table: each api_*.cpp brings its own part
    const luaL_Reg* parts[] = {api_core, api_ui, api_media};
    for (const luaL_Reg* part : parts) luaL_setfuncs(L, part, 0);
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

std::vector<std::string> plug::plugin_files(const std::string& dir) {
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
bool plug::wanted(const Core& c, const std::string& name) {
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

void plug::view_gone(Core& c) {  // a plugin went (disabled, removed, unloaded): its view ends (the configured look again), its radio stops, its window closes
    auto up = [&](const std::string& n) { return std::any_of(c.plugins.begin(), c.plugins.end(), [&](auto& p) { return p->name == n && !p->dead; }); };
    if (!c.view.by.empty() && !up(c.view.by)) c.view_end();
    if (!c.nicklist_by.empty() && !up(c.nicklist_by)) nicklist_by(c, "");
    if (!c.fold_by.empty() && !up(c.fold_by)) fold_by(c, "");
    if (c.radio_on() && !up(c.radio.by)) c.radio_stop();
    if (c.pwin.open && !up(c.pwin.by)) c.pwin = Core::PWin{}, c.dirty = true, c.notify(Ev::PWIN, nullptr);
}
void plug::reap(Core& c) {
    if (c.plugin_depth) return;
    c.plugins.erase(std::remove_if(c.plugins.begin(), c.plugins.end(), [](auto& p) { return p->dead; }), c.plugins.end());
    view_gone(c);
}

void plug::load_one(Core& c, const std::string& name) {
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
    view_gone(*this);
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
        if (p->dead || h == p->on.end() || (!emit_only.empty() && p->name != emit_only)) continue;
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
                else if (auto* n = std::get_if<int>(&v)) lua_pushinteger(L, *n);
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

std::vector<std::array<std::string, 3>> Core::plugin_commands() const {
    std::vector<std::array<std::string, 3>> out;
    for (auto& p : plugins)
        if (!p->dead)
            for (auto& [cmd, i] : p->cmds) out.push_back({cmd, i.usage, p->name});
    return out;
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
