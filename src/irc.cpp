// IRC protocol: parsing, connection lifecycle, CAP/SASL, message handling.
#include <algorithm>
#include <cstring>
#include <fnmatch.h>
#include <sys/stat.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include "core.h"

static const char* WANT_CAPS[] = {
    "multi-prefix", "away-notify", "account-notify", "extended-join", "chghost",
    "echo-message", "batch", "server-time", "message-tags", "userhost-in-names",
    "invite-notify", "setname", "cap-notify", "account-tag", "sasl", "draft/chathistory",
    "labeled-response", "draft/multiline", "extended-monitor", "draft/read-marker", "draft/message-redaction",
    "draft/channel-rename", "draft/pre-away", "draft/account-registration", "standard-replies",
    // not asked for: sts (never requested, only read), no-implicit-names (the nick list needs NAMES)
    // ZNC: other clients' messages (self-message), backlog on request (playback, see 001),
    // and the pre-IRCv3 names of server-time and batch that ZNC 1.6 still offers
    "znc.in/self-message", "znc.in/playback", "znc.in/server-time-iso", "znc.in/batch"};

// ---- parsing ---------------------------------------------------------------
const std::string& Msg::p(size_t i) const {
    static const std::string empty;
    return i < params.size() ? params[i] : empty;
}

static std::string unescape_tag(const std::string& v) {
    std::string o;
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i] != '\\') { o += v[i]; continue; }
        if (++i >= v.size()) break;
        char c = v[i];
        o += c == ':' ? ';' : c == 's' ? ' ' : c == 'r' ? '\r' : c == 'n' ? '\n' : c;
    }
    return o;
}

bool irc_parse(const std::string& line, Msg& m) {
    m = Msg{};
    size_t i = 0, n = line.size();
    auto word = [&]() {
        size_t sp = line.find(' ', i);
        if (sp == std::string::npos) sp = n;
        std::string w = line.substr(i, sp - i);
        i = sp;
        while (i < n && line[i] == ' ') i++;
        return w;
    };
    if (i < n && line[i] == '@') {
        i++;
        for (auto& t : split(word(), ';')) {
            size_t eq = t.find('=');
            if (eq == std::string::npos) m.tags[t] = "";
            else m.tags[t.substr(0, eq)] = unescape_tag(t.substr(eq + 1));
        }
    }
    if (i < n && line[i] == ':') {
        i++;
        m.prefix = word();
        m.nick = m.prefix.substr(0, m.prefix.find('!'));
    }
    m.cmd = word();
    for (auto& c : m.cmd) c = (char)toupper((unsigned char)c);
    while (i < n) {
        if (line[i] == ':') { m.params.push_back(line.substr(i + 1)); break; }
        m.params.push_back(word());
    }
    return !m.cmd.empty();
}

std::string irc_lower(const std::string& s, int cm) {
    std::string o = s;
    char top = cm == 1 ? 'Z' : ']';  // ascii: A-Z only; rfc1459s: also []\ -> {}|
    for (auto& c : o) {
        if (c >= 'A' && c <= top) c = (char)(c + 32);
        else if (c == '~' && cm == 0) c = '^';  // strict-rfc1459 leaves ~ alone
    }
    return o;
}

std::string escape_tag(const std::string& v) {
    std::string o;
    for (char c : v) o += c == ';' ? "\\:" : c == ' ' ? "\\s" : c == '\\' ? "\\\\" : c == '\r' ? "\\r" : c == '\n' ? "\\n" : std::string(1, c);
    return o;
}

// a short quote of a line for replies and reactions: no colors, no reply arrow of its own, 40 characters
std::string reply_snippet(const std::string& text) {
    std::string t = text;
    if (t.rfind("\x1d\x03" "14\xe2\x86\xaa", 0) == 0) {  // our "↪ nick: ..." prefix
        size_t e = t.find("\x0f ");
        if (e != std::string::npos) t = t.substr(e + 2);
    }
    t = mirc_strip(t);
    size_t i = 0, n = 0;
    while (i < t.size() && n < 40) utf8_next(t, i), n++;
    return "\u201c" + t.substr(0, i) + (i < t.size() ? "\u2026" : "") + "\u201d";
}

static std::string iso_utc(time_t t, const char* frac) {  // 2024-01-02T03:04:05 + frac
    char out[40] = "";
    tm g{};
    if (gmtime_r(&t, &g)) strftime(out, sizeof out, "%Y-%m-%dT%H:%M:%S", &g);
    return out + std::string(frac);
}

time_t parse_server_time(const std::string& iso) {
    tm t{};
    if (!strptime(iso.c_str(), "%Y-%m-%dT%H:%M:%S", &t)) return 0;
    return timegm(&t);
}

// ---- Server helpers --------------------------------------------------------
static std::string wire(const std::string& line) {
    std::string l = line;
    for (auto& c : l)
        if (c == '\r' || c == '\n' || c == '\0') c = ' ';  // no command injection
    return l + "\r\n";
}

void Server::send_now(const std::string& line) { conn.out += wire(line); }

void Server::send(const std::string& line) {
    std::string l = line;
    // labeled-response: what a typed command makes the server say comes back with its label (its tab)
    if (label_buf && caps.count("labeled-response") && !l.empty() && l[0] != '@' && l.rfind("BATCH ", 0) && l.rfind("QUIT", 0)) {
        std::string id = "i" + std::to_string(label_next++);
        labels[id] = label_buf;
        if (labels.size() > 200) labels.erase(labels.begin());  // ponytail: an unanswered one goes eventually, not the oldest first
        l = "@label=" + id + " " + l;
    }
    if (state != SS::CONNECTED || flood_ms <= 0) { send_now(l); return; }
    sendq.push_back(wire(l));
    drain(now_ms());
}

int Server::drain(long long now) {
    if (sendq.empty()) return -1;
    tokens = std::min<double>(flood_burst, tokens + (double)(now - tokens_at) / flood_ms);
    tokens_at = now;
    while (!sendq.empty() && tokens >= 1) conn.out += sendq.front(), sendq.pop_front(), tokens -= 1;
    return sendq.empty() ? -1 : (int)((1 - tokens) * flood_ms) + 1;
}

int Core::drain() {
    int wait = 1000;
    long long now = now_ms();
    if (!hist_saved) hist_saved = now;
    else if (now - hist_saved > 5 * 60 * 1000) save_history();  // a crash loses 5 minutes, not the session
    for (auto& s : servers) {
        int w = s->drain(now);
        if (w >= 0) wait = std::min(wait, w);
    }
    return wait;
}

bool Core::ignored(const Server* s, const std::string& prefix) const {
    if (prefix.find('!') == std::string::npos) return false;  // servers are never ignored
    std::string who = irc_lower(prefix);
    for (auto& mask : split(ini_get(cfg, "server." + s->cfg.name, "ignore"), ' '))
        if (fnmatch(irc_lower(mask).c_str(), who.c_str(), 0) == 0) return true;
    return false;
}

bool Server::tag_ok(const std::string& tag) const {
    if (!caps.count("message-tags")) return false;
    auto it = isupport.find("CLIENTTAGDENY");  // "*,-typing": all but typing; "draft/react": just that
    auto base = [](std::string t) {            // "+draft/react", "react": the same tag
        if (!t.empty() && t[0] == '+') t.erase(0, 1);
        return t.rfind("draft/", 0) == 0 ? t.substr(6) : t;
    };
    bool ok = true;
    if (it != isupport.end())
        for (auto& t : split(it->second, ',')) {
            if (t == "*") ok = false;
            else if (base(t[0] == '-' ? t.substr(1) : t) == base(tag)) ok = t[0] == '-';
        }
    return ok;
}

bool Server::is_chan(const std::string& t) const {
    return !t.empty() && chantypes.find(t[0]) != std::string::npos;
}

int Server::prefix_rank(const std::string& pfx) const {
    if (pfx.empty()) return (int)prefix_chars.size();
    size_t r = prefix_chars.find(pfx[0]);
    return r == std::string::npos ? (int)prefix_chars.size() : (int)r;
}

// ---- base64 / SASL ---------------------------------------------------------
static std::string hmac256(const std::string& key, const std::string& data) {
    unsigned char out[32];
    unsigned int len = sizeof out;
    HMAC(EVP_sha256(), key.data(), (int)key.size(), (const unsigned char*)data.data(),
         data.size(), out, &len);
    return std::string((char*)out, len);
}

static void send_auth(Server* s, const std::string& payload) {
    std::string e = b64(payload);
    for (size_t i = 0; i < e.size(); i += 400) s->send("AUTHENTICATE " + e.substr(i, 400));
    if (e.empty() || e.size() % 400 == 0) s->send("AUTHENTICATE +");
}

static std::string sasl_mech(Server* s) {
    if (!s->cfg.sasl_mech.empty()) return s->cfg.sasl_mech;
    return s->cfg.sasl_user.empty() && !s->cfg.cert.empty() ? "EXTERNAL" : "PLAIN";
}

