// /exec: run a shell command without blocking the client. Its output streams
// into the cmd tab, or with -o is said in the tab you typed it in once it ends.
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "core.h"
#include "web.h"  // hash_password, check_password
#include "session.h"  // cfg_side
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <utility>

static const size_t MAX_JOB_LINES = 1000;  // then it's killed: `yes` must not fill memory

// one output line for a chat line: no ANSI escapes, tabs expanded, no CR
static std::string clean(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        char ch = s[i];
        if (ch == '\x1b') {  // ESC [ params letter, or ESC + one char
            if (i + 1 < s.size() && s[i + 1] == '[') {
                i += 2;
                while (i < s.size() && !isalpha((unsigned char)s[i])) i++;
            } else i++;
        } else if (ch == '\t') o.append(8 - o.size() % 8, ' ');
        else if (ch != '\r') o += ch;
    }
    return o;
}

// the shell a /exec line runs in: bash or zsh (your login shell when it is one), else bash. /bin/sh is dash, or bash in
// POSIX mode on macOS, where echo -e and echo -n print the flag as text
static std::string exec_shell() {
    const char* sh = getenv("SHELL");
    std::string base = sh ? sh : "";
    base = base.substr(base.rfind('/') + 1);
    if (base == "bash" || base == "zsh") return sh;
    return access("/bin/bash", X_OK) == 0 ? "/bin/bash" : "/bin/sh";
}

// /exec [-o] [-y] command
void Core::exec_cmd(Buffer* b, const std::string& args) {
    if (from_web && !opt_on(nullptr, "exec_web", false)) {
        print(b, LK::ERROR, "/exec is off in the webchat: it runs commands on this machine (/set exec_web on to allow it)");
        return;
    }
    Job j;
    std::string cmd = args;
    for (;;) {
        if (cmd.rfind("-o ", 0) == 0) j.say = true;
        else if (cmd.rfind("-y ", 0) == 0) j.yes = true;
        else break;
        cmd = after_words(cmd, 1);
    }
    if (cmd.empty()) {
        if (jobs.empty()) print(b, LK::INFO, "Usage: /exec [-o] command   (no commands running)");
        for (auto& r : jobs) print(b, LK::INFO, "running " + std::to_string(time(nullptr) - r.started) + "s: " + r.cmd);
        return;
    }
    if (j.say && (b->type == BT::STATUS || !b->srv)) { print(b, LK::ERROR, "/exec -o sends to a channel or query: type it there"); return; }
    int p[2];
    if (pipe(p) < 0) { print(b, LK::ERROR, std::string("pipe: ") + strerror(errno)); return; }
    std::string sh = exec_shell();
    pid_t pid = fork();
    if (pid < 0) { ::close(p[0]), ::close(p[1]); print(b, LK::ERROR, std::string("fork: ") + strerror(errno)); return; }
    if (pid == 0) {  // child: own process group (the timeout kills it all), no terminal, no sockets
        setpgid(0, 0);
        for (int sig : {SIGPIPE, SIGHUP, SIGINT, SIGTERM}) signal(sig, SIG_DFL);
        int nul = ::open("/dev/null", O_RDONLY);
        dup2(nul, 0), dup2(p[1], 1), dup2(p[1], 2);
        for (int fd = 3; fd < 1024; fd++) ::close(fd);
        execl(sh.c_str(), sh.substr(sh.rfind('/') + 1).c_str(), "-c", cmd.c_str(), (char*)nullptr);
        _exit(127);
    }
    ::close(p[1]);
    fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);
    fcntl(p[0], F_SETFD, FD_CLOEXEC);
    j.pid = pid, j.fd = p[0], j.cmd = cmd, j.started = time(nullptr), j.to = b;
    jobs.push_back(j);
    if (!j.say) print(bufs.front().get(), LK::INFO, "\x02$ " + cmd + "\x02");
}

void Core::job_line(Job& j, const std::string& raw) {
    if (j.nlines >= MAX_JOB_LINES) return;  // killed, the pipe is draining
    std::string l = clean(raw);
    if (j.say) { if (!l.empty()) j.out.push_back(l); }
    else print(bufs.front().get(), LK::INFO, "  " + l);
    if (++j.nlines == MAX_JOB_LINES) {
        kill(-j.pid, SIGKILL);
        j.note = "stopped after " + std::to_string(MAX_JOB_LINES) + " lines";
    }
}

