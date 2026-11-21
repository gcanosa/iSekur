// isekur.* for this machine and sound: what system it is, which programs it has, the speech engines, the webchat's voice, the radio.
#include "plugin/internal.h"

using namespace plug;

// isekur.platform(): the system's name in lowercase: "darwin", "linux", "freebsd", "openbsd", "netbsd", "dragonfly"
static int l_platform(lua_State* L) {
    struct utsname u;
    std::string s = uname(&u) == 0 ? u.sysname : "";
    lua_pushstring(L, to_lower(s).c_str());
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

// isekur.radio(url, {name, host, direct, volume}): plays a stream (http or https; a YouTube channel's /live too) for the webchat's browsers and,
// with host, this machine's speakers. direct: nothing runs here, the webchat's browsers connect to the stream themselves (an https one that isn't YouTube). true, or nil and why (no ffmpeg, no yt-dlp, no player). isekur.radio(false) stops yours.
// isekur.radio() says what's on: {state = "off", "resolving" or "playing", name, host, direct, web (a webchat is attached), muted, volume, err (why the last one stopped)}
static int l_radio(lua_State* L) {
    Plugin* p = self(L);
    Core& c = *p->c;
    if (lua_isnone(L, 1)) {
        bool mine = c.radio_on() && c.radio.by == p->name;
        lua_createtable(L, 0, 5);
        lua_pushstring(L, !mine ? "off" : c.radio.st == Core::Radio::PLAYING ? "playing" : "resolving"), lua_setfield(L, -2, "state");
        lua_pushstring(L, mine ? c.radio.name.c_str() : ""), lua_setfield(L, -2, "name");
        lua_pushboolean(L, mine && c.radio.host), lua_setfield(L, -2, "host");
        lua_pushboolean(L, c.web_up), lua_setfield(L, -2, "web");
        lua_pushboolean(L, mine && c.radio.muted), lua_setfield(L, -2, "muted");
        lua_pushboolean(L, mine && c.radio.direct), lua_setfield(L, -2, "direct");
        lua_pushinteger(L, c.radio.volume), lua_setfield(L, -2, "volume");
        lua_pushstring(L, c.radio.err.c_str()), lua_setfield(L, -2, "err");
        return 1;
    }
    if (!lua_isstring(L, 1)) {
        if (c.radio_on() && c.radio.by == p->name) c.radio_stop(), c.radio.muted = false;  // Stop: the next one starts with sound
        return 0;
    }
    std::string url = str_arg(L, 1), name = url;
    bool host = false, direct = false;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "name");
        if (lua_isstring(L, -1)) name = one_line(lua_tostring(L, -1)).substr(0, 120);
        lua_pop(L, 1);
        lua_getfield(L, 2, "host");
        host = lua_toboolean(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "direct");
        direct = lua_toboolean(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "volume");
        if (lua_isnumber(L, -1)) c.radio.volume = (int)std::clamp<lua_Number>(lua_tonumber(L, -1), 0, 150);
        lua_pop(L, 1);
    }
    std::string why = c.radio_start(p->name, url, name, host, direct);
    if (!why.empty()) return lua_pushnil(L), lua_pushstring(L, why.c_str()), 2;
    return lua_pushboolean(L, 1), 1;
}

// isekur.radio_volume(percent): the volume of your radio, 0-150 (100: as it comes), live; returns it. isekur.radio's volume option sets it at the start
static int l_radio_volume(lua_State* L) {
    Plugin* p = self(L);
    Core& c = *p->c;
    if (!lua_isnone(L, 1) && (!c.radio_on() || c.radio.by == p->name)) c.radio_volume((int)luaL_checkinteger(L, 1));
    return lua_pushinteger(L, c.radio.volume), 1;
}
// isekur.radio_mute(on): mutes or unmutes your radio, the webchat's sound and the player on this machine; the state
static int l_radio_mute(lua_State* L) {
    Plugin* p = self(L);
    Core& c = *p->c;
    if (c.radio_on() && c.radio.by == p->name && !lua_isnone(L, 1)) c.radio_mute(lua_toboolean(L, 1));
    return lua_pushboolean(L, c.radio_on() && c.radio.by == p->name && c.radio.muted), 1;
}

// what the isekur table gets from this file (host.cpp's load registers it)
const luaL_Reg plug::api_media[] = {
    {"radio", l_radio}, {"radio_mute", l_radio_mute}, {"radio_volume", l_radio_volume},
    {"platform", l_platform}, {"which", l_which}, {"run", l_run}, {"speak_web", l_speak_web},
    {nullptr, nullptr}};
