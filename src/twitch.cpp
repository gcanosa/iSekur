// Twitch chat: IRC with its own CAPs, tags and commands (irc.chat.twitch.tv).
// The hooks in irc.cpp and core.cpp only run for a server whose host twitch_host()
// says is Twitch; make notwitch builds this with ISEKUR_NO_TWITCH: always false.
#include <cstring>
#include "core.h"

#ifndef ISEKUR_NO_TWITCH
bool twitch_host(const std::string& h) { return h.size() >= 9 && h.compare(h.size() - 9, 9, "twitch.tv") == 0; }

// The login is lowercase and the password an OAuth token. Twitch doesn't list its
// CAPs in CAP LS (nor holds registration): ask for them straight away.
void twitch_login(Server* s) {
    s->nick = irc_lower(s->nick);
    std::string pass = s->cfg.pass;
    if (!pass.empty() && pass.rfind("oauth:", 0) != 0) pass = "oauth:" + pass;
    if (!pass.empty()) s->send("PASS " + pass);
    s->send("CAP REQ :twitch.tv/tags twitch.tv/commands twitch.tv/membership"), s->cap_done = true;
}

// A Twitch server tab opens with this: "TWITCH" in plain block letters (the same
// font as iSekur's banner, not Twitch's logo or wordmark) in a purple gradient,
// shadow darker. mIRC \x04 colors, so the webchat shows it too.
std::vector<std::string> twitch_banner() {
    static const char* art[] = {
        u8"████████╗██╗    ██╗██╗████████╗ ██████╗██╗  ██╗",
        u8"╚══██╔══╝██║    ██║██║╚══██╔══╝██╔════╝██║  ██║",
        u8"   ██║   ██║ █╗ ██║██║   ██║   ██║     ███████║",
        u8"   ██║   ██║███╗██║██║   ██║   ██║     ██╔══██║",
        u8"   ██║   ╚███╔███╔╝██║   ██║   ╚██████╗██║  ██║",
        u8"   ╚═╝    ╚══╝╚══╝ ╚═╝   ╚═╝    ╚═════╝╚═╝  ╚═╝"};
    static const char* fill[] = {"bf94ff", "a970ff", "9146ff", "9146ff", "7d2ff0", "6a1fd6"};  // top lighter
    std::vector<std::string> out;
    for (int y = 0; y < 6; y++) {
        std::string s = art[y], line, cur;
        for (size_t i = 0; i < s.size();) {
            uint32_t cp = utf8_next(s, i);
            std::string want = cp == ' ' ? cur : std::string("\x04") + (cp == 0x2588 ? fill[y] : "4b2a8a");
            if (want != cur) line += want, cur = want;
            utf8_put(line, cp);
        }
        out.push_back(line);
    }
    out.push_back("\x03" "14" "Twitch chat over IRC \xc2\xb7 iSekur is not affiliated with or endorsed by Twitch");
    out.push_back("");
    return out;
}

// Twitch tags on a message: display-name (when it's only the login's case), color,
// badges as @ (broadcaster, mod) or + (vip) in the nick list. Speakers join the
// nick list too: big channels send no JOINs.
bool twitch_user(Buffer* b, const Msg& m, Line& l) {  // true: nick list changed
    auto tag = [&](const char* k) { auto t = m.tags.find(k); return t == m.tags.end() ? std::string() : t->second; };
    std::string dn = tag("display-name"), badges = "," + tag("badges");
    if (!dn.empty() && irc_lower(dn) == irc_lower(l.nick)) l.nick = dn;
    if (!tag("color").empty()) l.color = parse_color(tag("color"));
    if (b->type != BT::CHANNEL) return false;
    std::string pfx = badges.find(",broadcaster/") != std::string::npos || badges.find(",moderator/") != std::string::npos ? "@"
                      : badges.find(",vip/") != std::string::npos ? "+" : "";
    NickEntry& e = b->nicks[irc_lower(m.nick)];
    if (e.nick == l.nick && e.prefix == pfx) return false;
    e.nick = l.nick, e.prefix = pfx;
    return true;
}

