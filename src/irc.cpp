// IRC protocol: parsing, connection lifecycle, CAP/SASL, message handling.
#include <algorithm>
#include <cstring>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include "core.h"

static const char* WANT_CAPS[] = {
    "multi-prefix", "away-notify", "account-notify", "extended-join", "chghost",
    "echo-message", "batch", "server-time", "message-tags", "userhost-in-names",
    "invite-notify", "setname", "cap-notify", "account-tag", "sasl"};

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

std::string irc_lower(const std::string& s) {
    std::string o = s;
    for (auto& c : o) {
        if (c >= 'A' && c <= ']') c = (char)(c + 32);  // A-Z and []\ -> a-z {}|
        else if (c == '~') c = '^';
    }
    return o;
}

time_t parse_server_time(const std::string& iso) {
    tm t{};
    if (!strptime(iso.c_str(), "%Y-%m-%dT%H:%M:%S", &t)) return 0;
    return timegm(&t);
}

// ---- Server helpers --------------------------------------------------------
void Server::send(const std::string& line) {
    std::string l = line;
    for (auto& c : l)
        if (c == '\r' || c == '\n' || c == '\0') c = ' ';  // no command injection
    conn.out += l + "\r\n";
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
    return !s->cfg.sasl_user.empty() || !s->cfg.cert.empty();
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
    s->sasl_step = 0, s->cap_done = false, s->nick_tries = 0;
    s->inbuf.clear(), s->umodes.clear();
    s->nick = s->cfg.nick;
    s->last_rx = time(nullptr);
    s->reconnect_at = 0;
    print(s->status, LK::INFO,
          "Connecting to " + s->cfg.host + ":" + std::to_string(s->cfg.port) +
              (s->cfg.tls ? " (TLS)" : ""));
    int tls = s->cfg.tls ? (s->cfg.tls_verify ? 1 : 2) : 0;
    if (!s->conn.connect(s->cfg.host, s->cfg.port, tls, s->cfg.cert, s->cfg.key)) {
        s->state = SS::CONNECTING;  // so disconnect() schedules a retry
        disconnect(s, s->conn.err);
        return;
    }
    s->state = SS::CONNECTING;
    if (!s->cfg.pass.empty()) s->send("PASS " + s->cfg.pass);
    s->send("CAP LS 302");
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
    size_t nl;
    while ((nl = s->inbuf.find('\n')) != std::string::npos) {
        std::string line = s->inbuf.substr(0, nl);
        s->inbuf.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        Msg m;
        s->last_rx = time(nullptr);
        if (irc_parse(line, m)) handle(s, m);
        if (s->state == SS::DISCONNECTED) return;  // handler closed it
    }
    if (!ok) disconnect(s, s->conn.err);
}

void Core::tick(time_t now) {
    for (auto& sp : servers) {
        Server* s = sp.get();
        if (s->state == SS::DISCONNECTED) {
            if (!s->user_quit && s->reconnect_at && now >= s->reconnect_at) connect(s);
        } else if (s->state != SS::CONNECTED) {
            if (now - s->last_rx > 60) disconnect(s, "connection timed out");
        } else if (now - s->last_rx > 240) {
            disconnect(s, "ping timeout");
        } else if (now >= s->ping_at) {
            s->ping_sent_ms = now_ms();
            s->send("PING :LAG" + std::to_string(s->ping_sent_ms));
            s->ping_at = now + 30;
        }
    }
}

// ---- handlers --------------------------------------------------------------
static bool hilights(const std::string& text, const std::string& nick) {
    std::string t = irc_lower(mirc_strip(text)), n = irc_lower(nick);
    for (size_t p = t.find(n); p != std::string::npos; p = t.find(n, p + 1)) {
        auto word = [](char ch) { return isalnum((unsigned char)ch) || strchr("[]\\`_^{|}-", ch); };
        bool l = p == 0 || !word(t[p - 1]), r = p + n.size() >= t.size() || !word(t[p + n.size()]);
        if (l && r) return true;
    }
    return false;
}

