// /exec: run a shell command without blocking the client. Its output streams
// into the cmd tab, or with -o is said in the tab you typed it in once it ends.
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <pwd.h>
#ifndef ISEKUR_NO_PAM
#include <security/pam_appl.h>
#endif
#include <sys/ioctl.h>
#if defined(__APPLE__)
#include <util.h>
#elif defined(__linux__) || defined(__CYGWIN__)
#include <pty.h>
#else
#include <libutil.h>
#endif
#include "vterm.h"
#include <sys/wait.h>
#include <unistd.h>
#include "core.h"
#include "web.h"  // hash_password, check_password

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
    pid_t pid = fork();
    if (pid < 0) { ::close(p[0]), ::close(p[1]); print(b, LK::ERROR, std::string("fork: ") + strerror(errno)); return; }
    if (pid == 0) {  // child: own process group (the timeout kills it all), no terminal, no sockets
        setpgid(0, 0);
        for (int sig : {SIGPIPE, SIGHUP, SIGINT, SIGTERM}) signal(sig, SIG_DFL);
        int nul = ::open("/dev/null", O_RDONLY);
        dup2(nul, 0), dup2(p[1], 1), dup2(p[1], 2);
        for (int fd = 3; fd < 1024; fd++) ::close(fd);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
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
            size_t cap = (size_t)std::max(1, atoi(opt(j.to->srv, "paste_confirm", "5").c_str()));
            if (j.out.size() > cap && !j.yes)
                print(j.to, LK::ERROR, j.cmd + ": " + std::to_string(j.out.size()) + " lines, more than paste_confirm (" +
                                           std::to_string(cap) + "), not sent. /exec -o -y sends them anyway");
            else
                for (auto& l : j.out) say(j.to, l);
            if (!end.empty()) print(j.to, LK::INFO, j.cmd + ": " + end);
        }
        jobs.erase(jobs.begin() + (long)i);
    }
}

#ifndef ISEKUR_NO_PAM
// PAM conversation: the login for a shown prompt, the password for a hidden one
static int pam_conv_fn(int n, const struct pam_message** msg, struct pam_response** resp, void* data) {
    auto* up = (std::pair<std::string, std::string>*)data;
    auto* r = (pam_response*)calloc((size_t)n, sizeof(pam_response));
    if (!r) return PAM_BUF_ERR;
    for (int i = 0; i < n; i++) {
        int style = msg[i]->msg_style;
        if (style == PAM_PROMPT_ECHO_OFF) r[i].resp = strdup(up->second.c_str());
        else if (style == PAM_PROMPT_ECHO_ON) r[i].resp = strdup(up->first.c_str());
    }
    *resp = r;
    return PAM_SUCCESS;
}
#endif

// the system's own check of a login and password (PAM service pam_service, default "login").
// Only the user iSekur runs as: the shell runs as that user whoever logs in.
// ponytail: blocks the loop while PAM checks (a few seconds after a wrong password)
bool Core::os_login(const std::string& login, const std::string& pass, std::string& why) {
#ifdef ISEKUR_NO_PAM
    why = "this build has no PAM to check it with";
    return false;
#else
    passwd* pw = getpwuid(getuid());
    if (!pw || login != pw->pw_name) { why = ""; return false; }  // same answer as a wrong password
    std::pair<std::string, std::string> up{login, pass};
    pam_conv conv{pam_conv_fn, &up};
    pam_handle_t* ph = nullptr;
    std::string svc = opt(nullptr, "pam_service", "login");
    int rc = pam_start(svc.c_str(), login.c_str(), &conv, &ph);
    if (rc == PAM_SUCCESS) rc = pam_authenticate(ph, 0);
    if (rc != PAM_SUCCESS && rc != PAM_AUTH_ERR && rc != PAM_USER_UNKNOWN) why = pam_strerror(ph, rc);
    pam_end(ph, rc);
    return rc == PAM_SUCCESS;
#endif
}

// ---- /shell: a real terminal in a tab ---------------------------------------
// The shell runs on a pty; libvterm (vendor/libvterm) keeps its screen, which the
// TUI draws cell by cell. Nothing the shell prints reaches the real terminal as
// escapes: titles, clipboard (OSC 52) and other requests stay inside libvterm.