// twitch.tv/commands: Twitch's own events, as lines in the channel
bool Core::twitch_handle(Server* s, const Msg& m, time_t ts) {
    auto line = [&](Buffer* b, LK k, const std::string& nick, const std::string& text) { add(b, Line{ts, k, nick, text}); };
    const std::string& cmd = m.cmd;
    auto tag = [&](const char* k) { auto t = m.tags.find(k); return t == m.tags.end() ? std::string() : t->second; };
    Buffer* b = s->is_chan(m.p(0)) ? find(s, m.p(0)) : nullptr;
    if (cmd == "USERNOTICE") {  // subs, gifts, raids, announcements
        if (!b) return true;
        std::string txt = tag("system-msg"), who = tag("display-name");
        if (txt.empty()) txt = tag("msg-id");
        if (!m.p(1).empty()) txt += (txt.empty() ? "" : " \xe2\x80\x94 ") + std::string("\x02") + (who.empty() ? tag("login") : who) + "\x02: " + m.p(1);
        line(b, LK::INFO, "", "\xe2\x98\x85 " + txt);
        return true;
    }
    if (cmd == "CLEARCHAT") {  // a timeout, a ban, or the whole chat cleared
        if (!b) return true;
        std::string d = tag("ban-duration");
        line(b, LK::MODE, "", m.p(1).empty() ? "Chat was cleared by a moderator"
                              : m.p(1) + (d.empty() ? " was banned" : " was timed out for " + d + "s"));
        return true;
    }
    if (cmd == "CLEARMSG") {
        if (b) line(b, LK::MODE, "", "A message from " + tag("login") + " was deleted: " + m.p(1));
        return true;
    }
    if (cmd == "ROOMSTATE") {  // all settings on join (only those on shown), then one per change
        if (!b) return true;
        bool full = m.tags.count("emote-only") && m.tags.count("slow");
        static const char* R[][2] = {{"emote-only", "emote-only"}, {"followers-only", "followers-only"},
                                     {"r9k", "unique-chat"}, {"slow", "slow mode"}, {"subs-only", "subscribers-only"}};
        std::string txt;
        for (auto& r : R) {
            auto t = m.tags.find(r[0]);
            if (t == m.tags.end()) continue;
            bool off = t->second == "0" || t->second == "-1";
            if (off && full) continue;
            std::string v = r[1];
            if (!off && !strcmp(r[0], "slow")) v += " " + t->second + "s";
            if (!off && !strcmp(r[0], "followers-only") && t->second != "0") v += " " + t->second + "m";
            txt += (txt.empty() ? "" : ", ") + v + (off ? " off" : "");
        }
        if (!txt.empty()) line(b, LK::MODE, "", "Room: " + txt);
        return true;
    }
    if (cmd == "RECONNECT") { disconnect(s, "Twitch asked us to reconnect"); return true; }
    if (cmd == "GLOBALUSERSTATE") {  // logged in; on a first login with no channels yet: what now
        for (auto& bp : bufs)
            if (bp->srv == s && bp->type == BT::CHANNEL) return true;
        print(s->status, LK::INFO, "Logged in to Twitch. Join a stream's chat with \x02/join streamer\x02, the name in "
                                   "twitch.tv/streamer (# and lowercase are added for you); /part leaves it and closes its tab");
        print(s->status, LK::INFO, "\x02/autojoin add #streamer\x02 here, then /save, rejoins it on every start");
        return true;
    }
    if (cmd == "USERSTATE" || cmd == "HOSTTARGET") return true;
    if (cmd == "NOTICE" && (m.p(1).find("Login authentication failed") != std::string::npos ||
                            m.p(1).find("Improperly formatted auth") != std::string::npos))
        print(s->status, LK::ERROR, "Twitch didn't take the token (expired, or not a chat:read chat:edit one): "
                                    "/twitch token shows how to get one");  // then the NOTICE itself, as usual
    return false;
}

// how to get an OAuth token: a Twitch app of your own and its sign-in page
static void token_help(Core& c, Buffer* b) {
    for (const char* l : {
             "\x02How to get a Twitch token\x02 (once; Twitch expires it after a while, then do it again):",
             " 1. https://dev.twitch.tv/console \xe2\x86\x92 Register Your Application: any name, OAuth Redirect URL "
             "\x02http://localhost\x02 (exactly), Category Chat Bot, Client Type Public",
             " 2. Copy the app's Client ID and open this, with it in place of YOUR_CLIENT_ID:",
             "    https://id.twitch.tv/oauth2/authorize?response_type=token&client_id=YOUR_CLIENT_ID"
             "&redirect_uri=http://localhost&scope=chat:read+chat:edit",
             " 3. Approve. The browser then lands on a localhost page that doesn't load: that's fine. From its address "
             "copy what comes after \x02" "access_token=\x02, up to the next &",
             " 4. /twitch and paste it when it asks for the token (\"oauth:\" is added for you)"})
        c.print(b, LK::INFO, l);
}