void Core::job_read(Job& j) {
    char buf[4096];
    ssize_t n;
    while ((n = read(j.fd, buf, sizeof buf)) > 0) {
        j.partial.append(buf, (size_t)n);
        size_t nl;
        while ((nl = j.partial.find('\n')) != std::string::npos) {
            job_line(j, j.partial.substr(0, nl));
            j.partial.erase(0, nl + 1);
        }
    }
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {  // EOF: output done
        if (!j.partial.empty()) job_line(j, j.partial), j.partial.clear();
        ::close(j.fd), j.fd = -1;
    }
}

// read the pipes poll() marked, reap what ended, kill what ran too long
void Core::jobs_poll(const std::vector<pollfd>& fds, size_t from) {
    for (size_t i = 0, k = from; i < jobs.size(); i++)
        if (jobs[i].fd >= 0 && k < fds.size()) {
            if (fds[k].revents) job_read(jobs[i]);
            k++;
        }
    int limit = atoi(opt(nullptr, "exec_timeout", "30").c_str());
    for (size_t i = 0; i < jobs.size();) {
        Job& j = jobs[i];
        if (limit > 0 && j.note.empty() && time(nullptr) - j.started >= limit) {
            kill(-j.pid, SIGKILL);
            j.note = "killed after " + std::to_string(limit) + "s (exec_timeout)";
        }
        int st = 0;
        if (j.fd >= 0 || waitpid(j.pid, &st, WNOHANG) == 0) { i++; continue; }  // still going
        std::string end = !j.note.empty() ? j.note
                          : WIFEXITED(st) && WEXITSTATUS(st) ? "exit " + std::to_string(WEXITSTATUS(st))
                          : WIFSIGNALED(st) ? "killed by signal " + std::to_string(WTERMSIG(st))
                                            : "";
        if (!j.say) {
            if (!end.empty()) print(bufs.front().get(), LK::INFO, "  [" + end + "]");
        } else if (index_of(j.to) < 0) {
            // the tab is gone: nothing to say it in
        } else {
            int cap = atoi(opt(j.to->srv, "paste_confirm", "5").c_str());  // 0: never ask, as for a paste
            if (cap > 0 && j.out.size() > (size_t)cap && !j.yes)
                print(j.to, LK::ERROR, j.cmd + ": " + std::to_string(j.out.size()) + " lines, more than paste_confirm (" +
                                           std::to_string(cap) + "), not sent. /exec -o -y sends them anyway");
            else
                for (auto& l : j.out) say(j.to, l);
            if (!end.empty()) print(j.to, LK::INFO, j.cmd + ": " + end);
        }
        jobs.erase(jobs.begin() + (long)i);
    }
}

// ---- /lock: a master password for the terminal and the webchat -------------
// ponytail: hides the session (the webchat gets none of it while locked); the config
// and logs are not encrypted by it
bool Core::lock_on() const {
    return opt_on(nullptr, "lock", false) && !opt(nullptr, "lock_password_hash", "").empty();
}

std::string Core::lock_count(const std::function<bool()>& ok, const std::string& wrong, const std::string& then) {
    time_t now = time(nullptr);
    if (now < lock_wait) return "Too many tries: wait " + std::to_string(lock_wait - now) + "s";
    if (ok()) { lock_fails = 0; return ""; }
    if (++lock_fails < 3) return wrong + (then.empty() ? "" : ": " + then);
    lock_fails = 0, lock_wait = now + 60;
    return wrong + ". Too many tries: wait 60s";
}

std::string Core::lock_try(const std::string& pass, const std::string& then) {
    return lock_count([&] { return check_password(pass, opt(nullptr, "lock_password_hash", "")); }, "Wrong password", then);
}

void Core::unlock(const std::string& pass) { unlock_by(lock_try(pass, "")); }

// ---- the webchat's Face ID: the devices (web.passkeys) -------------------------
// a line each: id, key, on (1/0), count, added, name; tab-separated. Public keys only: nothing in it unlocks anything
static std::string passkeys_path(const std::string& cfg_path) {
    std::string base = cfg_path.substr(cfg_path.rfind('/') + 1);  // isekur.conf: web.passkeys, like web.sessions
    return base == "isekur.conf" ? cfg_path.substr(0, cfg_path.rfind('/') + 1) + "web.passkeys" : cfg_side(cfg_path, "web.passkeys");
}

