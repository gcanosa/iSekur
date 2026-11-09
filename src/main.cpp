// iSekur: colorful terminal IRC client with a built-in webchat.
#include <clocale>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <pwd.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <algorithm>
#if __has_include(<execinfo.h>)
#include <execinfo.h>
#endif
#ifdef __linux__
#include <link.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#endif
#include "core.h"
#include "session.h"
#include "tui.h"
#include "web.h"
#include "bnc.h"

static volatile sig_atomic_t g_term = 0;

// A crash (segfault, abort...) appends what it knows to isekur.crash next to the config, then dies as
// it would have (a core dump too, if ulimit -c allows): the signal and the faulting address, the build id
// (which binary: with its symbols, addr2line -Cfpe isekur.debug 0x... resolves the +0x offsets; atos on macOS), the end
// of the log (glibc says "malloc(): corrupted top size" there before it aborts), what the last frame drew and the last
// things that happened (core.h crumb), then the backtrace. Only async-signal-safe calls in the handler; the file is
// opened up front, and the handler runs on its own stack, so a stack overflow gets written too.
static int g_crash_fd = -1;
static char g_crash_head[256], g_build_id[72];
static char g_altstack[1 << 16];
#ifdef __linux__
static int find_build_id(dl_phdr_info* i, size_t, void*) {  // the main program's GNU build-id note
    for (int k = 0; k < i->dlpi_phnum; k++) {
        const ElfW(Phdr)& ph = i->dlpi_phdr[k];
        if (ph.p_type != PT_NOTE) continue;
        const char* p = (const char*)(i->dlpi_addr + ph.p_vaddr);
        const char* end = p + ph.p_memsz;
        while (p + sizeof(ElfW(Nhdr)) <= end) {
            auto* n = (const ElfW(Nhdr)*)p;
            auto* desc = (const unsigned char*)p + sizeof *n + ((n->n_namesz + 3) & ~3u);
            if (n->n_type == NT_GNU_BUILD_ID && n->n_descsz <= 32)
                for (unsigned j = 0; j < n->n_descsz; j++) snprintf(g_build_id + 2 * j, 3, "%02x", desc[j]);
            p = (const char*)desc + ((n->n_descsz + 3) & ~3u);
        }
    }
    return 1;  // the first object is the program
}
#endif
static void on_crash(int sig, siginfo_t* si, void*) {
    if (g_crash_fd >= 0) {
        auto put = [](const char* s) { (void)!write(g_crash_fd, s, strlen(s)); };
        put(g_crash_head);
        char n[160];
        snprintf(n, sizeof n, "signal %d, code %d, address %p, at %lld\nbuild-id %s\n", sig, si ? si->si_code : 0,
                 si ? si->si_addr : nullptr, (long long)time(nullptr), *g_build_id ? g_build_id : "?");  // ponytail: snprintf isn't strictly signal-safe, fine in practice
        put(n);
        struct stat st;  // stderr is the log in the background process
        if (fstat(2, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
            char tail[400];
            off_t from = st.st_size > (off_t)sizeof tail ? st.st_size - (off_t)sizeof tail : 0;
            ssize_t got = pread(2, tail, sizeof tail - 1, from);
            if (got > 0) tail[got] = 0, put("end of the log:\n"), put(tail), put(tail[got - 1] == '\n' ? "" : "\n");
        }
        crumbs_write(g_crash_fd);
        put("backtrace:\n");
#if __has_include(<execinfo.h>)
        void* bt[64];
        backtrace_symbols_fd(bt, backtrace(bt, 64), g_crash_fd);
#endif
        put("\n");
    }
    signal(sig, SIG_DFL);
    raise(sig);
}
static std::string g_crash_path;
static void crash_dumps(const std::string& path) {
    g_crash_fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    g_crash_path = path;
    atexit([] {  // a clean exit with no crash logged leaves no empty isekur.crash behind
        struct stat st;
        if (g_crash_fd >= 0 && fstat(g_crash_fd, &st) == 0 && st.st_size == 0) unlink(g_crash_path.c_str());
    });
    snprintf(g_crash_head, sizeof g_crash_head, "--- %s crashed: ", version_string().c_str());
#ifdef __linux__
    dl_iterate_phdr(find_build_id, nullptr);
#elif defined(__APPLE__)
    auto* mh = (const mach_header_64*)_dyld_get_image_header(0);
    auto* lc = (const load_command*)(mh + 1);
    for (uint32_t i = 0; mh && i < mh->ncmds; i++, lc = (const load_command*)((const char*)lc + lc->cmdsize))
        if (lc->cmd == LC_UUID)
            for (int j = 0; j < 16; j++) snprintf(g_build_id + 2 * j, 3, "%02x", ((const uuid_command*)lc)->uuid[j]);
#endif
#if __has_include(<execinfo.h>)
    void* warm[1];
    backtrace(warm, 1);  // loads libgcc now: the first call may malloc, not allowed in the handler
#endif
    stack_t ss{};
    ss.ss_sp = g_altstack, ss.ss_size = sizeof g_altstack;
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;  // a crash inside the handler ends it, no loop
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL}) sigaction(sig, &sa, nullptr);
}

