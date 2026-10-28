// Self-checks for the parsing code: run with `make test`.
#include <cassert>
#include <cstdio>
#include "core.h"
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
    assert(parse_color("#ff8800") == 0xff8800 && parse_color("default") == -1 && parse_color("196") == 0xff0000);

    assert(unb64(b64("hello!")) == "hello!" && b64("") == "" && unb64("!!!") == "");

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
    assert(up == "# top\n[general]\ntheme = neon\nlog = on\n\n[server.x]\nhost = h\n\n[web]\nport = 9000\n");
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
    core.exec(home, "/help ban");
    assert(mirc_strip(home->lines.back().text).find("No such command: /ban") == 0);

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
    core.exec(chb, "/kb r1 go away");
    assert(sent() == "MODE #c +b *!*@h2\r\nKICK #c r1 :go away\r\n");
    core.exec(chb, "/tempban r2 5");  // host unknown: USERHOST first, the 302 finishes it
    assert(sent() == "USERHOST r2\r\n");
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