static bool sasl_wanted(Server* s) {
    return s->cfg.auto_identify && (!s->cfg.sasl_user.empty() || !s->cfg.cert.empty());
}

static void cap_end(Server* s) {
    if (!s->cap_done) s->send("CAP END"), s->cap_done = true;
}

// One AUTHENTICATE step. Server payloads here are short; ponytail: no
// reassembly of >400-byte server chunks.
static void sasl_step(Core& c, Server* s, const std::string& data) {
    std::string mech = sasl_mech(s);
    if (mech == "EXTERNAL") { s->send("AUTHENTICATE +"); return; }
    const std::string& u = s->cfg.sasl_user;
    if (mech == "PLAIN") { send_auth(s, u + '\0' + u + '\0' + s->cfg.sasl_pass); return; }
    // SCRAM-SHA-256 (RFC 5802/7677)
    if (s->sasl_step == 0) {
        unsigned char rnd[18];
        RAND_bytes(rnd, sizeof rnd);
        std::string name;
        for (char ch : u) name += ch == ',' ? "=2C" : ch == '=' ? "=3D" : std::string(1, ch);
        s->scram_bare = "n=" + name + ",r=" + b64(std::string((char*)rnd, sizeof rnd));
        s->sasl_step = 1;
        send_auth(s, "n,," + s->scram_bare);
        return;
    }
    std::string msg = unb64(data);
    if (s->sasl_step == 1) {
        std::string nonce, salt, cnonce = s->scram_bare.substr(s->scram_bare.find(",r=") + 3);
        int iter = 0;
        for (auto& kv : split(msg, ',')) {
            if (kv.rfind("r=", 0) == 0) nonce = kv.substr(2);
            else if (kv.rfind("s=", 0) == 0) salt = unb64(kv.substr(2));
            else if (kv.rfind("i=", 0) == 0) iter = atoi(kv.c_str() + 2);
        }
        if (nonce.rfind(cnonce, 0) != 0 || salt.empty() || iter < 1 || iter > 1000000) {
            c.print(s->status, LK::ERROR, "SASL: bad SCRAM server message");
            s->send("AUTHENTICATE *");
            return;
        }
        unsigned char sp[32];
        PKCS5_PBKDF2_HMAC(s->cfg.sasl_pass.data(), (int)s->cfg.sasl_pass.size(),
                          (const unsigned char*)salt.data(), (int)salt.size(), iter,
                          EVP_sha256(), 32, sp);
        std::string salted((char*)sp, 32);
        std::string ckey = hmac256(salted, "Client Key");
        unsigned char stored[32];
        SHA256((const unsigned char*)ckey.data(), ckey.size(), stored);
        std::string final_wo = "c=biws,r=" + nonce;
        std::string auth = s->scram_bare + "," + msg + "," + final_wo;
        std::string csig = hmac256(std::string((char*)stored, 32), auth);
        std::string proof = ckey;
        for (size_t i = 0; i < proof.size(); i++) proof[i] ^= csig[i];
        s->scram_sig = b64(hmac256(hmac256(salted, "Server Key"), auth));
        s->sasl_step = 2;
        send_auth(s, final_wo + ",p=" + b64(proof));
        return;
    }
    if (msg != "v=" + s->scram_sig) {
        c.print(s->status, LK::ERROR, "SASL: server signature mismatch, aborting");
        s->send("AUTHENTICATE *");
        return;
    }
    s->send("AUTHENTICATE +");
}

// ---- lifecycle -------------------------------------------------------------
void Core::connect(Server* s) {
    s->conn.close();
    s->user_quit = false;
    s->caps.clear(), s->caps_ls.clear();
    s->sasl_step = 0, s->cap_done = false, s->sasl_ok = false, s->nick_tries = 0, s->whois.clear(), s->list.clear(), s->listing = false;
    s->away = s->auto_away = false;
    s->hist_batches.clear(), s->cap_vals.clear(), s->isupport.clear(), s->casemap = 0;
    s->labels.clear(), s->batch_labels.clear(), s->multi.clear(), s->who_auto.clear();
    s->online.clear(), s->ison_at = 0, s->ison_wait = 0, s->typing_sent.clear(), s->motd_done = false;
    s->inbuf.clear(), s->umodes.clear(), s->sendq.clear(), s->kb_wait.clear();
    s->tokens = s->flood_burst, s->tokens_at = now_ms();
    s->nick = s->last_nick.empty() ? s->cfg.nick : s->last_nick;
    s->last_rx = time(nullptr);
    s->reconnect_at = 0;
    int tls = s->cfg.tls ? (s->cfg.tls_verify ? 1 : 2) : 0, port = s->cfg.port;
    std::string why;
    // STS: a server that asked for TLS gets TLS, verified, on its port: now (the upgrade) or until the policy runs out
    auto pol = sts.find(irc_lower(s->cfg.host));
    if (pol != sts.end() && pol->second.second <= time(nullptr)) sts.erase(pol), sts_save(), pol = sts.end();
    if (!s->cfg.tls && !s->twitch()) {
        if (s->sts_port) tls = 1, port = s->sts_port, why = ", STS";
        else if (pol != sts.end()) tls = 1, port = pol->second.first, why = ", STS policy";
    }
    print(s->status, LK::INFO,
          "Connecting to " + s->cfg.host + ":" + std::to_string(port) + (tls ? " (TLS" + why + ")" : ""));
    if (!s->conn.connect(s->cfg.host, port, tls, s->cfg.cert, s->cfg.key)) {
        s->state = SS::CONNECTING;  // so disconnect() schedules a retry
        disconnect(s, s->conn.err);
        return;
    }
    s->state = SS::CONNECTING;
    if (s->twitch()) twitch_login(s);  // twitch.cpp
    else {
        // bouncers: ZNC "user/network:pass", soju "user/network" in user or SASL
        const std::string& pw = s->cfg.pass;
        if (!pw.empty()) s->send("PASS " + std::string(pw[0] == ':' || pw.find(' ') != std::string::npos ? ":" : "") + pw);
        s->send("CAP LS 302");
    }
    s->send("NICK " + s->nick);
    s->send("USER " + s->cfg.user + " 0 * :" + s->cfg.realname);
    notify(Ev::BUFFER, s->status);
}

void Core::disconnect(Server* s, const std::string& why) {
    if (s->state == SS::DISCONNECTED) return;
    s->conn.close();
    s->state = SS::DISCONNECTED;
    for (auto& b : bufs)
        if (b->srv == s && b->type == BT::CHANNEL) {
            b->joined = false;
            b->nicks.clear();
            notify(Ev::BUFFER, b.get());
        }
    if (s->user_quit) {
        print(s->status, LK::INFO, "Disconnected" + (why.empty() ? "" : ": " + why));
    } else {
        s->backoff = s->backoff ? std::min(s->backoff * 2, 300) : 5;
        s->reconnect_at = time(nullptr) + s->backoff;
        print(s->status, LK::ERROR,
              "Disconnected: " + why + " (reconnecting in " + std::to_string(s->backoff) + "s)");
    }
    notify(Ev::BUFFER, s->status);
}

void Core::on_readable(Server* s, short rev) {
    bool ok = s->conn.pump(rev, s->inbuf);
    if (s->state == SS::CONNECTING && s->conn.st == Conn::OPEN) s->state = SS::REGISTERING;
    // one erase for the whole read, not one per line: a /list or NAMES burst is thousands of lines
    size_t at = 0, nl;
    std::string line;
    while ((nl = s->inbuf.find('\n', at)) != std::string::npos) {
        line.assign(s->inbuf, at, nl - at);
        at = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        Msg m;
        s->last_rx = time(nullptr);
        if (irc_parse(line, m)) handle(s, m);
        if (s->state == SS::DISCONNECTED) return;  // handler closed it (connect() cleared inbuf)
    }
    s->inbuf.erase(0, at);
    if (!ok) disconnect(s, s->conn.err);
}