static std::vector<pid_t> hung_up;  // closed shells, reaped in terms_poll
static int login_fails = 0;
static time_t login_lock = 0;       // 3 wrong logins: /shell waits a minute

Term::~Term() {
    if (fd >= 0) ::close(fd);  // the shell gets SIGHUP and passes it on to its jobs
    if (pid > 0 && kill(pid, SIGHUP) == 0) hung_up.push_back(pid);
    if (vt) vterm_free(vt);
}

static void term_out(const char* s, size_t n, void* user) {  // keys and replies to the shell
    Term* t = (Term*)user;
    // ponytail: a shell not reading for 64KB drops keys; queue them if that ever matters
    while (n && t->fd >= 0) {
        ssize_t w = write(t->fd, s, n);
        if (w <= 0) break;
        s += w, n -= (size_t)w;
    }
}

// a line scrolled off the top: kept as text in the tab (the mouse wheel scrolls back to it)
static int term_pushline(int cols, const VTermScreenCell* cells, void* user) {
    Term* t = (Term*)user;
    std::string l;
    for (int i = 0; i < cols; i++) {
        uint32_t ch = cells[i].chars[0];
        if (ch == (uint32_t)-1 || (i && cells[i - 1].width == 2)) continue;  // a wide char's right half
        utf8_put(l, ch < 0x20 ? ' ' : ch);
    }
    while (!l.empty() && l.back() == ' ') l.pop_back();
    Line ln{time(nullptr), LK::INFO, "", l};
    ln.nolog = true;
    t->c->add(t->b, ln);
    return 1;
}

static int term_prop(VTermProp p, VTermValue* v, void* user) {
    if (p == VTERM_PROP_CURSORVISIBLE) ((Term*)user)->cursor = v->boolean;
    return 1;  // the rest (title, mouse modes...) are ignored
}

void Core::shell_cmd(Buffer* b) {
    if (from_web) { print(b, LK::ERROR, "/shell works in the terminal only"); return; }
#if defined(ISEKUR_NO_PAM) && defined(__CYGWIN__)
    print(b, LK::ERROR, "/shell needs PAM to check the login, and Cygwin has none");
    return;
#elif defined(ISEKUR_NO_PAM)
    print(b, LK::ERROR, "/shell needs PAM to check the login, and this build has none: install the PAM "
                        "headers (libpam0g-dev on Debian/Ubuntu, pam-devel on Fedora) and rebuild iSekur");
    return;
#endif
    if (b->type == BT::SHELL && b->term) { print(b, LK::ERROR, "This tab has a shell running"); return; }
    Buffer* sb = b->type == BT::SHELL ? b : nullptr;  // a shell tab whose shell ended: log in there again
    if (!sb) {
        int n = 0;
        for (auto& t : bufs) n += t->type == BT::SHELL;
        sb = open(nullptr, BT::SHELL, n ? "shell" + std::to_string(n + 1) : "shell");
    }
    select(sb);
    if (time(nullptr) < login_lock) {
        print(sb, LK::ERROR, "Too many wrong logins: try again in " + std::to_string(login_lock - time(nullptr)) + "s");
        return;
    }
    shell_login(sb, "");
}

// like sitting down at the machine: its login and password before a shell
void Core::shell_login(Buffer* sb, const std::string& err) {
    const std::string title = "Shell login";
    ask_pass(sb, "login", [this, sb, title](const std::string& login) {
        if (from_web) { print(sb, LK::ERROR, "/shell works in the terminal only"); return; }
        ask_pass(sb, "password", [this, sb, login](const std::string& pass) {
            if (from_web) { print(sb, LK::ERROR, "/shell works in the terminal only"); return; }
            std::string why;
            if (os_login(login, pass, why)) { login_fails = 0; shell_start(sb); return; }
            std::string bad = "Login incorrect" + (why.empty() ? "" : ": " + why);
            if (++login_fails < 3) { shell_login(sb, bad); return; }
            login_fails = 0, login_lock = time(nullptr) + 60;
            print(sb, LK::ERROR, bad + ". Too many wrong logins: /shell again in a minute, Ctrl+W closes the tab");
        }, true, title, {"login: " + login});
    }, false, title, err.empty() ? std::vector<std::string>{} : std::vector<std::string>{"!" + err});
}