static int usage() {
    puts("usage: isekur [-c config] [-d|--daemon] [-a|--attach] [-l|--list] [--web|--no-web] [--bnc] [--hash-password] [-v|--version]\n"
         "  -c FILE          config file (default ~/.config/isekur/isekur.conf)\n"
         "  -d, --daemon     run headless in the background; attach with --attach or the webchat\n"
         "  -a, --attach     bring a detached iSekur (Alt+D, /detach, --daemon) back to this terminal;\n"
         "                   with none running, start a new one\n"
         "  -l, --list       the iSekurs running (one per config) and how to attach each\n"
         "  --web, --no-web  start the webchat or not, whatever [web] autostart says\n"
         "  --bnc            run headless in the background as a bouncer only (no terminal, no webchat;\n"
         "                   --web adds it): other IRC clients connect to [bnc]\n"
         "  --hash-password  read a password and print a password_hash value ([web] or [bnc])");
    return 1;
}

static int hash_pw_cmd() {
    termios old{}, quiet{};
    bool tty = tcgetattr(0, &old) == 0;
    if (tty) quiet = old, quiet.c_lflag &= ~(tcflag_t)ECHO, tcsetattr(0, TCSANOW, &quiet);
    fprintf(stderr, "Password: ");
    char buf[512] = "";
    bool got = fgets(buf, sizeof buf, stdin) != nullptr;
    if (tty) tcsetattr(0, TCSANOW, &old), fputc('\n', stderr);
    buf[strcspn(buf, "\r\n")] = 0;
    if (!got || !*buf) return 1;
    printf("password_hash = %s\n", hash_password(buf).c_str());
    return 0;
}

// Background iSekur in a new session, stdio on the log: 1 in it, 0 back in
// the caller when keep_parent (which then attaches to it), -1 if fork failed.
// Done before anything starts a thread: fork() keeps only the calling one,
// and on macOS a child of a threaded process can't use the resolver.
static int daemonize(const std::string& log, bool keep_parent) {
    fflush(stdout), fflush(stderr);
    pid_t p = fork();
    if (p < 0) return -1;
    if (p > 0) {
        if (!keep_parent) _exit(0);
        waitpid(p, nullptr, 0);  // the middle process exits right away
        return 0;
    }
    setsid();
    if ((p = fork()) < 0) _exit(1);
    if (p > 0) _exit(0);
    int in = open("/dev/null", O_RDONLY), out = open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    dup2(in, 0);
    if (out >= 0) dup2(out, 1), dup2(out, 2);
    return 1;
}