void Core::tick(time_t now) {
    dcc_tick(now);
    check_update(now);
    int lock_idle = atoi(opt(nullptr, "lock_idle", "0").c_str());  // minutes without input, 0 = never
    if (lock_idle > 0 && !locked && now - last_input >= lock_idle * 60 && lock_on()) set_lock(true);
    for (auto& sp : servers) {
        Server* s = sp.get();
        if (s->state == SS::DISCONNECTED) {
            if (!s->user_quit && s->reconnect_at && now >= s->reconnect_at) connect(s);
        } else if (s->state != SS::CONNECTED) {
            if (now - s->last_rx > 60) disconnect(s, "connection timed out");
        } else if (now - s->last_rx > 240) {
            disconnect(s, "ping timeout");
        } else {
            if (now >= s->ping_at) {
                s->ping_sent_ms = now_ms();
                s->send_now("PING :LAG" + std::to_string(s->ping_sent_ms));
                s->ping_at = now + 30;
            }
            auto_away(s, now);
            chan_timers(s, now);
            mark_read(s, now);
            // /notify without MONITOR: ISON once a minute, from the end of the MOTD on
            std::string watch = ini_get(cfg, "server." + s->cfg.name, "notify");
            if (s->ison_at && now >= s->ison_at && !watch.empty() && !s->isupport.count("MONITOR")) {
                std::string l;
                for (auto& n : split(watch, ' '))
                    if (!n.empty() && l.size() + n.size() < 400) l += " " + n;
                s->send("ISON" + l);
                s->ison_wait++, s->ison_at = now + 60;
            }
        }
        long long ms = now_ms();  // +typing runs out: 6s after the last "active"
        for (auto& b : bufs) {
            if (b->srv != s) continue;
            for (auto it = b->typing.begin(); it != b->typing.end();) {
                if (it->second <= ms) it = b->typing.erase(it), dirty = true, notify(Ev::BUFFER, b.get());
                else ++it;
            }
        }
    }
}

void Core::set_attached(bool a) {
    if (a == attached) return;
    attached = a;
    if (a) last_input = time(nullptr);  // coming back counts as activity
    else detached_at = time(nullptr);
}

// auto_away (on): away when nobody has been attached for 30s (daemon mode,
// no browser) or, with auto_away_idle = N, after N minutes without input.
// A manual /away is left alone. [server.X] auto_away = off turns it off on that
// network only (and brings you back there if it had set you away).
// away_announce (off): also /me it in that network's joined channels.
bool Core::away_wanted(Server* s, time_t now) const {
    int idle = atoi(opt(s, "auto_away_idle", "0").c_str());
    return opt_on(s, "auto_away", true) && ((!attached && now - detached_at >= 30) || (idle > 0 && now - last_input >= idle * 60));
}

void Core::auto_away(Server* s, time_t now) {
    bool want = away_wanted(s, now);
    auto announce = [&](const std::string& t) {
        if (!opt_on(s, "away_announce", false)) return;
        std::vector<Buffer*> chans;  // say() may add lines: collect first
        for (auto& b : bufs)
            if (b->srv == s && b->type == BT::CHANNEL && b->joined) chans.push_back(b.get());
        for (Buffer* b : chans) say(b, t, true);
    };
    std::string msg = opt(s, "away_message", "Not here right now");
    if (want && !s->away && !s->auto_away) {
        s->auto_away = true;
        s->send("AWAY :" + msg);
        announce("is away: " + msg);
    } else if (!want && s->auto_away && s->away) {
        s->auto_away = false;
        s->send("AWAY");
        announce("is back");
    }
}

// ---- STS policies: "host port until" lines in sts next to the config ----------
static std::string sts_path(const std::string& cfg_path) {
    return cfg_path.empty() ? "" : cfg_path.substr(0, cfg_path.rfind('/') + 1) + "sts";
}

void Core::sts_load() {
    sts.clear();
    std::string p = sts_path(cfg_path);
    FILE* f = p.empty() ? nullptr : fopen(p.c_str(), "r");
    if (!f) return;
    char host[256];
    int port;
    long long until;
    while (fscanf(f, "%255s %d %lld", host, &port, &until) == 3)
        if (port > 0 && port < 65536 && until > time(nullptr)) sts[host] = {port, (time_t)until};
    fclose(f);
}

void Core::sts_save() {
    std::string p = sts_path(cfg_path);
    if (p.empty()) return;
    mode_t old = umask(077);
    FILE* f = fopen(p.c_str(), "w");
    umask(old);
    if (!f) return;
    for (auto& [h, v] : sts) fprintf(f, "%s %d %lld\n", h.c_str(), v.first, (long long)v.second);
    fclose(f);
}

// draft/read-marker: the tabs on screen are read up to their newest message, on every client of the account
void Core::mark_read(Server* s, time_t now) {
    if (!s->caps.count("draft/read-marker") || !attached) return;
    for (Buffer* b : {active, split_buf}) {
        if (!b || b->srv != s || b->type == BT::STATUS || b->name[0] == '*') continue;
        time_t newest = 0;
        for (auto it = b->lines.rbegin(); it != b->lines.rend(); ++it)
            if (it->kind == LK::MSG || it->kind == LK::ACTION || it->kind == LK::NOTICE) { newest = it->ts; break; }
        if (newest <= b->read_sent || newest > now + 60) continue;
        b->read_sent = b->read_ts = newest;
        s->send("MARKREAD " + b->name + " timestamp=" + iso_utc(newest, ".999Z"));
    }
}

Line* Core::by_msgid(Buffer* b, const std::string& id) {
    if (b && !id.empty())
        for (auto it = b->lines.rbegin(); it != b->lines.rend(); ++it)
            if (it->msgid == id) return &*it;
    return nullptr;
}

// ---- handlers --------------------------------------------------------------
// your nick or one of hilight_words, as a whole word ("isekur" isn't in "isekurs"); a word with
// * is a glob matched against each word of the text
static bool hilights(const std::string& text, const std::string& nick, const std::string& words) {
    std::string t = irc_lower(mirc_strip(text));
    auto word = [](char ch) { return isalnum((unsigned char)ch) || strchr("[]\\`_^{|}-", ch); };
    auto has = [&](const std::string& n) {
        for (size_t p = t.find(n); !n.empty() && p != std::string::npos; p = t.find(n, p + 1)) {
            bool l = p == 0 || !word(t[p - 1]), r = p + n.size() >= t.size() || !word(t[p + n.size()]);
            if (l && r) return true;
        }
        return false;
    };
    auto glob = [&](const std::string& g) {
        for (size_t i = 0; i < t.size();) {
            size_t j = i;
            while (j < t.size() && word(t[j])) j++;
            if (j > i && fnmatch(g.c_str(), t.substr(i, j - i).c_str(), 0) == 0) return true;
            i = j + 1;
        }
        return false;
    };
    if (has(irc_lower(nick))) return true;
    for (auto& w : split(irc_lower(words), ' '))
        if (w.find('*') != std::string::npos ? glob(w) : has(w)) return true;
    return false;
}

// a NAMES again (/names) keeps what WHOX and the tags taught us
static void add_nick(Buffer* b, const std::string& nick, const std::string& prefix, const std::string& host) {
    NickEntry& e = b->nicks[b->srv->lower(nick)];
    e.nick = nick, e.prefix = prefix;
    if (!host.empty()) e.host = host;
}

// user@host for every channel entry of nick (WHO replies, CHGHOST)
static void set_host(Core& c, Server* s, const std::string& nick, const std::string& host) {
    for (auto& b : c.bufs) {
        if (b->srv != s) continue;
        auto it = b->nicks.find(s->lower(nick));
        if (it != b->nicks.end()) it->second.host = host;
    }
}

static std::string host_of(const Msg& m) {
    size_t bang = m.prefix.find('!');
    return bang == std::string::npos ? "" : m.prefix.substr(bang + 1);
}

