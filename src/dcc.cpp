// /dcc: files sent with DCC SEND, both ways, in the background. A file offered
// to us waits for /dcc get, then goes into dcc_dir; one we offer (/dcc send)
// waits on a port for them to connect. RESUME/ACCEPT continue a broken one.
// Passive DCC (the receiver listens) isn't taken yet.
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <sys/stat.h>
#include <unistd.h>
#include "core.h"

static const size_t MAX_OFFERS = 20;    // waiting at once: an offer flood can't grow without end
static const time_t STALL_SECS = 120;   // a transfer with no progress this long is dropped
static const time_t WAIT_SECS = 300;    // our offer, not taken
static const time_t RESUME_SECS = 30;   // no ACCEPT: download it again from the start
static const size_t QUEUE = 1 << 18;    // read ahead into the socket, and the most read from it at once

Dcc::~Dcc() {
    if (f) fclose(f);
    if (lfd >= 0) close(lfd);
}

// 512M, 3.2G, 16G, 1.5T: the unit that keeps it under 1024, one decimal below 10
std::string human_bytes(unsigned long long n) {
    const char* u = "BKMGTP";
    double v = (double)n;
    while (v >= 1024 && u[1]) v /= 1024, u++;
    char buf[32];
    snprintf(buf, sizeof buf, v < 10 && *u != 'B' ? "%.1f%c" : "%.0f%c", v, *u);
    std::string s = buf;
    if (s.size() > 3 && s.compare(s.size() - 3, 2, ".0") == 0) s.erase(s.size() - 3, 2);  // 16.0G -> 16G
    return s;
}

std::string Core::dcc_dir(const Server* s) const {
    std::string dir = opt(s, "dcc_dir", "~/Downloads");
    if (dir.rfind("~/", 0) == 0)
        if (const char* h = getenv("HOME")) dir = h + dir.substr(1);
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    return dir;
}

// a name from the network: no directories, not hidden, no control codes (mIRC colors included)
static std::string clean_file(std::string n) {
    size_t sl = n.find_last_of("/\\");
    if (sl != std::string::npos) n.erase(0, sl + 1);
    for (auto& ch : n)
        if ((unsigned char)ch < 0x20 || ch == 0x7f) ch = '_';
    n.erase(0, std::min(n.find_first_not_of(". "), n.size()));
    if (n.size() > 200) n.erase(0, n.size() - 200);  // the end keeps the extension
    return n.empty() ? "file" : n;
}

static std::string quoted(const std::string& f) { return f.find(' ') != std::string::npos ? "\"" + f + "\"" : f; }

// `"file name" a b c` or `file a b c`: the file, then the words after it
static bool split_file(const std::string& r, std::string& file, std::vector<std::string>& w) {
    std::string rest;
    if (!r.empty() && r[0] == '"') {
        size_t q = r.find('"', 1);
        if (q == std::string::npos) return false;
        file = r.substr(1, q - 1), rest = r.substr(q + 1);
    } else {
        size_t sp = r.find(' ');
        file = r.substr(0, sp), rest = sp == std::string::npos ? "" : r.substr(sp + 1);
    }
    for (auto& x : split(rest, ' '))
        if (!x.empty()) w.push_back(x);
    return !file.empty();
}

static bool digits(const std::string& s) { return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos; }

// where a transfer's news goes: the tab you're in if it's that network's, else its server tab
static Buffer* where(Core& c, Server* s) { return c.active && c.active->srv == s ? c.active : s->status; }

static std::string of_size(const Dcc* d) { return d->size ? human_bytes(d->size) : "size not given"; }
static std::string tag(const Dcc* d) { return "DCC #" + std::to_string(d->id) + ": "; }

// something you'd want to know about a transfer: its line, the bell, and a popup in the webchat
static void note(Core& c, Dcc* d, LK k, const std::string& title, const std::string& text) {
    Buffer* b = where(c, d->srv);
    c.print(b, k, text);
    c.ring(d->srv);
    Line l{time(nullptr), k, title, text};
    c.notify(Ev::NOTE, b, &l);
}

