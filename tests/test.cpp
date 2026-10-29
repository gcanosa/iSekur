// Self-checks for the parsing code: run with `make test`.
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <unistd.h>
#include "core.h"
#include "session.h"
#include "web.h"

static std::string client_frame(const std::string& p) {  // masked, like a browser
    std::string f = ws_frame(p);
    f[1] = (char)(f[1] | 0x80);
    size_t hdr = f.size() - p.size();
    const char mask[4] = {0x12, 0x34, 0x56, 0x78};
    std::string body = p;
    for (size_t i = 0; i < body.size(); i++) body[i] ^= mask[i % 4];
    return f.substr(0, hdr) + std::string(mask, 4) + body;
}

int main() {
    Msg m;
    assert(irc_parse("@time=2024-01-02T03:04:05.000Z;msgid=a\\sb :nick!u@h PRIVMSG #chan :hello there", m));
    assert(m.cmd == "PRIVMSG" && m.nick == "nick" && m.p(0) == "#chan" && m.p(1) == "hello there");
    assert(m.tags["msgid"] == "a b");
    assert(parse_server_time(m.tags["time"]) == 1704164645);
    assert(irc_parse("PING :x y", m) && m.cmd == "PING" && m.p(0) == "x y" && m.prefix.empty());
    assert(irc_parse(":srv 005 me PREFIX=(ov)@+ CHANTYPES=# :are supported", m) && m.params.size() == 4);
    assert(irc_parse("ping", m) && m.cmd == "PING");
    assert(irc_lower("Nick[A]\\~") == "nick{a}|^");

    assert(mirc_strip("\x02" "bold\x02 \x03" "4,1red\x03 \x04" "ff0000x\x0f.") == "bold red x.");
    Style seen;
    mirc_walk("\x03" "12,04Z", {}, [&](uint32_t cp, const Style& s) { if (cp == 'Z') seen = s; });
    assert(seen.fg == 0x0000fc && seen.bg == 0xff0000);

    size_t i = 0;
    assert(utf8_next("\xc3\xa9", i) == 0xe9 && i == 2);
    i = 0;
    assert(utf8_next("\xe9x", i) == 0xe9 && i == 1);  // Latin-1 fallback
    i = 0;
    assert(utf8_next("\xc0\x80", i) == 0xc0);          // overlong rejected

    Ini ini = ini_parse("# c\n[a]\nk = v ; note\nc = #chan\n[b]\nx=1\n");
    assert(ini["a"]["k"] == "v" && ini["a"]["c"] == "#chan" && ini_get(ini, "b", "x") == "1");
    assert(ini_get(ini, "b", "missing", "d") == "d");
    assert(ini_get(ini_parse("[a]\nk =    ; placeholder\n"), "a", "k", "d") == "d");  // empty = default
    {  // the banner keeps its art under the colors, with or without a theme
        Theme th;
        th.nicks = {0xff0000, 0x00ff00};
        th.el["timestamp"].fg = 0x333333;
        for (const Theme& t : {th, Theme{}}) {
            auto bl = banner_lines(t);
            assert(bl.size() == 6 && mirc_strip(bl[0]).rfind(u8"██╗███████╗", 0) == 0);
        }
        assert(banner_lines(th)[0].rfind("\x04" "ff0000", 0) == 0);
        th.el["default"].fg = 0xeeeeee;
        assert(banner_lines(th, true)[0].rfind("\x04" "eeeeee", 0) == 0 && banner_lines(th, true)[0].find("808080") != std::string::npos);
    }
    {  // first-run config: every setting, no example servers, no placeholder nick
        Ini f = ini_parse(fresh_config());
        for (auto& [sec, kv] : f) assert(sec.rfind("server.", 0) != 0);
        assert(f["general"].count("nick") && f["general"]["nick"].empty() && f["web"]["user"].empty());
        assert(f["general"]["bell_notifications"] == "on" && f["general"].count("log_dir") && f["web"]["port"] == "8443");
    }
    assert(parse_color("#ff8800") == 0xff8800 && parse_color("default") == -1 && parse_color("196") == 0xff0000);

    assert(unb64(b64("hello!")) == "hello!" && b64("") == "" && unb64("!!!") == "");

    {  // /e2e: same password opens, a wrong one or a flipped byte is caught
        std::string k = e2e_key("hunter2", "0123456789abcdef"), bad = e2e_key("hunter3", "0123456789abcdef"), out;
        assert(k.size() == 32 && k == e2e_key("hunter2", "0123456789abcdef") && k != bad);
        std::string sealed = e2e_seal(k, "hola \x01" "ACTION x\x01");
        assert(sealed != e2e_seal(k, "hola \x01" "ACTION x\x01"));  // fresh nonce
        assert(e2e_open(k, sealed, out) && out == "hola \x01" "ACTION x\x01");
        assert(!e2e_open(bad, sealed, out) && out.empty());
        std::string raw = unb64(sealed);
        raw[14] ^= 1;
        assert(!e2e_open(k, b64(raw), out) && !e2e_open(k, "short", out));
        assert(("+iSE " + e2e_seal(k, std::string(259, 'x'))).size() <= 400);  // say() chunk fits
        // channels: the channel name is AAD, a line copied to another channel doesn't open
        std::string cs = e2e_seal(k, "hi", "#a");
        assert(e2e_open(k, cs, out, "#a") && out == "hi");
        assert(!e2e_open(k, cs, out, "#b") && !e2e_open(k, cs, out));
    }

    int op;
    std::string p;
    std::string big(70000, 'x');
    for (const std::string& msg : {std::string("hi"), std::string(300, 'y'), big}) {
        std::string f = client_frame(msg);
        assert(ws_parse(f, op, p) == (long)f.size() && op == 1 && p == msg);
        assert(ws_parse(f.substr(0, f.size() - 1), op, p) == 0);  // incomplete
    }
    assert(ws_parse(ws_frame("unmasked"), op, p) == -1);
    std::string part = client_frame("frag"), ping = client_frame("x");
    part[0] = 0x01;  // text, not FIN
    ping[0] = (char)0x09;  // ping without FIN: control frames can't be split
    bool fin = true;
    assert(ws_parse(part, op, p) == -1);  // callers that don't handle fragments
    assert(ws_parse(part, op, p, &fin) == (long)part.size() && !fin && op == 1 && p == "frag");
    assert(ws_parse(ping, op, p, &fin) == -1);

    assert(json_str("a\"b\\\n\xe9") == "\"a\\\"b\\\\\\u000a\xc3\xa9\"");
    auto j = json_flat("{\"t\":\"input\", \"buf\": 12, \"text\":\"x\\\"y\\u00e9\\ud83d\\ude00\"}");
    assert(j["t"] == "input" && j["buf"] == "12" && j["text"] == "x\"y\xc3\xa9\xf0\x9f\x98\x80");

    std::string h = hash_password("s3cret");
    assert(check_password("s3cret", h) && !check_password("s3cre", h) && !check_password("x", "junk"));

    assert(safe_name("../../etc/passwd") == "_.._.._etc_passwd" && safe_name("#Chan[x]") == "#chan{x}");
    assert(safe_name(".hidden") == "_.hidden" && safe_name("") == "_");

    Line ll;
    assert(parse_log_line("2026-10-27 19:09:42 <nick> hi > there", ll) && ll.kind == LK::MSG && ll.nick == "nick" && ll.text == "hi > there");
    assert(parse_log_line("2026-10-27 19:09:42 * nick waves hi", ll) && ll.kind == LK::ACTION && ll.text == "waves hi");
    assert(parse_log_line("2026-10-27 19:09:42 -serv- note", ll) && ll.kind == LK::NOTICE && ll.nick == "serv");
    assert(parse_log_line("2026-10-27 19:09:42 -!- x joined", ll) && ll.kind == LK::INFO && ll.text == "x joined");
    assert(!parse_log_line("--- Log opened 2026-10-27 19:09:12", ll));

    Ini ch;
    ch["general"]["theme"] = "neon";
    ch["general"]["log"] = "on";
    ch["web"]["port"] = "9000";
    std::string up = ini_update("# top\n[general]\ntheme = default ; old\n\n[server.x]\nhost = h\n", ch);
    assert(up == "# top\n[general]\ntheme = neon    ; old\nlog = on\n\n[server.x]\nhost = h\n\n[web]\nport = 9000\n");
    assert(ini_parse(up)["general"]["log"] == "on" && ini_parse(up)["server.x"]["host"] == "h");

    Core core;
    core.cfg["general"]["ignore"] = "spam*!*@* *!*@*.Evil.Host";
    assert(core.ignored("SpamBot!u@h") && core.ignored("x!y@a.evil.host"));
    assert(!core.ignored("friend!u@good.host") && !core.ignored("irc.server.net"));

    Buffer* home = core.open(nullptr, BT::STATUS, "isekur");  // /grep: case-insensitive, nick or text
    core.add(home, Line{0, LK::MSG, "Bob", "Hello \x02World\x02"});
    core.add(home, Line{0, LK::MSG, "eve", "unrelated"});
    core.exec(home, "/grep bob");
    assert(home->lines.size() == 4 && home->lines[2].text.find("1 match ") != std::string::npos);
    assert(home->lines[3].nolog && home->lines[3].text.find("<Bob> Hello World") != std::string::npos);
    core.exec(home, "/grep world");  // earlier results are not matched again
    assert(home->lines.size() == 6);

    // command reference: unique names, every one with examples, groups listed in one run each
    std::set<std::string> names, groups;
    std::string last_group;
    for (auto& k : commands()) {
        assert(names.insert(k.name).second && !k.examples.empty() && *k.summary && *k.details);
        if (last_group != k.group) assert(groups.insert(k.group).second), last_group = k.group;
    }
    assert(find_command("J") && std::string(find_command("j")->name) == "join" && !find_command("nope"));
    size_t before = home->lines.size();
    core.exec(home, "/help /join");  // usage, aliases, details, "Examples", 3 examples
    assert(home->lines.size() == before + 7 && home->lines.back().nolog);
    assert(mirc_strip(home->lines[before].text).rfind("/join #channel [key]", 0) == 0);
    core.exec(home, "/help bogus");
    assert(mirc_strip(home->lines.back().text).find("No such command: /bogus") == 0);

    // channel op commands against a fake connected server; flood_ms 0 = lines go out at once
    ServerCfg tc;
    tc.name = "t", tc.nick = "me";
    Server* ts = core.add_server(tc);
    ts->state = SS::CONNECTED, ts->flood_ms = 0;
    ts->prefix_modes = "qaohv", ts->prefix_chars = "~&@%+";
    Buffer* chb = core.open(ts, BT::CHANNEL, "#c");
    chb->joined = true;
    auto feed = [&](const std::string& l) { Msg mm; assert(irc_parse(l, mm)); core.handle(ts, mm); };
    auto sent = [&]() { std::string o = ts->conn.out; ts->conn.out.clear(); return o; };
    feed(":me!u@h NICK XXX");  // a reconnect keeps the nick we had, not cfg.nick
    assert(ts->nick == "XXX" && ts->last_nick == "XXX");
    feed(":XXX!u@h NICK me");
    feed(":srv 005 me MODES :are supported");  // no value: unlimited
    assert(ts->max_modes == 12);
    feed(":srv 005 me MODES=3 :are supported");
    feed(":srv 353 me = #c :@me!u@me.host +v1!u@h1 r1!u@h2 %hop!u@h3 @op2!x@h4 r2");
    sent();
    core.exec(chb, "/massvoice");
    assert(sent() == "MODE #c +vv r1 r2\r\n");
    core.exec(chb, "/massop");
    assert(sent() == "MODE #c +ooo hop r1 r2\r\nMODE #c +o v1\r\n");
    core.exec(chb, "/massdeop");
    assert(sent() == "MODE #c -o op2\r\n");
    core.exec(chb, "/masskick bye");  // asks first
    assert(sent().empty() && chb->lines.back().text.find("/masskick -yes bye") != std::string::npos);
    core.exec(chb, "/masskick -yes bye");
    assert(sent() == "KICK #c r1 :bye\r\nKICK #c r2 :bye\r\nKICK #c v1 :bye\r\n");
    // one-user shortcuts and the channel a channel tab implies (MODES=3 from above)
    core.exec(chb, "/op a b c d");
    assert(sent() == "MODE #c +ooo a b c\r\nMODE #c +o d\r\n");
    core.exec(chb, "/ban troll *!*@bad.host");
    assert(sent() == "MODE #c +bb troll!*@* *!*@bad.host\r\n");
    core.exec(chb, "/ban");
    assert(sent() == "MODE #c +b\r\n");
    core.exec(chb, "/kick r1 bye now");
    assert(sent() == "KICK #c r1 :bye now\r\n");
    core.exec(chb, "/kick #other r1 bye");
    assert(sent() == "KICK #other r1 :bye\r\n");
    core.exec(chb, "/mode +m");
    assert(sent() == "MODE #c +m\r\n");
    core.exec(chb, "/invite alice");
    assert(sent() == "INVITE alice #c\r\n");
    core.exec(home, "/op a");  // not a channel tab
    assert(sent().empty());
    // a password asked by the webchat is answered from there: a line typed in the terminal is chat
    core.from_web = true, core.exec(chb, "/oper admin"), core.from_web = false;
    assert(core.pass_buf == chb && !core.asking(chb));
    core.exec(chb, "hello");
    assert(sent().rfind("PRIVMSG #c :hello", 0) == 0 && core.pass_buf == chb);
    core.from_web = true, core.exec(chb, "s3cret"), core.from_web = false;
    assert(sent() == "OPER admin :s3cret\r\n" && !core.pass_buf);
    core.exec(chb, "/kb r1 go away");
    assert(sent() == "MODE #c +b *!*@h2\r\nKICK #c r1 :go away\r\n");
    core.exec(chb, "/tempban r2 5");  // host unknown: USERHOST first, the 302 finishes it
    assert(sent() == "USERHOST r2\r\n");
    feed(":srv 302 me :zz=+a@b");  // someone else's (/quote USERHOST zz): not the kickban's answer
    assert(sent().empty() && ts->kb_wait.size() == 1);
    feed(":srv 302 me :r2=+u@h9");
    assert(sent() == "MODE #c +b *!*@h9\r\nKICK #c r2 :Banned for 5 minutes\r\n" && ts->tempbans.size() == 1);
    time_t t0 = time(nullptr);
    core.chan_timers(ts, t0 + 299);
    assert(sent().empty());
    core.chan_timers(ts, t0 + 301);
    assert(sent() == "MODE #c -b *!*@h9\r\n" && ts->tempbans.empty());
    core.exec(chb, "/topicrotate 1 first | second ");
    core.chan_timers(ts, t0);
    core.chan_timers(ts, t0 + 30);
    core.chan_timers(ts, t0 + 60);
    assert(sent() == "TOPIC #c :first\r\nTOPIC #c :second\r\n");
    core.exec(chb, "/topicrotate off");
    {  // /e2e #c: what we send opens on the way back in, a wrong password doesn't; the menu flips
        core.exec(chb, "/e2e #c hunter2");
        assert(e2e_on(chb) && core.tab_menu(chb)[2].second == "/e2e off");
        core.say(chb, "secret", false);
        std::string line = sent();
        assert(line.rfind("PRIVMSG #c :+iSE ", 0) == 0 && line.find("secret") == std::string::npos);
        feed(":r1!u@h2 " + line.substr(0, line.size() - 2));
        assert(chb->lines.back().text == "\xf0\x9f\x94\x92 secret");
        ts->chan_e2e["#c"] = e2e_key("nope", "isekur-chan:#c");
        feed(":r1!u@h2 " + line.substr(0, line.size() - 2));
        assert(chb->lines.back().text.find("wrong password?") != std::string::npos);
        core.exec(chb, "/e2e off");
        assert(!e2e_on(chb) && core.tab_menu(chb)[2].second == "/e2e #c");
        // an autojoin channel's key goes to the config (the key, not the password) and comes back
        core.cfg_path = "build/test-e2e.conf";
        unlink(core.cfg_path.c_str());
        core.exec(chb, "/autojoin add");
        core.exec(chb, "/e2e #c hunter2");
        Ini disk;
        assert(ini_load(core.cfg_path, disk));
        std::string kv = ini_get(disk, "server.t", "e2e_keys");
        assert(kv == "#c:" + b64(e2e_key("hunter2", "isekur-chan:#c")) && kv.find("hunter2") == std::string::npos);
        Core fresh;
        fresh.cfg = disk;
        assert(fresh.add_server(tc)->chan_e2e["#c"] == ts->chan_e2e["#c"]);
        core.exec(chb, "/e2e off");
        assert(ini_load(core.cfg_path, disk) && ini_get(disk, "server.t", "e2e_keys").empty());
        core.exec(chb, "/autojoin del");
        unlink(core.cfg_path.c_str()), core.cfg_path.clear();
    }
    core.chan_timers(ts, t0 + 999);
    assert(sent().empty());
    core.exec(chb, "/chanstats");
    bool counted = false;
    for (auto& l : chb->lines) counted |= mirc_strip(l.text).find("Users: 6  (2 ops, 1 halfops, 1 voiced, 2 regular; 0 away)") == 0;
    assert(counted);
    chb->nicks["me"].prefix = "+";  // not an op any more
    core.exec(chb, "/lockdown");
    assert(sent().empty() && chb->lines.back().kind == LK::ERROR);
    chb->nicks["me"].prefix = "@";
    core.exec(chb, "/lockdown");
    assert(sent() == "MODE #c +mntsi\r\n");

    // the nick menu (terminal right click and webchat): op items only for ops; folding a server tab
    auto has_item = [&](const std::string& l) {
        for (auto& [lab, cmd] : core.nick_menu(chb, "bob")) if (lab == l) return true;
        return false;
    };
    assert(has_item("Whois") && has_item("Kickban"));
    chb->nicks["me"].prefix = "";
    assert(has_item("Whois") && !has_item("Kick") && !has_item("Kickban"));
    chb->nicks["me"].prefix = "@";
    core.fold(ts->status);
    assert(ts->status->collapsed);
    core.fold(chb);  // only server tabs fold
    assert(!chb->collapsed);
    core.fold(ts->status);
    assert(!ts->status->collapsed);
    // the channel tab menu: autojoin add/del flips with the list, a joined channel asks before closing
    auto tab_cmd = [&](const std::string& l) {
        for (auto& [lab, cmd] : core.tab_menu(chb)) if (lab == l) return cmd;
        return std::string("-");
    };
    bool joined = chb->joined;
    chb->joined = true;
    assert(tab_cmd("Close")[0] == '?' && tab_cmd("Clear") == "/clear");
    chb->joined = joined;
    core.exec(chb, tab_cmd("Add to autojoin"));
    assert(tab_cmd("Remove from autojoin") == "/autojoin del");
    core.exec(chb, "/autojoin del");
    assert(tab_cmd("Add to autojoin") == "/autojoin add" && core.tab_menu(ts->status).empty());

    // highlights: the nick and hilight_words, whole words, * globs; the default part/quit message
    auto hl = [&](const std::string& t) { feed(":bob!u@h PRIVMSG #c :" + t); return chb->lines.back().hilight; };
    assert(hl("hey ME!") && !hl("isekur rocks"));
    core.exec(chb, "/hilight add iSekur");
    core.exec(chb, "/hilight add ger*");
    assert(hl("isekur rocks") && !hl("isekurs rock") && hl("hi Gerardo") && !hl("hi roger"));
    core.exec(chb, "/hilight del isekur");
    assert(!hl("isekur rocks") && core.unsaved["general"]["hilight_words"] == "ger*");
    core.exec(chb, "/part");
    assert(sent() == "PART #c :iSekur " ISEKUR_VERSION " :: colorful terminal IRC client :: https://github.com/gcanosa/iSekur\r\n");
    core.cfg["server.t"]["part_message"] = "bye";
    core.exec(chb, "/part");
    assert(sent() == "PART #c :bye\r\n");

    // autojoin with keys, NickServ when SASL didn't log in, rejoin of open tabs with their key
    core.exec(chb, "/autojoin add");
    core.exec(chb, "/join #k s3cret");
    sent();
    core.exec(chb, "/autojoin add #K");  // keeps the /join key
    core.exec(chb, "/autojoin add #x");
    core.exec(chb, "/autojoin del #X");
    assert(core.cfg["server.t"]["autojoin"] == "#c,#K s3cret" && core.unsaved["server.t"].count("autojoin"));
    core.open(ts, BT::CHANNEL, "#open");
    ts->cfg.nickserv_pass = "pw";
    feed(":srv 001 me :Welcome");
    assert(sent() == "CAP END\r\nPRIVMSG NickServ :IDENTIFY pw\r\nJOIN #c\r\nJOIN #K s3cret\r\nJOIN #open\r\n");
    ts->sasl_ok = true;
    feed(":srv 001 me :Welcome");
    assert(sent().find("NickServ") == std::string::npos);
    core.exec(home, "/autojoin");  // no server in the cmd tab
    {  // snap: the open channel tabs, with keys, written to the config right away
        std::string path = "build/test-snap.conf";
        remove(path.c_str());
        std::swap(core.cfg_path, path);
        core.open(ts, BT::CHANNEL, "#k");
        core.exec(home, "/autojoin snap");
        Ini f;
        assert(ini_load(core.cfg_path, f) && f["server.t"]["autojoin"].find("#k s3cret") != std::string::npos &&
               f["server.t"]["autojoin"].find("#open") != std::string::npos && !core.unsaved.count("server.t"));
        assert(ts->cfg.autojoin.size() == (size_t)std::count(f["server.t"]["autojoin"].begin(), f["server.t"]["autojoin"].end(), ',') + 1);
        std::swap(core.cfg_path, path);
    }

    // whois: framed, in the active tab
    core.select(chb);
    size_t wb = chb->lines.size();
    feed(":srv 311 me Bob ~u h.example * :Bob Real");
    feed(":srv 319 me Bob :@#c #d");
    feed(":srv 317 me Bob 3725 1700000000 :seconds idle, signon time");
    feed(":srv 318 me Bob :End of /WHOIS list.");
    feed(":srv 318 me Bob :End of /WHOIS list.");  // a stray end: no second frame
    std::vector<std::string> wl;
    for (size_t k = wb; k < chb->lines.size(); k++) wl.push_back(mirc_strip(chb->lines[k].text));
    assert(wl.size() == 6 && wl[0].rfind("┌─ whois Bob", 0) == 0 && wl[1] == "│ Bob (~u@h.example): Bob Real" &&
           wl[2] == "│ channels @#c #d" && wl[3].rfind("│ idle 1h2m5s, signed on ", 0) == 0 && wl[4] == "└─────────");
    wb = chb->lines.size();  // a 311 without the "*" (SekurIRCd) is framed too
    feed(":srv 311 me Erick erick c.example :Erick R");
    feed(":srv 318 me Erick :End of /WHOIS list.");
    assert(mirc_strip(chb->lines[wb + 1].text) == "│ Erick (erick@c.example): Erick R" && mirc_strip(chb->lines.back().text) == "└─────────");

    core.exec(home, "/detach");  // nothing attached (the webchat typed it)
    assert(!core.detach_req && home->lines.back().kind == LK::ERROR);
    core.tty = true;
    core.exec(home, "/detach");
    assert(core.detach_req);

    // one iSekur per config: the second listener finds the first; close() removes the socket
    std::string serr, sp = sock_path_for("build/isekur-test.conf");
    Session s1, s2;
#ifdef __CYGWIN__
    assert(s1.listen(sp, serr) == 0);  // no detaching there
#else
    assert(sp == "build/isekur.sock" && s1.listen(sp, serr) == 1 && s2.listen(sp, serr) == -1);
    s1.close();
    assert(access(sp.c_str(), F_OK) != 0 && s2.listen(sp, serr) == 1);
    s2.close();
    assert(Session().listen(std::string(200, 'x'), serr) == 0 && serr.find("too long") != std::string::npos);
#endif

    assert(ctcp_version().rfind(version_string() + " :: ", 0) == 0 && version_string().find(ISEKUR_VERSION) != std::string::npos);

    Server sv;  // token bucket: 2 at once, then one per second
    sv.flood_burst = 2, sv.flood_ms = 1000, sv.tokens = 2, sv.tokens_at = 0;
    for (int i = 0; i < 4; i++) sv.sendq.push_back("L" + std::to_string(i) + "\r\n");
    assert(sv.drain(0) == 1001 && sv.conn.out == "L0\r\nL1\r\n");
    assert(sv.drain(500) > 0 && sv.sendq.size() == 2);
    assert(sv.drain(1000) == 1001 && sv.sendq.size() == 1);
    assert(sv.drain(2000) == -1 && sv.conn.out == "L0\r\nL1\r\nL2\r\nL3\r\n");

    puts("all tests passed");
}
