// Built-in bouncer ([bnc]): an IRC server for your other clients, in front of iSekur's own
// connections. A client logs in with PASS user/network:password (or user/network in USER,
// as ZNC and soju take it), gets a welcome made from iSekur's state (005, channels, topics,
// names, the last lines of each tab), then the network's lines as they come, with the IRCv3 it
// asked for (server-time, message-tags, echo-message, batch, draft/chathistory served from
// iSekur's own tabs) and the rest taken off: batches unwrapped, iSekur's own background queries
// (WHO on join, ISON, its chathistory) kept to itself. What it sends goes upstream as is;
// messages go through iSekur, so its tabs, the webchat and the other clients see them too.
#include "bnc.h"
#include <algorithm>
#include <unistd.h>
#include <openssl/crypto.h>
#include <openssl/ssl.h>
#include "web.h"  // check_password, gen_selfsigned

static const size_t MAX_CLIENTS = 16, MAX_OUT = 8 << 20;
static const char* SRV = "isekur";  // the name we answer as
// what clients may ask for. Offered before the login says which network, so whatever the network lacks just
// never comes (extended-join: JOINs with an unknown account and realname)
static const std::set<std::string> OFFER = {"server-time", "message-tags", "echo-message", "batch", "draft/chathistory",
                                            "labeled-response", "away-notify", "account-notify", "account-tag", "chghost",
                                            "setname", "extended-join", "multi-prefix", "userhost-in-names", "invite-notify"};
static const int HIST_MAX = 500;  // CHATHISTORY's limit
static bool is_msg(const Line& l) { return (l.kind == LK::MSG || l.kind == LK::ACTION || l.kind == LK::NOTICE) && !l.nick.empty() && !l.nolog; }

Bnc::~Bnc() { stop(); }

bool Bnc::autostart(const Ini& cfg) {
    std::string v = ini_get(cfg, "bnc", "autostart", "off");
    return v == "on" || v == "true" || v == "yes" || v == "1";
}

void Bnc::stop() {
    for (auto& cl : clients)
        if (cl->reg) gone(*cl, "BNC stopped");
    clients.clear();
    if (ctx) SSL_CTX_free(ctx), ctx = nullptr;
    if (lfd >= 0) close(lfd), lfd = -1;
    Server::tap = nullptr;
    url.clear();
}

bool Bnc::start(std::string& err) {
    if (running()) return true;
    if (!start_(err)) { stop(); return false; }
    Server::tap = [this](Server* s, bool out, const std::string& l) { out ? to_server(s, l) : from_server(s, l); };
    return true;
}