void Core::shell_start(Buffer* sb) {
    const char* sh = getenv("SHELL");
    std::string path = sh && *sh ? sh : "/bin/sh";
    auto t = std::make_unique<Term>();
    t->c = this, t->b = sb;
    winsize ws{(unsigned short)t->rows, (unsigned short)t->cols, 0, 0};
    int fd = -1;
    pid_t pid = forkpty(&fd, nullptr, nullptr, &ws);
    if (pid < 0) { print(sb, LK::ERROR, std::string("forkpty: ") + strerror(errno)); return; }
    if (pid == 0) {  // child: a new session on the pty, nothing of iSekur's open
        for (int sig : {SIGPIPE, SIGHUP, SIGINT, SIGTERM, SIGQUIT, SIGCHLD, SIGTSTP, SIGTTIN, SIGTTOU}) signal(sig, SIG_DFL);
        for (int f = 3; f < 1024; f++) ::close(f);
        setenv("TERM", "xterm-256color", 1);
        if (const char* home = getenv("HOME")) if (chdir(home)) {}
        std::string arg0 = "-" + path.substr(path.rfind('/') + 1);  // "-zsh": a login shell
        execl(path.c_str(), arg0.c_str(), (char*)nullptr);
        _exit(127);
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    t->pid = pid, t->fd = fd;
    t->vt = vterm_new(t->rows, t->cols);
    vterm_set_utf8(t->vt, 1);
    vterm_output_set_callback(t->vt, term_out, t.get());
    VTermScreen* scr = vterm_obtain_screen(t->vt);
    static VTermScreenCallbacks cb{};
    cb.sb_pushline = term_pushline, cb.settermprop = term_prop;
    vterm_screen_set_callbacks(scr, &cb, t.get());
    vterm_screen_enable_altscreen(scr, 1);
    vterm_screen_reset(scr, 1);
    sb->term = std::move(t);
    dirty = true;
}

void Core::term_resize(Term* t, int rows, int cols) {
    if (rows < 1 || cols < 1 || (rows == t->rows && cols == t->cols)) return;
    t->rows = rows, t->cols = cols;
    vterm_set_size(t->vt, rows, cols);
    winsize ws{(unsigned short)rows, (unsigned short)cols, 0, 0};
    ioctl(t->fd, TIOCSWINSZ, &ws);  // the shell gets SIGWINCH
}

// read the ptys poll() marked (their fds from fds[from] on, in tab order); a shell that
// exited leaves its last screen as text and the tab back to normal
void Core::terms_poll(const std::vector<pollfd>& fds, size_t from) {
    std::vector<Buffer*> ended;
    for (auto& bp : bufs) {
        Term* t = bp->term.get();
        if (!t || t->fd < 0 || from >= fds.size()) continue;
        if (fds[from++].revents) {
            char buf[65536];
            ssize_t n;
            while ((n = read(t->fd, buf, sizeof buf)) > 0) vterm_input_write(t->vt, buf, (size_t)n);
            if (n == 0 || (errno != EAGAIN && errno != EINTR)) ended.push_back(bp.get());  // EIO on Linux once it exits
            dirty = true;
            if (bp.get() != active && !bp->activity) bp->activity = 1, notify(Ev::BUFFER, bp.get());
        }
    }
    for (Buffer* b : ended) {
        Term* t = b->term.get();
        VTermScreen* scr = vterm_obtain_screen(t->vt);
        int last = -1;  // the screen down to its last non-empty row
        std::vector<std::string> rows;
        for (int r = 0; r < t->rows; r++) {
            std::string l;
            for (int c2 = 0; c2 < t->cols; c2++) {
                VTermScreenCell cl;
                vterm_screen_get_cell(scr, VTermPos{r, c2}, &cl);
                if (cl.chars[0] == (uint32_t)-1) continue;  // a wide char's right half
                utf8_put(l, cl.chars[0] < 0x20 ? ' ' : cl.chars[0]);
            }
            while (!l.empty() && l.back() == ' ') l.pop_back();
            if (!l.empty()) last = r;
            rows.push_back(l);
        }
        for (int r = 0; r <= last; r++) {
            Line ln{time(nullptr), LK::INFO, "", rows[(size_t)r]};
            ln.nolog = true;
            add(b, ln);
        }
        int st = 0;
        pid_t pid = t->pid;
        t->pid = -1;  // reaped here, not hung up
        std::string end = waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) ? ": exit " + std::to_string(WEXITSTATUS(st)) : "";
        b->term.reset();
        print(b, LK::INFO, "\x02[shell ended" + end + "]\x02 /shell starts another, Ctrl+W closes the tab");
    }
    for (size_t i = 0; i < hung_up.size();)
        if (waitpid(hung_up[i], nullptr, WNOHANG) != 0) hung_up.erase(hung_up.begin() + (long)i);
        else i++;
}