std::vector<Core::Passkey>& Core::passkeys() {
    if (std::exchange(passkeys_read, true)) return passkeys_;
    FILE* f = fopen(passkeys_path(cfg_path).c_str(), "r");
    if (!f) return passkeys_;
    char ln[4096];
    while (fgets(ln, sizeof ln, f)) {
        auto p = split(std::string(ln).substr(0, strcspn(ln, "\r\n")), '\t');
        if (p.size() < 6 || p[0].empty() || unb64(p[1]).empty()) continue;
        passkeys_.push_back({p[0], p[1], p[5], p[2] == "1", (uint32_t)strtoul(p[3].c_str(), nullptr, 10), (time_t)atoll(p[4].c_str())});
    }
    fclose(f);
    return passkeys_;
}

bool Core::passkeys_save(Buffer* b) {
    std::string path = passkeys_path(cfg_path), tmp = path + ".tmp";
    mode_t old = umask(077);
    FILE* f = fopen(tmp.c_str(), "w");
    umask(old);
    bool ok = f;
    for (auto& k : passkeys()) {
        std::string name = k.name;
        for (auto& ch : name) if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
        if (f) fprintf(f, "%s\t%s\t%d\t%u\t%lld\t%s\n", k.id.c_str(), k.key.c_str(), k.on ? 1 : 0, k.count, (long long)k.added, name.c_str());
    }
    if (f && fclose(f) != 0) ok = false;
    if (ok && rename(tmp.c_str(), path.c_str()) != 0) ok = false;
    if (!ok) print(b, LK::ERROR, "Can't write " + path + ": " + strerror(errno));
    return ok;
}

bool Core::bio_on() {
    if (!lock_on() || !opt_on(nullptr, "lock_biometric", false)) return false;
    for (auto& k : passkeys()) if (k.on) return true;
    return false;
}

void Core::unlock_by(const std::string& e) {
    active_at = last_input = time(nullptr);  // typing it is input: else the screensaver it was over comes right back (still idle)
    if (e.empty()) {
        set_lock(false);
        screen_end();  // the box was over a screensaver: that's done too
        return;
    }
    lock_msg = "!" + e;
    lock_anim = 'f', lock_anim_at = now_ms(), dirty = true;
    notify(Ev::LOCK, nullptr);  // the webchat's box shows it too
}

void Core::set_lock(bool on, bool saver) {
    locked = on, lock_msg.clear(), dirty = true;
    if (on) lock_grace_until[0] = lock_grace_until[1] = 0;  // locked: the next guarded /set asks again
    crumb("lock", on ? "on" : "off");
    lock_anim = on ? 'l' : 'u', lock_anim_at = now_ms();
    notify(Ev::LOCK, nullptr);
    if (on && saver) plugin_emit("lock", nullptr, "");  // Alt+S, /lock, lock_idle: the screensaver plugin starts one under the password box
}

static bool save_general(Core& c, const std::map<std::string, std::string>& kv, Buffer* b) {
    Ini part;
    for (auto& [k, v] : kv) c.cfg["general"][k] = part["general"][k] = v, c.unsaved["general"].erase(k);
    std::string err;
    if (ini_save(c.cfg_path, part, err)) return true;
    c.print(b, LK::ERROR, err);
    return false;
}

void Core::screen_end() {
    if (screen_by.empty()) return;
    crumb("screen", "end");
    screen_by.clear(), dirty = true;
    if (locked) lock_anim = 'l', lock_anim_at = now_ms();  // under it all along: the box drops in now
    notify(Ev::SCREEN, nullptr);
}

void Core::view_end() {
    if (view.by.empty()) return;
    view = View{}, dirty = true;
    notify(Ev::THEME, nullptr);  // the webchat: the configured theme and layout again
}

void Core::lock_saver() {
    if (locked && screen_by.empty()) plugin_emit("lock", nullptr, "");
}

bool Core::wake() {
    active_at = last_input = time(nullptr);  // a key, click or move is input too: autoaway's idle and lock_idle count it
    if (screen_by.empty() || locked) return false;  // locked: the screensaver stays, the password box comes over it
    screen_end();
    return true;
}