bool Bnc::start_(std::string& err) {
    const Ini& cfg = c.cfg;
    std::string bind = ini_get(cfg, "bnc", "bind", "127.0.0.1");
    int port = atoi(ini_get(cfg, "bnc", "port").c_str());
    if (port <= 0 || port > 65535) port = 6698;
    user = ini_get(cfg, "bnc", "user");
    pass_hash = ini_get(cfg, "bnc", "password_hash");
    if (user.empty() || pass_hash.rfind("pbkdf2$", 0) != 0) {
        err = "[bnc] needs user and password_hash: /set bnc.user name, then /bnc password";
        return false;
    }
    std::string t = ini_get(cfg, "bnc", "tls", "on");
    bool tls = !(t == "off" || t == "false" || t == "no" || t == "0");
    if (tls) {
        std::string crt = ini_get(cfg, "bnc", "cert"), key = ini_get(cfg, "bnc", "key");
        if (crt.empty()) {
            std::string dir = c.cfg_path.substr(0, c.cfg_path.rfind('/') + 1);
            crt = dir + "bnc.crt", key = dir + "bnc.key";
            if (access(crt.c_str(), R_OK) || access(key.c_str(), R_OK)) {
                if (!gen_selfsigned(crt, key, err)) return false;
                c.print(c.bufs.front().get(), LK::INFO, "Generated self-signed BNC certificate " + crt);
            }
        }
        ctx = SSL_CTX_new(TLS_server_method());
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        SSL_CTX_set_mode(ctx, SSL_MODE_RELEASE_BUFFERS);
        if (SSL_CTX_use_certificate_chain_file(ctx, crt.c_str()) != 1 ||
            SSL_CTX_use_PrivateKey_file(ctx, (key.empty() ? crt : key).c_str(), SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(ctx) != 1) {
            err = "BNC TLS: " + ssl_errstr();
            return false;
        }
    }
    lfd = listen_tcp(bind, port, err);
    if (lfd < 0) { err = "BNC listen " + bind + ":" + std::to_string(port) + ": " + err; return false; }
    url = std::string(tls ? "ircs://" : "irc://") + (bind.find(':') != std::string::npos ? "[" + bind + "]" : bind) + ":" + std::to_string(port);
    bool loopback = bind == "127.0.0.1" || bind == "::1" || bind == "localhost";
    if (!tls && !loopback) c.print(c.bufs.front().get(), LK::ERROR, "BNC without TLS on " + bind + ": the password goes over the network in clear");
    return true;
}

bool Bnc::attached() const {
    for (auto& cl : clients)
        if (cl->reg && !cl->conn.closing) return true;
    return false;
}

std::vector<std::string> Bnc::status() {
    if (!running()) return {"BNC is stopped (/bnc start)"};
    std::vector<std::string> out{"BNC on " + url + ", log in as " + user + "/<network>:<password>"};
    time_t now = time(nullptr);
    for (auto& cl : clients)
        if (cl->reg) out.push_back("  " + cl->ip + "  " + (cl->net.empty() ? "(no network)" : cl->net) + " as " + cl->nick + ", for " + fmt_dur((long)(now - cl->since)));
    if (out.size() == 1) out.push_back("  no clients attached");
    return out;
}

Server* Bnc::server(const std::string& net) const {
    for (auto& s : c.servers)
        if (irc_lower(s->cfg.name) == irc_lower(net)) return s.get();
    return nullptr;
}

static std::string iso_time(time_t t) {
    char b[32];
    tm g{};
    gmtime_r(&t, &g);
    strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S.000Z", &g);
    return b;
}

void Bnc::send(Client& cl, const std::string& line, time_t ts, const std::string& tags, const std::string& batch) {
    if (cl.conn.closing) return;
    if (cl.conn.out.size() > MAX_OUT) { cl.conn.out.clear(), cl.conn.closing = true; return; }  // not reading: let go of it
    std::string t;
    auto add = [&](const std::string& x) { t += (t.empty() ? "" : ";") + x; };
    if (!batch.empty() && cl.caps.count("batch")) add("batch=" + batch);
    if (ts && cl.caps.count("server-time")) add("time=" + iso_time(ts));
    for (auto& tg : split(tags, ';')) {
        std::string k = tg.substr(0, tg.find('='));
        if (!k.empty() && (k == "label" ? cl.caps.count("labeled-response") > 0
                                        : cl.caps.count("message-tags") || (k == "account" && cl.caps.count("account-tag"))))
            add(tg);
    }
    cl.conn.out += (t.empty() ? "" : "@" + t + " ") + line + "\r\n";
}

// a parsed line's tags as they go on the wire, less those that are ours to give (time, batch) or iSekur's (label)
static std::string wire_tags(const Msg& m, bool client_only = false) {
    std::string t;
    for (auto& [k, v] : m.tags) {
        if (k == "time" || k == "batch" || k == "label" || (client_only && k[0] != '+')) continue;
        t += (t.empty() ? "" : ";") + k + (v.empty() ? "" : "=" + escape_tag(v));
    }
    return t;
}

// a tab's line as a client sees it: from us (self) or them, to the channel, or to us (me) in a query
static std::string msg_line(const Buffer* b, const Line& l, const std::string& self, const std::string& me, bool stamp) {
    std::string text = (stamp ? "[" + fmt_time(l.ts, "%H:%M") + "] " : "") + l.text;
    if (l.kind == LK::ACTION) text = "\x01" "ACTION " + text + "\x01";
    std::string to = b->type == BT::CHANNEL || l.self ? b->name : me;
    return ":" + (l.self ? self : l.nick) + (l.kind == LK::NOTICE ? " NOTICE " : " PRIVMSG ") + to + " :" + text;
}

// a NAMES entry ("@+nick!user@host" or less) as the client asked: every prefix or the first, the host or not
static std::string name_as(const std::string& n, const std::string& prefixes, bool multi, bool uh) {
    size_t p = n.find_first_not_of(prefixes);
    if (p == std::string::npos) return "";
    std::string nick = n.substr(p);
    return n.substr(0, multi ? p : std::min<size_t>(p, 1)) + (uh ? nick : nick.substr(0, nick.find('!')));
}

// a line from a client, tags and prefix gone (we offer no message-tags)
static std::string bare(const std::string& raw) {
    size_t i = 0;
    for (char lead : {'@', ':'})
        if (i < raw.size() && raw[i] == lead) {
            i = raw.find(' ', i);
            if (i == std::string::npos) return "";
            i = raw.find_first_not_of(' ', i);
            if (i == std::string::npos) return "";
        }
    return raw.substr(i);
}

// our nick!user@host as the channels know it, else just the nick
static std::string self_prefix(Core& c, Server* s) {
    for (auto& b : c.bufs)
        if (b->srv == s && b->type == BT::CHANNEL)
            if (auto it = b->nicks.find(s->lower(s->nick)); it != b->nicks.end() && !it->second.host.empty())
                return s->nick + "!" + it->second.host;
    return s->nick;
}

void Bnc::add_fds(std::vector<pollfd>& v) {
    fd_start = v.size(), fd_count = 0;
    if (lfd < 0) return;
    v.push_back({lfd, POLLIN, 0});
    for (auto& cl : clients) v.push_back({cl->conn.fd, cl->conn.events(), 0});
    fd_count = 1 + clients.size();
}

void Bnc::process(const std::vector<pollfd>& v) {
    if (lfd < 0) return;
    time_t now = time(nullptr);
    std::vector<Client*> dead;
    for (size_t k = 1; k < fd_count && k - 1 < clients.size(); k++) {
        const pollfd& p = v[fd_start + k];
        Client& cl = *clients[k - 1];
        if (!p.revents) continue;
        bool ok = cl.conn.pump(p.revents, cl.in);
        if (!cl.in.empty()) cl.rx = now;
        size_t at = 0, nl;
        while (ok && !cl.conn.closing && (nl = cl.in.find('\n', at)) != std::string::npos) {
            std::string l = cl.in.substr(at, nl - at);
            at = nl + 1;
            while (!l.empty() && l.back() == '\r') l.pop_back();
            if (!l.empty()) line(cl, l);
        }
        cl.in.erase(0, at);
        if (cl.in.size() > 16384) cl.conn.closing = true;  // no line is that long
        if (!ok) dead.push_back(&cl);
    }
    if (v[fd_start].revents & POLLIN)
        for (;;) {
            auto cl = std::make_unique<Client>();
            if (!cl->conn.accept_from(lfd, ctx)) break;
            if (clients.size() >= MAX_CLIENTS) continue;  // dropped: the destructor closes it
            cl->ip = cl->conn.host, cl->since = cl->rx = now;
            clients.push_back(std::move(cl));
        }
    for (auto& cp : clients) {
        Client& cl = *cp;
        if (!cl.reg) {
            if (now - cl.since > 30) dead.push_back(&cl);  // never logged in
            continue;
        }
        Server* s = cl.net.empty() ? nullptr : server(cl.net);
        if (!s && !cl.net.empty()) { send(cl, "ERROR :" + cl.net + " is gone from iSekur"), cl.conn.closing = true; continue; }
        if (now - cl.rx > 240) { dead.push_back(&cl); continue; }  // asleep, or the network went away
        if (now - cl.rx >= 90 && now - cl.pinged >= 90) send(cl, std::string("PING :") + SRV), cl.pinged = now;
        if (!s) continue;  // on no network yet
        if (cl.seen == SS::CONNECTED && s->state != SS::CONNECTED) {  // its channels close; the rejoins open them again
            send(cl, std::string(":") + SRV + " NOTICE " + cl.nick + " :*** Disconnected from " + cl.net + ", iSekur is reconnecting");
            for (auto& [k, ch] : cl.chans) send(cl, ":" + cl.nick + " PART " + ch + " :Disconnected from " + cl.net);
            cl.chans.clear();
        }
        else if (cl.seen != SS::CONNECTED && s->state == SS::CONNECTED) {
            send(cl, std::string(":") + SRV + " NOTICE " + cl.nick + " :*** Connected to " + cl.net);
            if (s->nick != cl.nick) send(cl, ":" + cl.nick + " NICK " + s->nick), cl.nick = s->nick;
        }
        cl.seen = s->state;
    }
    clients.erase(std::remove_if(clients.begin(), clients.end(),
                                 [&](auto& cl) {
                                     if (std::find(dead.begin(), dead.end(), cl.get()) == dead.end()) return false;
                                     if (cl->reg) gone(*cl, cl->conn.err.empty() ? "gone" : cl->conn.err);
                                     pend.erase(std::remove_if(pend.begin(), pend.end(), [&](const Pend& p) { return p.cl == cl.get(); }), pend.end());
                                     for (auto* mp : {&labs, &lab_batches})
                                         for (auto it = mp->begin(); it != mp->end();) it = it->second.cl == cl.get() ? mp->erase(it) : std::next(it);
                                     return true;
                                 }),
                  clients.end());
}

void Bnc::gone(Client& cl, const std::string& why) {
    c.print(c.bufs.front().get(), LK::INFO, "◀ BNC gone: " + cl.ip + " (" + (cl.net.empty() ? "no network" : cl.net) + "), attached " +
                                                fmt_dur((long)(time(nullptr) - cl.since)) + (why == "gone" ? "" : ": " + why));
    cl.reg = false;
}

void Bnc::cap(Client& cl, const Msg& m) {
    std::string sub = m.p(0);
    for (auto& ch : sub) ch = (char)toupper((unsigned char)ch);
    std::string me = cl.nick.empty() ? "*" : cl.nick;
    auto join = [](const std::set<std::string>& v) {
        std::string o;
        for (auto& x : v) o += (o.empty() ? "" : " ") + x;
        return o;
    };
    if (sub == "LS") {
        if (!cl.reg) cl.capneg = true;
        send(cl, std::string(":") + SRV + " CAP " + me + " LS :" + join(OFFER));
    } else if (sub == "REQ") {
        if (!cl.reg) cl.capneg = true;
        auto want = split(m.p(1), ' ');
        bool ok = true;
        for (auto& w : want)
            if (!w.empty() && !OFFER.count(w[0] == '-' ? w.substr(1) : w)) ok = false;
        for (auto& w : want) {
            if (!ok || w.empty()) continue;
            if (w[0] == '-') cl.caps.erase(w.substr(1));
            else cl.caps.insert(w);
        }
        send(cl, std::string(":") + SRV + " CAP " + me + (ok ? " ACK :" : " NAK :") + m.p(1));
    } else if (sub == "LIST") {
        send(cl, std::string(":") + SRV + " CAP " + me + " LIST :" + join(cl.caps));
    } else if (sub == "END") {
        cl.capneg = false;
        if (!cl.reg) login(cl);
    }
}

void Bnc::line(Client& cl, const std::string& raw) {
    Msg m;
    if (!irc_parse(raw, m)) return;
    const std::string& cmd = m.cmd;
    if (cmd == "CAP") { cap(cl, m); return; }
    std::string lab = m.tags.count("label") ? m.tags.at("label") : "", labt = lab.empty() ? "" : "label=" + escape_tag(lab);
    if (cmd == "PING") { send(cl, std::string(":") + SRV + " PONG " + SRV + " :" + m.p(0), 0, labt); return; }
    if (cmd == "PONG") return;
    if (cmd == "QUIT") {  // this client goes; iSekur stays on the network
        send(cl, "ERROR :Detached from iSekur");
        cl.conn.closing = true;
        return;
    }
    if (!cl.reg) {
        if (cmd == "PASS") cl.pass = m.p(0);
        else if (cmd == "NICK") cl.nick = m.p(0), login(cl);
        else if (cmd == "USER") cl.user = m.p(0), login(cl);
        return;
    }
    if (cmd == "PASS" || cmd == "USER") return;
    if (cmd == "PRIVMSG" && irc_lower(m.p(0)) == "*isekur") { control(cl, m.p(1)); return; }
    Server* s = cl.net.empty() ? nullptr : server(cl.net);
    if (!s) {
        if (cl.net.empty() && (cmd == "PRIVMSG" || cmd == "JOIN")) tell(cl, "Not on a network: /msg *isekur help");
        return;
    }
    if (cmd == "CHATHISTORY") { history(cl, s, m, labt); return; }  // ours, never upstream
    if (s->state != SS::CONNECTED) {
        send(cl, std::string(":") + SRV + " NOTICE " + cl.nick + " :*** " + cl.net + " is not connected: not sent");
        return;
    }
    time_t now = time(nullptr);
    c.last_input = now;  // someone's here: no auto-away
    origin = &cl;
    std::string tgt = m.p(0), text = m.p(1);
    if ((cmd == "PRIVMSG" || cmd == "NOTICE" || cmd == "TAGMSG") && s->caps.count("echo-message")) {
        pend.erase(std::remove_if(pend.begin(), pend.end(), [&](const Pend& p) { return now - p.at > 30; }), pend.end());
        pend.push_back({&cl, s->cfg.name, cmd, tgt, text, now});
    }
    // its client tags (+draft/reply, +typing...), where both sides do message-tags
    std::string ct = cl.caps.count("message-tags") && s->caps.count("message-tags") ? wire_tags(m, true) : "";
    if (!ct.empty()) {
        std::string ok;
        for (auto& t : split(ct, ';'))
            if (s->tag_ok(t.substr(1, t.find('=') - 1))) ok += (ok.empty() ? "" : ";") + t;
        ct = ok;
    }
    // labeled-response: the network answers our label, from_server hands that to this client under its own;
    // a network without it can't say which answer is whose: ACK now, the answers come unlabeled
    if (!lab.empty() && s->caps.count("labeled-response")) {
        std::string our = "b" + std::to_string(++lab_n);
        labs[our] = {&cl, lab};
        if (labs.size() > 200) labs.erase(labs.begin());  // ponytail: an unanswered one goes eventually
        ct = "label=" + our + (ct.empty() ? "" : ";" + ct);
    } else if (!lab.empty()) send(cl, std::string(":") + SRV + " ACK", 0, labt);
    if (!ct.empty()) ct = "@" + ct + " ";
    // a message to one channel or nick: through iSekur (its tab shows it, long lines are split, /e2e)
    bool action = text.rfind("\x01" "ACTION ", 0) == 0;
    if (cmd == "PRIVMSG" && m.params.size() >= 2 && tgt.find(',') == std::string::npos && !tgt.empty() && tgt[0] != '*' &&
        (text.empty() || text[0] != '\x01' || action)) {
        Buffer* b = c.find(s, tgt);
        if (!b && !s->is_chan(tgt)) b = c.open(s, BT::QUERY, tgt);
        if (b && (b->type == BT::CHANNEL || b->type == BT::QUERY)) {
            if (action) {
                text = text.substr(8);
                if (!text.empty() && text.back() == '\x01') text.pop_back();
            }
            if (ct.empty()) c.say(b, text, action);
            else {  // with its tags it goes as it is; the tab gets it from the echo, or now
                s->send(ct + bare(raw));
                if (!s->caps.count("echo-message")) c.add(b, Line{now, action ? LK::ACTION : LK::MSG, s->nick, text, true});
            }
            origin = nullptr;
            return;
        }
    }
    std::string out = bare(raw);
    if (!out.empty() && (cmd != "TAGMSG" || !ct.empty())) s->send(ct + out);
    origin = nullptr;
}

void Bnc::login(Client& cl) {
    if (cl.capneg || cl.nick.empty() || cl.user.empty() || cl.reg) return;
    time_t now = time(nullptr);
    std::string u = cl.user, pw = cl.pass, net;
    if (size_t col = pw.find(':'); col != std::string::npos) u = pw.substr(0, col), pw = pw.substr(col + 1);
    if (size_t sl = u.find('/'); sl != std::string::npos) net = u.substr(sl + 1), u = u.substr(0, sl);
    if (size_t at = u.find('@'); at != std::string::npos) u = u.substr(0, at);  // ZNC's user@client
    auto& f = fails[cl.ip];
    auto refuse = [&](const std::string& why) {
        send(cl, std::string(":") + SRV + " 464 " + cl.nick + " :" + why);
        send(cl, "ERROR :Closing link: " + why);
        cl.conn.closing = true;
    };
    if (f.second > now) { refuse("Too many failed logins, try again in " + fmt_dur((long)(f.second - now))); return; }
    // both checks every time, so the timing doesn't say which was wrong
    bool user_ok = u.size() == user.size() && CRYPTO_memcmp(u.data(), user.data(), u.size()) == 0;
    bool pass_ok = check_password(pw, pass_hash);
    if (!user_ok || !pass_ok) {
        if (++f.first >= 5) f.second = now + std::min<time_t>(3600, 60L << std::min(f.first - 5, 6));
        c.print(c.bufs.front().get(), LK::ERROR, "✕ BNC login failed: " + cl.ip + (f.second > now ? ", locked out " + fmt_dur((long)(f.second - now)) : ""));
        refuse("Password incorrect (log in as user/network:password)");
        if (fails.size() > 1000)
            for (auto it = fails.begin(); it != fails.end();) it = it->second.second < now ? fails.erase(it) : std::next(it);
        return;
    }
    fails.erase(cl.ip);
    if (net.empty() && c.servers.size() == 1) net = c.servers.front()->cfg.name;
    Server* s = server(net);
    // no such network: logged in all the same, with none; *isekur says why and picks or adds one
    std::string why = s ? "" : c.servers.empty() ? "iSekur has no networks yet" : net.empty() ? "Which network?" : "No network " + net + " in iSekur";
    cl.reg = true, cl.since = now;
    welcome(cl, s, why);
    c.print(c.bufs.front().get(), LK::INFO, "▶ BNC attached: " + cl.ip + " (" + (s ? cl.net : why + ": no network, /msg *isekur") + ")");
}

static std::string nets_of(const Core& c) {
    std::string o;
    for (auto& sv : c.servers) o += (o.empty() ? "" : ", ") + sv->cfg.name + (sv->state == SS::CONNECTED ? "" : " (not connected)");
    return o.empty() ? "none yet" : o;
}

// *isekur: a query that lists, picks and adds networks, for a client on none (or to switch)
void Bnc::tell(Client& cl, const std::string& text) { send(cl, std::string(":*isekur!isekur@") + SRV + " PRIVMSG " + cl.nick + " :" + text); }

void Bnc::control(Client& cl, const std::string& text) {
    auto w = split(text, ' ');
    std::string verb = w.empty() ? "" : irc_lower(w[0]);
    if (verb == "networks") { tell(cl, "Networks: " + nets_of(c)); return; }
    if (verb == "use" && w.size() > 1) {
        Server* s = server(w[1]);
        if (!s) { tell(cl, "No network " + w[1] + ". Networks: " + nets_of(c)); return; }
        attach(cl, s, "");
        c.print(c.bufs.front().get(), LK::INFO, "▶ BNC attached: " + cl.ip + " (" + cl.net + ")");
        return;
    }
    if (verb == "server" && w.size() > 1) {  // iSekur's own /server: in the config (unsaved, like /set) and connecting
        size_t n = c.servers.size();
        Buffer* was = c.active;
        c.exec(c.bufs.front().get(), "/" + text);
        // ponytail: /server selects the tab it opens or connects; an unchanged tab and no new server: it failed
        Server* s = c.servers.size() > n ? c.servers.back().get() : c.active != was && c.active ? c.active->srv : nullptr;
        if (!s) { tell(cl, "Not added: iSekur's console says why"); return; }
        attach(cl, s, "");
        tell(cl, s->cfg.name + " added; /msg *isekur save keeps it, then log in as " + user + "/" + s->cfg.name + ":<password>");
        c.print(c.bufs.front().get(), LK::INFO, "▶ BNC attached: " + cl.ip + " (" + cl.net + ")");
        return;
    }
    if (verb == "save") { c.exec(c.bufs.front().get(), "/save"); tell(cl, "Saved to iSekur's config"); return; }
    tell(cl, "networks: the networks iSekur has");
    tell(cl, "use <network>: go to one");
    tell(cl, "server <host> [port] [-tls] [-name network]: add one and connect, e.g. server irc.sekurnet.org 6697 -tls -name sekurnet");
    tell(cl, "save: write iSekur's config");
}

void Bnc::welcome(Client& cl, Server* s, const std::string& why) {
    std::string me = s && !s->nick.empty() ? s->nick : cl.nick;
    cl.nick = me;
    std::string net = !s ? "" : s->network.empty() ? s->cfg.name : s->network;
    auto num = [&](const char* code, const std::string& rest) { send(cl, std::string(":") + SRV + " " + code + " " + me + " " + rest); };
    num("001", s ? ":Welcome to " + net + " through iSekur, " + me : ":Welcome to iSekur's bouncer, " + me);
    num("002", std::string(":Your host is ") + SRV + ", running " + version_string());
    num("003", ":This is iSekur's bouncer" + (s ? " for " + s->cfg.name : ""));
    num("004", std::string(SRV) + " iSekur-" ISEKUR_VERSION " iowx " + "beIklmnopstv");
    attach(cl, s, why);
}

// onto network s (from none, or from another: its channels PARTed); none: *isekur says why and what to do
void Bnc::attach(Client& cl, Server* s, const std::string& why) {
    for (auto& [k, ch] : cl.chans) send(cl, ":" + cl.nick + " PART " + ch + " :Leaving " + cl.net);
    cl.chans.clear();
    auto num = [&](const char* code, const std::string& rest) { send(cl, std::string(":") + SRV + " " + code + " " + cl.nick + " " + rest); };
    if (!s) {
        cl.net.clear(), cl.seen = SS::DISCONNECTED;
        num("005", "NETWORK=iSekur CHANTYPES=# PREFIX=(ov)@+ CASEMAPPING=rfc1459 :are supported by this server");
        num("422", ":No network: /msg *isekur help");
        tell(cl, why + ". Networks: " + nets_of(c));
        if (!c.servers.empty()) tell(cl, "/msg *isekur use <network> goes to one; next time log in as " + user + "/<network>:<password>");
        tell(cl, "/msg *isekur server irc.sekurnet.org 6697 -tls -name sekurnet adds a network and connects (help: the rest)");
        return;
    }
    std::string me = s->nick.empty() ? cl.nick : s->nick;
    if (me != cl.nick) send(cl, ":" + cl.nick + " NICK " + me), cl.nick = me;
    cl.net = s->cfg.name, cl.seen = s->state;
    std::string net = s->network.empty() ? s->cfg.name : s->network;
    std::vector<std::string> toks;
    for (auto& [k, val] : s->isupport)
        if (!k.empty() && k[0] != '-' && k != "CHATHISTORY" && k != "MSGREFTYPES")  // ours below
            toks.push_back(val.empty() ? k : k + "=" + val);
    if (s->isupport.empty())
        toks = {"CHANTYPES=" + s->chantypes, "PREFIX=(" + s->prefix_modes + ")" + s->prefix_chars, "NETWORK=" + net};
    toks.push_back("CHATHISTORY=" + std::to_string(HIST_MAX)), toks.push_back("MSGREFTYPES=msgid,timestamp");
    for (size_t i = 0; i < toks.size(); i += 12) {
        std::string l;
        for (size_t j = i; j < toks.size() && j < i + 12; j++) l += toks[j] + " ";
        num("005", l + ":are supported by this server");
    }
    num("375", std::string(":- ") + SRV + " Message of the day -");
    num("372", ":- " + s->cfg.name + (s->state == SS::CONNECTED ? " is connected" : " is not connected: iSekur is reconnecting"));
    num("376", ":End of /MOTD command.");
    std::string umodes = s->umodes;
    if (!umodes.empty() && umodes[0] == '+') umodes.erase(0, 1);
    if (!umodes.empty()) send(cl, ":" + me + " MODE " + me + " :+" + umodes);
    if (s->away) num("306", ":You have been marked as being away");
    std::string self = self_prefix(c, s);
    // a chathistory client asks for what it wants itself
    int backlog = cl.caps.count("draft/chathistory") ? 0 : std::max(0, atoi(ini_get(c.cfg, "bnc", "backlog", "50").c_str()));
    for (auto& bp : c.bufs) {
        Buffer* b = bp.get();
        if (b->srv != s || b->name.empty() || b->name[0] == '*') continue;
        if (b->type == BT::CHANNEL && b->joined) {
            std::string ext;
            if (cl.caps.count("extended-join")) {
                auto me_e = b->nicks.find(s->lower(me));
                bool known = me_e != b->nicks.end();
                ext = " " + (known && !me_e->second.account.empty() ? me_e->second.account : "*") + " :" + (known ? me_e->second.realname : "");
            }
            send(cl, ":" + self + " JOIN " + b->name + ext), cl.chans[s->lower(b->name)] = b->name;
            if (!b->topic.empty()) num("332", b->name + " :" + b->topic);
            std::string names;
            for (auto& [k, e] : b->nicks) {
                std::string n = (cl.caps.count("multi-prefix") ? e.prefix : e.prefix.substr(0, 1)) + e.nick +
                                (cl.caps.count("userhost-in-names") && !e.host.empty() ? "!" + e.host : "");
                if (names.size() + n.size() > 400) num("353", "= " + b->name + " :" + names), names.clear();
                names += (names.empty() ? "" : " ") + n;
            }
            if (!names.empty()) num("353", "= " + b->name + " :" + names);
            num("366", b->name + " :End of /NAMES list.");
        } else if (b->type != BT::QUERY) continue;
        // the tab's last lines: messages only, timed if it took server-time, else with the time in front
        size_t n = 0, from = b->lines.size();
        while (from > 0 && n < (size_t)backlog)
            if (is_msg(b->lines[--from])) n++;
        bool timed = cl.caps.count("server-time");
        for (size_t i = from; i < b->lines.size() && backlog; i++)
            if (const Line& l = b->lines[i]; is_msg(l))
                send(cl, msg_line(b, l, self, me, !timed), l.ts, l.msgid.empty() ? "" : "msgid=" + escape_tag(l.msgid));
    }
}

// what iSekur gets from the network, as the clients on it see it
void Bnc::from_server(Server* s, const std::string& line) {
    bool any = false;
    for (auto& cl : clients) any = any || (cl->reg && cl->net == s->cfg.name);
    if (!any || s->state != SS::CONNECTED) return;  // registering: 001-005 and the MOTD were ours to give
    Msg m;
    if (!irc_parse(line, m)) return;
    const std::string& cmd = m.cmd;
    // an answer to a client's labeled command: to that client alone, under its label (a BATCH of them, in that batch)
    Lab* to = nullptr;
    std::string ref;  // the labeled batch it's in
    if (auto lt = m.tags.find("label"); lt != m.tags.end()) {
        auto it = labs.find(lt->second);
        if (it == labs.end() && cmd == "ACK") return;  // iSekur's own
        if (it != labs.end()) {
            if (cmd == "BATCH" && m.p(0).size() > 1 && m.p(0)[0] == '+') {
                Lab l = it->second;
                labs.erase(it);
                ref = m.p(0).substr(1), lab_batches[ref] = l;
                if (l.cl->caps.count("batch"))
                    send(*l.cl, ":" + m.prefix + " BATCH +" + ref + line.substr(line.find(m.p(0)) + m.p(0).size()), 0, "label=" + escape_tag(l.label));
                return;
            }
            static Lab one;
            one = it->second, to = &one;
            labs.erase(it);
        }
    }
    if (auto bt = m.tags.find("batch"); bt != m.tags.end())
        if (auto it = lab_batches.find(bt->second); it != lab_batches.end()) to = &it->second, ref = bt->second;
    if (cmd == "BATCH" && m.p(0).size() > 1 && m.p(0)[0] == '-')
        if (auto it = lab_batches.find(m.p(0).substr(1)); it != lab_batches.end()) {
            if (it->second.cl->caps.count("batch")) send(*it->second.cl, ":" + m.prefix + " BATCH " + m.p(0));
            lab_batches.erase(it);
            return;
        }
    // what iSekur asks for itself, and batches (unwrapped)
    static const std::set<std::string> drop = {"PING", "PONG", "CAP", "AUTHENTICATE", "BATCH", "MARKREAD", "CHATHISTORY",
                                               "REDACT", "002", "003", "004", "005"};
    if (drop.count(cmd)) return;
    // lines only clients that asked for them get
    static const std::map<std::string, std::string> need = {{"AWAY", "away-notify"}, {"ACCOUNT", "account-notify"},
                                                            {"CHGHOST", "chghost"}, {"SETNAME", "setname"}};
    auto nd = need.find(cmd);
    if (auto bt = m.tags.find("batch"); bt != m.tags.end() && s->hist_batches.count(bt->second)) return;  // history replays
    // our own line back (echo-message): to every client but one that sent it and didn't ask for echoes
    bool echo = (cmd == "PRIVMSG" || cmd == "NOTICE" || cmd == "TAGMSG") && !m.nick.empty() && s->lower(m.nick) == s->lower(s->nick);
    Client* from = echo ? take(s, cmd, m.p(0), m.p(1)) : nullptr;
    // iSekur's WHOX after a join (token 777) and its end; a client's own /who of that channel in the same
    // moment loses its 315 to it (ponytail: the replies still come)
    // (a client iSekur uses 777 too: its WHOX of a channel ours isn't asking about goes through)
    if ((cmd == "354" && m.p(1) == "777" && s->who_auto.count(s->lower(m.p(2)))) || (cmd == "315" && s->who_auto.count(s->lower(m.p(1))))) return;
    if (cmd == "303" && s->ison_wait > 0) return;
    std::string out = line[0] == '@' ? line.substr(line.find_first_not_of(' ', line.find(' '))) : line;  // tags: send() puts back the asked for
    // JOIN with or without extended-join's account and realname ("*" and "" when the network didn't say)
    std::string join_ext = cmd == "JOIN" ? ":" + m.prefix + " JOIN " + m.p(0) + " " + (m.params.size() >= 3 ? m.p(1) : "*") + " :" + m.p(2) : "";
    if (cmd == "JOIN") out = ":" + m.prefix + " JOIN " + m.p(0);
    auto names = [&](bool multi, bool uh) {  // a 353 as one client asked for it
        std::string ns;
        for (auto& n : split(m.p(3), ' '))
            if (std::string a = name_as(n, s->prefix_chars, multi, uh); !a.empty()) ns += (ns.empty() ? "" : " ") + a;
        return ":" + m.prefix + " 353 " + m.p(0) + " " + m.p(1) + " " + m.p(2) + " :" + ns;
    };
    time_t ts = m.tags.count("time") ? parse_server_time(m.tags["time"]) : 0;
    std::string tags = wire_tags(m);
    for (auto& cl : clients) {
        if (!cl->reg || cl->net != s->cfg.name) continue;
        if (to && cl.get() != to->cl && !echo) continue;  // someone else's answer
        bool sender = cl.get() == from || (to && cl.get() == to->cl);
        if (echo && sender && !cl->caps.count("echo-message")) {  // its own line: ACK the label instead
            if (to && ref.empty()) send(*cl, std::string(":") + SRV + " ACK", 0, "label=" + escape_tag(to->label));
            continue;
        }
        if ((cmd == "TAGMSG" && !cl->caps.count("message-tags")) || (nd != need.end() && !cl->caps.count(nd->second))) continue;
        if (cmd == "INVITE" && s->lower(m.p(0)) != s->lower(cl->nick) && !cl->caps.count("invite-notify")) continue;
        bool me = s->lower(m.nick) == s->lower(cl->nick);
        if (cmd == "NICK" && me) cl->nick = m.p(0);
        if (cmd == "JOIN" && me) cl->chans[s->lower(m.p(0))] = m.p(0);
        if ((cmd == "PART" && me) || (cmd == "KICK" && s->lower(m.p(1)) == s->lower(cl->nick))) cl->chans.erase(s->lower(m.p(0)));
        std::string l = cmd == "JOIN" && cl->caps.count("extended-join") ? join_ext
                      : cmd == "353" && m.params.size() >= 4 ? names(cl->caps.count("multi-prefix"), cl->caps.count("userhost-in-names"))
                                                             : out;
        bool mine = to && cl.get() == to->cl;
        send(*cl, l, ts, mine && ref.empty() ? tags + (tags.empty() ? "" : ";") + "label=" + escape_tag(to->label) : tags, mine ? ref : "");
    }
}

// the client that sent this line we see echoed, if one did (say() may have cut it in parts: those match in turn)
Bnc::Client* Bnc::take(Server* s, const std::string& cmd, const std::string& tgt, const std::string& text) {
    for (auto it = pend.begin(); it != pend.end(); ++it) {
        if (it->net != s->cfg.name || it->cmd != cmd || s->lower(it->tgt) != s->lower(tgt)) continue;
        Client* who = it->cl;
        if (it->text == text || text.empty()) pend.erase(it);
        else if (text.size() < it->text.size() && it->text.compare(0, text.size(), text) == 0)
            it->text.erase(0, it->text.find_first_not_of(' ', text.size()));
        else continue;
        return who;
    }
    return nullptr;
}

// CHATHISTORY LATEST|BEFORE|AFTER|AROUND|BETWEEN target ref [ref] limit, and TARGETS, from the lines iSekur keeps
// (its scrollback): what a client that does chathistory (another iSekur, Goguma...) loads on join and scrolling up.
// Refs: *, timestamp= (to the second: Lines keep no more) or msgid= (where the network gave one)
void Bnc::history(Client& cl, Server* s, const Msg& m, const std::string& label) {
    std::string sub = m.p(0);
    for (auto& ch : sub) ch = (char)toupper((unsigned char)ch);
    std::string id = "h" + std::to_string(++batch_n), me = cl.nick, self = self_prefix(c, s);
    auto lim = [&](size_t i) { int n = atoi(m.p(i).c_str()); return (size_t)std::min(n <= 0 ? HIST_MAX : n, HIST_MAX); };
    auto fail = [&](const std::string& code, const std::string& why) { send(cl, std::string(":") + SRV + " FAIL CHATHISTORY " + code + " " + sub + " :" + why); };
    auto when = [](const std::string& ref) -> time_t { return ref.rfind("timestamp=", 0) == 0 ? parse_server_time(ref.substr(10)) : 0; };
    if (sub == "TARGETS") {  // TARGETS ts1 ts2 limit: the tabs with messages between them, oldest first
        time_t a = when(m.p(1)), z = when(m.p(2));
        if (a > z) std::swap(a, z);
        std::vector<std::pair<time_t, std::string>> hits;
        for (auto& b : c.bufs) {
            if (b->srv != s || (b->type != BT::CHANNEL && b->type != BT::QUERY) || b->name.empty() || b->name[0] == '*') continue;
            for (auto it = b->lines.rbegin(); it != b->lines.rend(); ++it)
                if (is_msg(*it)) {
                    if (it->ts >= a && it->ts <= z) hits.push_back({it->ts, b->name});
                    break;
                }
        }
        std::sort(hits.begin(), hits.end());
        if (hits.size() > lim(3)) hits.resize(lim(3));
        send(cl, std::string(":") + SRV + " BATCH +" + id + " draft/chathistory-targets", 0, label);
        for (auto& [t, n] : hits) send(cl, std::string(":") + SRV + " CHATHISTORY TARGETS " + n + " " + iso_time(t), 0, "", id);
        send(cl, std::string(":") + SRV + " BATCH -" + id);
        return;
    }
    static const std::set<std::string> subs = {"LATEST", "BEFORE", "AFTER", "AROUND", "BETWEEN"};
    if (!subs.count(sub)) { fail("INVALID_PARAMS", "Unknown subcommand"); return; }  // ponytail: FAILs go unlabeled
    std::string tgt = m.p(1);
    if (tgt.empty()) { fail("INVALID_PARAMS", "No target"); return; }
    Buffer* b = c.find(s, tgt);
    std::vector<const Line*> ms;
    if (b && (b->type == BT::CHANNEL || b->type == BT::QUERY))
        for (auto& l : b->lines)
            if (is_msg(l)) ms.push_back(&l);
    size_t n = ms.size();
    // where a ref falls: the first message at or after it (before), the first one past it (after)
    auto at = [&](const std::string& ref, bool past) -> size_t {
        if (ref == "*") return past ? 0 : n;
        if (ref.rfind("msgid=", 0) == 0) {
            for (size_t i = 0; i < n; i++)
                if (ms[i]->msgid == ref.substr(6)) return past ? i + 1 : i;
            return n;  // not kept (anymore): nothing past it, everything before
        }
        time_t t = when(ref);
        size_t i = 0;
        while (i < n && (past ? ms[i]->ts <= t : ms[i]->ts < t)) i++;
        return i;
    };
    size_t from = 0, to = 0;
    if (sub == "LATEST") to = n, from = std::max(m.p(2) == "*" ? 0 : at(m.p(2), true), n > lim(3) ? n - lim(3) : 0);
    else if (sub == "BEFORE") to = at(m.p(2), false), from = to > lim(3) ? to - lim(3) : 0;
    else if (sub == "AFTER") from = std::min(at(m.p(2), true), n), to = std::min(n, from + lim(3));
    else if (sub == "AROUND") {
        size_t mid = at(m.p(2), false);
        from = mid > lim(3) / 2 ? mid - lim(3) / 2 : 0, to = std::min(n, from + lim(3));
    } else {  // BETWEEN: from the first ref towards the second; backwards when the first is the later one
        bool back = when(m.p(2)) > when(m.p(3)) && m.p(2).rfind("timestamp=", 0) == 0;
        const std::string &r1 = back ? m.p(3) : m.p(2), &r2 = back ? m.p(2) : m.p(3);
        from = at(r1, true), to = std::max(from, at(r2, false));
        if (to - from > lim(4)) back ? from = to - lim(4) : to = from + lim(4);
    }
    send(cl, std::string(":") + SRV + " BATCH +" + id + " chathistory " + tgt, 0, label);
    for (size_t i = from; i < to && i < n; i++)
        send(cl, msg_line(b, *ms[i], self, me, false), ms[i]->ts, ms[i]->msgid.empty() ? "" : "msgid=" + escape_tag(ms[i]->msgid), id);
    send(cl, std::string(":") + SRV + " BATCH -" + id);
}

// what goes to the network: our messages, shown to the clients as sent by us. With echo-message the
// server's echo does that (from_server), with its msgid and time
void Bnc::to_server(Server* s, const std::string& line) {
    if (s->caps.count("echo-message")) return;
    Msg m;
    if (!irc_parse(line, m) || (m.cmd != "PRIVMSG" && m.cmd != "NOTICE" && m.cmd != "TAGMSG") || m.params.empty()) return;
    std::string text = m.p(1);
    if (m.cmd == "NOTICE" && !text.empty() && text[0] == '\x01') return;  // CTCP replies
    std::string self, tags = wire_tags(m, true), rest = m.cmd == "TAGMSG" ? "" : " :" + mask_services(m.p(0), text);
    for (auto& cl : clients) {
        if (!cl->reg || cl->net != s->cfg.name || (cl.get() == origin && !cl->caps.count("echo-message"))) continue;
        if (m.cmd == "TAGMSG" && !cl->caps.count("message-tags")) continue;
        if (self.empty()) self = self_prefix(c, s);
        send(*cl, ":" + self + " " + m.cmd + " " + m.p(0) + rest, time(nullptr), tags);
    }
}