// a line typed in a shell tab with no shell running (before the login, or after it ended)
void Core::shell_line(Buffer* b, const std::string& l) {
    print(b, LK::ERROR, from_web ? "The shell works in the terminal only" : "No shell running here: /shell logs in and starts one");
}

// ---- /lock: a master password for the terminal -------------------------------
// ponytail: hides the session on screen only; the config, logs and webchat are not
// encrypted or locked by it (the webchat has its own login)
static int unlock_fails = 0;
static time_t unlock_wait = 0;

bool Core::lock_on() const {
    return opt_on(nullptr, "lock", false) && !opt(nullptr, "lock_password_hash", "").empty();
}

void Core::unlock(const std::string& pass) {
    if (time(nullptr) < unlock_wait) {
        lock_msg = "!Too many tries: wait " + std::to_string(unlock_wait - time(nullptr)) + "s";
    } else if (check_password(pass, opt(nullptr, "lock_password_hash", ""))) {
        locked = false, unlock_fails = 0, lock_msg.clear();
    } else if (++unlock_fails >= 3) {
        unlock_fails = 0, unlock_wait = time(nullptr) + 60;
        lock_msg = "!Wrong password. Too many tries: wait 60s";
    } else lock_msg = "!Wrong password";
    dirty = true;
}

static bool save_general(Core& c, const std::map<std::string, std::string>& kv, Buffer* b) {
    Ini part;
    for (auto& [k, v] : kv) c.cfg["general"][k] = part["general"][k] = v, c.unsaved["general"].erase(k);
    std::string err;
    if (ini_save(c.cfg_path, part, err)) return true;
    c.print(b, LK::ERROR, err);
    return false;
}

// /lock: lock now; /lock on: set the password and lock from now on; /lock off: stop
void Core::lock_cmd(Buffer* b, const std::string& args) {
    if (from_web && !args.empty()) { print(b, LK::ERROR, "/lock on|off works in the terminal only"); return; }
    if (args.empty()) {
        if (!lock_on()) { print(b, LK::ERROR, "Locking is off: /lock on sets a password"); return; }
        locked = true, lock_msg.clear(), dirty = true;
        return;
    }
    const std::string title = "Lock password";
    if (args == "on") {  // with locking already on, it's a change: the current password first
        auto set_new = [this, b, title]() {
            ask_pass(b, "new password", [this, b, title](const std::string& p1) {
                ask_pass(b, "again", [this, b, p1](const std::string& p2) {
                    if (p1 != p2) { print(b, LK::ERROR, "The passwords don't match: /lock on to try again"); return; }
                    if (save_general(*this, {{"lock", "on"}, {"lock_password_hash", hash_password(p1)}}, b))
                        print(b, LK::INFO, "Locking on: iSekur asks for this password at startup, on every attach and after /lock");
                }, true, title, {"Type it again"});
            }, true, title, {"Asked at startup, on attach and after /lock"});
        };
        if (!lock_on()) { set_new(); return; }
        ask_pass(b, "current password", [this, b, set_new](const std::string& p) {
            if (!check_password(p, opt(nullptr, "lock_password_hash", ""))) { print(b, LK::ERROR, "Wrong password: it stays as it was"); return; }
            set_new();
        }, true, title, {"Changing it: the current one first"});
    } else if (args == "off") {
        if (!lock_on()) { print(b, LK::INFO, "Locking is already off"); return; }
        ask_pass(b, "password", [this, b](const std::string& p) {
            if (!check_password(p, opt(nullptr, "lock_password_hash", ""))) { print(b, LK::ERROR, "Wrong password: locking stays on"); return; }
            if (save_general(*this, {{"lock", "off"}, {"lock_password_hash", ""}}, b)) print(b, LK::INFO, "Locking off");
        }, true, title, {"The current one, to turn locking off"});
    } else print(b, LK::ERROR, "Usage: /lock [on|off]");
}