// the lock key: locks at once, terminal and webchat. Without a password it only says how to set one
void Core::lock_now(Buffer* b) {
    if (lock_on()) set_lock(true);
    else print(b, LK::INFO, "No lock password yet: /lock sets one and locks");
}

// /lock: lock now (no password yet: set one first); /lock on: set the password; /lock off: stop
// off and a change ask for the current password, so the webchat may run them too
void Core::lock_cmd(Buffer* b, const std::string& args) {
    const std::string title = "Lock password";
    if (args.empty() && lock_on()) { set_lock(true); return; }
    if (args.empty() || args == "on") {  // with locking already on, it's a change: the current password first
        bool now = args.empty();
        auto set_new = [this, b, title, now]() {
            ask_pass(b, "new password", [this, b, title, now](const std::string& p1) {
                ask_pass(b, "again", [this, b, p1, now](const std::string& p2) {
                    if (p1 != p2) { print(b, LK::ERROR, std::string("The passwords don't match: ") + (now ? "/lock" : "/lock on") + " to try again"); return; }
                    if (!save_general(*this, {{"lock", "on"}, {"lock_password_hash", hash_password(p1)}}, b)) return;
                    print(b, LK::INFO, "Locking on: iSekur asks for this password at startup, on every attach and after /lock");
                    if (now) set_lock(true);
                }, true, title, {"Type it again"});
            }, true, title, {now ? "Locks the terminal and the webchat now, and at startup and on attach" : "Asked at startup, on attach and after /lock"});
        };
        if (!lock_on()) { set_new(); return; }
        ask_pass(b, "current password", [this, b, set_new](const std::string& p) {
            if (auto e = lock_try(p, "it stays as it was"); !e.empty()) { print(b, LK::ERROR, e); return; }
            set_new();
        }, true, title, {"Changing it: the current one first"});
    } else if (args == "off") {
        if (!lock_on()) { print(b, LK::INFO, "Locking is already off"); return; }
        ask_pass(b, "password", [this, b](const std::string& p) {
            if (auto e = lock_try(p, "locking stays on"); !e.empty()) { print(b, LK::ERROR, e); return; }
            if (save_general(*this, {{"lock", "off"}, {"lock_password_hash", ""}}, b)) print(b, LK::INFO, "Locking off");
        }, true, title, {"The current one, to turn locking off"});
    } else if (args == "devices" || args.rfind("devices ", 0) == 0) {  // the webchat's Face ID devices: list, or del one
        auto& ks = passkeys();
        std::string rest = trim(args.substr(7));
        if (rest.empty()) {
            if (ks.empty()) { print(b, LK::INFO, "No Face ID devices: the webchat's settings (Security) sets one up"); return; }
            print(b, LK::INFO, std::string("Face ID devices (lock_biometric ") + (opt_on(nullptr, "lock_biometric", false) ? "on" : "off") + "):");
            for (auto& k : ks) print(b, LK::INFO, "  " + k.name + (k.on ? "" : " (off)") + ", added " + fmt_time(k.added, "%Y-%m-%d"));
            return;
        }
        if (rest.rfind("del ", 0) != 0) { print(b, LK::ERROR, "Usage: /lock devices [del <name>]"); return; }
        std::string name = trim(rest.substr(4));
        int hit = -1, n = 0;
        for (size_t i = 0; i < ks.size(); i++) if (irc_lower(ks[i].name) == irc_lower(name)) hit = (int)i, n++;
        if (n != 1) { print(b, LK::ERROR, n ? "More than one device is called " + name + ": remove it in the webchat's settings" : "No Face ID device called " + name + " (/lock devices lists them)"); return; }
        std::string id = ks[hit].id;
        ask_pass(b, "password", [this, b, id](const std::string& p) {
            if (auto e = lock_try(p, "the device stays"); !e.empty()) { print(b, LK::ERROR, e); return; }
            auto& v = passkeys();
            auto it = std::find_if(v.begin(), v.end(), [&](const Passkey& k) { return k.id == id; });
            if (it == v.end()) return;
            std::string was = it->name;
            v.erase(it);
            if (passkeys_save(b)) print(b, LK::INFO, "Removed " + was + ": it can't unlock any more (its passkey may stay in the phone's settings)");
        }, true, title, {"Removing a Face ID device: the lock password first"});
    } else print(b, LK::ERROR, "Usage: /lock [on|off|devices]");
}