static void finish(Core& c, Dcc* d, const std::string& err) {
    d->conn.close();
    if (d->f) fclose(d->f), d->f = nullptr;
    if (d->lfd >= 0) ::close(d->lfd), d->lfd = -1;
    d->st = err.empty() ? Dcc::DONE : Dcc::FAILED;
    d->err = err;
    d->ended = time(nullptr);
    long secs = std::max<long>(1, (long)(time(nullptr) - d->at));
    std::string moved = human_bytes(d->got - d->pos) + " in " + std::to_string(secs) + "s (" +
                        human_bytes((d->got - d->pos) / (unsigned long long)secs) + "/s)";
    if (err.empty() && d->send)
        note(c, d, LK::INFO, "DCC sent", tag(d) + "sent " + d->file + " to " + d->nick + ", " + moved);
    else if (err.empty())
        note(c, d, LK::INFO, "DCC complete", tag(d) + "got " + d->file + " from " + d->nick + ", " + moved + ", saved as " + d->path);
    else
        note(c, d, LK::ERROR, err == "cancelled" ? "DCC stopped" : "DCC failed", tag(d) + d->file + (d->send ? " to " : " from ") + d->nick + ": " + err + " after " +
                                                 human_bytes(d->got) + (d->size ? " of " + human_bytes(d->size) : "") +
                                                 (!d->send && d->got ? " (the part stays in " + d->path + ", /dcc get resumes it)" : ""));
    if (!d->send && !d->got && !d->path.empty()) unlink(d->path.c_str());
    c.dirty = true;
}

// connect to them and take the file, appending from d->pos
static void receive(Core& c, Dcc* d) {
    d->f = fopen(d->path.c_str(), d->pos ? "ab" : "wb");
    if (!d->f) { finish(c, d, "can't write " + d->path + ": " + strerror(errno)); return; }
    d->st = Dcc::RUN;
    d->got = d->rate_got = d->pos;
    d->at = d->last_rx = time(nullptr);
    d->conn.in_cap = QUEUE;  // a fast sender can't keep us reading past the next poll
    if (!d->conn.connect(d->host, d->port, 0, "", "")) finish(c, d, d->conn.err);
}

// SEND file ip port [size] [token]; RESUME file port pos (they take ours from pos); ACCEPT file port pos (they let us resume)
void Core::dcc_ctcp(Server* s, const std::string& nick, const std::string& arg) {
    std::string type = arg.substr(0, arg.find(' '));
    for (auto& ch : type) ch = (char)toupper((unsigned char)ch);
    std::string file;
    std::vector<std::string> w;
    bool ok = arg.size() > type.size() && split_file(arg.substr(type.size() + 1), file, w);
    auto find = [&](Dcc::St st) -> Dcc* {
        for (auto& d : dccs)
            if (d->srv == s && d->st == st && irc_lower(d->nick) == irc_lower(nick) && w.size() > 1 &&
                std::to_string(d->port) == w[0])
                return d.get();
        return nullptr;
    };
    if (type == "RESUME" || type == "ACCEPT") {
        Dcc* d = ok ? find(type == "RESUME" ? Dcc::WAIT : Dcc::RESUME) : nullptr;
        unsigned long long pos = d && digits(w[1]) ? strtoull(w[1].c_str(), nullptr, 10) : 0;
        if (!d || !pos || (d->size && pos >= d->size)) {
            print(where(*this, s), LK::INFO, "DCC " + type + " from " + nick + " for nothing we know of: " + arg);
            return;
        }
        if (type == "RESUME") {
            d->pos = pos;
            s->send("PRIVMSG " + nick + " :\x01" "DCC ACCEPT " + quoted(file) + " " + w[0] + " " + w[1] + "\x01");
            print(where(*this, s), LK::INFO, tag(d) + nick + " resumes " + d->file + " at " + human_bytes(pos));
        } else {
            d->pos = pos;  // what they accept, normally what we asked
            receive(*this, d);
        }
        return;
    }
    if (type != "SEND") {
        print(where(*this, s), LK::INFO, "DCC " + type + " from " + nick + " (iSekur only takes files, DCC SEND)");
        return;
    }
    int port = ok && w.size() > 1 && digits(w[1]) ? atoi(w[1].c_str()) : -1;
    if (port < 0 || port > 65535) {
        print(where(*this, s), LK::ERROR, "Bad DCC SEND from " + nick + ": " + arg);
        return;
    }
    file = clean_file(file);
    if (port == 0) {
        print(where(*this, s), LK::INFO, nick + " offers " + file + " with passive DCC, which iSekur can't take yet: ask for a normal DCC SEND");
        return;
    }
    size_t waiting = std::count_if(dccs.begin(), dccs.end(), [](auto& d) { return d->st == Dcc::OFFER; });
    if (waiting >= MAX_OFFERS) {
        print(where(*this, s), LK::ERROR, "DCC offer of " + file + " from " + nick + " ignored: " +
                                              std::to_string(MAX_OFFERS) + " offers are already waiting (/dcc close them)");
        return;
    }
    std::string ip = w[0];
    if (digits(ip)) {
        unsigned long v = strtoul(ip.c_str(), nullptr, 10);
        ip = std::to_string(v >> 24 & 255) + "." + std::to_string(v >> 16 & 255) + "." + std::to_string(v >> 8 & 255) + "." +
             std::to_string(v & 255);
    }
    auto d = std::make_unique<Dcc>();
    d->id = dcc_next++, d->srv = s, d->nick = nick, d->file = file, d->host = ip, d->port = port;
    d->size = w.size() > 2 ? strtoull(w[2].c_str(), nullptr, 10) : 0;
    d->at = time(nullptr);
    std::string id = std::to_string(d->id);
    note(*this, d.get(), LK::INFO, "DCC incoming", nick + " offers " + file + " (" + of_size(d.get()) + ", from " +
                                                     (ip.find(':') != std::string::npos ? "[" + ip + "]" : ip) + ":" + w[1] +
                                                     "): /dcc get " + id + " downloads it, /dcc close " + id + " refuses it");
    dccs.push_back(std::move(d));
}