// /twitch: the Twitch tab (made if needed): connect, or ask for what's missing.
// /twitch token: how to get a token
void Core::twitch_cmd(Buffer* b, const std::string& arg) {
    if (arg == "token") { token_help(*this, b); return; }
    Server* t = nullptr;
    for (auto& sv : servers)
        if (sv->twitch()) { t = sv.get(); break; }
    if (!t) {
        std::string name;
        for (auto& [sec, kv] : cfg)
            if (sec.rfind("server.", 0) == 0 && twitch_host(ini_get(cfg, sec, "host"))) { name = sec.substr(7); break; }
        if (name.empty()) {  // remembered like /server does: /save keeps it
            name = "twitch";
            for (auto& [k, v] : std::map<std::string, std::string>{{"host", "irc.chat.twitch.tv"}, {"port", "6697"}, {"tls", "true"}})
                cfg["server.twitch"][k] = unsaved["server.twitch"][k] = v;
        }
        t = add_server(server_cfg(name));
    }
    select(t->status);
    if (t->state != SS::DISCONNECTED) return;
    std::string sec = "server." + t->cfg.name;
    bool has_nick = !ini_get(cfg, sec, "nick").empty(), has_pass = !ini_get(cfg, sec, "password").empty();
    auto go = [this, t, sec]() {
        t->cfg = server_cfg(t->cfg.name);  // what was just typed in
        if (unsaved.count(sec)) print(t->status, LK::INFO, "Not saved yet: /save keeps it for the next start");
        connect(t);
    };
    if (has_nick && has_pass) {
        ask_pass(t->status, "answer (y/n)", [this, t](const std::string& a) {
            if (a[0] == 'y' || a[0] == 'Y') t->cfg = server_cfg(t->cfg.name), connect(t);
            else print(t->status, LK::INFO, "Not connected: /twitch or /connect " + t->cfg.name + " when you want");
        }, false, "Connect to Twitch?", {"as " + ini_get(cfg, sec, "nick") + " on " + t->cfg.host});
        return;
    }
    print(t->status, LK::INFO, "To connect, Twitch needs your login and an OAuth token with the chat:read and chat:edit scopes:");
    if (!has_nick) print(t->status, LK::INFO, "  /set " + sec + ".nick yourlogin");
    if (!has_pass) print(t->status, LK::INFO, "  /set " + sec + ".password oauth:yourtoken");
    print(t->status, LK::INFO, "then /save and /twitch again, or type them in the box now");
    if (!has_pass) token_help(*this, t->status);
    auto token = [this, t, sec, go]() {
        ask_pass(t->status, "OAuth token", [this, sec, go](const std::string& p) {
            cfg[sec]["password"] = unsaved[sec]["password"] = p;
            go();
        }, true, "Twitch", {"a token with chat:read and chat:edit", "\"oauth:\" is added if you leave it out"});
    };
    if (has_nick) token();
    else ask_pass(t->status, "Twitch login", [this, sec, has_pass, token, go](const std::string& n) {
        cfg[sec]["nick"] = unsaved[sec]["nick"] = irc_lower(n);
        has_pass ? go() : token();
    }, false, "Twitch", {"your Twitch username, the one you log in with"});
}

#else  // make notwitch
bool twitch_host(const std::string&) { return false; }
void twitch_login(Server*) {}
std::vector<std::string> twitch_banner() { return {}; }
bool twitch_user(Buffer*, const Msg&, Line&) { return false; }
bool Core::twitch_handle(Server*, const Msg&, time_t) { return false; }
void Core::twitch_cmd(Buffer* b, const std::string&) {
    print(b, LK::ERROR, "This iSekur was built without Twitch support (make notwitch): build it with plain make for /twitch");
}
#endif