void Core::handle(Server* s, const Msg& m) {
    time_t ts = time(nullptr);
    auto st = m.tags.find("time");
    if (st != m.tags.end())
        if (time_t t = parse_server_time(st->second)) ts = t;
    auto line = [&](Buffer* b, LK k, const std::string& nick, const std::string& text,
                    bool self = false, bool hl = false) {
        add(b, Line{ts, k, nick, text, self, hl});
    };
    auto is_me = [&](const std::string& n) { return s->lower(n) == s->lower(s->nick); };
    const std::string& cmd = m.cmd;
    // labeled-response: the reply (or the batch it opens) names the command it answers, and so its tab
    int label_id = 0;
    if (auto lt = m.tags.find("label"); lt != m.tags.end()) {
        auto f = s->labels.find(lt->second);
        if (f != s->labels.end()) label_id = f->second, s->labels.erase(f);
        if (cmd == "BATCH" && label_id && m.p(0).size() > 1 && m.p(0)[0] == '+') s->batch_labels[m.p(0).substr(1)] = label_id;
    } else if (auto bt = m.tags.find("batch"); bt != m.tags.end()) {
        auto f = s->batch_labels.find(bt->second);
        if (f != s->batch_labels.end()) label_id = f->second;
    }
    if (cmd == "ACK") return;  // a labeled command with nothing to say
    // what we know of a nick from every message: its account (account-tag), a bot (the bot tag)
    auto each_entry = [&](const std::string& nick, const std::function<bool(NickEntry&)>& fn) {
        for (auto& b : bufs) {
            if (b->srv != s) continue;
            auto it = b->nicks.find(s->lower(nick));
            if (it != b->nicks.end() && fn(it->second)) notify(Ev::BUFFER, b.get()), dirty = true;
        }
    };
    if (!m.nick.empty() && m.prefix.find('!') != std::string::npos) {
        auto at = m.tags.find("account");
        if (at != m.tags.end() && s->caps.count("account-tag"))
            each_entry(m.nick, [&](NickEntry& e) { bool ch = e.account != at->second; e.account = at->second; return ch; });
        if (m.tags.count("bot") && (cmd == "PRIVMSG" || cmd == "NOTICE"))
            each_entry(m.nick, [&](NickEntry& e) { bool ch = !e.bot; e.bot = true; return ch; });
    }
    // where server replies go: the active buffer if it's ours, else status
    // the tab the user typed a command in, for its replies (whois, errors...): its label's, else within 30s, if still open
    auto typed_buf = [&]() -> Buffer* {
        int id = label_id ? label_id : time(nullptr) - s->typed_at > 30 ? 0 : s->typed_in;
        for (auto& b : bufs)
            if (id && b->id == id && b->srv == s) return b.get();
        return nullptr;
    };
    auto reply_buf = [&]() {
        if (Buffer* b = typed_buf()) return b;
        return active && active->srv == s && s->state == SS::CONNECTED ? active : s->status;
    };

    if (cmd == "PING") { s->send_now("PONG :" + m.p(0)); return; }
    if (cmd == "PONG") {
        if (m.p(1).rfind("LAG", 0) == 0) {
            s->lag = (now_ms() - atoll(m.p(1).c_str() + 3)) / 1000.0;
            dirty = true;
            notify(Ev::BUFFER, s->status);
        }
        return;
    }
    if (cmd == "CAP") {
        const std::string& sub = m.p(1);
        if (sub == "LS" || sub == "NEW") {
            bool more = m.p(2) == "*";
            for (auto& c : split(more ? m.p(3) : m.p(2), ' ')) {
                size_t eq = c.find('=');
                s->caps_ls.insert(c.substr(0, eq));
                s->cap_vals[c.substr(0, eq)] = eq == std::string::npos ? "" : c.substr(eq + 1);
            }
            if (more) return;
            // STS: over plain text, reconnect with TLS on its port now; over verified TLS, remember it for duration
            auto sv = s->cap_vals.find("sts");
            if (sv != s->cap_vals.end() && !s->twitch()) {
                std::map<std::string, std::string> kv;
                for (auto& p : split(sv->second, ',')) kv[p.substr(0, p.find('='))] = p.find('=') == std::string::npos ? "" : p.substr(p.find('=') + 1);
                int port = atoi(kv["port"].c_str());
                if (!s->conn.tls && port > 0 && port < 65536) {
                    print(s->status, LK::INFO, "STS: " + s->cfg.host + " wants TLS, reconnecting on port " + std::to_string(port));
                    s->sts_port = port;
                    s->conn.close();
                    s->state = SS::DISCONNECTED, s->reconnect_at = time(nullptr);  // tick() connects, with TLS
                    notify(Ev::BUFFER, s->status);
                    return;
                }
                if (s->conn.tls == 1 && kv.count("duration")) {
                    long dur = atol(kv["duration"].c_str());
                    std::string h = irc_lower(s->cfg.host);
                    int tport = s->cfg.tls ? s->cfg.port : s->sts_port ? s->sts_port : sts.count(h) ? sts[h].first : s->cfg.port;
                    if (dur > 0) sts[h] = {tport, time(nullptr) + dur};
                    else sts.erase(h);
                    if (!s->cfg.tls) sts_save();  // a tls = true server needs no policy on disk
                }
            }
            std::string req;
            for (const char* w : WANT_CAPS)
                if (s->caps_ls.count(w) && !s->caps.count(w) &&
                    (strcmp(w, "sasl") || sasl_wanted(s)))
                    req += (req.empty() ? "" : " ") + std::string(w);
            if (!req.empty()) s->send("CAP REQ :" + req);
            else if (s->state != SS::CONNECTED) cap_end(s);
        } else if (sub == "ACK") {
            bool sasl = false;
            for (auto& c : split(m.p(2), ' ')) {
                if (c.empty()) continue;
                if (c[0] == '-') { s->caps.erase(c.substr(1)); continue; }
                s->caps.insert(c);
                if (c == "sasl") sasl = true;
                // draft/pre-away: nobody's here (daemon, no browser): away from the start, not 30s in
                if (c == "draft/pre-away" && s->state != SS::CONNECTED && away_wanted(s, time(nullptr)) && !s->away)
                    s->auto_away = true, s->send("AWAY :" + opt(s, "away_message", "Not here right now"));
            }
            if (sasl && s->state != SS::CONNECTED) {
                s->send("AUTHENTICATE " + sasl_mech(s));
            } else if (s->state != SS::CONNECTED) {
                cap_end(s);
            }
        } else if (sub == "NAK") {
            if (s->state != SS::CONNECTED) cap_end(s);
        } else if (sub == "DEL") {
            for (auto& c : split(m.p(2), ' ')) s->caps.erase(c), s->caps_ls.erase(c);
        }
        return;
    }
    if (cmd == "AUTHENTICATE") { sasl_step(*this, s, m.p(0)); return; }
    if (cmd == "900") { print(s->status, LK::INFO, m.p(3)); return; }
    if (cmd == "903") { print(s->status, LK::INFO, "SASL authentication successful"); s->sasl_ok = true; cap_end(s); return; }
    if (cmd == "902" || cmd == "904" || cmd == "905" || cmd == "906" || cmd == "908") {
        print(s->status, LK::ERROR, "SASL: " + (m.params.empty() ? cmd : m.params.back()));  // a bare "904" must not crash us
        cap_end(s);
        return;
    }
    if (cmd == "305") s->away = s->auto_away = false;  // then printed as usual
    if (cmd == "306") s->away = true;
    if (cmd == "001") {
        s->state = SS::CONNECTED;
        s->nick = m.p(0);
        s->backoff = 0;
        s->ping_at = time(nullptr) + 30;
        if (s->caps_ls.empty()) s->cap_done = true;  // CAP ignored (psyBNC): a CAP END now would go upstream
        else cap_end(s);
        print(s->status, LK::INFO, m.p(1));
        // ponytail: IDENTIFY goes out right before the JOINs, so a +r channel can still beat
        // NickServ; SASL has no such race. Wait for the 900/NOTICE if that bites.
        if (s->cfg.auto_identify && !s->cfg.nickserv_pass.empty() && !s->sasl_ok) s->send("PRIVMSG NickServ :IDENTIFY " + s->cfg.nickserv_pass), s->ident_at = time(nullptr);
        std::vector<std::string> chans;  // autojoin, then the channel tabs still open (reconnects)
        for (auto& e : s->cfg.autojoin) {
            auto w = split(e, ' ');
            if (w.size() > 1) s->chan_keys[s->lower(w[0])] = w[1];
            chans.push_back(w[0]);
        }
        for (auto& b : bufs)
            if (b->srv == s && b->type == BT::CHANNEL &&
                std::find_if(chans.begin(), chans.end(), [&](const std::string& c) { return s->lower(c) == s->lower(b->name); }) == chans.end())
                chans.push_back(b->name);
        // ZNC playback module: with the cap ZNC holds the backlog back until asked; ask for
        // what came after the newest line we have (the same second again: the replay check drops it)
        if (s->caps.count("znc.in/playback")) {
            time_t last = 0;
            for (auto& b : bufs)
                if (b->srv == s)
                    for (auto it = b->lines.rbegin(); it != b->lines.rend(); ++it)
                        if (it->kind == LK::MSG || it->kind == LK::ACTION || it->kind == LK::NOTICE) { last = std::max(last, it->ts); break; }
            s->send("PRIVMSG *playback :PLAY * " + std::to_string(last));
        }
        for (auto& ch : chans) {
            if (s->twitch()) ch = irc_lower(ch);  // Twitch ignores a JOIN #Name
            auto k = s->chan_keys.find(s->lower(ch));
            s->send("JOIN " + ch + (k != s->chan_keys.end() ? " " + k->second : ""));
        }
        notify(Ev::BUFFER, s->status);
        return;
    }
    if (cmd == "005") {
        for (size_t i = 1; i + 1 < m.params.size(); i++) {
            const std::string& t = m.params[i];
            if (t.empty()) continue;
            if (t[0] == '-') { s->isupport.erase(t.substr(1)); continue; }
            size_t eq = t.find('=');
            s->isupport[t.substr(0, eq)] = eq == std::string::npos ? "" : t.substr(eq + 1);
            if (t.rfind("CASEMAPPING=", 0) == 0) {
                std::string v = t.substr(12);
                s->casemap = v == "ascii" || v == "rfc7613" || v == "precis" ? 1 : v == "strict-rfc1459" ? 2 : 0;
            } else if (t.rfind("PREFIX=(", 0) == 0) {
                size_t close = t.find(')');
                if (close != std::string::npos) {
                    s->prefix_modes = t.substr(8, close - 8);
                    s->prefix_chars = t.substr(close + 1);
                }
            } else if (t.rfind("CHANTYPES=", 0) == 0) {
                s->chantypes = t.substr(10);
            } else if (t.rfind("CHANMODES=", 0) == 0) {
                s->chanmodes = t.substr(10);
            } else if (t == "MODES" || t.rfind("MODES=", 0) == 0) {  // no value: unlimited
                s->max_modes = t.size() <= 6 ? 12 : std::max(1, std::min(atoi(t.c_str() + 6), 12));
            } else if (t.rfind("NETWORK=", 0) == 0) {
                s->network = t.substr(8);
                for (auto& b : bufs)  // the webchat groups tabs by network name
                    if (b->srv == s) notify(Ev::BUFFER, b.get());
            }
        }
        return;
    }
    if (cmd == "433" && s->state != SS::CONNECTED) {
        std::string next = s->nick_tries++ == 0 && !s->cfg.altnick.empty() ? s->cfg.altnick
                                                                           : s->nick + "_";
        print(s->status, LK::ERROR, "Nick " + m.p(1) + " in use, trying " + next);
        s->nick = next;
        s->send("NICK " + next);
        return;
    }
    if (cmd == "353") {  // NAMES
        Buffer* b = find(s, m.p(2));
        if (!b) return;
        for (auto& n : split(m.p(3), ' ')) {
            size_t k = 0;
            while (k < n.size() && s->prefix_chars.find(n[k]) != std::string::npos) k++;
            std::string nick = n.substr(k);  // userhost-in-names: nick!user@host
            size_t bang = nick.find('!');
            add_nick(b, nick.substr(0, bang), n.substr(0, k), bang == std::string::npos ? "" : nick.substr(bang + 1));
        }
        return;
    }
    if (cmd == "366") {
        Buffer* b = find(s, m.p(1));
        if (b) notify(Ev::BUFFER, b), dirty = true;
        // WHOX after our own join: accounts, real names, away and bots for the nick list (not for huge channels)
        if (b && s->who_auto.count(s->lower(b->name))) {
            if (s->isupport.count("WHOX") && b->nicks.size() <= 500) s->send("WHO " + b->name + " %tcuhnfar,777");
            else s->who_auto.erase(s->lower(b->name));
        }
        return;
    }
    if (cmd == "354" && m.p(1) == "777" && m.params.size() >= 9) {  // me 777 #chan user host nick flags account :realname
        Buffer* b = find(s, m.p(2));
        if (!b) return;
        auto it = b->nicks.find(s->lower(m.p(5)));
        if (it == b->nicks.end()) return;
        NickEntry& e = it->second;
        const std::string& fl = m.p(6);
        std::string bot = s->isupport.count("BOT") ? s->isupport["BOT"] : "";
        e.host = m.p(3) + "@" + m.p(4), e.away = !fl.empty() && fl[0] == 'G';
        e.account = m.p(7) == "0" ? "" : m.p(7), e.realname = m.params.back();
        e.bot = !bot.empty() && fl.find(bot[0]) != std::string::npos;
        return;
    }
    if (cmd == "315" && s->who_auto.erase(s->lower(m.p(1)))) {  // the end of our join's WHO: draw the list once
        if (Buffer* b = find(s, m.p(1))) notify(Ev::BUFFER, b), dirty = true;
        return;
    }
    if (cmd == "332" || cmd == "TOPIC") {
        bool num = cmd == "332";
        Buffer* b = find(s, m.p(num ? 1 : 0));
        if (!b) return;
        b->topic = m.p(num ? 2 : 1);
        line(b, LK::TOPIC, m.nick,
             num ? "Topic: " + b->topic : m.nick + " changed the topic to: " + b->topic);
        notify(Ev::BUFFER, b);
        return;
    }
    if (cmd == "333" || cmd == "329") return;  // topic setter, channel creation time
    if (cmd == "324") {
        if (Buffer* b = find(s, m.p(1))) {
            b->modes = m.p(2).substr(m.p(2).rfind('+', 0) == 0);
            notify(Ev::BUFFER, b), dirty = true;
        }
        return;
    }
    if (cmd == "221") { s->umodes = m.p(1).substr(m.p(1).rfind('+', 0) == 0); dirty = true; return; }

    if (cmd == "JOIN") {
        const std::string& ch = m.p(0);
        if (is_me(m.nick)) {
            bool fresh = !find(s, ch);  // rejoins after reconnect don't steal focus
            Buffer* b = open(s, BT::CHANNEL, ch);
            b->joined = true;
            b->nicks.clear();
            // catch up from the server: what was said since the last line we have
            int fetch = atoi(opt(s, "history_fetch", "100").c_str());
            if (fetch > 0 && s->caps.count("draft/chathistory")) {
                time_t last = 0;
                for (auto& l : b->lines)
                    if (l.kind == LK::MSG || l.kind == LK::ACTION || l.kind == LK::NOTICE) last = l.ts;
                std::string when = last ? "timestamp=" + iso_utc(last, ".000Z") : "*";
                s->send("CHATHISTORY " + std::string(last ? "AFTER " : "LATEST ") + ch + " " + when + " " +
                        std::to_string(std::min(fetch, 500)));
            }
            line(b, LK::JOIN, m.nick, "You have joined " + ch, true);
            if (!s->twitch()) s->send("MODE " + ch), s->who_auto.insert(s->lower(ch));  // Twitch: 421 unknown command
            if (fresh) select(b), notify(Ev::FOCUS, b);  // the webchat follows too
            return;
        }
        Buffer* b = find(s, ch);
        if (!b) return;
        add_nick(b, m.nick, "", host_of(m));
        std::string acct = s->caps.count("extended-join") && m.p(1) != "*" && !m.p(1).empty()
                               ? " [" + m.p(1) + "]" : "";
        if (s->caps.count("extended-join")) {  // JOIN #chan account :real name
            NickEntry& e = b->nicks[s->lower(m.nick)];
            e.account = m.p(1) == "*" ? "" : m.p(1);
            if (m.params.size() > 2) e.realname = m.p(2);
        }
        line(b, LK::JOIN, m.nick, m.nick + " (" + host_of(m) + ")" + acct + " has joined " + ch);
        notify(Ev::BUFFER, b);
        return;
    }
    if (cmd == "PART" || cmd == "KICK") {
        bool kick = cmd == "KICK";
        Buffer* b = find(s, m.p(0));
        if (!b) return;
        std::string who = kick ? m.p(1) : m.nick, why = m.p(kick ? 2 : 1);
        b->nicks.erase(s->lower(who));
        std::string txt = kick ? who + " was kicked by " + m.nick
                               : who + " (" + host_of(m) + ") has left " + b->name;
        if (!why.empty()) txt += " (" + why + ")";
        if (is_me(who)) b->joined = false, b->nicks.clear();
        line(b, LK::PART, who, txt, is_me(who));
        notify(Ev::BUFFER, b);
        if (kick && is_me(who) && opt_on(s, "rejoin_on_kick", false)) {  // a ban makes it fail once (474), no loop
            std::string key;
            for (auto& e : s->cfg.autojoin)
                if (auto w = split(e, ' '); w.size() > 1 && s->lower(w[0]) == s->lower(b->name)) key = " " + w[1];
            print(b, LK::INFO, "Rejoining " + b->name + " (rejoin_on_kick)");
            s->send("JOIN " + b->name + key);
        }
        return;
    }
    if (cmd == "QUIT") {
        for (auto& bp : bufs) {
            Buffer* b = bp.get();
            if (b->srv != s) continue;
            bool in = b->nicks.erase(s->lower(m.nick)) ||
                      (b->type == BT::QUERY && s->lower(b->name) == s->lower(m.nick));
            if (!in) continue;
            line(b, LK::QUIT, m.nick, m.nick + " (" + host_of(m) + ") has quit (" + m.p(0) + ")");
            notify(Ev::BUFFER, b);
        }
        return;
    }
    if (cmd == "NICK") {
        const std::string& nn = m.p(0);
        bool me = is_me(m.nick);
        if (me) s->nick = nn, remember_nick(s, nn);
        auto e = s->e2e.find(irc_lower(m.nick));  // the session follows the nick
        if (e != s->e2e.end() && !me) {
            E2E moved = e->second;
            moved.nick = nn;
            s->e2e.erase(e);
            s->e2e[irc_lower(nn)] = moved;
        }
        for (auto& bp : bufs) {
            Buffer* b = bp.get();
            if (b->srv != s) continue;
            auto it = b->nicks.find(s->lower(m.nick));
            bool query = b->type == BT::QUERY && s->lower(b->name) == s->lower(m.nick);
            if (it == b->nicks.end() && !query && !(me && b->type == BT::STATUS)) continue;
            if (it != b->nicks.end()) {
                NickEntry e = it->second;
                e.nick = nn;
                b->nicks.erase(it);
                b->nicks[s->lower(nn)] = e;
            }
            if (query) b->name = nn;
            line(b, LK::NICK, m.nick,
                 me ? "You are now known as " + nn : m.nick + " is now known as " + nn, me);
            notify(Ev::BUFFER, b);
        }
        return;
    }
    if (cmd == "MODE") {
        const std::string& tgt = m.p(0);
        if (!s->is_chan(tgt)) {
            if (is_me(tgt)) {  // user modes
                bool add = true;
                for (char ch : m.p(1)) {
                    if (ch == '+' || ch == '-') { add = ch == '+'; continue; }
                    size_t at = s->umodes.find(ch);
                    if (add && at == std::string::npos) s->umodes += ch;
                    if (!add && at != std::string::npos) s->umodes.erase(at, 1);
                }
                dirty = true;
            }
            return;
        }
        Buffer* b = find(s, tgt);
        if (!b) return;
        auto groups = split(s->chanmodes + ",,,", ',');
        std::string A = groups.size() > 0 ? groups[0] : "", B = groups.size() > 1 ? groups[1] : "",
                    C = groups.size() > 2 ? groups[2] : "";
        bool add = true;
        size_t arg = 2;
        for (char ch : m.p(1)) {
            if (ch == '+' || ch == '-') { add = ch == '+'; continue; }
            size_t pm = s->prefix_modes.find(ch);
            if (pm != std::string::npos && pm < s->prefix_chars.size()) {  // a malformed PREFIX= has fewer chars
                auto it = b->nicks.find(s->lower(m.p(arg++)));
                if (it == b->nicks.end()) continue;
                std::string& pf = it->second.prefix;
                char pc = s->prefix_chars[pm];
                size_t at = pf.find(pc);
                if (add && at == std::string::npos) pf += pc;
                if (!add && at != std::string::npos) pf.erase(at, 1);
                std::sort(pf.begin(), pf.end(), [&](char x, char y) {
                    return s->prefix_chars.find(x) < s->prefix_chars.find(y);
                });
            } else if (A.find(ch) != std::string::npos || B.find(ch) != std::string::npos ||
                       (add && C.find(ch) != std::string::npos)) {
                arg++;
            } else {
                size_t at = b->modes.find(ch);
                if (add && at == std::string::npos) b->modes += ch;
                if (!add && at != std::string::npos) b->modes.erase(at, 1);
            }
        }
        std::string args;
        for (size_t i = 1; i < m.params.size(); i++) args += (i > 1 ? " " : "") + m.params[i];
        line(b, LK::MODE, m.nick, "mode/" + tgt + " [" + args + "] by " + m.nick);
        notify(Ev::BUFFER, b);
        return;
    }
    if (cmd == "AWAY") {
        for (auto& b : bufs) {
            if (b->srv != s) continue;
            auto it = b->nicks.find(s->lower(m.nick));
            if (it != b->nicks.end()) it->second.away = !m.params.empty(), notify(Ev::BUFFER, b.get());
        }
        dirty = true;
        return;
    }
    if (cmd == "BATCH") {  // remember chathistory and ZNC buffer batches: their lines are replays
        const std::string& ref = m.p(0);
        if (ref.size() < 2) return;
        std::string id = ref.substr(1);
        if (ref[0] == '+' && (m.p(1).find("chathistory") != std::string::npos || m.p(1) == "znc.in/playback"))
            s->hist_batches.insert(id);
        else if (ref[0] == '+' && m.p(1) == "draft/multiline" && m.params.size() > 2)
            s->multi[id] = Server::Multi{m, "PRIVMSG", {}};
        else if (ref[0] == '-') {
            s->hist_batches.erase(id), s->batch_labels.erase(id);
            auto mu = s->multi.find(id);
            if (mu == s->multi.end()) return;
            // draft/multiline: one message of many lines, each shown as a line of its own (with the batch's
            // time, msgid and, inside a chathistory batch, that batch: a replay stays a replay)
            Server::Multi done = std::move(mu->second);
            s->multi.erase(mu);
            for (auto& text : done.lines) {
                Msg x;
                x.tags = done.open.tags, x.prefix = done.open.prefix, x.nick = done.open.nick;
                x.cmd = done.cmd, x.params = {done.open.p(2), text};
                handle(s, x);
                if (s->state == SS::DISCONNECTED) return;
            }
        }
        return;
    }
    if (cmd == "CHGHOST") { set_host(*this, s, m.nick, m.p(0) + "@" + m.p(1)); return; }
    if (cmd == "ACCOUNT") {  // account-notify: logged in as p(0), "*" = logged out
        std::string a = m.p(0) == "*" ? "" : m.p(0);
        each_entry(m.nick, [&](NickEntry& e) { bool ch = e.account != a; e.account = a; return ch; });
        return;
    }
    if (cmd == "SETNAME") {
        each_entry(m.nick, [&](NickEntry& e) { bool ch = e.realname != m.p(0); e.realname = m.p(0); return ch; });
        if (is_me(m.nick)) print(reply_buf(), LK::INFO, "Your real name is now: " + m.p(0));
        return;
    }
    if (cmd == "RENAME") {  // draft/channel-rename: RENAME #old #new :reason, the tab, its key and autojoin follow
        Buffer* b = find(s, m.p(0));
        if (!b || m.p(1).empty()) return;
        std::string old = b->name, nn = m.p(1);
        auto k = s->chan_keys.find(s->lower(old));
        if (k != s->chan_keys.end()) s->chan_keys[s->lower(nn)] = k->second, s->chan_keys.erase(s->lower(old));
        for (auto& e : s->cfg.autojoin) {
            auto w = split(e, ' ');
            if (!w.empty() && s->lower(w[0]) == s->lower(old)) e = nn + (w.size() > 1 ? " " + w[1] : "");
        }
        if (auto w = s->who_auto.find(s->lower(old)); w != s->who_auto.end()) s->who_auto.erase(w), s->who_auto.insert(s->lower(nn));
        b->name = nn;
        line(b, LK::INFO, m.nick, "Channel " + old + " is now " + nn + (m.p(2).empty() ? "" : " (" + m.p(2) + ")") +
                                       (s->cfg.autojoin.empty() ? "" : ": /autojoin snap keeps the new name"));
        notify(Ev::BUFFER, b), dirty = true;
        return;
    }
    if (cmd == "MARKREAD") {  // draft/read-marker: read up to this time on some client of ours
        Buffer* b = find(s, m.p(0));
        time_t t = b && m.p(1).rfind("timestamp=", 0) == 0 ? parse_server_time(m.p(1).substr(10)) : 0;
        if (!t || t <= b->read_ts) return;
        b->read_ts = t, b->read_sent = std::max(b->read_sent, t);
        time_t newest = 0;
        for (auto it = b->lines.rbegin(); it != b->lines.rend(); ++it)
            if (it->kind == LK::MSG || it->kind == LK::ACTION || it->kind == LK::NOTICE) { newest = it->ts; break; }
        if (t >= newest && (b->activity || b->unread)) b->activity = 0, b->unread = 0, notify(Ev::BUFFER, b), dirty = true;
        return;
    }
    if (cmd == "REDACT") {  // draft/message-redaction: REDACT target msgid [:reason]
        bool self = is_me(m.nick);
        Buffer* b = s->is_chan(m.p(0)) ? find(s, m.p(0)) : find(s, self ? m.p(0) : m.nick);
        Line* l = by_msgid(b, m.p(1));
        if (!l) return;
        bool own = s->lower(l->nick) == s->lower(m.nick);
        l->text = "\x1d\x03" "14[message deleted" + (own ? std::string() : " by " + m.nick) + (m.p(2).empty() ? "" : ": " + m.p(2)) + "]\x0f";
        l->hilight = false;
        dirty = true, notify(Ev::CLEAR, b);  // the webchat draws the tab again
        return;
    }
    if (cmd == "REGISTER" || cmd == "VERIFY") {  // draft/account-registration: SUCCESS|VERIFICATION_REQUIRED account :message
        print(reply_buf(), LK::INFO, m.params.back());
        if (m.p(0) == "SUCCESS" && !s->reg_pass.empty() && s->cfg.sasl_user.empty()) {  // SASL logs in next time
            std::string sec = "server." + s->cfg.name;
            s->cfg.sasl_user = cfg[sec]["sasl_user"] = unsaved[sec]["sasl_user"] = m.p(1);
            s->cfg.sasl_pass = cfg[sec]["sasl_pass"] = unsaved[sec]["sasl_pass"] = s->reg_pass;
            print(reply_buf(), LK::INFO, "sasl_user and sasl_pass are set for " + s->cfg.name + ": /save to log in with SASL next time");
        }
        if (m.p(0) == "SUCCESS") s->reg_pass.clear();
        return;
    }
    if (cmd == "FAIL" || cmd == "WARN" || cmd == "NOTE") {  // standard replies: CMD CODE [context...] :description
        std::string ctx;
        for (size_t i = 2; i + 1 < m.params.size(); i++) ctx += " " + m.params[i];
        print(reply_buf(), cmd == "NOTE" ? LK::INFO : LK::ERROR,
              (cmd == "WARN" ? "Warning: " : "") + (m.p(0) == "*" || m.p(0).empty() ? "" : m.p(0) + ": ") +
                  (m.params.size() > 2 ? m.params.back() : m.p(1)) + " (" + m.p(1) + ctx + ")");
        if (cmd == "FAIL" && m.p(0) == "REGISTER") s->reg_pass.clear();
        return;
    }
    if (cmd == "730" || cmd == "731") {  // MONITOR: :nick!u@h,... online / :nick,... offline
        for (auto& who : split(m.params.back(), ','))
            if (!who.empty()) notify_seen(s, who.substr(0, who.find('!')), cmd == "730", who.find('!') == std::string::npos ? "" : who.substr(who.find('!') + 1));
        return;
    }
    if (cmd == "734") { print(s->status, LK::ERROR, "/notify: " + m.params.back() + " (" + m.p(1) + " at most)"); return; }
    if (cmd == "303" && s->ison_wait > 0) {  // our ISON: the watched nicks that are on
        s->ison_wait--;
        std::set<std::string> on;
        for (auto& n : split(m.p(1), ' ')) on.insert(s->lower(n));
        for (auto& n : split(ini_get(cfg, "server." + s->cfg.name, "notify"), ' '))
            if (!n.empty()) notify_seen(s, n, on.count(s->lower(n)) > 0);
        return;
    }
    if ((cmd == "376" || cmd == "422") && !s->motd_done) s->motd_done = true, notify_start(s);  // 005 is in by now; then printed as usual
    if (cmd == "352") set_host(*this, s, m.p(5), m.p(2) + "@" + m.p(3));  // WHO; then printed as usual
    if (cmd == "302" && !s->kb_wait.empty()) {  // USERHOST for a /kickban: "nick*=+user@host"
        KickBan kb = s->kb_wait.front();
        std::string uh;
        bool others = false;
        for (auto& e : split(m.p(1), ' ')) {
            size_t eq = e.find('=');
            if (eq == std::string::npos || e.size() < eq + 3) continue;
            std::string n = e.substr(0, eq);
            if (!n.empty() && n.back() == '*') n.pop_back();
            if (s->lower(n) == s->lower(kb.nick)) uh = e.substr(eq + 2);
            else others = true;
        }
        if (!uh.empty() || !others) {  // else someone else's USERHOST (/quote): printed as usual
            s->kb_wait.pop_front();
            Buffer* b = find(s, kb.chan);
            if (b && uh.empty()) print(b, LK::ERROR, kb.nick + " is no longer on IRC");
            else if (b) kickban(s, b, kb, uh);
            return;
        }
    }
    if (s->twitch() && twitch_handle(s, m, ts)) return;  // twitch.cpp
    if ((cmd == "PRIVMSG" || cmd == "NOTICE") && m.tags.count("batch")) {  // a draft/multiline line: kept for its batch's end
        auto mu = s->multi.find(m.tags.at("batch"));
        if (mu != s->multi.end()) {
            auto& ls = mu->second.lines;
            if (m.tags.count("draft/multiline-concat") && !ls.empty()) ls.back() += m.p(1);
            else ls.push_back(m.p(1));
            mu->second.cmd = cmd;
            return;
        }
    }
    if ((cmd == "PRIVMSG" || cmd == "NOTICE" || cmd == "INVITE" || cmd == "TAGMSG") && !is_me(m.nick) && ignored(s, m.prefix))
        return;  // includes CTCP: ignored users get no replies either
    auto tagv = [&](const char* name) {  // a client tag, final or draft: "reply" -> +reply or +draft/reply
        auto t = m.tags.find(std::string("+") + name);
        if (t == m.tags.end()) t = m.tags.find(std::string("+draft/") + name);
        return t == m.tags.end() ? std::string() : t->second;
    };
    if (cmd == "TAGMSG") {  // +typing, +draft/react
        std::string tgt = m.p(0);
        while (tgt.size() > 1 && s->prefix_chars.find(tgt[0]) != std::string::npos && s->is_chan(tgt.substr(1))) tgt.erase(0, 1);
        bool self = is_me(m.nick);
        Buffer* b = s->is_chan(tgt) ? find(s, tgt) : find(s, self ? tgt : m.nick);
        if (!b) return;
        auto ty = m.tags.find("+typing");
        if (ty != m.tags.end() && !self) {
            if (ty->second == "active") b->typing[s->lower(m.nick)] = now_ms() + 6000;
            else b->typing.erase(s->lower(m.nick));
            notify(Ev::BUFFER, b), dirty = true;
        }
        std::string react = tagv("react");
        if (!react.empty()) {
            Line* t = by_msgid(b, tagv("reply"));
            std::string to = !t ? "" : " to " + (t->self ? std::string("your") : t->nick + "'s") + " message " + reply_snippet(t->text);
            line(b, LK::INFO, m.nick, (self ? std::string("You") : m.nick) + " reacted " + react + to, self);
        }
        return;
    }
    if (cmd == "INVITE") {
        print(reply_buf(), LK::INFO, m.nick + " invites " + m.p(0) + " to " + m.p(1));
        if (is_me(m.p(0))) ring(s);
        return;
    }
    if (cmd == "PRIVMSG" || cmd == "NOTICE") {
        bool notice = cmd == "NOTICE";
        std::string tgt = m.p(0), text = m.p(1);
        // STATUSMSG: "@#chan"
        while (tgt.size() > 1 && s->prefix_chars.find(tgt[0]) != std::string::npos &&
               s->is_chan(tgt.substr(1)))
            tgt.erase(0, 1);
        bool self = is_me(m.nick);
        if (self && is_me(tgt) && s->caps.count("echo-message")) {  // to ourselves: the echo and the delivery, one message
            auto id = m.tags.find("msgid");
            std::string key = id != m.tags.end() ? id->second : cmd + " " + text;
            time_t now = time(nullptr);
            if (key == s->to_self && now - s->to_self_at < 2) { s->to_self.clear(); return; }
            s->to_self = key, s->to_self_at = now;
        }
        auto bt = m.tags.find("batch");
        bool replay = bt != m.tags.end() && s->hist_batches.count(bt->second);
        bool server_msg = m.prefix.find('!') == std::string::npos && !m.prefix.empty();
        LK kind = notice ? LK::NOTICE : LK::MSG;
        // "*** Notice -- ..." while you are +o/+s: the *snotices tab, not your server tab
        bool snote = notice && server_msg && s->state == SS::CONNECTED && text.rfind("*** ", 0) == 0 &&
                     s->umodes.find_first_of("os") != std::string::npos;
        bool locked = false;  // /e2e: decrypted
        if (!notice && text.rfind("+iSE ", 0) == 0 && s->is_chan(tgt)) {
            auto e = s->chan_e2e.find(irc_lower(tgt));
            std::string plain;
            if (e != s->chan_e2e.end() && e2e_open(e->second, text.substr(5), plain, irc_lower(tgt))) text = plain, locked = true;
            else text = std::string("[\xf0\x9f\x94\x92 encrypted message: can't decrypt, ") +
                        (e == s->chan_e2e.end() ? "no password (/e2e " + tgt + ")" : "wrong password?") + "]";
        } else if (!notice && text.rfind("+iSE ", 0) == 0) {
            auto e = s->e2e.find(irc_lower(self ? tgt : m.nick));
            std::string plain;
            if (e != s->e2e.end() && e2e_open(e->second.key, text.substr(5), plain)) text = plain, locked = true;
            else text = std::string("[\xf0\x9f\x94\x92 encrypted message: can't decrypt, ") +
                        (e == s->e2e.end() ? "no /e2e session" : "wrong password?") + "]";
        }
        if (text.size() > 1 && text[0] == '\x01') {
            std::string ctcp = text.substr(1, text.find('\x01', 1) - 1);
            std::string verb = ctcp.substr(0, ctcp.find(' '));
            std::string arg = ctcp.size() > verb.size() ? ctcp.substr(verb.size() + 1) : "";
            if (verb == "ACTION") {
                kind = LK::ACTION;
                text = arg;
            } else if (replay) {
                return;  // old CTCP requests: neither shown nor answered
            } else if (verb == "DCC" && !notice) {
                if (!self) dcc_ctcp(s, m.nick, arg);
                return;
            } else if (verb.rfind("ISEKUR-E2E", 0) == 0) {
                if (!notice && !self && !locked) e2e_ctcp(s, m.nick, verb, arg);
                return;
            } else {
                time_t now = time(nullptr);
                std::string rep, note;
                // self: our own request echoed back, unless it was sent to us
                if (!notice && (!self || is_me(tgt))) {
                    if (verb == "VERSION" && opt_on(s, "ctcp_version", true))
                        rep = "VERSION " + ctcp_version();
                    else if (verb == "PING" && opt_on(s, "ctcp_ping", true))
                        rep = "PING " + arg.substr(0, 64);
                    else if (verb == "TIME" && opt_on(s, "ctcp_time", true))
                        rep = "TIME " + fmt_time(now, "%a %b %d %H:%M:%S %Y %z");
                    // one reply per 2s per server: a CTCP flood must not get us
                    // killed for excess flood
                    if (!rep.empty() && now - s->ctcp_at < 2) rep.clear(), note = " (ignored: flood)";
                    else if (rep.empty()) note = " (no reply)";
                    if (!rep.empty()) {
                        s->ctcp_at = now;
                        s->send("NOTICE " + m.nick + " :\x01" + rep + "\x01");
                    }
                }
                if (notice && verb == "PING" && !arg.empty() &&
                    arg.find_first_not_of("0123456789") == std::string::npos) {
                    char rtt[48];
                    snprintf(rtt, sizeof rtt, "%.3fs", (now_ms() - atoll(arg.c_str())) / 1000.0);
                    arg = rtt;
                }
                print(reply_buf(), LK::INFO,
                      "CTCP " + verb + (notice ? " reply" : "") + " from " + m.nick +
                          (arg.empty() ? "" : ": " + arg) + note);
                return;
            }
        }
        Buffer* b;
        if (s->is_chan(tgt)) {
            b = find(s, tgt);
            if (!b) b = s->status;
        } else if (notice && (server_msg || s->state != SS::CONNECTED || m.nick.empty())) {
            b = snote ? open(s, BT::QUERY, "*snotices") : s->status;
        } else {
            std::string peer = self ? tgt : m.nick;
            if (self) text = mask_services(tgt, text);
            b = find(s, peer);
            // our automatic IDENTIFY and what NickServ says to it: the server tab, not a new NickServ tab
            bool ident = s->ident_at && time(nullptr) - s->ident_at < 30 && s->lower(peer) == "nickserv";
            if (!b) b = ident ? s->status : notice ? reply_buf() : open(s, BT::QUERY, peer);
        }
        bool hl = !self && !snote && !muted(b) && (b->type == BT::QUERY && !notice ? true : hilights(text, s->nick, ini_get(cfg, "server." + s->cfg.name, "hilight_words")));
        std::string who = m.nick.empty() ? s->cfg.host : m.nick;
        if (replay) {
            // the AFTER timestamp has 1s precision: skip what the log already showed
            std::string plain = mirc_strip(text);
            for (auto it = b->lines.rbegin(); it != b->lines.rend() && it->ts >= ts; ++it)  // newest first
                if (it->ts == ts && it->nick == who && mirc_strip(it->text) == plain) return;
            hl = false;  // old news: no alerts, no copies to window 1
        }
        b->typing.erase(s->lower(who));  // a message ends their typing
        std::string rid = tagv("reply");
        if (!rid.empty()) {  // +draft/reply: "↪ alice: “what they said”" before the text
            Line* t = by_msgid(b, rid);
            if (t && t->self && !self && !muted(b) && !replay && !snote) hl = true;  // a reply to you is a mention
            text = "\x1d\x03" "14\xe2\x86\xaa " + (t ? t->nick + ": " + reply_snippet(t->text) : std::string("an earlier message")) + "\x0f " + text;
        }
        Line l{ts, kind, who, locked ? "\xf0\x9f\x94\x92 " + text : text, self, hl};
        if (auto id = m.tags.find("msgid"); id != m.tags.end()) l.msgid = id->second;
        if (s->twitch() && !self && !server_msg && twitch_user(b, m, l)) notify(Ev::BUFFER, b);
        add(b, l);
        if (hl) ring(s);
        return;
    }
    if (cmd == "ERROR") { print(s->status, LK::ERROR, m.p(0)); return; }
    if (cmd == "464")  // then printed as usual
        print(s->status, LK::INFO, "Server password: password = ... in [server." + s->cfg.name + "] (ZNC: user/network:pass)");
    if (cmd == "WALLOPS") {  // with the server notices: oper chatter in one place
        line(open(s, BT::QUERY, "*snotices"), LK::INFO, "", "\x02!" + (m.nick.empty() ? m.prefix : m.nick) + "!\x02 " + m.p(0));
        return;
    }

    // /list: a table in the server's *channels tab, busiest first, drawn when the 323 ends it
    if (cmd == "321" || (cmd == "322" && !s->listing)) {
        s->list.clear(), s->listing = true, s->list_sort = 'u', s->list_rev = false;
        Buffer* lb = open(s, BT::QUERY, "*channels");
        lb->lines.clear(), lb->scroll = lb->unread = 0;
        notify(Ev::CLEAR, lb);
        add(lb, Line{ts, LK::INFO, "", "Listing channels...", false, false, true});
        select(lb), notify(Ev::FOCUS, lb);
        if (cmd == "321") return;
    }
    if (cmd == "322" && m.params.size() > 2) {  // me #chan users :topic
        s->list.push_back({m.p(1), atoi(m.p(2).c_str()), m.params.size() > 3 ? m.params.back() : ""});
        return;
    }
    if (cmd == "323") {
        s->listing = false;
        list_table(s);
        return;
    }

    // whois: a framed block in the tab you asked from
    static const std::set<std::string> whois_lines = {"301", "307", "310", "312", "313", "317", "319", "320",
                                                      "330", "335", "338", "378", "379", "671", "276"};
    // "me nick user host * :real name"; some servers (SekurIRCd) leave out the "*"
    if (cmd == "311" && m.params.size() > 4) {
        s->whois = s->lower(m.p(1));
        print(reply_buf(), LK::INFO, "┌─ whois \x02" + m.p(1) + "\x02 ─────────");
        print(reply_buf(), LK::INFO, "│ " + m.p(1) + " (" + m.p(2) + "@" + m.p(3) + "): " + m.params.back());
        return;
    }
    if (!s->whois.empty() && m.params.size() > 2 && s->lower(m.p(1)) == s->whois) {
        if (cmd == "318") {
            s->whois.clear();
            print(reply_buf(), LK::INFO, "└─────────");
            return;
        }
        if (whois_lines.count(cmd)) {
            std::string txt;
            if (cmd == "317") {  // nick idle signon :seconds idle, signon time
                long idle = atol(m.p(2).c_str());
                txt = "idle ";
                if (idle >= 86400) txt += std::to_string(idle / 86400) + "d";
                if (idle >= 3600) txt += std::to_string(idle / 3600 % 24) + "h";
                if (idle >= 60) txt += std::to_string(idle / 60 % 60) + "m";
                txt += std::to_string(idle % 60) + "s";
                if (m.params.size() > 4) txt += ", signed on " + fmt_time(atol(m.p(3).c_str()), "%Y-%m-%d %H:%M");
            } else if (cmd == "312") txt = "server " + m.p(2) + (m.params.size() > 3 ? " (" + m.p(3) + ")" : "");
            else if (cmd == "319") {  // ponytail: wrap at ~60 cols so the │ frame survives a long list
                std::string ln = "channels";
                for (auto& c : split(m.p(2), ' ')) {
                    if (ln.size() > 9 && ln.size() + 1 + c.size() > 60) print(reply_buf(), LK::INFO, "│ " + ln), ln = "        ";
                    ln += " " + c;
                }
                txt = ln;
            }
            else if (cmd == "301") txt = "away: " + m.p(2);
            else if (cmd == "330" && m.params.size() > 3) txt = "logged in as " + m.p(2);
            else for (size_t i = 2; i < m.params.size(); i++) txt += (i > 2 ? " " : "") + m.params[i];
            print(reply_buf(), LK::INFO, "│ " + txt);
            return;
        }
    }

    // generic numerics: route to a channel buffer if the reply names one
    if (cmd.size() == 3 && isdigit((unsigned char)cmd[0])) {
        Buffer* b = nullptr;
        if (m.params.size() > 2 && s->is_chan(m.p(1))) b = find(s, m.p(1));
        bool motd = cmd == "372" || cmd == "375" || cmd == "376" || cmd == "422";
        // not in that channel (join failures 471/473/474/475/477...): where it was typed, else the server tab
        bool chan = m.params.size() > 2 && s->is_chan(m.p(1));
        if (!b) b = motd ? s->status : chan ? (typed_buf() ? typed_buf() : s->status) : reply_buf();
        std::string txt;
        size_t from = b->type == BT::CHANNEL && b->name == m.p(1) ? 2 : 1;  // channel is implied
        for (size_t i = from; i < m.params.size(); i++) txt += (i > from ? " " : "") + m.params[i];
        print(b, cmd[0] == '4' || cmd[0] == '5' ? LK::ERROR : LK::INFO, txt);
        return;
    }
    print(s->status, LK::INFO, cmd + " " + m.p(0) + " " + m.p(1));
}