// into dcc_dir, never over another file: "a.zip", "a (1).zip", ... A smaller one with
// the name is taken as a broken download of it and resumed, unless `fresh`
static void start(Core& c, Buffer* b, Dcc* d, bool fresh) {
    std::string dir = c.dcc_dir(d->srv);
    mkdir_p(dir);
    std::string base = d->file, ext;
    size_t dot = base.rfind('.');
    if (dot != std::string::npos && dot > 0) ext = base.substr(dot), base.erase(dot);
    d->pos = 0;
    int fd = -1;
    for (int n = 0; n < 1000 && fd < 0; n++) {
        d->path = dir + "/" + base + (n ? " (" + std::to_string(n) + ")" : "") + ext;
        struct stat st;
        if (!fresh && d->size && lstat(d->path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 &&
            (unsigned long long)st.st_size < d->size) {
            d->pos = (unsigned long long)st.st_size;
            break;
        }
        fd = open(d->path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0 && errno != EEXIST) break;
    }
    if (d->pos) {
        d->st = Dcc::RESUME;
        d->at = time(nullptr);
        d->srv->send("PRIVMSG " + d->nick + " :\x01" "DCC RESUME " + quoted(d->file) + " " + std::to_string(d->port) + " " +
                     std::to_string(d->pos) + "\x01");
        c.print(b, LK::INFO, tag(d) + "resuming " + d->path + " at " + human_bytes(d->pos) + " of " + human_bytes(d->size) +
                                 ", asking " + d->nick);
        return;
    }
    if (fd < 0) {
        c.print(b, LK::ERROR, tag(d) + "can't write " + d->path + ": " + strerror(errno) + " (dcc_dir sets the folder)");
        d->path.clear();
        return;
    }
    close(fd);
    c.print(b, LK::INFO, tag(d) + "downloading " + d->file + " (" + of_size(d) + ") from " + d->nick + " into " + d->path);
    receive(c, d);
}

static bool private_ip(const std::string& ip) {
    return ip.rfind("10.", 0) == 0 || ip.rfind("192.168.", 0) == 0 || ip.rfind("127.", 0) == 0 ||
           (ip.rfind("172.", 0) == 0 && atoi(ip.c_str() + 4) >= 16 && atoi(ip.c_str() + 4) <= 31) ||
           ip.rfind("fe80:", 0) == 0 || ip.rfind("fd", 0) == 0 || ip == "::1";
}

// a port to offer a file on: dcc_ports ("5000-5010", default any), on the address the
// IRC connection uses unless dcc_ip says another (behind a router: the public one)
static int dcc_listen(Core& c, Server* s, std::string& ip, int& port, std::string& err) {
    ip = c.opt(s, "dcc_ip", "");
    if (ip.empty()) {
        sockaddr_storage ss;
        socklen_t l = sizeof ss;
        char h[NI_MAXHOST] = "";
        if (getsockname(s->conn.fd, (sockaddr*)&ss, &l) == 0) getnameinfo((sockaddr*)&ss, l, h, sizeof h, nullptr, 0, NI_NUMERICHOST);
        ip = h;
        if (ip.rfind("::ffff:", 0) == 0) ip.erase(0, 7);
    }
    if (ip.empty()) { err = "no address to offer (set dcc_ip)"; return -1; }
    std::string ports = c.opt(s, "dcc_ports", ""), bind = ip.find(':') != std::string::npos ? "::" : "0.0.0.0";
    int lo = ports.empty() ? 0 : atoi(ports.c_str()), hi = lo;
    if (ports.find('-') != std::string::npos) hi = atoi(ports.c_str() + ports.find('-') + 1);
    for (int p = lo; p <= hi && p <= 65535; p++) {
        int fd = listen_tcp(bind, p, err);
        if (fd < 0) continue;
        sockaddr_storage ss;
        socklen_t l = sizeof ss;
        getsockname(fd, (sockaddr*)&ss, &l);
        port = ntohs(ss.ss_family == AF_INET6 ? ((sockaddr_in6*)&ss)->sin6_port : ((sockaddr_in*)&ss)->sin_port);
        return fd;
    }
    if (ports.size()) err = "no free port in dcc_ports " + ports + (err.empty() ? "" : ": " + err);
    return -1;
}

static void offer(Core& c, Buffer* b, const std::string& nick, const std::string& what) {
    Server* s = b->srv;
    if (!s || s->state != SS::CONNECTED) { c.print(b, LK::ERROR, "/dcc send: not connected here"); return; }
    std::string dir = c.dcc_dir(s), path = what;
    bool bare = what.find('/') == std::string::npos;  // a name in dcc_dir
    // from the webchat only what's in dcc_dir: it must not reach every file this user can read
    if (c.from_web && !bare) { c.print(b, LK::ERROR, "From the webchat /dcc send takes a file in " + dir + " only"); return; }
    if (bare) path = dir + "/" + what;
    else if (path.rfind("~/", 0) == 0)
        if (const char* h = getenv("HOME")) path = h + path.substr(1);
    struct stat st;
    if (stat(path.c_str(), &st) || !S_ISREG(st.st_mode)) {
        c.print(b, LK::ERROR, "/dcc send: no file " + path + (bare ? " (a name alone is looked for in dcc_dir)" : ""));
        return;
    }
    if (access(path.c_str(), R_OK)) { c.print(b, LK::ERROR, "/dcc send: can't read " + path + ": " + strerror(errno)); return; }
    std::string ip, err;
    int port = 0;
    int lfd = dcc_listen(c, s, ip, port, err);
    if (lfd < 0) { c.print(b, LK::ERROR, "/dcc send: " + err); return; }
    auto d = std::make_unique<Dcc>();
    d->id = c.dcc_next++, d->srv = s, d->nick = nick, d->path = path, d->send = true, d->lfd = lfd, d->port = port;
    d->file = path.substr(path.rfind('/') + 1), d->size = (unsigned long long)st.st_size;
    d->st = Dcc::WAIT, d->at = time(nullptr);
    std::string adv = ip;  // IPv4 goes as one 32-bit number
    unsigned a1, a2, a3, a4;
    if (ip.find(':') == std::string::npos && sscanf(ip.c_str(), "%u.%u.%u.%u", &a1, &a2, &a3, &a4) == 4)
        adv = std::to_string(((unsigned long long)a1 << 24) | (a2 << 16) | (a3 << 8) | a4);
    s->send("PRIVMSG " + nick + " :\x01" "DCC SEND " + quoted(d->file) + " " + adv + " " + std::to_string(port) + " " +
            std::to_string(d->size) + "\x01");
    c.print(b, LK::INFO, tag(d.get()) + "offering " + d->file + " (" + human_bytes(d->size) + ") to " + nick + " on " +
                             (ip.find(':') != std::string::npos ? "[" + ip + "]" : ip) + ":" + std::to_string(port) +
                             ", waiting " + std::to_string(WAIT_SECS / 60) + " minutes for them to take it");
    if (private_ip(ip) && c.opt(s, "dcc_ip", "").empty())
        c.print(b, LK::INFO, "That's a private address: from outside your network set dcc_ip to your public one and forward dcc_ports to this machine");
    c.dccs.push_back(std::move(d));
}

void Core::dcc_cmd(Buffer* b, const std::string& args) {
    std::vector<std::string> a;
    for (auto& x : split(args, ' '))
        if (!x.empty()) a.push_back(x);
    std::string sub = a.empty() ? "list" : a[0];
    // by number, or the newest from a nick (on this network), or the newest of all
    auto pick = [&](bool offers) -> Dcc* {
        Dcc* f = nullptr;
        for (auto& d : dccs)
            if ((!offers || d->st == Dcc::OFFER) &&
                (a.size() < 2 || std::to_string(d->id) == a[1] || (d->srv == b->srv && irc_lower(d->nick) == irc_lower(a[1]))))
                f = d.get();
        return f;
    };
    if (sub == "list") {
        if (dccs.empty()) print(b, LK::INFO, "No DCC transfers; files sent to you show up here first, /dcc send nick file offers one");
        for (auto& d : dccs) {
            std::string id = std::to_string(d->id), st;
            switch (d->st) {
            case Dcc::OFFER: st = "offered, /dcc get " + id; break;
            case Dcc::RESUME: st = "asked to resume at " + human_bytes(d->pos); break;
            case Dcc::WAIT: st = "waiting for them to take it"; break;
            case Dcc::DONE: st = "done" + (d->send ? "" : ", " + d->path); break;
            case Dcc::FAILED: st = d->err; break;
            case Dcc::RUN:
                st = human_bytes(d->got) + (d->size ? " of " + human_bytes(d->size) + " (" + std::to_string(d->got * 100 / d->size) + "%)" : "") +
                     ", " + human_bytes(d->rate) + "/s";
            }
            print(b, LK::INFO, "#" + id + (d->send ? " ⇡ " : " ⇣ ") + d->file + " (" + of_size(d.get()) + ") " +
                                   (d->send ? "to " : "from ") + d->nick + ": " + st);
        }
        return;
    }
    if (sub == "get") {
        Dcc* d = pick(true);
        if (!d) print(b, LK::ERROR, "No DCC offer" + (a.size() > 1 ? " " + a[1] : "") + " waiting (/dcc list)");
        else start(*this, b, d, false);
        return;
    }
    if (sub == "send" && a.size() > 2) { offer(*this, b, a[1], after_words(args, 2)); return; }
    if (sub == "close" && a.size() > 1) {
        Dcc* d = pick(false);
        if (!d) { print(b, LK::ERROR, "No DCC " + a[1] + " (/dcc list)"); return; }
        if (d->st == Dcc::RUN || d->st == Dcc::WAIT || d->st == Dcc::RESUME) { finish(*this, d, "cancelled"); return; }
        print(b, LK::INFO, tag(d) + d->file + (d->st == Dcc::OFFER ? " refused" : " taken off the list"));
        dccs.erase(std::find_if(dccs.begin(), dccs.end(), [&](auto& p) { return p.get() == d; }));
        dirty = true;
        return;
    }
    print(b, LK::ERROR, "Usage: /dcc [list] | get [number|nick] | send nick file | close number|nick");
}

// keep QUEUE bytes of the file waiting in the socket's buffer
static void send_more(Dcc* d) {
    char buf[65536];
    while (d->f && d->conn.out.size() < QUEUE) {
        size_t n = fread(buf, 1, sizeof buf, d->f);
        if (!n) { fclose(d->f), d->f = nullptr; break; }
        d->conn.out.append(buf, n), d->queued += n;
    }
}

// matched by fd, not position: /dcc close may have run since the fds were collected
void Core::dccs_poll(const std::vector<pollfd>& fds, size_t from) {
    auto rev_of = [&](int fd) -> short {
        for (size_t i = from; fd >= 0 && i < fds.size(); i++)
            if (fds[i].fd == fd) return fds[i].revents;
        return 0;
    };
    time_t now = time(nullptr);
    for (auto& dp : dccs) {
        Dcc* d = dp.get();
        if (d->st == Dcc::WAIT) {  // they connect: send from pos (0, or where their RESUME said)
            if (!(rev_of(d->lfd) & POLLIN) || !d->conn.accept_from(d->lfd, nullptr)) continue;
            ::close(d->lfd), d->lfd = -1;
            d->f = fopen(d->path.c_str(), "rb");
            if (!d->f || fseeko(d->f, (off_t)d->pos, SEEK_SET)) { finish(*this, d, "can't read " + d->path + ": " + strerror(errno)); continue; }
            d->st = Dcc::RUN, d->got = d->rate_got = d->queued = d->pos, d->at = d->last_rx = now;
            print(where(*this, d->srv), LK::INFO, tag(d) + "sending " + d->file + " to " + d->nick + " (" + d->conn.host + ")" +
                                                      (d->pos ? " from " + human_bytes(d->pos) : ""));
            send_more(d);
            continue;
        }
        if (d->st != Dcc::RUN) continue;
        short rev = rev_of(d->conn.fd);
        if (!rev) continue;
        std::string in;
        bool ok = d->conn.pump(rev, in);
        if (d->send) {  // in: their acks, 32-bit big-endian counts of what they have
            d->acks += in;
            bool acked = false;
            uint32_t last = 0;
            for (; d->acks.size() >= 4; d->acks.erase(0, 4), acked = true)
                last = (uint32_t)(unsigned char)d->acks[0] << 24 | (uint32_t)(unsigned char)d->acks[1] << 16 |
                       (uint32_t)(unsigned char)d->acks[2] << 8 | (unsigned char)d->acks[3];
            if (d->acks.size() > 64) d->acks.clear();
            if (ok) send_more(d);
            unsigned long long got = d->queued - d->conn.out.size();
            if (got != d->got || acked) d->got = got, d->last_rx = now;
            bool all = !d->f && d->conn.out.empty();
            if (all && acked && last == (uint32_t)d->size) finish(*this, d, "");
            else if (!ok) finish(*this, d, all ? "" : d->conn.err);
            continue;
        }
        if (!in.empty()) {
            if (d->size && d->got + in.size() > d->size) in.resize((size_t)(d->size - d->got));
            if (fwrite(in.data(), 1, in.size(), d->f) != in.size()) { finish(*this, d, std::string("can't write: ") + strerror(errno)); continue; }
            d->got += in.size(), d->last_rx = now;
            // the ack: bytes so far, 32 bits big-endian; only the newest matters, unless one is half sent
            uint32_t n = (uint32_t)d->got;
            if (d->conn.out.size() % 4 == 0) d->conn.out.clear();
            d->conn.out += std::string{(char)(n >> 24), (char)(n >> 16), (char)(n >> 8), (char)n};
            if (d->size && d->got >= d->size) {
                std::string junk;
                d->conn.pump(POLLOUT, junk);  // the last ack: some senders wait for it
                finish(*this, d, "");
                continue;
            }
        }
        // closed: fine when the size wasn't given and something came
        if (!ok) finish(*this, d, d->size || d->got == d->pos ? d->conn.err : "");
    }
}

void Core::dcc_tick(time_t now) {
    static time_t last = 0;
    if (now == last) return;
    last = now;
    for (auto& dp : dccs) {
        Dcc* d = dp.get();
        if (d->st == Dcc::WAIT && now - d->at > WAIT_SECS) finish(*this, d, "not taken in " + std::to_string(WAIT_SECS / 60) + " minutes");
        if (d->st == Dcc::RESUME && now - d->at > RESUME_SECS) {
            print(where(*this, d->srv), LK::INFO, tag(d) + d->nick + " didn't answer the resume: downloading it again from the start");
            start(*this, where(*this, d->srv), d, true);
        }
        if (d->st != Dcc::RUN) continue;
        d->rate = d->got - d->rate_got, d->rate_got = d->got;
        if (now - d->last_rx <= STALL_SECS) continue;
        if (d->send && !d->f && d->conn.out.empty()) finish(*this, d, "");  // all of it went out, they just never said so
        else finish(*this, d, d->got > d->pos ? "stalled, nothing for " + std::to_string(STALL_SECS) + "s" : "no answer from " + d->host);
    }
}