static void add_nick(Buffer* b, const std::string& nick, const std::string& prefix) {
    b->nicks[irc_lower(nick)] = NickEntry{nick, prefix, false};
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
    auto is_me = [&](const std::string& n) { return irc_lower(n) == irc_lower(s->nick); };
    // where server replies go: the active buffer if it's ours, else status
    auto reply_buf = [&]() {
        return active && active->srv == s && s->state == SS::CONNECTED ? active : s->status;
    };
    const std::string& cmd = m.cmd;

    if (cmd == "PING") { s->send("PONG :" + m.p(0)); return; }
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
            for (auto& c : split(more ? m.p(3) : m.p(2), ' '))
                s->caps_ls.insert(c.substr(0, c.find('=')));
            if (more) return;
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
                if (c[0] == '-') { s->caps.erase(c.substr(1)); continue; }
                s->caps.insert(c);
                if (c == "sasl") sasl = true;
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
    if (cmd == "903") { print(s->status, LK::INFO, "SASL authentication successful"); cap_end(s); return; }
    if (cmd == "902" || cmd == "904" || cmd == "905" || cmd == "906" || cmd == "908") {
        print(s->status, LK::ERROR, "SASL: " + m.params.back());
        cap_end(s);
        return;
    }
    if (cmd == "001") {
        s->state = SS::CONNECTED;
        s->nick = m.p(0);
        s->backoff = 0;
        s->ping_at = time(nullptr) + 30;
        cap_end(s);  // servers that ignored CAP never got here waiting
        print(s->status, LK::INFO, m.p(1));
        std::vector<std::string> chans = s->cfg.autojoin;
        for (auto& b : bufs)
            if (b->srv == s && b->type == BT::CHANNEL &&
                std::find(chans.begin(), chans.end(), b->name) == chans.end())
                chans.push_back(b->name);
        for (auto& ch : chans) s->send("JOIN " + ch);
        notify(Ev::BUFFER, s->status);
        return;
    }
    if (cmd == "005") {
        for (size_t i = 1; i + 1 < m.params.size(); i++) {
            const std::string& t = m.params[i];
            if (t.rfind("PREFIX=(", 0) == 0) {
                size_t close = t.find(')');
                if (close != std::string::npos) {
                    s->prefix_modes = t.substr(8, close - 8);
                    s->prefix_chars = t.substr(close + 1);
                }
            } else if (t.rfind("CHANTYPES=", 0) == 0) {
                s->chantypes = t.substr(10);
            } else if (t.rfind("CHANMODES=", 0) == 0) {
                s->chanmodes = t.substr(10);
            } else if (t.rfind("NETWORK=", 0) == 0) {
                s->network = t.substr(8);
                notify(Ev::BUFFER, s->status);
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
            std::string nick = n.substr(k);
            add_nick(b, nick.substr(0, nick.find('!')), n.substr(0, k));
        }
        return;
    }
    if (cmd == "366") { if (Buffer* b = find(s, m.p(1))) notify(Ev::BUFFER, b), dirty = true; return; }
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
            line(b, LK::JOIN, m.nick, "You have joined " + ch);
            s->send("MODE " + ch);
            if (fresh) select(b);
            return;
        }
        Buffer* b = find(s, ch);
        if (!b) return;
        add_nick(b, m.nick, "");
        std::string acct = s->caps.count("extended-join") && m.p(1) != "*" && !m.p(1).empty()
                               ? " [" + m.p(1) + "]" : "";
        line(b, LK::JOIN, m.nick, m.nick + " (" + host_of(m) + ")" + acct + " has joined " + ch);
        notify(Ev::BUFFER, b);
        return;
    }
    if (cmd == "PART" || cmd == "KICK") {
        bool kick = cmd == "KICK";
        Buffer* b = find(s, m.p(0));
        if (!b) return;
        std::string who = kick ? m.p(1) : m.nick, why = m.p(kick ? 2 : 1);
        b->nicks.erase(irc_lower(who));
        std::string txt = kick ? who + " was kicked by " + m.nick
                               : who + " (" + host_of(m) + ") has left " + b->name;
        if (!why.empty()) txt += " (" + why + ")";
        if (is_me(who)) b->joined = false, b->nicks.clear();
        line(b, LK::PART, who, txt);
        notify(Ev::BUFFER, b);
        return;
    }
    if (cmd == "QUIT") {
        for (auto& bp : bufs) {
            Buffer* b = bp.get();
            if (b->srv != s) continue;
            bool in = b->nicks.erase(irc_lower(m.nick)) ||
                      (b->type == BT::QUERY && irc_lower(b->name) == irc_lower(m.nick));
            if (!in) continue;
            line(b, LK::QUIT, m.nick, m.nick + " (" + host_of(m) + ") has quit (" + m.p(0) + ")");
            notify(Ev::BUFFER, b);
        }
        return;
    }
    if (cmd == "NICK") {
        const std::string& nn = m.p(0);
        bool me = is_me(m.nick);
        if (me) s->nick = nn;
        for (auto& bp : bufs) {
            Buffer* b = bp.get();
            if (b->srv != s) continue;
            auto it = b->nicks.find(irc_lower(m.nick));
            bool query = b->type == BT::QUERY && irc_lower(b->name) == irc_lower(m.nick);
            if (it == b->nicks.end() && !query && !(me && b->type == BT::STATUS)) continue;
            if (it != b->nicks.end()) {
                NickEntry e = it->second;
                e.nick = nn;
                b->nicks.erase(it);
                b->nicks[irc_lower(nn)] = e;
            }
            if (query) b->name = nn;
            line(b, LK::NICK, m.nick,
                 me ? "You are now known as " + nn : m.nick + " is now known as " + nn);
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
            if (pm != std::string::npos) {
                auto it = b->nicks.find(irc_lower(m.p(arg++)));
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
            auto it = b->nicks.find(irc_lower(m.nick));
            if (it != b->nicks.end()) it->second.away = !m.params.empty(), notify(Ev::BUFFER, b.get());
        }
        dirty = true;
        return;
    }
    if (cmd == "ACCOUNT" || cmd == "CHGHOST" || cmd == "SETNAME" || cmd == "BATCH" ||
        cmd == "TAGMSG")
        return;
    if (cmd == "INVITE") {
        print(reply_buf(), LK::INFO, m.nick + " invites " + m.p(0) + " to " + m.p(1));
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
        bool server_msg = m.prefix.find('!') == std::string::npos && !m.prefix.empty();
        LK kind = notice ? LK::NOTICE : LK::MSG;
        if (text.size() > 1 && text[0] == '\x01') {
            std::string ctcp = text.substr(1, text.find('\x01', 1) - 1);
            std::string verb = ctcp.substr(0, ctcp.find(' '));
            std::string arg = ctcp.size() > verb.size() ? ctcp.substr(verb.size() + 1) : "";
            if (verb == "ACTION") {
                kind = LK::ACTION;
                text = arg;
            } else {
                if (!notice && !self) {
                    std::string rep = verb == "VERSION" ? "VERSION iSekur IRC client"
                                    : verb == "PING"    ? "PING " + arg
                                    : verb == "TIME"    ? "TIME " + fmt_time(time(nullptr), "%c")
                                                        : "";
                    if (!rep.empty()) s->send("NOTICE " + m.nick + " :\x01" + rep + "\x01");
                }
                print(reply_buf(), LK::INFO,
                      "CTCP " + verb + (notice ? " reply" : "") + " from " + m.nick +
                          (arg.empty() ? "" : ": " + arg));
                return;
            }
        }
        Buffer* b;
        if (s->is_chan(tgt)) {
            b = find(s, tgt);
            if (!b) b = s->status;
        } else if (notice && (server_msg || s->state != SS::CONNECTED || m.nick.empty())) {
            b = s->status;
        } else {
            std::string peer = self ? tgt : m.nick;
            b = find(s, peer);
            if (!b) b = notice ? reply_buf() : open(s, BT::QUERY, peer);
        }
        bool hl = !self && (b->type == BT::QUERY && !notice ? true : hilights(text, s->nick));
        line(b, kind, m.nick.empty() ? s->cfg.host : m.nick, text, self, hl);
        return;
    }
    if (cmd == "ERROR") { print(s->status, LK::ERROR, m.p(0)); return; }

    // generic numerics: route to a channel buffer if the reply names one
    if (cmd.size() == 3 && isdigit((unsigned char)cmd[0])) {
        Buffer* b = nullptr;
        if (m.params.size() > 2 && s->is_chan(m.p(1))) b = find(s, m.p(1));
        bool motd = cmd == "372" || cmd == "375" || cmd == "376" || cmd == "422";
        if (!b) b = motd ? s->status : reply_buf();
        std::string txt;
        size_t from = b->type == BT::CHANNEL && b->name == m.p(1) ? 2 : 1;  // channel is implied
        for (size_t i = from; i < m.params.size(); i++) txt += (i > from ? " " : "") + m.params[i];
        print(b, cmd[0] == '4' || cmd[0] == '5' ? LK::ERROR : LK::INFO, txt);
        return;
    }
    print(s->status, LK::INFO, cmd + " " + m.p(0) + " " + m.p(1));
}
