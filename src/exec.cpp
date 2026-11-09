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
    if (p == VTERM_PROP_MOUSE) ((Term*)user)->mouse = v->number;
    if (p == VTERM_PROP_ALTSCREEN && v->boolean) ((Term*)user)->up = true;  // the remote iSekur's screen
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

// `ssh -G` output -> "user@hostname:port", "" when it says nothing (an old ssh)
std::string ssh_dest(const std::string& g) {
    std::string user, hostname, port;
    for (auto& l : split(g, '\n')) {
        size_t sp = l.find(' ');
        if (sp == std::string::npos) continue;
        std::string k = l.substr(0, sp), v = l.substr(sp + 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
        if (k == "user" && user.empty()) user = v;
        else if (k == "hostname" && hostname.empty()) hostname = v;
        else if (k == "port" && port.empty()) port = v;
    }
    if (hostname.empty()) return "";
    if (hostname.find(':') != std::string::npos) hostname = "[" + hostname + "]";  // IPv6
    return (user.empty() ? "" : user + "@") + hostname + (port.empty() ? "" : ":" + port);
}

// `ssh -G host`: what ssh would use after ~/.ssh/config. No shell in between, and the UI waits no more than 3s for it
// (a `Match exec` in the config can run anything)
static std::string ssh_config(const std::string& host, const std::string& port) {
    int p[2];
    if (pipe(p)) return "";
    pid_t pid = fork();
    if (pid < 0) { ::close(p[0]), ::close(p[1]); return ""; }
    if (pid == 0) {
        for (int sig : {SIGPIPE, SIGHUP, SIGINT, SIGTERM}) signal(sig, SIG_DFL);
        int nul = ::open("/dev/null", O_RDWR);
        dup2(nul, 0), dup2(p[1], 1), dup2(nul, 2);
        for (int fd = 3; fd < 1024; fd++) ::close(fd);
        if (port.empty()) execlp("ssh", "ssh", "-G", "--", host.c_str(), (char*)nullptr);
        else execlp("ssh", "ssh", "-G", "-p", port.c_str(), "--", host.c_str(), (char*)nullptr);
        _exit(127);
    }
    ::close(p[1]);
    std::string out;
    long long until = now_ms() + 3000;
    for (;;) {
        long long left = until - now_ms();
        pollfd pf{p[0], POLLIN, 0};
        char buf[4096];
        ssize_t n;
        if (left <= 0 || out.size() > 65536 || poll(&pf, 1, (int)left) <= 0 || (n = read(p[0], buf, sizeof buf)) <= 0) break;
        out.append(buf, (size_t)n);
    }
    ::close(p[0]);
    kill(pid, SIGKILL);  // it's done, or too slow
    waitpid(pid, nullptr, 0);
    return out;
}

// /attach host: ssh runs the other machine's iSekur --attach in a tab like /shell's.
// No login asked here: ssh asks for (or uses keys for) its own.
void Core::attach_cmd(Buffer* b, const std::string& args) {
    if (from_web) { print(b, LK::ERROR, "/attach works in the terminal only"); return; }
    auto w = split(args, ' ');
    std::string port;  // -p N (anywhere): ssh's own -p, the one option let through, digits only
    for (size_t i = 0; i < w.size(); i++)
        if (w[i] == "-p" && port.empty()) {
            port = i + 1 < w.size() ? w[i + 1] : "";
            bool num = !port.empty() && port.size() <= 5 && port.find_first_not_of("0123456789") == std::string::npos;
            if (!num || atoi(port.c_str()) < 1 || atoi(port.c_str()) > 65535) { print(b, LK::ERROR, "/attach -p takes a port, 1-65535"); return; }
            w.erase(w.begin() + (long)i, w.begin() + (long)i + 2);
            break;
        }
    // words that go into ssh's arguments and the remote shell's command line: never an
    // option ("-oProxyCommand=..." would run a command here), never a quote or a $
    auto safe = [](const std::string& x, const char* extra) {
        bool ok = !x.empty() && x[0] != '-';
        for (char ch : x) ok = ok && (isalnum((unsigned char)ch) || strchr(extra, ch));
        return ok;
    };
    std::string host = w.empty() ? "" : w[0], rcfg = w.size() > 1 ? w[1] : "";
    if (w.size() > 2 || !safe(host, "@._-:[]") || (!rcfg.empty() && !safe(rcfg, "/._-~+"))) {
        print(b, LK::ERROR, "Usage: /attach [-p port] host [config]  (host: user@host or a Host from ~/.ssh/config; "
                            "config: that machine's isekur -c file, for one of several iSekurs there)");
        return;
    }
    std::string name = host;  // vps, vps:work for ~/.config/isekur/work.conf, vps:fun for ~/fun/isekur.conf
    if (!rcfg.empty()) {
        std::string p = rcfg;
        while (p.size() > 1 && p.back() == '/') p.pop_back();
        std::string base = p.substr(p.rfind('/') + 1);
        if (base.size() > 5 && base.compare(base.size() - 5, 5, ".conf") == 0) base.resize(base.size() - 5);
        if (base == "isekur" && p.rfind('/') != std::string::npos && p.rfind('/') > 0) {  // the folder tells them apart
            std::string dir = p.substr(0, p.rfind('/'));
            base = dir.substr(dir.rfind('/') + 1);
        }
        name += ":" + base;
    }
    Buffer* ab = open(nullptr, BT::SHELL, name);
    select(ab);
    if (ab->term) return;  // already attached there
    // what ssh will really reach after ~/.ssh/config: ssh -G only reads config, no network
    std::string g = ssh_config(host, port);
    std::string dest = ssh_dest(g);
    print(ab, LK::INFO, "\u25b6 ssh " + (dest.empty() ? (port.empty() ? "" : "-p " + port + " ") + host : dest + " (" + host + ")") +
                            ": iSekur --attach there" + (rcfg.empty() ? "" : " with " + rcfg) + " (Alt+D or /detach inside leaves it running)");
    shell_start(ab, host, rcfg, port);
}

void Core::shell_start(Buffer* sb, const std::string& host, const std::string& rcfg, const std::string& port) {
    const char* sh = getenv("SHELL");
    std::string path = sh && *sh ? sh : "/bin/sh";
    auto t = std::make_unique<Term>();
    t->c = this, t->b = sb, t->host = host, t->rcfg = rcfg, t->port = port;
    winsize ws{(unsigned short)t->rows, (unsigned short)t->cols, 0, 0};
    int fd = -1;
    pid_t pid = forkpty(&fd, nullptr, nullptr, &ws);
    if (pid < 0) { print(sb, LK::ERROR, std::string("forkpty: ") + strerror(errno)); return; }
    if (pid == 0) {  // child: a new session on the pty, nothing of iSekur's open
        for (int sig : {SIGPIPE, SIGHUP, SIGINT, SIGTERM, SIGQUIT, SIGCHLD, SIGTSTP, SIGTTIN, SIGTTOU}) signal(sig, SIG_DFL);
        for (int f = 3; f < 1024; f++) ::close(f);
        setenv("TERM", "xterm-256color", 1);
        if (const char* home = getenv("HOME")) if (chdir(home)) {}
        if (!host.empty()) {  // a login shell there, so isekur is on its PATH (Homebrew's too)
            std::string cmd = "sh -c 'exec \"${SHELL:-sh}\" -lc \"isekur " + (rcfg.empty() ? "" : "-c " + rcfg + " ") + "--attach\"'";
            // a wrong port or a dead host gives up in 10s, a link that dies later in ~45s, not the OS's minutes
            const char* o[] = {"-o", "ConnectTimeout=10", "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=3"};
            if (port.empty()) execlp("ssh", "ssh", "-t", o[0], o[1], o[2], o[3], o[4], o[5], "--", host.c_str(), cmd.c_str(), (char*)nullptr);
            else execlp("ssh", "ssh", "-t", o[0], o[1], o[2], o[3], o[4], o[5], "-p", port.c_str(), "--", host.c_str(), cmd.c_str(), (char*)nullptr);
            _exit(127);
        }
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
    if (!host.empty()) {  // something on the screen until ssh says more (the remote iSekur clears it)
        std::string m = "Connecting to " + host + (port.empty() ? "" : " port " + port) + "... (Ctrl+C on an empty line gives up)\r\n";
        vterm_input_write(t->vt, m.data(), m.size());
    }
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
        // its terminal closed, so it is exiting: a moment for its status, never more (a process that closed its tty and
        // carries on is reaped later with the hung up ones, not waited for with the whole client stopped)
        pid_t gone = 0;
        for (int i = 0; i < 20 && !(gone = waitpid(pid, &st, WNOHANG)); i++) usleep(5000);
        if (!gone) hung_up.push_back(pid);
        std::string end = gone == pid && WIFEXITED(st) && WEXITSTATUS(st) ? ": exit " + std::to_string(WEXITSTATUS(st)) : "";
        std::string host = (t->port.empty() || t->host.empty() ? "" : "-p " + t->port + " ") + t->host + (t->rcfg.empty() ? "" : " " + t->rcfg);
        time_t t0 = t->t0;
        b->term.reset();
        if (host.empty()) print(b, LK::INFO, "\x02[shell ended" + end + "]\x02 /shell starts another, Ctrl+W closes the tab");
        else print(b, LK::INFO, "\u25c0 \x02[ssh " + host + " ended after " + fmt_dur((long)(time(nullptr) - t0)) + end + "]\x02 /attach " + host +
                                    " again, Ctrl+W closes the tab");
    }
    for (size_t i = 0; i < hung_up.size();)
        if (waitpid(hung_up[i], nullptr, WNOHANG) != 0) hung_up.erase(hung_up.begin() + (long)i);
        else i++;
}

// a line typed in a shell tab with no shell running (before the login, or after it ended)
void Core::shell_line(Buffer* b, const std::string& l) {
    if (!from_web && b->term && b->term->fd >= 0) {  // /attach still connecting: a line for ssh (yes to a new host key)
        std::string w = l + "\r";
        if (::write(b->term->fd, w.data(), w.size()) < 0) {}
        return;
    }
    print(b, LK::ERROR, from_web ? "The shell works in the terminal only" : "No shell running here: /shell logs in and starts one, /attach host reaches another iSekur");
}

// ---- /lock: a master password for the terminal and the webchat -------------
// ponytail: hides the session (the webchat gets none of it while locked); the config
// and logs are not encrypted by it
static int unlock_fails = 0;
static time_t unlock_wait = 0;

bool Core::lock_on() const {
    return opt_on(nullptr, "lock", false) && !opt(nullptr, "lock_password_hash", "").empty();
}

void Core::unlock(const std::string& pass) {
    if (time(nullptr) < unlock_wait) {
        lock_msg = "!Too many tries: wait " + std::to_string(unlock_wait - time(nullptr)) + "s";
    } else if (check_password(pass, opt(nullptr, "lock_password_hash", ""))) {
        unlock_fails = 0, set_lock(false);
        return;
    } else if (++unlock_fails >= 3) {
        unlock_fails = 0, unlock_wait = time(nullptr) + 60;
        lock_msg = "!Wrong password. Too many tries: wait 60s";
    } else lock_msg = "!Wrong password";
    lock_anim = 'f', lock_anim_at = now_ms(), dirty = true;
    notify(Ev::LOCK, nullptr);  // the webchat's box shows it too
}

void Core::set_lock(bool on) {
    locked = on, lock_msg.clear(), dirty = true;
    crumb("lock", on ? "on" : "off");
    lock_anim = on ? 'l' : 'u', lock_anim_at = now_ms();
    notify(Ev::LOCK, nullptr);
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

bool Core::wake() {
    active_at = last_input = time(nullptr);  // a key, click or move is input too: autoaway's idle and lock_idle count it
    if (screen_by.empty()) return false;
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