// macOS poll() can't wait on terminals (POLLNVAL right away: a busy loop), so
// there it's select(), fine for the few dozen descriptors a client has
static int wait_fds(std::vector<pollfd>& fds, int timeout_ms) {
#ifdef __APPLE__
    fd_set r, w;
    FD_ZERO(&r), FD_ZERO(&w);
    int maxfd = -1;
    for (auto& p : fds) {
        if (p.fd >= FD_SETSIZE) return poll(fds.data(), (nfds_t)fds.size(), timeout_ms);
        if (p.events & POLLIN) FD_SET(p.fd, &r);
        if (p.events & POLLOUT) FD_SET(p.fd, &w);
        maxfd = std::max(maxfd, p.fd);
    }
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    int n = select(maxfd + 1, &r, &w, nullptr, timeout_ms < 0 ? nullptr : &tv);
    for (auto& p : fds)
        p.revents = n <= 0 ? 0 : (short)((FD_ISSET(p.fd, &r) ? POLLIN : 0) | (FD_ISSET(p.fd, &w) ? POLLOUT : 0));
    return n;
#else
    return poll(fds.data(), (nfds_t)fds.size(), timeout_ms);
#endif
}

int main(int argc, char** argv) {
    setlocale(LC_ALL, "");
    signal(SIGPIPE, SIG_IGN);
    bool daemon = false, attach = false, list = false, bnc_mode = false;
    int web_flag = -1;  // --web / --no-web; -1 = [web] autostart decides
    Core c;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-c" && i + 1 < argc) c.cfg_path = argv[++i];
        else if (a == "-d" || a == "--daemon") daemon = true;
        else if (a == "-a" || a == "--attach") attach = true;
        else if (a == "-l" || a == "--list") list = true;
        else if (a == "--bnc") bnc_mode = daemon = true;
        else if (a == "--web") web_flag = 1;
        else if (a == "--no-web") web_flag = 0;
        else if (a == "--hash-password") return hash_pw_cmd();
        else if (a == "-v" || a == "--version") {
            puts(version_string().c_str());
            puts("A small, very colorful terminal IRC client with a built-in webchat\n"
                 "Author:  Gerardo Canosa (github.com/gcanosa)\n"
                 "Source:  https://github.com/gcanosa/iSekur\n"
                 "Issues:  https://github.com/gcanosa/iSekur/issues\n"
                 "IRC:     #iSekur on irc.sekurnet.org\n"
                 "License: GPL-3.0-or-later");
            return 0;
        }
        else return usage();
    }
    bool own_cfg = !c.cfg_path.empty();
    if (c.cfg_path.empty()) c.cfg_path = default_config_path();
    if (list) {  // the default config's folder, and -c's
        auto dir = [](const std::string& p) { return p.find('/') == std::string::npos ? std::string(".") : p.substr(0, p.rfind('/')); };
        return list_main({dir(default_config_path()), dir(c.cfg_path)}, default_config_path());
    }
    std::string how = std::string("isekur --attach") + (own_cfg ? " -c " + c.cfg_path : "");
    std::string sock = sock_path_for(c.cfg_path);
    // --attach with nothing running starts one, like plain isekur, and says it's new (below)

    // One iSekur per config (a second one would join the same channels
    // twice): starting it again attaches to the one that runs.
    Session ses;
    std::string err;
    int ls = ses.listen(sock, err);
    if (ls < 0 && daemon) {
        fprintf(stderr, "iSekur is already running with %s. Attach to it: %s\n", c.cfg_path.c_str(), how.c_str());
        return 1;
    }
    if (ls < 0) return attach_main(sock, how);
    if (bnc_mode) {  // say what's missing here, not in the log once in the background
        Ini pre;
        ini_load(c.cfg_path, pre);
        if (ini_get(pre, "bnc", "user").empty() || ini_get(pre, "bnc", "password_hash").rfind("pbkdf2$", 0) != 0) {
            fprintf(stderr, "--bnc needs [bnc] user and password_hash in %s (isekur --hash-password makes one,\n"
                            "or /set bnc.user and /bnc password in iSekur)\n", c.cfg_path.c_str());
            return 1;
        }
    }
    // Like tmux: the IRC side runs in the background from the start and this
    // process lends it the terminal, so detaching (Alt+D, a closed window, a
    // dropped SSH) only ends this side. Without the socket (`local`), the
    // terminal is driven here and there is no detaching.
    std::string log = cfg_side(c.cfg_path, "log");  // isekur.conf: isekur.log
    bool local = !daemon && ls == 0;
    if (!local) {
        int d = daemonize(log, !daemon);
        if (d < 0) { perror("fork"); return 1; }
        if (d == 0) return attach_main(sock, how);
        signal(SIGHUP, SIG_IGN);
    }
    crash_dumps(cfg_side(c.cfg_path, "crash"));
    signal(SIGTERM, [](int) { g_term = 1; });
    signal(SIGINT, [](int) { g_term = 1; });

    c.daemon = !local && !ses.listening();  // the webchat is the only way back in
    c.load_config();
    Buffer* home = c.bufs.front().get();
    if (ls == 0) c.print(home, LK::ERROR, "Detaching is not available: " + err);
    if (attach && !daemon)
        c.print(home, LK::INFO, "\x03" "01,08" "\x02" " NEW SESSION " "\x0f" " Nothing was running to attach to: this is a fresh start, not a recovered session");
    Web web(c);
    // --daemon always starts the webchat; it can also be attached from a terminal
    bool web_on = bnc_mode ? web_flag == 1 : daemon || (web_flag < 0 ? Web::autostart(c.cfg) : web_flag == 1);
    if (web_on) {
        if (!web.start(err)) {
            c.print(home, LK::ERROR, "Webchat not started: " + err);
            if (c.daemon && !bnc_mode) { fprintf(stderr, "daemon mode needs the webchat: %s\n", err.c_str()); return 1; }
        } else c.info_note(home, "Webchat on " + web.url);
    } else if (!bnc_mode) c.info_note(home, "Webchat off: /web start, or /set web.autostart on");
    c.web_url = web.url;
    Bnc bnc(c);
    if (bnc_mode || Bnc::autostart(c.cfg)) {
        if (!bnc.start(err)) {
            c.print(home, LK::ERROR, "BNC not started: " + err);
            if (bnc_mode) { fprintf(stderr, "--bnc: %s\n", err.c_str()); return 1; }
        } else c.info_note(home, "BNC on " + bnc.url);
    } else c.info_note(home, "BNC off: /bnc start, or /set bnc.autostart on");
    c.fifo_open();  // [general] fifo: a named pipe (not on native Windows)
    c.listener = [&](Ev e, Buffer* b, const Line* l) { web.on_event(e, b, l); };

    Tui tui(c);
    if (local && !tui.init(err)) { fprintf(stderr, "terminal: %s\n", err.c_str()); return 1; }
    c.tty = local;
    if (local && c.lock_on()) c.set_lock(true);
    bool hinted = false;
    time_t tty_on = 0, tty_off = 0;  // the terminal's last attach and detach

    // banner_anim: a light sweeps the banner (home's first 6 lines) once, ~1.2s,
    // when a terminal first shows it (usually an attach a moment after start); not in banner_mono
    long long anim0 = 0, screen_drawn = 0;
    bool anim_due = c.opt_on(nullptr, "banner_anim", true) && !c.opt_on(nullptr, "banner_mono", false);
    time_t last_sec = 0;
    std::vector<pollfd> fds;
    std::vector<Server*> polled;
    // nothing in a stage may take iSekur down (and every IRC connection with it): an exception is said in the console
    auto guard = [&](const char* stage, auto&& fn) {
        crumb_stage(stage);
        try { fn(); } catch (const std::exception& e) { c.print(home, LK::ERROR, std::string("Internal error in ") + stage + ": " + e.what()); }
    };
    while (!c.quit && !g_term) {
        fds.clear(), polled.clear();
        if (c.tty) {
            int tty = -1, rsz = -1;
            tui.fds(tty, rsz);
            fds.push_back({tty, POLLIN, 0});
            fds.push_back({rsz, POLLIN, 0});
        }
        size_t srv0 = fds.size();
        for (auto& s : c.servers)
            if (s->conn.fd >= 0) fds.push_back({s->conn.fd, s->conn.events(), 0}), polled.push_back(s.get());
        web.add_fds(fds);
        bnc.add_fds(fds);
        ses.add_fds(fds);
        size_t fifo0 = fds.size();
        if (c.fifo_fd >= 0) fds.push_back({c.fifo_fd, POLLIN, 0});
        size_t job0 = fds.size();
        for (auto& j : c.jobs)
            if (j.fd >= 0) fds.push_back({j.fd, POLLIN, 0});
        size_t term0 = fds.size();
        for (auto& b : c.bufs)
            if (b->term && b->term->fd >= 0) fds.push_back({b->term->fd, POLLIN, 0});
        size_t dcc0 = fds.size();
        for (auto& d : c.dccs)
            if (d->conn.fd >= 0) fds.push_back({d->conn.fd, d->conn.events(), 0});
            else if (d->lfd >= 0) fds.push_back({d->lfd, POLLIN, 0});

        Buffer* about = c.tty && c.active && !c.active->srv && c.active->name == "*about" && c.active->lines.size() >= 6 ? c.active : nullptr;
        bool typing = c.tty && now_ms() < c.typing_until;  // a line is typing itself out: redraw ~30 times a second
        int wait = c.drain();
        if (anim0 || typing || (c.tty && c.lock_anim)) wait = std::min(wait, 33);
        if (!c.screen_by.empty()) wait = std::min(wait, 1000 / c.screen_fps);
        if (c.flash_until) wait = std::min(wait, (int)std::max(0LL, c.flash_until - now_ms()));
        if (about) {  // ~30 frames a second while the light sweeps or a line types, else asleep until the next sweep
            long long ph = (now_ms() - c.about_t0) % 3500;
            wait = std::min(wait, ph < 1200 || !c.about_typed.empty() ? 33 : (int)(3500 - ph));
        }
        if (wait_fds(fds, wait) < 0 && errno != EINTR) break;
        if (typing) c.dirty = true;
        if (c.flash_until && now_ms() >= c.flash_until) c.flash.clear(), c.flash_until = 0, c.dirty = true;
        guard("shell tabs", [&] { c.terms_poll(fds, term0); });  // first: nothing has closed a tab yet, so the order still holds
        guard("DCC", [&] { c.dccs_poll(fds, dcc0); });
        // servers first: their handlers never delete servers, so `polled` stays valid
        crumb_stage("IRC servers");
        for (size_t i = 0; i < polled.size(); i++)
            if (fds[srv0 + i].revents) c.on_readable(polled[i], fds[srv0 + i].revents);
        // the attach process quit (closed window, dropped SSH) or the terminal hung up
        bool hung = !local && (ses.lost || (c.tty && (fds[0].revents & (POLLHUP | POLLERR))));
        if (c.tty && !hung && (fds[0].revents || fds[1].revents)) guard("keyboard input", [&] { tui.on_readable(); });
        crumb_stage("webchat"), web.process(fds);
        crumb_stage("BNC"), bnc.process(fds);
        guard("FIFO", [&] { c.fifo_poll(fds, fifo0); });
        guard("/exec", [&] { c.jobs_poll(fds, job0); });
        crumb_stage("session");
        int t = ses.process(fds, c.tty);
        // the audit lines: who attached from where, for how long, and who was turned away
        if (!ses.refused.empty())
            c.print(home, LK::ERROR, "\u2715 Refused attach: " + ses.refused + ", a terminal is already attached"), ses.refused.clear();
        if (c.detach_req && local) c.print(c.active, LK::ERROR, "Detaching is not available: " + err);
        else if (c.tty && (hung || c.detach_req)) {
            tui.shutdown();
            ses.release('D');
            c.tty = false;
            tty_off = time(nullptr);
            crumb("tty", hung ? "gone" : "detach");
            c.print(home, LK::INFO, std::string(hung ? "\u25c0 Terminal gone: " : "\u25c0 Detached: ") + ses.who + ", attached " +
                                        fmt_dur((long)(tty_off - tty_on)) + "; " + how + " brings it back");
        }
        c.detach_req = false;
        if (t >= 0) {
            if (!tui.init(err, t)) {
                c.print(home, LK::ERROR, "Attach failed: " + err), ses.release('F');
                fprintf(stderr, "attach failed: %s\n", err.c_str());
            }
            else {
                c.tty = c.dirty = true;
                crumb("tty", "attach");
                tty_on = time(nullptr);
                const passwd* pw = getpwuid(geteuid());  // always us: the socket takes no one else
                c.print(home, LK::INFO, "\u25b6 Attached: " + std::string(pw ? pw->pw_name : "?") + ", " + ses.who +
                                            (tty_off ? ", after " + fmt_dur((long)(tty_on - tty_off)) + " away" : ""));
                if (c.lock_on()) c.set_lock(true);  // every attach asks again (the webchat too)
                if (!hinted) c.print(home, LK::INFO, "Alt+D (Option+D on a Mac) or /detach leaves iSekur running in the background"), hinted = true;
            }
        }
        crumb_stage("webchat out"), web.flush();
        if (c.web_req != Core::WebReq::NONE) {
            Buffer* to = c.index_of(c.web_req_buf) >= 0 ? c.web_req_buf : c.bufs.front().get();
            if (c.web_req == Core::WebReq::RESTART && web.running()) web.stop(), c.web_req = Core::WebReq::START;
            if (c.web_req == Core::WebReq::STOP && web.running()) {
                web.stop();
                c.info_note(to, "Webchat stopped");
            } else if (c.web_req == Core::WebReq::STOP) {
                c.info_note(to, "Webchat is not running");
            } else if (web.running()) {
                c.info_note(to, "Webchat already on " + web.url);
            } else if (!web.start(err)) {
                c.print(to, LK::ERROR, "Webchat not started: " + err);
            } else {
                c.info_note(to, "Webchat on " + web.url);
            }
            c.web_req = Core::WebReq::NONE, c.web_url = web.url;
        }
        if (c.bnc_req != Core::BncReq::NONE) {
            Buffer* to = c.index_of(c.bnc_req_buf) >= 0 ? c.bnc_req_buf : c.bufs.front().get();
            auto rq = c.bnc_req;
            c.bnc_req = Core::BncReq::NONE;
            if (rq == Core::BncReq::RESTART && bnc.running()) bnc.stop(), rq = Core::BncReq::START;
            if (rq == Core::BncReq::STATUS)
                for (auto& l : bnc.status()) c.info_note(to, l);
            else if (rq == Core::BncReq::STOP) c.info_note(to, bnc.running() ? (bnc.stop(), "BNC stopped") : "BNC is not running");
            else if (bnc.running()) c.info_note(to, "BNC already on " + bnc.url);
            else if (!bnc.start(err)) c.print(to, LK::ERROR, "BNC not started: " + err);
            else c.info_note(to, "BNC on " + bnc.url);
        }
        c.drain();

        time_t now = time(nullptr);
        c.set_attached(c.tty || web.attached() || bnc.attached());
        if (bool ui = c.tty || web.attached(); ui != c.ui_on) {  // someone sits down: idle starts over; nobody left: no screensaver
            c.ui_on = ui;
            if (ui) c.active_at = now;
            else c.screen_end();
        }
        if (now != last_sec) last_sec = now, c.dirty = true, guard("the clock tick", [&] { c.tick(now); });  // status bar clock, uptime seconds; pings, reconnects, away: all by the second
        if (anim_due && c.tty && !c.locked) anim_due = false, anim0 = home->lines.size() >= 6 ? now_ms() : 0;
        if (!c.screen_by.empty() && c.tty && now_ms() - screen_drawn >= 1000 / c.screen_fps) screen_drawn = now_ms(), c.dirty = true;
        if (c.lock_anim) {  // the lock screen's animations: 900ms at most (tui.cpp draw_lock)
            if (now_ms() - c.lock_anim_at < 900) c.dirty = true;
            else c.lock_anim = 0, c.dirty = true;
        }
        if (anim0) {
            double t = (now_ms() - anim0) / 1200.0;  // 0..1 over the sweep
            bool done = t >= 1 || !c.tty || home->lines.size() < 6;
            auto art = banner_lines(c.theme, false, done ? -99 : -10 + t * 64);
            for (size_t i = 0; i < 6 && i < home->lines.size(); i++) home->lines[i].text = art[i];
            if (done) anim0 = 0;
            c.dirty = true;
        }
        if (about && !c.about_typed.empty()) {  // type the lines out in turn, ~30 a second, a ▌ cursor ahead
            long long t = now_ms() - c.about_t0 - 400;  // after the first sweep has started
            bool all = true;
            for (int idx : c.about_typed) {
                if (idx >= (int)about->lines.size() || idx >= (int)c.about_full.size()) continue;
                const std::string& full = c.about_full[(size_t)idx];
                size_t len = mirc_strip(full).size(), n = t <= 0 ? 0 : (size_t)(t / 33);  // ponytail: byte length, the typed lines are ASCII
                std::string want = n >= len ? full : mirc_prefix(full, n) + "\x0f\u258C";
                if (about->lines[(size_t)idx].text != want) about->lines[(size_t)idx].text = want, c.dirty = true;
                if (n < len) { all = false; break; }  // the next line waits for this one
                t -= (long long)len * 33 + 300;
            }
            if (all) c.about_typed.clear();
        }
        if (about) {  // the light crosses the banner every 3.5s, for as long as the tab is shown
            long long ph = (now_ms() - c.about_t0) % 3500;
            static bool lit = false;
            if (ph < 1200 || lit) {
                auto art = banner_lines(c.theme, false, ph < 1200 ? -10 + ph / 1200.0 * 64 : -99);
                for (size_t i = 0; i < 6; i++) about->lines[i].text = art[i];
                lit = ph < 1200, c.dirty = true;
            }
        }
        if (c.tty && c.dirty) guard("drawing", [&] { tui.draw(); });
    }

    // flush QUITs for up to a second
    for (auto& s : c.servers)
        if (s->state != SS::DISCONNECTED && !s->user_quit) s->send_now("QUIT :" + (c.quit_msg.empty() ? c.bye(s.get(), "quit_message") : c.quit_msg));
    long long until = now_ms() + 1000;
    while (now_ms() < until) {
        fds.clear(), polled.clear();
        for (auto& s : c.servers)
            if (s->conn.st == Conn::OPEN && !s->conn.out.empty())
                fds.push_back({s->conn.fd, POLLOUT, 0}), polled.push_back(s.get());
        if (fds.empty()) break;
        poll(fds.data(), (nfds_t)fds.size(), 100);
        for (size_t i = 0; i < polled.size(); i++)
            if (fds[i].revents) polled[i]->conn.pump(fds[i].revents, polled[i]->inbuf);
    }
    c.save_history();
    c.fifo_close();
    if (c.tty) tui.shutdown();
    ses.release('Q');
    ses.close();
    return 0;
}
