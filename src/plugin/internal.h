// What the plugin engine's files share (src/plugin/*.cpp). Private to them: core.h only forward-declares Plugin.
//
//   host.cpp       the engine: one sandboxed Lua state per plugin, loading, events, settings
//   api_core.cpp   isekur.* for chat, settings, the plugin's store, the network (http, embed, proxy) and the state of the client
//   api_ui.cpp     isekur.* for what is drawn: windows, screensaver, focus view, fold, nick colors, theme, tabs
//   api_media.cpp  isekur.* for this machine and sound: platform, which, run, speak_web, radio
//   catalog.cpp    the catalog (install, update, remove), /plugin and what the plugin windows show
//   radio.cpp      the radio's ffmpeg relay (Core::radio_*)
//
// A new isekur.* call: write its l_name in the api_*.cpp it belongs to and add it to that file's table at the bottom.
#pragma once
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
#include <memory>
#include "core.h"
#include "lua/lua.hpp"
#include "net.h"

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

namespace plug {

inline bool truthy(const std::string& v) { return v == "on" || v == "true" || v == "yes" || v == "1"; }
inline Plugin* self(lua_State* L) { return *(Plugin**)lua_getextraspace(L); }

inline Server* srv_arg(lua_State* L, int i) {
    if (lua_isnoneornil(L, i)) return nullptr;
    std::string n = luaL_checkstring(L, i);
    for (auto& s : self(L)->c->servers)
        if (s->cfg.name == n) return s.get();
    return nullptr;
}
inline Server* live_srv(lua_State* L, int i) {
    Server* s = srv_arg(L, i);
    if (!s || s->state != SS::CONNECTED) luaL_error(L, "not connected: %s", luaL_optstring(L, i, "(no server)"));
    return s;
}
inline std::string str_arg(lua_State* L, int i) { size_t n; const char* s = luaL_checklstring(L, i, &n); return {s, n}; }
inline std::string opt_str(lua_State* L, int i) { return lua_isnoneornil(L, i) ? "" : str_arg(L, i); }
// srv's tab for target, else srv's server tab, else the main window
inline Buffer* tab(lua_State* L, int si, int ti) {
    Core& c = *self(L)->c;
    Server* s = srv_arg(L, si);
    if (!s) return c.bufs.empty() ? nullptr : c.bufs.front().get();
    std::string t = opt_str(L, ti);
    Buffer* b = t.empty() ? nullptr : c.find(s, t);
    return b ? b : s->status;
}
inline std::string one_line(std::string s) { return s.substr(0, s.find_first_of("\r\n")); }

// host.cpp
bool call(Plugin* p, int nargs, int nres);  // runs the function under its nargs arguments on top of the stack; false: it failed (and said so)
void reap(Core& c);                         // drops the dead ones (not while a plugin runs)
void view_gone(Core& c);                    // a plugin went: its view, radio, window and the like end
std::vector<std::string> plugin_files(const std::string& dir);
bool wanted(const Core& c, const std::string& name);  // on anywhere in the config
void load_one(Core& c, const std::string& name);

// api_ui.cpp: one plugin at a time draws these, and ends with the plugin
void nicklist_by(Core& c, const std::string& by);
void fold_by(Core& c, const std::string& by);

// api_core.cpp
inline constexpr time_t STORE_EVERY = 5;  // seconds: a busy channel's seen updates are one write, not one per line
bool store_flush(Plugin* p);              // the whole store through a temporary file; false: it couldn't be written (tried again later)
void http_poll();                         // isekur.http's finished fetches, on every tick

// each file's part of the isekur table
extern const luaL_Reg api_core[], api_ui[], api_media[];

}  // namespace plug
