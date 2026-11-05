// Self-checks for the parsing code: run with `make test`.
#include <algorithm>
#include <cassert>
#include <sys/stat.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <csignal>
#include <cstring>
#include <functional>
#include <unistd.h>
#include <netinet/in.h>
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
    signal(SIGPIPE, SIG_IGN);  // like main.cpp: a closed resolver pipe or socket must not kill us
    Msg m;
    assert(irc_parse("@time=2024-01-02T03:04:05.000Z;msgid=a\\sb :nick!u@h PRIVMSG #chan :hello there", m));
    assert(m.cmd == "PRIVMSG" && m.nick == "nick" && m.p(0) == "#chan" && m.p(1) == "hello there");
    assert(m.tags["msgid"] == "a b");
    assert(parse_server_time(m.tags["time"]) == 1704164645);
    assert(irc_parse("PING :x y", m) && m.cmd == "PING" && m.p(0) == "x y" && m.prefix.empty());
    assert(irc_parse(":srv 005 me PREFIX=(ov)@+ CHANTYPES=# :are supported", m) && m.params.size() == 4);
    assert(irc_parse("ping", m) && m.cmd == "PING");
    assert(irc_lower("Nick[A]\\~") == "nick{a}|^");

    assert(release_tag("HTTP/1.1 302 Found\r\nLocation: https://github.com/gcanosa/iSekur/releases/tag/v0.6\r\n\r\n") == "0.6");
    assert(release_tag("HTTP/1.1 302 Found\r\nlocation: https://github.com/x/releases/tag/v1.10.2\r\nServer: gh\r\n\r\n") == "1.10.2");
    assert(release_tag("HTTP/1.1 302 Found\r\nLocation: https://github.com/x/releases\r\n\r\n").empty());
    assert(release_tag("").empty());
    assert(version_newer("0.10", "0.9") && version_newer("0.6", "0.5") && version_newer("1.0", "0.9.9") && version_newer("0.5.1", "0.5"));
    assert(!version_newer("0.5", "0.5") && !version_newer("0.4", "0.5") && !version_newer("", "0.5"));

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
        Theme mirc;  // one nick color (mIRC doesn't color nicks): mIRC's colors for the letters, dark ones on white
        mirc.nicks = {0x000000};
        mirc.el["default"] = Style{0x000000, 0xffffff};
        assert(banner_lines(mirc)[0].rfind("\x04" "ff0000", 0) == 0 && banner_lines(mirc)[1].find("\x04" "00007f") != std::string::npos);
        mirc.el["default"].bg = 0x000000;  // mirc-dark: the bright ones, unless the dialog is gray (light = 1)
        assert(banner_lines(mirc)[0].rfind("\x04" "ff5555", 0) == 0 && banner_lines(mirc, false, -99, 1)[0].rfind("\x04" "ff0000", 0) == 0);
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
    // attach / webchat audit lines
    assert(typing_text({"a"}) == "a is typing\u2026" && typing_text({"a", "b"}) == "a and b are typing\u2026");
    assert(typing_text({"a", "b", "c"}) == "a, b and c are typing\u2026" && typing_text({}) == "");
    assert(typing_text({"a", "b", "c", "d", "e", "f"}) == "a, b and 4 others are typing\u2026");
    assert(fmt_dur(9) == "9s" && fmt_dur(2832) == "47m12s" && fmt_dur(90061) == "1d1h1m");
    assert(attach_who("203.0.113.7 52144 10.0.0.2 22", "/dev/ttys004") == "203.0.113.7 (ssh) on /dev/ttys004");
    assert(attach_who("", "/dev/ttys001") == "local on /dev/ttys001" && attach_who("", "") == "local");
    assert(ssh_dest("user ethernet\nhostname 203.0.113.7\nport 22\nidentityfile ~/.ssh/id\n") == "ethernet@203.0.113.7:22");
    assert(ssh_dest("hostname 2001:db8::1\nport 2222\n") == "[2001:db8::1]:2222" && ssh_dest("") == "");
    assert(client_ip("203.0.113.9", "1.2.3.4") == "203.0.113.9");             // not from a local proxy: ignored
    assert(client_ip("127.0.0.1", "6.6.6.6, 198.51.100.23") == "198.51.100.23");  // the proxy's own hop
    assert(client_ip("::1", "2001:db8::5") == "2001:db8::5" && client_ip("127.0.0.1", "") == "127.0.0.1");
    assert(client_ip("127.0.0.1", "evil host") == "127.0.0.1");
    assert(ua_short("Mozilla/5.0 (Macintosh; Intel Mac OS X 14.5; rv:131.0) Gecko/20100101 Firefox/131.0") == "Firefox on macOS");
    assert(ua_short("Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 Version/18.0 Mobile/15E148 Safari/604.1") == "Safari on iPhone");
    assert(ua_short("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/130.0 Safari/537.36 Edg/130.0") == "Edge on Windows");
    assert(ua_short("") == "" && ua_short("curl/8.7.1") == "");
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
    {  // passwords sent back as typed: "enc:" in the file, plain in memory; hand-typed ones get encrypted on load
        char d[] = "/tmp/isekur-enc-XXXXXX";
        assert(mkdtemp(d));
        std::string path = std::string(d) + "/c.conf", err;
        FILE* f = fopen(path.c_str(), "w");
        fputs("[server.x]\nnickserv_pass = hunter2\n", f), fclose(f);
        Ini in;
        assert(ini_load(path, in) && in["server.x"]["nickserv_pass"] == "hunter2");
        std::string text;
        { FILE* g = fopen(path.c_str(), "r"); char buf[512]; text.assign(buf, fread(buf, 1, sizeof buf, g)); fclose(g); }
        assert(text.find("hunter2") == std::string::npos && text.find("nickserv_pass = enc:") != std::string::npos);
        Ini ch2;
        ch2["server.x"]["sasl_pass"] = "s3cret";
        ch2["web"]["password_hash"] = "$hash";
        assert(ini_save(path, ch2, err));
        Ini back;
        assert(ini_load(path, back) && back["server.x"]["sasl_pass"] == "s3cret" && back["web"]["password_hash"] == "$hash");
        unlink((std::string(d) + "/secret.key").c_str());
        assert(ini_load(path, back) && back["server.x"]["sasl_pass"].empty());  // no key: as if unset
        unlink(path.c_str()), rmdir(d);
    }

    {  // no .theme anywhere: it runs uncoloured and says where it looked
        char d[] = "/tmp/isekur-nothemes-XXXXXX";
        assert(mkdtemp(d));
        std::string conf = std::string(d) + "/isekur.conf", oldhome = getenv("HOME") ? getenv("HOME") : "";
        FILE* f = fopen(conf.c_str(), "w");
        fputs("[general]\ntheme = nope\ntheme_dir = /nonexistent\n", f), fclose(f);
        setenv("HOME", d, 1), unsetenv("XDG_CONFIG_HOME"), unsetenv("XDG_DATA_HOME");
        assert(chdir("build") == 0);  // away from the repo's themes/
        Core nt;
        nt.cfg_path = conf;
        nt.load_config();
        assert(chdir("..") == 0);
        setenv("HOME", oldhome.c_str(), 1);
        bool said = false;
        for (auto& l : nt.bufs.front()->lines)
            said |= l.kind == LK::ERROR && l.text.find("No themes found") != std::string::npos && l.text.find("/nonexistent") != std::string::npos;
        assert(said && nt.theme.el.empty());
        unlink(conf.c_str()), rmdir(d);
    }

    Core core;

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
    // /attach: never an ssh option or more than a host, never from the webchat; no tab opened
    size_t tabs = core.bufs.size();
    for (const char* bad : {"/attach", "/attach -oProxyCommand=touch /tmp/x", "/attach a b c", "/attach a;b", "/attach a -x",
                            "/attach a $(id)", "/attach a 'x'", "/attach a x\"y", "/attach -p 22 -x a", "/attach -p 22 a b c"}) {
        core.exec(home, bad);
        assert(home->lines.back().text.find("Usage: /attach [-p port] host") == 0 && core.bufs.size() == tabs);
    }
    for (const char* bad : {"/attach -p a", "/attach -p 0 a", "/attach -p 70000 a", "/attach -p -oX=y a", "/attach a -p"}) {  // -p: a port, nothing else
        core.exec(home, bad);
        assert(home->lines.back().text == "/attach -p takes a port, 1-65535" && core.bufs.size() == tabs);
    }
    {  // a shell or remote tab gets a right-click menu: its keys all go to the tab
        Buffer* sh = core.open(nullptr, BT::SHELL, "vps");
        auto tm = core.tab_menu(sh);
        assert(tm.size() == 1 && tm[0].first == "Close" && tm[0].second == "/close");  // a remote one: "Disconnect", asking first
        core.close(sh);
    }
    core.from_web = true;
    core.exec(home, "/attach vps");
    core.from_web = false;
    assert(home->lines.back().text == "/attach works in the terminal only" && core.bufs.size() == tabs);
    {  // with a lock password, exec_web (and lock itself) change only after typing it
        core.cfg["general"]["lock"] = "on", core.cfg["general"]["lock_password_hash"] = hash_password("pw");
        core.exec(home, "/set exec_web on");
        assert(core.pass_buf == home && ini_get(core.cfg, "general", "exec_web").empty());
        core.exec(home, "nope");
        assert(home->lines.back().text == "Wrong password: it stays as it was" && ini_get(core.cfg, "general", "exec_web").empty());
        core.exec(home, "/set lock off"), core.exec(home, "nope");
        assert(core.cfg["general"]["lock"] == "on");
        core.exec(home, "/set exec_web on"), core.exec(home, "pw");
        assert(core.cfg["general"]["exec_web"] == "on" && !core.pass_buf);
        core.exec(home, "/set exec_web on");  // no change: nothing asked
        assert(!core.pass_buf);
        core.exec(home, "/exit -y"), core.exec(home, "nope");  // /exit too: the lock password, or iSekur stays
        assert(home->lines.back().text == "Wrong password: iSekur keeps running" && !core.quit);
        core.exec(home, "/exit -y");
        assert(core.pass_buf == home && !core.quit);
        core.exec(home, "");  // Esc / empty: cancelled
        assert(!core.quit && !core.pass_buf);
        core.lock_now(home);  // the lock key (Alt+S): locks at once, no prompt
        assert(core.locked && !core.pass_buf);
        core.set_lock(false);
        core.unsaved.clear(), core.set_was.clear();
        for (auto k : {"lock", "lock_password_hash", "exec_web"}) core.cfg["general"].erase(k);
        core.lock_now(home);  // no password: only says how
        assert(!core.locked && !core.pass_buf && home->lines.back().text == "No lock password yet: /lock sets one and locks");
    }

    // channel op commands against a fake connected server; flood_ms 0 = lines go out at once
    ServerCfg tc;
    tc.name = "t", tc.nick = "me";
    Server* ts = core.add_server(tc);
    ts->state = SS::CONNECTED, ts->flood_ms = 0;
    ts->prefix_modes = "qaohv", ts->prefix_chars = "~&@%+";
    {  // ignores are per network: [server.t] ignore, nothing global
        core.cfg["server.t"]["ignore"] = "spam*!*@* *!*@*.Evil.Host";
        assert(core.ignored(ts, "SpamBot!u@h") && core.ignored(ts, "x!y@a.evil.host"));
        assert(!core.ignored(ts, "friend!u@good.host") && !core.ignored(ts, "irc.server.net"));
        core.cfg["server.t"].erase("ignore");
        core.cfg["server.other"]["ignore"] = core.cfg["general"]["ignore"] = "spam*!*@*";
        assert(!core.ignored(ts, "SpamBot!u@h"));  // another network's list, or a global one, isn't used
        core.cfg.erase("server.other"), core.cfg["general"].erase("ignore");
    }
    Buffer* chb = core.open(ts, BT::CHANNEL, "#c");
    chb->joined = true;
    auto feed = [&](const std::string& l) { Msg mm; assert(irc_parse(l, mm)); core.handle(ts, mm); };
    {  // keep_history: a tab's last log_history lines come back after a restart, log off
        core.cfg_path = "build/test-hist.conf";
        unlink("build/test-hist.history");
        core.cfg["general"]["log_history"] = "2";
        Buffer* h = core.open(ts, BT::CHANNEL, "#Hist");
        core.add(h, Line{1000, LK::MSG, "bob", "one"});
        core.add(h, Line{2000, LK::MSG, "me", "two"});
        core.add(h, Line{3000, LK::ACTION, "bob", "waves"});
        Line g{3000, LK::INFO, "", "grep hit"};
        g.nolog = true;
        core.add(h, g);
        core.hist_cache["gone\t#old"] = {Line{1000, LK::MSG, "x", "kept"}};  // a tab not open now
        core.hist_loaded = true;
        core.save_history();
        core.close(h);
        core.hist_cache.clear(), core.hist_loaded = false;  // a fresh start
        h = core.open(ts, BT::CHANNEL, "#hist");
        assert(h->lines.size() == 3 && h->lines[0].text == "two" && h->lines[0].self && h->lines[1].kind == LK::ACTION);
        assert(h->lines[2].text == "--- end of last session ---" && h->lines[2].nolog);
        assert(core.hist_cache.count("gone\t#old") && !core.hist_cache.count("t\t#hist"));
        core.close(h);
        core.cfg["general"]["keep_history"] = "off";
        core.hist_cache.clear(), core.hist_loaded = false;
        h = core.open(ts, BT::CHANNEL, "#hist");
        assert(h->lines.empty());
        core.close(h);
        core.cfg["general"].erase("keep_history"), core.cfg["general"].erase("log_history");
        core.hist_cache.clear(), core.hist_loaded = true;  // nothing to keep: no empty file left behind
        core.save_history();
        assert(access("build/test-hist.history", F_OK) != 0);
        unlink("build/test-hist.history"), core.cfg_path.clear();
    }
    {  // /move N: among the server's tabs, after its server tab; a drag runs it
        Buffer *m1 = core.open(ts, BT::CHANNEL, "#m1"), *m2 = core.open(ts, BT::QUERY, "m2");
        auto names = [&]() { std::string o; for (auto& p : core.bufs) if (p->srv == ts) o += p->name + ","; return o; };
        std::string st = ts->status->name + ",";
        assert(names() == st + "#c,#m1,m2,");
        core.exec(m2, "/move 1");
        assert(names() == st + "m2,#c,#m1,");
        core.exec(m2, "/move 2");
        assert(names() == st + "#c,m2,#m1,");
        core.exec(m2, "/move 9");
        assert(names() == st + "#c,#m1,m2,");
        // the autojoin list follows the channel tabs, written to the config at once
        core.cfg_path = "build/test-move.conf";
        unlink(core.cfg_path.c_str());
        ts->cfg.autojoin = {"#x", "#m1 key", "#c"};
        core.exec(m1, "/move 1");
        assert(names() == st + "#m1,#c,m2,");
        assert((ts->cfg.autojoin == std::vector<std::string>{"#m1 key", "#c", "#x"}));
        Ini disk;
        assert(ini_load(core.cfg_path, disk) && ini_get(disk, "server.t", "autojoin") == "#m1 key,#c,#x");
        // a server tab moves with its tabs, among the servers: [general] server_order
        ServerCfg uc;
        uc.name = "u", uc.nick = "me";
        Server* us = core.add_server(uc);
        Buffer* uch = core.open(us, BT::CHANNEL, "#u");
        auto order = [&]() { std::string o; for (auto& p : core.bufs) if (p->srv) o += p->name + ","; return o; };
        std::string before = order();
        core.exec(us->status, "/move 1");
        assert(order().rfind("u,#u,", 0) == 0 && core.bufs[0]->srv == nullptr);  // the console stays first
        assert(ini_load(core.cfg_path, disk) && ini_get(disk, "general", "server_order") == "u,t");
        assert(core.servers[0].get() == us);
        core.exec(us->status, "/move 9");  // past the last: at the end again
        assert(order() == before && ini_load(core.cfg_path, disk) && ini_get(disk, "general", "server_order") == "t,u");
        core.close(uch), core.close(us->status);
        unlink(core.cfg_path.c_str()), core.cfg_path.clear();
        ts->cfg.autojoin.clear(), core.cfg.erase("server.t"), core.cfg["general"].erase("server_order");
        core.close(m1), core.close(m2);
    }
    auto sent = [&]() { std::string o = ts->conn.out; ts->conn.out.clear(); return o; };
    feed(":me!u@h NICK XXX");  // a reconnect keeps the nick we had, not cfg.nick
    assert(ts->nick == "XXX" && ts->last_nick == "XXX");
    feed(":XXX!u@h NICK me");
    {  // user modes: letters only (a stray byte after them showed in the status bar)
        feed(":srv 221 me :+iwzT\x01");
        assert(ts->umodes == "iwzT");
        feed(":me!u@h MODE me :+x\x02-w");
        assert(ts->umodes == "izTx");
        ts->umodes.clear();
        core.exec(ts->status, "/nick");  // alone: says which
        assert(ts->status->lines.back().text == "Your nickname is me");
        Buffer* was = core.active;
        core.exec(ts->status, "/query bob");
        Buffer* q = core.active;
        assert(q && q->type == BT::QUERY && q->srv == ts);
        feed(":me!u@h NICK me2");  // your own change shows in the tab you're on too
        assert(q->lines.back().text == "You are now known as me2");
        feed(":me2!u@h NICK me");
        // UnrealIRCd ends labeled replies with \r\r\n: every CR goes, or the nick keeps one and the next change isn't ours
        ts->inbuf = "@label=i1 :me NICK :me3\r\r\n@label=i2 :me3 NICK :me\r\r\n";
        core.on_readable(ts, 0);
        assert(ts->nick == "me" && ts->inbuf.empty() && q->lines.back().text == "You are now known as me");
        sent();
        core.exec(ts->status, "/back");  // /back: AWAY with nothing
        assert(sent().find("AWAY\r\n") != std::string::npos);
        core.close(q), core.select(was);
        core.exec(home, "/quit");  // /quit is one server's: not from a tab without one
        assert(!core.quit && home->lines.back().text.find("Not a server's tab") == 0);
    }
    feed(":srv 005 me MODES :are supported");  // no value: unlimited
    assert(ts->max_modes == 12);
    feed(":srv 904");  // a bare SASL failure numeric: no params to print, no crash
    assert(ts->status->lines.back().text == "SASL: 904");
    ts->cap_done = false, sent();  // it ended CAP negotiation; the 001 test below starts it fresh
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

    // /part, /close and Ctrl+W: PART (given message, else part_message), the tab closes, the server tab says so
    {
        Buffer* pb = core.open(ts, BT::CHANNEL, "#p");
        pb->joined = true, sent();
        core.exec(pb, "/part off to bed");
        assert(sent() == "PART #p :off to bed\r\n" && !core.find(ts, "#p") && ts->status->lines.back().text == "You left #p");
        pb = core.open(ts, BT::CHANNEL, "#p");
        pb->joined = true;
        core.cfg["general"]["part_message"] = "cya";
        core.close(pb);  // Ctrl+W, /close
        assert(sent() == "PART #p :cya\r\n" && !core.find(ts, "#p"));
        core.cfg["general"].erase("part_message");
        core.exec(chb, "/part #elsewhere");  // no tab for it: just the PART
        assert(sent().rfind("PART #elsewhere :", 0) == 0 && core.find(ts, "#c"));
    }

    // /split: a second pane; picking its tab swaps focus, its lines don't light it up, closing it unsplits
    {
        Buffer* sp = core.open(ts, BT::CHANNEL, "#sp");
        Buffer* sq = core.open(ts, BT::CHANNEL, "#sq");
        core.select(sq);
        core.exec(sq, "/split #nope");
        assert(!core.split_buf && sq->lines.back().kind == LK::ERROR);
        core.exec(sq, "/split stack #SP");
        assert(core.split_buf == sp && core.split_mode == 2 && !core.split_first);
        core.add(sp, Line{0, LK::MSG, "bob", "hi"});
        assert(sp->activity == 0 && sp->unread == 0);  // on screen in the other pane
        core.select(sp);  // F6, a click, its tab: the panes stay put, the focus moves
        assert(core.active == sp && core.split_buf == sq && core.split_first);
        feed(":" + ts->nick + "!u@h JOIN #new");  // a /join while split: the new channel takes the focused pane, the other stays
        Buffer* nw = core.find(ts, "#new");
        assert(nw && core.active == nw && core.split_buf == sq);
        core.select(sp), core.close(nw), sent();
        core.exec(sp, "/split side");  // only the layout
        assert(core.split_mode == 1 && core.split_buf == sq);
        core.close(sq);  // the other pane's tab goes: one pane again
        assert(!core.split_buf && core.active == sp);
        core.exec(sp, "/split #c"), core.exec(sp, "/unsplit");
        assert(!core.split_buf);
        core.split_mode = 0, core.close(sp), core.select(chb), sent();
    }

    // rejoin_on_kick: off, a kick just leaves; on, a JOIN right away (with the autojoin key)
    {
        Buffer* kb = core.open(ts, BT::CHANNEL, "#k");
        kb->joined = true, sent();
        feed(":op!u@h KICK #k me :out");
        assert(!kb->joined && sent().empty());
        core.cfg["general"]["rejoin_on_kick"] = "on";
        ts->cfg.autojoin.push_back("#k sekrit");
        kb->joined = true;
        feed(":op!u@h KICK #k me :out again");
        assert(sent() == "JOIN #k sekrit\r\n");
        feed(":op!u@h KICK #k someoneelse :not me");
        assert(sent().empty());
        core.cfg["general"].erase("rejoin_on_kick"), ts->cfg.autojoin.pop_back();
        core.close(kb), sent();
    }

    // replies go where the command was typed, not the active tab; untyped join failures: the server tab
    {
        Buffer* other = core.open(ts, BT::CHANNEL, "#other");
        core.select(other);
        core.exec(ts->status, "/join #r"), sent();
        feed(":srv 477 me #r :Cannot join channel (+r)");
        assert(ts->status->lines.back().text.find("(+r)") != std::string::npos && other->lines.empty());
        core.exec(chb, "/whois x"), sent();
        feed(":srv 401 me x :No such nick");
        assert(chb->lines.back().text.find("No such nick") != std::string::npos);
        ts->typed_at = 0;  // long ago (an autojoin): not the active #other
        feed(":srv 474 me #b :Cannot join channel (+b)");
        assert(ts->status->lines.back().text.find("(+b)") != std::string::npos && other->lines.empty());
        core.close(other), core.select(chb), sent();
    }
    {  // /rawlog: every line in and out in a *raw tab, secrets blanked; the fallbacks show what they used to hide
        assert(raw_redact("PASS hunter2") == "PASS ***" && raw_redact("REGISTER me * pw") == "REGISTER ***");
        assert(raw_redact("@label=i3 PRIVMSG NickServ :IDENTIFY me pw") == "@label=i3 PRIVMSG NickServ :IDENTIFY ***");
        assert(raw_redact(":me!u@h PRIVMSG nickserv :identify pw") == ":me!u@h PRIVMSG nickserv :identify ***");
        assert(raw_redact("AUTHENTICATE PLAIN") == "AUTHENTICATE PLAIN" && raw_redact("AUTHENTICATE bWU=") == "AUTHENTICATE ***");
        assert(raw_redact("OPER admin pw") == "OPER admin ***" && raw_redact("PRIVMSG #c :hi") == "PRIVMSG #c :hi");
        assert(raw_redact("NS IDENTIFY pw") == "NS IDENTIFY ***" && raw_redact("") == "");
        core.exec(chb, "/rawlog");
        Buffer* rb = core.find(ts, "*raw");
        assert(rb && core.active == rb && ts->raw_hook);
        ts->send("PRIVMSG NickServ :IDENTIFY secret"), sent();
        assert(rb->lines.back().text == "\xe2\x86\x92 PRIVMSG NickServ :IDENTIFY ***" && rb->lines.back().nolog);
        core.exec(rb, "hello");  // not a chat: nothing sent
        assert(rb->lines.back().kind == LK::ERROR && sent().empty());
        core.exec(rb, "/rawlog");  // again: off, the tab goes
        assert(!core.find(ts, "*raw") && !ts->raw_hook);
        core.select(chb);
        feed(":srv CAP me LIST :sasl multi-prefix");
        assert(ts->status->lines.back().text == "CAP LIST: sasl multi-prefix");
        feed(":srv 005 me AWAYLEN=200 SILENCE :are supported by this server");
        assert(ts->status->lines.back().text == "ISUPPORT: AWAYLEN=200 SILENCE");
        feed(":srv FROB a b :c d");
        assert(ts->status->lines.back().text == "srv FROB a b c d");
    }

    // Twitch: display-name + color tags, badges in the nick list, its own commands, lowercase joins
    if (!twitch_host("irc.chat.twitch.tv")) {  // make notwitch: /twitch only says so
        core.exec(home, "/twitch");
        assert(home->lines.back().text.find("without Twitch support") != std::string::npos && twitch_banner().empty());
    } else {
        ServerCfg twc;
        twc.name = "tw", twc.nick = "me", twc.host = "irc.chat.twitch.tv";
        Server* tw = core.add_server(twc);
        assert(tw->twitch() && !ts->twitch());
        auto tbn = twitch_banner();  // block letters, same width, colors only (no stray bytes)
        assert(tbn.size() == 8 && mirc_strip(tbn[0]) == u8"████████╗██╗    ██╗██╗████████╗ ██████╗██╗  ██╗");
        tw->state = SS::CONNECTED, tw->flood_ms = 0;
        Buffer* tb = core.open(tw, BT::CHANNEL, "#streamer");
        auto tfeed = [&](const std::string& l) { Msg mm; assert(irc_parse(l, mm)); core.handle(tw, mm); };
        tfeed("@badges=moderator/1;color=#FF0000;display-name=CoolGuy :coolguy!coolguy@coolguy.tmi.twitch.tv PRIVMSG #streamer :hi");
        assert(tb->lines.back().nick == "CoolGuy" && tb->lines.back().color == 0xff0000);
        assert(tb->nicks.at("coolguy").prefix == "@");
        tfeed("@ban-duration=600 :tmi.twitch.tv CLEARCHAT #streamer :coolguy");
        assert(tb->lines.back().text == "coolguy was timed out for 600s");
        tfeed("@emote-only=0;followers-only=-1;r9k=0;room-id=1;slow=0;subs-only=0 :tmi.twitch.tv ROOMSTATE #streamer");
        assert(tb->lines.back().text == "coolguy was timed out for 600s");  // all off: nothing to say
        tfeed("@room-id=1;slow=30 :tmi.twitch.tv ROOMSTATE #streamer");
        assert(tb->lines.back().text == "Room: slow mode 30s");
        tfeed("@display-name=Bob;login=bob;msg-id=sub;system-msg=Bob\\ssubscribed! :tmi.twitch.tv USERNOTICE #streamer :yay");
        assert(mirc_strip(tb->lines.back().text) == "\xe2\x98\x85 Bob subscribed! \xe2\x80\x94 Bob: yay");
        core.exec(tb, "/join #BigName");
        assert(tw->conn.out == "JOIN #bigname\r\n");
        tw->conn.out.clear(), tb->joined = true;
        core.exec(tb, "/part bye");  // Twitch: the tab goes too, and PART has no message
        assert(tw->conn.out == "PART #streamer\r\n" && !core.find(tw, "#streamer"));
        // /twitch: no login or token in [server.tw] yet, so it asks for both (cancelled before connecting)
        core.exec(home, "/twitch token");
        assert(home->lines.back().text.find("/twitch and paste it") != std::string::npos);
        tfeed(":tmi.twitch.tv NOTICE * :Login authentication failed");
        assert(tw->status->lines[tw->status->lines.size() - 2].text.find("/twitch token") != std::string::npos);
        tw->state = SS::DISCONNECTED;
        core.exec(home, "/twitch");
        assert(core.active == tw->status && core.pass_buf == tw->status && tw->status->lines.size() > 8);
        core.exec(tw->status, "MyLogin");
        assert(core.cfg["server.tw"]["nick"] == "mylogin" && core.pass_buf == tw->status && core.pass_hidden);
        core.exec(tw->status, "");
        assert(!core.pass_buf && tw->state == SS::DISCONNECTED);
    }
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
        assert(e2e_on(chb) && core.tab_menu(chb)[3].second == "/e2e off");
        core.say(chb, "secret", false);
        std::string line = sent();
        assert(line.rfind("PRIVMSG #c :+iSE ", 0) == 0 && line.find("secret") == std::string::npos);
        feed(":r1!u@h2 " + line.substr(0, line.size() - 2));
        assert(chb->lines.back().text == "\xf0\x9f\x94\x92 secret");
        ts->chan_e2e["#c"] = e2e_key("nope", "isekur-chan:#c");
        feed(":r1!u@h2 " + line.substr(0, line.size() - 2));
        assert(chb->lines.back().text.find("wrong password?") != std::string::npos);
        core.exec(chb, "/e2e off");
        assert(!e2e_on(chb) && core.tab_menu(chb)[3].second == "/e2e #c");
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
    {  // the nicklist's selection: one menu for all of them, kicks ask first
        std::map<std::string, std::string> mm;
        for (auto& [lab, cmd] : core.nick_menu(chb, "bob carol")) mm[lab] = cmd;
        assert(mm["Op"] == "/op bob carol" && mm["Devoice"] == "/devoice bob carol" && mm["Mention"] == "bob, carol: ");
        assert(mm["Kick..."] == "?Kick 2 users from #c?\n/kick #c bob\n/kick #c carol" && !mm.count("Query"));
    }
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
    {  // a private chat tab: the nick's commands (no Query/Mention), then Clear and Close
        Buffer* q = core.open(ts, BT::QUERY, "NickServ");
        auto qm = core.tab_menu(q);
        auto has = [&](const std::string& l) { for (auto& it : qm) if (it.first == l) return it.second; return std::string("-"); };
        assert(!qm.empty() && !qm[0].first.empty() && has("Whois") == "/whois NickServ" && has("Ignore") == "/ignore NickServ");
        assert(has("Close") == "/close" && has("Clear") == "/clear" && has("Query") == "-" && has("Mention") == "-");
        core.close(q);
    }
    bool joined = chb->joined;
    chb->joined = true;
    assert(tab_cmd("Close")[0] == '?' && tab_cmd("Clear") == "/clear");
    chb->joined = joined;
    core.exec(chb, tab_cmd("Add to autojoin"));
    assert(tab_cmd("Remove from autojoin") == "/autojoin del");
    core.exec(chb, "/autojoin del");
    assert(tab_cmd("Add to autojoin") == "/autojoin add");
    // a server tab's menu: reconnect / disconnect while up, fold, close (asks first)
    {
        auto sm = core.tab_menu(ts->status);
        assert(sm.size() == 8 && sm[0].second == "/reconnect" && sm[1].second == "/disconnect" && sm[2].first == "Hide its channels" &&
               sm[5].second.rfind("?Disconnect from ", 0) == 0 && sm[6].second == "/server rename t" && sm[7].second.rfind("?Delete t:", 0) == 0 &&
               sm[7].second.find("\n/server del t") != std::string::npos);
        // connect at startup or not: flips with the setting and goes to the config at once
        std::string path = "build/test-autoconnect.conf";
        remove(path.c_str());
        std::swap(core.cfg_path, path);
        bool was = ts->cfg.autoconnect;
        ts->cfg.autoconnect = true;
        assert(sm[3].second == "/autoconnect off" || core.tab_menu(ts->status)[3].second == "/autoconnect off");
        core.exec(ts->status, core.tab_menu(ts->status)[3].second);
        Ini f;
        assert(!ts->cfg.autoconnect && ini_load(core.cfg_path, f) && f["server.t"]["autoconnect"] == "false" && !core.unsaved.count("server.t"));
        assert(core.tab_menu(ts->status)[3] == std::make_pair(std::string("Connect at startup"), std::string("/autoconnect on")));
        core.exec(ts->status, "/autoconnect on");
        assert(ts->cfg.autoconnect && ini_load(core.cfg_path, f) && f["server.t"]["autoconnect"] == "true");
        core.exec(ts->status, "/autojoin add #saved");  // add and del are written at once too, without a /save
        assert(ini_load(core.cfg_path, f) && f["server.t"]["autojoin"].find("#saved") != std::string::npos);
        core.exec(ts->status, "/autojoin del #saved");
        assert(ini_load(core.cfg_path, f) && f["server.t"]["autojoin"].find("#saved") == std::string::npos);
        ts->cfg.autoconnect = was;
        remove(core.cfg_path.c_str());
        std::swap(core.cfg_path, path);
        core.exec(ts->status, "/fold");
        assert(ts->status->collapsed && core.tab_menu(ts->status)[2].first == "Show its channels");
        core.exec(ts->status, "/fold");
        assert(!ts->status->collapsed);
    }

    // /server del: the server, its tabs and its [server.x] go, from memory and from the file; the comments of the
    // section after it stay
    {
        std::string text = "[general]\nnick = me\n\n[server.a]\nhost = a\n; a's note\nport = 1\n\n# the next one\n[server.b]\nhost = b\n";
        std::string d = ini_drop(text, "server.a");
        assert(d.find("[server.a]") == std::string::npos && d.find("host = a") == std::string::npos && d.find("a's note") == std::string::npos &&
               d.find("# the next one\n[server.b]\nhost = b") != std::string::npos && d.find("[general]\nnick = me") != std::string::npos);
        assert(ini_drop(text, "server.zzz") == text && ini_drop(text, "server.b").find("[server.b]") == std::string::npos);
        // a new key goes above the comments that end its section (they're the next one's), not between them and it
        std::string up = ini_update(text, Ini{{"server.a", {{"nick", "x"}}}});
        assert(up.find("port = 1\nnick = x\n\n# the next one\n[server.b]") != std::string::npos);
        std::string path = "build/test-serverdel.conf";
        { std::ofstream f(path); f << "[general]\nnick = me\n\n[server.gone2]\nhost = irc.invalid\nautoconnect = false\n"; }
        std::swap(core.cfg_path, path);
        core.cfg["server.gone2"]["host"] = "irc.invalid", core.cfg["server.gone2"]["autoconnect"] = "false";
        Server* g = core.add_server(core.server_cfg("gone2"));
        Buffer* gc = core.open(g, BT::CHANNEL, "#x");
        size_t n = core.servers.size(), nb = core.bufs.size();
        core.exec(gc, "/server del gone2");  // typed in one of its own tabs
        Ini f;
        assert(core.servers.size() == n - 1 && core.bufs.size() == nb - 2 && !core.cfg.count("server.gone2") &&
               ini_load(core.cfg_path, f) && !f.count("server.gone2") && f["general"]["nick"] == "me");
        core.exec(home, "/server del gone2");
        assert(home->lines.back().text == "No server named gone2");
        remove(core.cfg_path.c_str());
        std::swap(core.cfg_path, path);
    }

    // /server with an IPv6 address: bare, or [addr]:port; the name is the address with - for its :
    {
        std::string path = "build/test-server6.conf";
        std::swap(core.cfg_path, path);
        core.exec(home, "/server [::1]:1 -nick v6");
        assert(core.cfg["server.--1"]["host"] == "::1" && core.cfg["server.--1"]["port"] == "1" && core.cfg["server.--1"]["nick"] == "v6");
        core.exec(home, "/server 2001:db8::1 1");
        assert(core.cfg["server.2001-db8--1"]["host"] == "2001:db8::1" && core.cfg["server.2001-db8--1"]["port"] == "1");
        core.exec(home, "/server ::1 1 -name six");
        assert(core.cfg["server.six"]["host"] == "::1");
        for (auto n : {"--1", "2001-db8--1", "six"}) core.exec(home, std::string("/server del ") + n);
        remove(core.cfg_path.c_str());
        std::swap(core.cfg_path, path);
    }

    // every key the settings know is in the template (where it can be set), and every key there is known
    {
        std::set<std::string> gen, srv;
        for (auto& [sec, k] : conf_keys())
            if (sec == "general") gen.insert(k);
            else if (sec.rfind("server.", 0) == 0) srv.insert(k);
        int bad = 0;
        auto no = [&](const char* what, const std::string& k) { fprintf(stderr, "%s: %s\n", what, k.c_str()), bad++; };
        for (auto& k : setting_keys()) {
            if (k.scope != 'n' && !gen.count(k.key)) no("not in the template's [general]", k.key);
            if (k.scope == 'n' && std::string(k.key) != "name" && !srv.count(k.key)) no("not in the template's [server.*]", k.key);
        }
        for (auto& k : gen) if (!key_info(k) || key_info(k)->scope == 'n') no("template [general] key not in KEYS as g or b", k);
        for (auto& k : srv) if (!key_info(k) || key_info(k)->scope == 'g') no("template [server.*] key not in KEYS as n or b", k);
        assert(!bad);
    }

    // /server rename: the section in the file (comments kept), the tab, unsaved keys, server_order and the logs folder
    {
        assert(ini_rename("[a]\n# note\nk = 1\n[b]\n", "a", "z") == "[z]\n# note\nk = 1\n[b]\n");
        std::string path = "build/test-rename.conf", logs = "build/test-rename-logs";
        system(("rm -rf " + logs).c_str());
        { std::ofstream f(path); f << "[general]\nnick = me\n\n[server.old1]\n# mine\nhost = irc.invalid\nautoconnect = false\n"; }
        std::swap(core.cfg_path, path);
        core.cfg["server.old1"]["host"] = "irc.invalid", core.cfg["server.old1"]["autoconnect"] = "false";
        core.cfg["server.old1"]["log_dir"] = logs, core.unsaved["server.old1"]["nick"] = "un";
        core.cfg["general"]["server_order"] = "x,old1";
        Server* g = core.add_server(core.server_cfg("old1"));
        mkdir_p(logs + "/old1");
        core.exec(g->status, "/server rename old1 bad/name");
        assert(g->cfg.name == "old1" && g->status->lines.back().text.find("letters, digits") != std::string::npos);
        core.exec(g->status, "/set server.old1.name new1");  // the settings window's name row
        Ini f;
        struct stat st;
        assert(g->cfg.name == "new1" && g->status->name == "new1" && !core.cfg.count("server.old1") && core.cfg["server.new1"]["host"] == "irc.invalid" &&
               core.unsaved["server.new1"]["nick"] == "un" && core.cfg["general"]["server_order"] == "x,new1" &&
               ini_load(core.cfg_path, f) && f.count("server.new1") && !f.count("server.old1") && stat((logs + "/new1").c_str(), &st) == 0);
        std::ifstream in(core.cfg_path);
        std::string txt((std::istreambuf_iterator<char>(in)), {});
        assert(txt.find("[server.new1]\n# mine\nhost") != std::string::npos);
        core.exec(home, "/server rename new1 " + core.servers.front()->cfg.name);
        assert(home->lines.back().text.find("already") != std::string::npos);
        core.exec(home, "/server del new1");
        core.cfg["general"].erase("server_order");
        remove(core.cfg_path.c_str());
        system(("rm -rf " + logs).c_str());
        std::swap(core.cfg_path, path);
    }

    // closing a /server tab (Ctrl+W): socket closed, server gone, nothing left for /save
    {
        size_t n = core.servers.size();
        core.exec(home, "/server irc.invalid 6667 -name gone");
        Server* g = core.servers.back().get();
        assert(core.servers.size() == n + 1 && g->conn.fd >= 0 && core.cfg.count("server.gone"));
        core.close(g->status);
        assert(core.servers.size() == n && !core.cfg.count("server.gone") && !core.unsaved.count("server.gone"));
    }

    // highlights: the nick and hilight_words, whole words, * globs; the default part/quit message
    auto hl = [&](const std::string& t) { feed(":bob!u@h PRIVMSG #c :" + t); return chb->lines.back().hilight; };
    assert(hl("hey ME!") && !hl("isekur rocks"));
    core.exec(chb, "/hilight add iSekur");
    core.exec(chb, "/hilight add ger*");
    assert(hl("isekur rocks") && !hl("isekurs rock") && hl("hi Gerardo") && !hl("hi roger"));
    core.exec(chb, "/hilight del isekur");
    assert(!hl("isekur rocks") && core.unsaved["server.t"]["hilight_words"] == "ger*" && !core.cfg["general"].count("hilight_words"));
    core.cfg["server.t"].erase("hilight_words"), core.cfg["server.other"]["hilight_words"] = core.cfg["general"]["hilight_words"] = "ger*";
    assert(!hl("hi Gerardo"));  // another network's words, or global ones, aren't used
    core.cfg.erase("server.other"), core.cfg["general"].erase("hilight_words"), core.cfg["server.t"]["hilight_words"] = "ger*";
    core.exec(chb, "/mute");  // a muted tab: no hilights; the menu offers the way back
    assert(core.muted(chb) && core.cfg["server.t"]["mute"] == "#c" && !hl("hey ME!") && core.tab_menu(chb)[1].second == "/mute off");
    core.exec(chb, "/mute off");
    assert(!core.muted(chb) && core.cfg["server.t"]["mute"].empty() && hl("hey ME!"));
    core.exec(chb, "/part");  // closes the tab: open it again for what follows
    assert(sent() == "PART #c :iSekur " ISEKUR_VERSION " :: colorful terminal IRC client :: https://github.com/gcanosa/iSekur\r\n");
    chb = core.open(ts, BT::CHANNEL, "#c"), chb->joined = true;
    core.cfg["server.t"]["part_message"] = "bye";
    core.exec(chb, "/part");
    assert(sent() == "PART #c :bye\r\n");
    chb = core.open(ts, BT::CHANNEL, "#c"), chb->joined = true;

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
    feed(":srv 001 me :Welcome");  // CAP never answered: no CAP END to pass upstream (psyBNC)
    assert(ts->cap_done && sent() == "PRIVMSG NickServ :IDENTIFY pw\r\nJOIN #c\r\nJOIN #K s3cret\r\nJOIN #open\r\n");
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
    wb = chb->lines.size();  // a long channel list wraps into framed lines, none past 60 cols
    feed(":srv 311 me Bob ~u h.example * :Bob Real");
    feed(":srv 319 me Bob :#i2p #oftc #asahi #Qubes_OS #moocows #debian-next #freedesktop #kvm #linux #asahi-dev #bash");
    feed(":srv 318 me Bob :End of /WHOIS list.");
    assert(chb->lines.size() - wb == 5);
    for (size_t k = wb + 1; k < wb + 4; k++) {
        std::string t = mirc_strip(chb->lines[k].text);
        assert(t.rfind("│ ", 0) == 0 && t.size() <= 64);
    }
    assert(mirc_strip(chb->lines[wb + 3].text).find("#bash") != std::string::npos);
    wb = chb->lines.size();  // a long nick and host wrap inside the frame too
    feed(":srv 311 me afofu1542904621975619051665290 ~AlpineFor ip-095-208-028-097.um33.pools.vodafone-ip.de * :The Lounge User");
    feed(":srv 318 me afofu1542904621975619051665290 :End of /WHOIS list.");
    assert(chb->lines.size() - wb == 5);
    for (size_t k = wb + 1; k < wb + 4; k++) {
        std::string t = mirc_strip(chb->lines[k].text);
        assert(t.rfind("│ ", 0) == 0 && t.size() <= 66);
    }
    assert(mirc_strip(chb->lines[wb + 3].text) == "│   The Lounge User");
    wb = chb->lines.size();  // a 311 without the "*" (SekurIRCd) is framed too
    feed(":srv 311 me Erick erick c.example :Erick R");
    feed(":srv 318 me Erick :End of /WHOIS list.");
    assert(mirc_strip(chb->lines[wb + 1].text) == "│ Erick (erick@c.example): Erick R" && mirc_strip(chb->lines.back().text) == "└─────────");

    // echo-message: a CTCP to our own nick arrives twice (echo + delivery): one reply, one line
    ts->caps.insert("echo-message"), ts->ctcp_at = 0, sent();
    feed(":me!u@h PRIVMSG me :\x01VERSION\x01");
    feed(":me!u@h PRIVMSG me :\x01VERSION\x01");
    std::string vr = sent();
    assert(vr.rfind("NOTICE me :\x01VERSION ", 0) == 0 && vr.find("\r\n") == vr.size() - 2);  // one reply
    feed(":me!u@h NOTICE me :\x01VERSION x\x01"), feed(":me!u@h NOTICE me :\x01VERSION x\x01");  // the reply: shown once
    size_t vn = 0;
    for (auto& b : core.bufs) for (auto& l : b->lines) vn += l.text.find("reply from me: x") != std::string::npos;
    assert(vn == 1);
    // our /ctcp to someone else, echoed back: not answered, not shown as "CTCP PING from me"
    size_t cl0 = 0, cl1 = 0;
    for (auto& b : core.bufs) cl0 += b->lines.size();
    feed(":me!u@h PRIVMSG bob :\x01PING 123\x01"), feed(":me!u@h NOTICE bob :\x01VERSION x\x01");
    for (auto& b : core.bufs) cl1 += b->lines.size();
    assert(sent().empty() && cl0 == cl1);
    // the automatic IDENTIFY echoed back: masked, in the server tab, and so is NickServ's answer; no NickServ tab
    assert(mask_services("NickServ", "identify me hunter2") == "identify me ********" && mask_services("#c", "IDENTIFY x") == "IDENTIFY x" &&
           mask_services("NickServ", "GHOST me pw") == "GHOST me ********" && mask_services("ChanServ", "IDENTIFY #c pw") == "IDENTIFY #c ********" &&
           mask_services("ChanServ", "REGISTER #c pw My channel") == "REGISTER #c ******** My channel" &&
           mask_services("ChanServ", "SET #c PASSWORD pw") == "SET #c PASSWORD ********" && mask_services("ChanServ", "OP #c bob") == "OP #c bob" &&
           mask_services("nickserv", "SET PASSWORD x") == "SET PASSWORD ********" && mask_services("NickServ", "INFO me") == "INFO me");
    // ...and where the input line starts hiding it (*s while typing, no history)
    assert(input_secret("#c", "/msg NickServ identify hunter2") == 23 && input_secret("#c", "/MSG nickserv ") == std::string::npos &&
           input_secret("NickServ", "id hunter2") == 3 && input_secret("#c", "identify x") == std::string::npos &&
           input_secret("#c", "/query nickserv SET PASSWORD x") == 29 && input_secret("#c", "/msg bob identify x") == std::string::npos);
    {  // REGISTER <password> <email>: only the password is hidden
        size_t e;
        assert(input_secret("#c", "/msg NickServ register pw me@x.org", &e) == 23 && e == 25);
        assert(mask_services("NickServ", "REGISTER pw me@x.org") == "REGISTER ******** me@x.org");
    }
    ts->ident_at = time(nullptr);
    feed(":me!u@h PRIVMSG NickServ :IDENTIFY hunter2");
    feed(":NickServ!s@services NOTICE me :You are now identified for me.");
    assert(!core.find(ts, "NickServ") && ts->status->lines.back().text.find("now identified") != std::string::npos);
    assert(ts->status->lines[ts->status->lines.size() - 2].text == "IDENTIFY ********");
    ts->ident_at = 0;
    {  // a notice without a query: the server tab, not the tab you're on; after a /command, that command's tab
        core.select(chb), ts->typed_at = 0;
        size_t n = chb->lines.size();
        feed(":NickServ!s@services NOTICE me :This nickname is registered.");
        assert(chb->lines.size() == n && ts->status->lines.back().text == "This nickname is registered.");
        feed(":CTCPServ!s@services PRIVMSG me :\x01VERSION\x01");
        assert(chb->lines.size() == n && ts->status->lines.back().text.find("CTCP VERSION from CTCPServ") == 0);
        sent();
        core.exec(chb, "/msg bob hi");
        feed(":bob!b@h NOTICE me :hello back");
        assert(chb->lines.back().text == "hello back" && !core.find(ts, "bob"));
    }
    ts->caps.erase("echo-message");

    // /typing: other people's messages get a typed_at, our own and info lines don't
    chb->quiet = false;
    core.exec(chb, "/typing on");  // quiet comes on with it...
    assert(chb->quiet && chb->quiet_typing);
    feed(":bob!u@h PRIVMSG #c :hello there");
    assert(chb->lines.back().typed_at > 0 && core.typing_until > chb->lines.back().typed_at);
    Line tl = chb->lines.back();
    assert(typed_chars(tl, 11, tl.typed_at + 99) == 3 && typed_chars(tl, 11, tl.typed_at - 5) == 0);
    assert(typed_chars(tl, 200, tl.typed_at + 2000) == 200);  // a long line: 2s at most
    core.exec(chb, "/typing");  // ...and goes off with it
    assert(!chb->quiet && !chb->quiet_typing);
    feed(":bob!u@h PRIVMSG #c :bye");
    assert(chb->lines.back().typed_at == 0);
    core.exec(chb, "/quiet on");  // quiet on purpose: /typing off leaves it
    core.exec(chb, "/typing on");
    assert(chb->quiet && !chb->quiet_typing);
    core.exec(chb, "/set typing_effect off");
    assert(chb->quiet);
    core.exec(chb, "/quiet off");

    // our /nick is remembered per server (in the file when the server is saved); /set nick drops it
    {
        std::string path = "build/test-nick.conf";
        unlink(path.c_str());
        std::swap(core.cfg_path, path);
        feed(":" + ts->nick + "!u@h NICK newme");
        assert(ts->nick == "newme" && ts->last_nick == "newme" && ini_get(core.cfg, "server." + ts->cfg.name, "last_nick") == "newme");
        Ini disk;
        bool in_file = ini_load(core.cfg_path, disk) && ini_get(disk, "server." + ts->cfg.name, "last_nick") == "newme";
        bool pending = core.unsaved.count("server." + ts->cfg.name) && core.unsaved["server." + ts->cfg.name].count("last_nick");
        assert(in_file != pending);  // one or the other, by whether the server is saved
        core.exec(home, "/set nick me");
        assert(ts->last_nick.empty() && ini_get(core.cfg, "server." + ts->cfg.name, "last_nick").empty());
        feed(":newme!u@h NICK me");
        // the settings window lists the usual server keys even when unset, with what they fall back to
        bool nick_row = false, tls_row = false;
        for (auto& r : core.setting_rows())
            if (r.sec == "server." + ts->cfg.name) nick_row |= r.key == "altnick" && !r.def.empty(), tls_row |= r.key == "tls_verify" && r.choices.size() == 2;
        assert(nick_row && tls_row);
        // /set back to the value it had: no longer unsaved
        {
            std::string sv = "server." + ts->cfg.name, was = ini_get(core.cfg, sv, "tls_verify");
            core.exec(home, "/set " + sv + ".tls_verify " + std::string(was == "false" ? "true" : "false"));
            assert(core.unsaved.count(sv) && core.unsaved[sv].count("tls_verify"));
            core.exec(home, "/set " + sv + ".tls_verify " + (was.empty() ? "\"\"" : was));
            assert(!core.unsaved.count(sv) || !core.unsaved[sv].count("tls_verify"));
        }
        // /set key "": back to the default; theme "" is the default theme, not "theme '' not found"
        core.exec(home, "/set theme \"\"");
        assert(core.theme.name == "default" && home->lines.back().text.find("(default)") != std::string::npos);
        // every /set key takes "" and 0 without breaking what reads it; port 0 is the usual port
        {
            Ini keep = core.cfg;
            auto keep_unsaved = core.unsaved;
            std::string sv = "server." + ts->cfg.name;
            for (auto& k : setting_keys()) {
                if (k.key == "name" || k.key == "lock" || k.key == "exec_web" || k.key == "lock_password_hash") continue;
                std::string full = (k.scope == 'n' ? sv + "." : "") + k.key;
                for (const char* v : {"0", "\"\""}) {
                    core.exec(home, "/set " + full + " " + v);
                    core.setting_rows(), core.settings(), core.server_cfg(ts->cfg.name);
                }
            }
            core.exec(home, "/set " + sv + ".port 0");
            assert(core.server_cfg(ts->cfg.name).port == 6667 || core.server_cfg(ts->cfg.name).port == 6697);
            core.cfg = keep, core.unsaved = keep_unsaved;
            core.exec(home, "/set theme \"\"");
            core.exec(home, "/set scrollback \"\"");
        }
        // ...in a sensible order: [general] first (by the template), then the servers (host first, nick next to
        // altnick, auto_identify among the login keys, unknown keys last)
        {
            std::vector<std::string> ord;
            for (auto& r : core.setting_rows()) ord.push_back(r.sec + "." + r.key);
            auto at = [&](const std::string& k) { return std::find(ord.begin(), ord.end(), k) - ord.begin(); };
            std::string sv = "server." + ts->cfg.name + ".";
            assert(at("general.nick") < at("general.paste_confirm") && at("general.typing_effect") < at(sv + "host"));
            assert(at(sv + "host") < at(sv + "nick") && at(sv + "nick") + 1 == at(sv + "altnick"));
            assert(at(sv + "password") < at(sv + "auto_identify") && at(sv + "auto_identify") < at(sv + "nickserv_pass"));
            assert(at(sv + "last_nick") == (long)ord.size());  // state, not a setting
        }
        // a network lists every [general] key it can override: unset, it inherits [general]'s value;
        // set, [general]'s row names the network. A [general]-only key set on a network does nothing
        {
            std::string sv = "server." + ts->cfg.name;
            core.cfg["general"]["log_history"] = "77";
            const Core::SetRow* inh = nullptr;
            auto rows = core.setting_rows();
            for (auto& r : rows) if (r.sec == sv && r.key == "log_history") inh = &r;
            assert(inh && inh->shared && inh->val.empty() && inh->def == "77" && inh->cat == sv + "/Logs");
            core.cfg[sv]["log_history"] = "5", core.cfg[sv]["theme"] = "mirc";
            rows = core.setting_rows();
            bool over = false, gen_theme = false;
            for (auto& r : rows) {
                over |= r.sec == "general" && r.key == "log_history" && r.over == ts->cfg.name + ": 5" && r.cat == "general/Logs";
                gen_theme |= r.sec == sv && r.key == "theme" && r.grp == "Other";
            }
            assert(over && !gen_theme && core.opt(ts, "log_history", "1") == "5" && core.opt(ts, "theme", "x") == ini_get(core.cfg, "general", "theme", "x"));
            core.cfg[sv].erase("log_history"), core.cfg[sv].erase("theme"), core.cfg["general"].erase("log_history");
            // nick_width unset: its row shows the running theme's width (what the chat uses), not "(not set)"
            bool nwrow = false;
            for (auto& r : core.setting_rows()) nwrow |= r.sec == "general" && r.key == "nick_width" && r.def == core.theme.lay("nick_width", "12") && !r.def.empty();
            assert(nwrow);
            // complete_suffix: ":" by default, any text, off/none for nothing (Tab and the webchat add the space)
            assert(core.complete_suffix() == ":");
            core.cfg["general"]["complete_suffix"] = ",";
            assert(core.complete_suffix() == ",");
            core.cfg["general"]["complete_suffix"] = "off";
            assert(core.complete_suffix().empty());
            core.cfg["general"].erase("complete_suffix");
        }
        unlink(core.cfg_path.c_str());
        std::swap(core.cfg_path, path);
    }

    // /guide: pages turn and stay in range; the box turns show_guide off, in the file too
    {
        std::string path = "build/test-guide.conf";
        unlink(path.c_str());
        std::swap(core.cfg_path, path);
        core.exec(home, "/guide 99");
        Buffer* g = core.find(nullptr, "*guide");
        assert(g && core.active == g && core.guide_page > 0);
        int last = core.guide_page;
        core.exec(g, "/guide next");
        assert(core.guide_page == last);
        core.exec(g, "/guide on"), core.exec(g, "/guide 1"), core.exec(g, "/guide prev");
        assert(core.guide_page == 0 && g->lines.back().text.find("☐") != std::string::npos);
        core.exec(g, "/guide off");
        Ini disk;
        assert(ini_load(core.cfg_path, disk) && ini_get(disk, "general", "show_guide") == "off");
        assert(g->lines.back().text.find("☑") != std::string::npos);
        core.exec(g, "/guide close");
        assert(!core.find(nullptr, "*guide"));
        unlink(core.cfg_path.c_str());
        std::swap(core.cfg_path, path);
    }

    // every shipped theme loads and colors the main parts
    auto tn = theme_names();
    assert(tn.size() >= 35 && std::find(tn.begin(), tn.end(), "dracula") != tn.end() && std::find(tn.begin(), tn.end(), "catppuccin-latte") != tn.end());
    for (auto& n : tn) {
        Theme th;
        std::string terr;
        assert(load_theme(n, th, terr));
        for (const char* k : {"default", "status", "tab_active", "input", "timestamp", "join", "error"}) assert(th.el.count(k));
        assert(th.nicks.size() >= (n.rfind("mirc", 0) == 0 || n.rfind("mono", 0) == 0 ? 1u : 6u));  // mirc, mono: one nick color
    }
    {  // mono: mIRC color codes in text keep the base colors, bold still works
        Theme th;
        std::string terr;
        auto walk = [] { std::vector<Style> v; mirc_walk("\x03" "04,02a\x04" "ff0000b\x02" "c", Style{0x111111, 0x222222, 0}, [&](uint32_t, const Style& st) { v.push_back(st); }); return v; };
        assert(load_theme("mono", th, terr) && !mirc_colors);
        auto v = walk();
        assert(v.size() == 3 && v[0].fg == 0x111111 && v[0].bg == 0x222222 && v[1].fg == 0x111111 && (v[2].attr & A_BOLD));
        assert(load_theme("mono-light", th, terr) && !mirc_colors && load_theme("default", th, terr) && mirc_colors);
        assert(walk()[0].fg != 0x111111);
    }
    {  // theme_dir: themes from there too, the shipped ones still found after it
        char d[] = "/tmp/isekur-themes-XXXXXX";
        assert(mkdtemp(d));
        std::string p = std::string(d) + "/mine.theme", terr, was = theme_dir;
        FILE* f = fopen(p.c_str(), "w");
        fputs("[colors]\ndefault = white black\n", f), fclose(f);
        theme_dir = d;
        Theme th;
        assert(load_theme("mine", th, terr) && load_theme("dracula", th, terr));
        auto names = theme_names();
        assert(std::find(names.begin(), names.end(), "mine") != names.end());
        theme_dir = was;
        unlink(p.c_str()), rmdir(d);
    }

    // mirc_prefix: n visible characters, the codes around them kept
    assert(mirc_prefix("\x02iSe\x02kur", 4) == "\x02iSe\x02k" && mirc_prefix("\x03" "04,12red", 2) == "\x03" "04,12re");
    assert(mirc_prefix("\x04" "ff00aaab", 1) == "\x04" "ff00aaa" && mirc_prefix("hi", 9) == "hi" && mirc_strip(mirc_prefix("a\x03" "1,b", 2)) == "a,");

    // /list: a table in *channels, busiest first, the [+modes] kept
    feed(":srv 321 me Channel :Users  Name");
    feed(":srv 322 me #small 3 :[+nt] hi");
    feed(":srv 322 me #big 250 :Big one");
    feed(":srv 323 me :End of /LIST");
    Buffer* lb = core.find(ts, "*channels");
    assert(lb && core.active == lb && lb->lines.size() == 5 && lb->lines[0].nolog);
    assert(mirc_strip(lb->lines[1].text).rfind("  #  Channel  Users \u25BC  Topic", 0) == 0);
    assert(mirc_strip(lb->lines[2].text) == " 1.  #big         250  Big one" && mirc_strip(lb->lines[3].text) == " 2.  #small         3  [+nt] hi");
    core.exec(lb, "/list sort name");  // A-Z, then Z-A on a second time
    assert(mirc_strip(lb->lines[2].text).rfind(" 1.  #big", 0) == 0 && mirc_strip(lb->lines[1].text).find("Channel \u25BC") != std::string::npos);
    core.exec(lb, "/list sort name");
    assert(mirc_strip(lb->lines[2].text).rfind(" 1.  #small", 0) == 0 && mirc_strip(lb->lines[1].text).find("Channel \u25B2") != std::string::npos);
    core.exec(lb, "/list sort users");
    assert(mirc_strip(lb->lines[2].text).rfind(" 1.  #big", 0) == 0);
    core.select(chb);

    {  // /sv: the CTCP VERSION text as an action
        int fl = ts->flood_ms;
        ts->flood_ms = 0, ts->sendq.clear(), sent();
        core.exec(chb, "/sv");  // send version: the CTCP VERSION text as an action
        std::string sv = sent();
        assert(sv.rfind("PRIVMSG #c :\x01" "ACTION is using iSekur " ISEKUR_VERSION, 0) == 0 && sv.find("isekur-site") != std::string::npos);
        core.exec(home, "/sv");
        assert(sent().empty() && home->lines.back().kind == LK::ERROR);
        ts->flood_ms = fl;
    }
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
    // each config its own socket, so several share a folder; isekur.conf keeps isekur.sock
    assert(sock_path_for("a/isekur.conf") == "a/isekur.sock" && sock_path_for("a/work.conf") == "a/work.sock" &&
           sock_path_for("a/.conf") == "a/.conf.sock" && sock_path_for("myconf") == "myconf.sock" && cfg_side("a/w.conf", "log") == "a/w.log");
    assert(sp == "build/isekur-test.sock" && s1.listen(sp, serr) == 1 && s2.listen(sp, serr) == -1);
    s1.close();
    assert(access(sp.c_str(), F_OK) != 0 && s2.listen(sp, serr) == 1);
    s2.close();
    assert(Session().listen(std::string(200, 'x'), serr) == 0 && serr.find("too long") != std::string::npos);
#endif

    assert(ctcp_version().rfind(version_string() + " :: ", 0) == 0 && version_string().find(ISEKUR_VERSION) != std::string::npos);

    {  // /dcc: an offer, then a real download from a loopback sender
        std::string derr;
        int lfd = listen_tcp("127.0.0.1", 0, derr);
        assert(lfd >= 0);
        sockaddr_in la{};
        socklen_t ll = sizeof la;
        getsockname(lfd, (sockaddr*)&la, &ll);
        std::string port = std::to_string(ntohs(la.sin_port));
        system("rm -rf build/dcctest");
        core.cfg["general"]["dcc_dir"] = "build/dcctest";
        core.cfg["server.t"]["dcc"] = "on";  // a server's own dcc isn't read: [general] decides
        core.cfg["general"]["dcc"] = "off";  // off: the offer is dropped, /dcc get and send refused
        feed(":bob!u@h PRIVMSG me :\x01" "DCC SEND x.txt 2130706433 " + port + " 11\x01");
        Buffer* nb = core.active && core.active->srv == ts ? core.active : ts->status;  // where DCC news goes
        assert(core.dccs.empty() && nb->lines.back().text.find("dcc is off") != std::string::npos);
        core.exec(chb, "/dcc send bob x.txt");
        assert(core.dccs.empty() && chb->lines.back().text == "DCC is off (/set dcc on)");
        core.cfg["server.t"].erase("dcc"), core.cfg["general"].erase("dcc");
        feed(":bob!u@h PRIVMSG me :\x01" "DCC SEND \"../.x/.hi there.txt\" 2130706433 " + port + " 11\x01");
        assert(core.dccs.size() == 1 && core.dccs[0]->file == "hi there.txt" && core.dccs[0]->host == "127.0.0.1");
        feed(":bob!u@h PRIVMSG me :\x01" "DCC SEND x ::1 0 5 tok\x01");  // passive: not taken
        assert(core.dccs.size() == 1);
        core.exec(chb, "/dcc get bob");
        Dcc* d = core.dccs[0].get();
        assert(d->st == Dcc::RUN && d->path == "build/dcctest/hi there.txt");
        int peer = -1;
        std::string acks;
        for (int i = 0; i < 500 && d->st == Dcc::RUN; i++) {
            std::vector<pollfd> pf{{d->conn.fd, d->conn.events(), 0}};
            poll(pf.data(), 1, 10);
            core.dccs_poll(pf, 0);
            if (peer < 0 && (peer = accept(lfd, nullptr, nullptr)) >= 0) assert(write(peer, "hello world", 11) == 11);
        }
        assert(d->st == Dcc::DONE && d->got == 11);
        char ab[8];
        pollfd pp{peer, POLLIN, 0};
        poll(&pp, 1, 2000);  // BSD/macOS: accept() hands the listener's O_NONBLOCK on, the ack may still be in flight
        assert(read(peer, ab, sizeof ab) >= 4);  // an ack of all 11 bytes, big-endian, comes last
        FILE* f = fopen("build/dcctest/hi there.txt", "rb");
        char fb[32] = "";
        assert(f && fread(fb, 1, sizeof fb, f) == 11 && std::string(fb) == "hello world");
        fclose(f), close(peer), close(lfd);

        // the loop main() runs, for the transfers only; `side` is the other end
        auto run = [&](Dcc* x, const std::function<void()>& side) {
            for (int i = 0; i < 500 && x->st != Dcc::DONE && x->st != Dcc::FAILED; i++) {
                std::vector<pollfd> pf;
                for (auto& y : core.dccs)
                    if (y->conn.fd >= 0) pf.push_back({y->conn.fd, y->conn.events(), 0});
                    else if (y->lfd >= 0) pf.push_back({y->lfd, POLLIN, 0});
                poll(pf.data(), pf.size(), 5);
                core.dccs_poll(pf, 0);
                side();
            }
        };
        auto dial = [](int port) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in a{};
            a.sin_family = AF_INET, a.sin_port = htons((uint16_t)port), a.sin_addr.s_addr = htonl(0x7f000001);
            assert(connect(fd, (sockaddr*)&a, sizeof a) == 0);
            return fd;
        };

        // resume: "r.txt" is there with 5 of its 11 bytes, so /dcc get asks for the rest
        f = fopen("build/dcctest/r.txt", "wb");
        fputs("hello", f), fclose(f);
        lfd = listen_tcp("127.0.0.1", 0, derr);
        ll = sizeof la, getsockname(lfd, (sockaddr*)&la, &ll);
        port = std::to_string(ntohs(la.sin_port));
        feed(":bob!u@h PRIVMSG me :\x01" "DCC SEND r.txt 2130706433 " + port + " 11\x01");
        sent();
        core.exec(chb, "/dcc get");
        d = core.dccs.back().get();
        assert(d->st == Dcc::RESUME && d->pos == 5 && sent() == "PRIVMSG bob :\x01" "DCC RESUME r.txt " + port + " 5\x01\r\n");
        feed(":bob!u@h PRIVMSG me :\x01" "DCC ACCEPT r.txt " + port + " 5\x01");
        assert(d->st == Dcc::RUN);
        peer = -1;
        run(d, [&] { if (peer < 0 && (peer = accept(lfd, nullptr, nullptr)) >= 0) assert(write(peer, " world", 6) == 6); });
        assert(d->st == Dcc::DONE && d->got == 11);
        f = fopen("build/dcctest/r.txt", "rb");
        memset(fb, 0, sizeof fb);
        assert(fread(fb, 1, sizeof fb, f) == 11 && std::string(fb) == "hello world");
        fclose(f), close(peer), close(lfd);

        // /dcc send: a name alone is in dcc_dir; we listen, they connect, read it all and ack it
        core.cfg["general"]["dcc_ip"] = "127.0.0.1";
        core.exec(chb, "/dcc send bob nope.txt");
        assert(core.dccs.back().get() == d && sent().empty());  // no such file: nothing offered
        core.exec(chb, "/dcc send bob r.txt");
        Dcc* o = core.dccs.back().get();
        std::string offer = sent();
        assert(o->send && o->st == Dcc::WAIT && offer.find("DCC SEND r.txt 2130706433 " + std::to_string(o->port) + " 11\x01") != std::string::npos);
        int rcv = dial(o->port);
        std::string got;
        run(o, [&] {
            char rb[64];
            ssize_t n = recv(rcv, rb, sizeof rb, MSG_DONTWAIT);
            if (n > 0) {
                got.append(rb, (size_t)n);
                unsigned char ack[4] = {0, 0, 0, (unsigned char)got.size()};
                assert(write(rcv, ack, 4) == 4);
            }
        });
        assert(o->st == Dcc::DONE && got == "hello world");
        close(rcv);

        // they resume ours at 6: we accept and send the rest only
        core.exec(chb, "/dcc send bob r.txt");
        o = core.dccs.back().get();
        sent();
        feed(":bob!u@h PRIVMSG me :\x01" "DCC RESUME r.txt " + std::to_string(o->port) + " 6\x01");
        assert(o->pos == 6 && sent() == "PRIVMSG bob :\x01" "DCC ACCEPT r.txt " + std::to_string(o->port) + " 6\x01\r\n");
        rcv = dial(o->port), got.clear();
        run(o, [&] {
            char rb[64];
            ssize_t n = recv(rcv, rb, sizeof rb, MSG_DONTWAIT);
            if (n > 0) {
                got.append(rb, (size_t)n);
                unsigned char ack[4] = {0, 0, 0, (unsigned char)(6 + got.size())};
                assert(write(rcv, ack, 4) == 4);
            }
        });
        assert(o->st == Dcc::DONE && got == "world");
        close(rcv);
    }

    {  // the webchat end to end: login, WebSocket, nick lists only when they change, older lines on request
        core.cfg["web"] = {{"bind", "127.0.0.1"}, {"port", "18799"}, {"user", "u"}, {"password_hash", hash_password("pw")}};
        std::string path = "build/test-web.conf", werr;
        std::swap(core.cfg_path, path);
        Web web(core);
        {  // the status bar's [away: ...]: each network's 306 state, connected only
            std::string key = json_str(ts->network.empty() ? ts->cfg.name : ts->network) + ":[";
            auto flag = [&] { std::string j = web.stat_json(); size_t at = j.find(key); assert(at != std::string::npos); return j.substr(j.find(']', at) - 2, 2); };
            assert(flag() == ",0");
            ts->away = true;
            assert(flag() == ",1");
            ts->state = SS::DISCONNECTED;
            assert(flag() == ",0");
            ts->state = SS::CONNECTED, ts->away = false;
        }
        assert(web.start(werr));
        core.listener = [&](Ev e, Buffer* b, const Line* l) { web.on_event(e, b, l); };
        auto step = [&] {
            std::vector<pollfd> pf;
            web.add_fds(pf);
            poll(pf.data(), pf.size(), 10);
            web.process(pf), web.flush();
        };
        auto talk = [&](const std::string& req, size_t want) {  // send, then read until `want` bytes or a quiet moment
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in a{};
            a.sin_family = AF_INET, a.sin_port = htons(18799), a.sin_addr.s_addr = htonl(0x7f000001);
            assert(connect(fd, (sockaddr*)&a, sizeof a) == 0 && write(fd, req.data(), req.size()) == (ssize_t)req.size());
            std::string got;
            for (int quiet = 0; quiet < 20 && got.size() < want;) {
                step();
                char rb[65536];
                ssize_t n = recv(fd, rb, sizeof rb, MSG_DONTWAIT);
                if (n > 0) got.append(rb, (size_t)n), quiet = 0;
                else quiet++;
            }
            return std::make_pair(fd, got);
        };
        auto [lfd2, login] = talk("POST /login HTTP/1.1\r\nHost: x\r\nContent-Length: 24\r\n\r\n{\"user\":\"u\",\"pass\":\"pw\"}", 1);
        close(lfd2);
        size_t ck = login.find("isekur_sid=");
        assert(login.rfind("HTTP/1.1 200", 0) == 0 && ck != std::string::npos);
        std::string cookie = login.substr(ck, login.find(';', ck) - ck);
        auto [ws, snap] = talk("GET /ws HTTP/1.1\r\nHost: x\r\nCookie: " + cookie + "\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n", 1 << 30);
        std::string me = "{\"t\":\"buf\",\"id\":" + std::to_string(chb->id) + ",";
        auto buf_msg = [&](const std::string& all) { size_t at = all.rfind(me); assert(at != std::string::npos); return all.substr(at, all.find("\"t\":\"", at + 5) - at); };
        assert(snap.find("101 Switching") != std::string::npos && buf_msg(snap).find("\"nicks\":[") != std::string::npos);
        auto more = [&](const std::string& out) {  // what the server says after we send out
            if (!out.empty()) assert(write(ws, out.data(), out.size()) == (ssize_t)out.size());
            std::string got;
            for (int i = 0; i < 30; i++) {
                step();
                char rb[65536];
                ssize_t n = recv(ws, rb, sizeof rb, MSG_DONTWAIT);
                if (n > 0) got.append(rb, (size_t)n);
            }
            return got;
        };
        core.notify(Ev::BUFFER, chb);  // the first update after a browser comes in has it once more,
        assert(buf_msg(more("")).find("\"nicks\":[") != std::string::npos);
        core.notify(Ev::BUFFER, chb);  // after that, say an unread count changed: the list stays out
        std::string upd = more("");
        assert(buf_msg(upd).find("\"nicks\"") == std::string::npos);
        feed(":newbie!u@h JOIN #c");  // it changed: in again
        assert(buf_msg(more("")).find("newbie") != std::string::npos);
        for (int i = 0; i < 700; i++) core.add(chb, Line{time(nullptr), LK::INFO, "", "fill " + std::to_string(i)});
        long long base = chb->added - (long long)chb->lines.size(), have = base + (long long)chb->lines.size() - 500;
        std::string old = more(client_frame("{\"t\":\"more\",\"buf\":" + std::to_string(chb->id) + ",\"before\":" + std::to_string(have) + "}"));
        size_t at = old.find("{\"t\":\"older\"");
        assert(at != std::string::npos && old.find("\"first\":" + std::to_string(std::max(base, have - 500)), at) != std::string::npos);
        assert(old.find("\"tf\":\"", at) != std::string::npos);  // the time stamp, formatted by iSekur
        {  // a tab picked in the browser moves the session's focus: the one left behind gets activity again (a /join had focused it)
            Buffer* was = core.active;
            core.select(chb);  // a tab picked in the terminal (Ctrl+A...): the browsers follow
            assert(more("").find("{\"t\":\"active\",\"buf\":" + std::to_string(chb->id) + "}") != std::string::npos);
            // an old page's echo (no "pick") moves nothing: two quick switches bounced between the tabs for ever
            more(client_frame("{\"t\":\"focus\",\"buf\":" + std::to_string(core.bufs.front()->id) + "}"));
            assert(core.active == chb);
            more(client_frame("{\"t\":\"focus\",\"buf\":" + std::to_string(core.bufs.front()->id) + ",\"pick\":\"1\"}"));
            assert(core.active == core.bufs.front().get());
            feed(":bob!u@h PRIVMSG #c :hi");
            assert(chb->activity == 2);
            core.select(was);
        }
        close(ws);
        core.listener = nullptr;
        web.stop();
        unlink("build/test-web.web.sessions");
        std::swap(core.cfg_path, path);
        core.max_lines = 150;  // /set scrollback: tabs keep that many
        core.add(chb, Line{time(nullptr), LK::INFO, "", "trim"});
        assert(chb->lines.size() == 150 && chb->lines.back().text == "trim");
        core.max_lines = 2000;
    }

    {  // a ZNC attach: caps, *playback, its buffer batch, *status, other clients' messages
        ServerCfg zc;
        zc.name = "znc", zc.nick = "me", zc.pass = "me/libera:pw";
        Server* z = core.add_server(zc);
        z->flood_ms = 0;
        auto zf = [&](const std::string& l) { Msg mm; assert(irc_parse(l, mm)); core.handle(z, mm); };
        auto zs = [&]() { std::string o = z->conn.out; z->conn.out.clear(); return o; };
        Buffer* zb = core.open(z, BT::CHANNEL, "#c");
        zb->lines.push_back(Line{1000, LK::MSG, "bob", "old", false, false});
        zf(":irc.znc.in CAP * LS :znc.in/self-message znc.in/playback server-time batch");
        assert(zs() == "CAP REQ :batch server-time znc.in/self-message znc.in/playback\r\n");
        zf(":irc.znc.in CAP * ACK :batch server-time znc.in/self-message znc.in/playback");
        assert(zs() == "CAP END\r\n");
        zf(":irc.znc.in 001 me2 :Welcome");
        std::string o = zs();
        assert(z->nick == "me2" && o.find("PRIVMSG *playback :PLAY * 1000\r\n") != std::string::npos);
        zf(":me2!u@h JOIN #c");
        zs();
        zf(":irc.znc.in BATCH +b1 znc.in/playback #c");
        zf("@batch=b1;time=1970-01-01T00:16:40.000Z :bob!u@h PRIVMSG #c :old");          // already have it
        zf("@batch=b1;time=1970-01-01T00:16:50.000Z :bob!u@h PRIVMSG #c :me2: missed");  // new, no ring
        zf("@batch=b1;time=1970-01-01T00:16:51.000Z :bob!u@h PRIVMSG me2 :\x01VERSION\x01");
        zf(":irc.znc.in BATCH -b1");
        assert(zb->lines.back().text == "me2: missed" && !zb->lines.back().hilight && zs().empty());  // old CTCP: no reply
        assert(std::count_if(zb->lines.begin(), zb->lines.end(), [](const Line& l) { return l.text == "old"; }) == 1);
        zf(":me2!u@h PRIVMSG bob :from my phone");  // self-message: goes to bob's tab, as ours
        Buffer* bob = core.find(z, "bob");
        assert(bob && bob->lines.back().self && bob->lines.back().nick == "me2");
        zf(":*status!znc@znc.in PRIVMSG me2 :You have no networks");
        Buffer* st = core.find(z, "*status");
        assert(st && st->type == BT::QUERY);
        core.exec(st, "help");  // a *name query you can talk to
        assert(zs() == "PRIVMSG *status :help\r\n");
        zf(":irc.znc.in 464 me2 :Password required");
        assert(z->status->lines.back().text == "Password required");
    }

    {  // IRCv3: STS, ISUPPORT, WHOX, accounts, typing, replies, reactions, labels, standard replies, redaction,
       // read markers, rename, MONITOR / ISON, multiline
        assert(irc_lower("Nick[A]\\~", 1) == "nick[a]\\~" && irc_lower("Nick[A]\\~", 2) == "nick{a}|~");
        assert(escape_tag("a b;c\\d") == "a\\sb\\:c\\\\d");
        ServerCfg vc;
        vc.name = "v3", vc.nick = "me", vc.host = "irc.v3.test";
        Server* v = core.add_server(vc);
        v->flood_ms = 0;
        auto vf = [&](const std::string& l) { Msg mm; assert(irc_parse(l, mm)); core.handle(v, mm); };
        auto vs = [&]() { std::string o = v->conn.out; v->conn.out.clear(); return o; };
        std::string path = "build/test-v3.conf";
        remove(path.c_str()), remove("build/sts");
        std::swap(core.cfg_path, path);
        // STS over plain text: TLS on its port, at once, nothing requested
        v->state = SS::REGISTERING;
        vf(":srv CAP * LS :sts=port=6697,duration=300 message-tags");
        assert(v->sts_port == 6697 && v->state == SS::DISCONNECTED && v->reconnect_at && vs().empty());
        // over verified TLS: the policy is kept, in the sts file next to the config
        v->conn.tls = 1, v->state = SS::REGISTERING, v->caps_ls.clear();
        vf(":srv CAP * LS :sts=duration=300 message-tags labeled-response batch draft/multiline=max-bytes=4096,max-lines=3 "
           "draft/read-marker draft/message-redaction setname account-tag account-notify extended-join echo-message");
        assert(core.sts.count("irc.v3.test") && core.sts["irc.v3.test"].first == 6697);
        core.sts.clear(), core.sts_load();
        assert(core.sts.count("irc.v3.test"));
        std::string o = vs();
        assert(o.rfind("CAP REQ :", 0) == 0 && o.find("sts") == std::string::npos && o.find("labeled-response") != std::string::npos);
        vf(":srv CAP me ACK :message-tags labeled-response batch draft/multiline draft/read-marker draft/message-redaction setname account-tag account-notify extended-join echo-message");
        assert(vs() == "CAP END\r\n");
        vf(":srv 001 me :Welcome");
        vs();
        vf(":srv 005 me CASEMAPPING=ascii MONITOR=2 WHOX BOT=B CLIENTTAGDENY=*,-typing,-draft/react,-draft/reply NICKLEN=9 :are supported");
        assert(v->casemap == 1 && v->isupport["MONITOR"] == "2" && v->tag_ok("typing") && v->tag_ok("react") && !v->tag_ok("foo"));
        vf(":srv 005 me -NICKLEN :are supported");
        assert(!v->isupport.count("NICKLEN"));
        // UTF8ONLY: invalid bytes go out as UTF-8 (Latin-1 é), valid UTF-8 unchanged
        v->send("PRIVMSG #v3 :caf\xe9 \xc3\xa9"), assert(vs().find("caf\xe9 \xc3\xa9\r\n") != std::string::npos);
        vf(":srv 005 me UTF8ONLY :are supported");
        v->send("PRIVMSG #v3 :caf\xe9 \xc3\xa9"), assert(vs().find("caf\xc3\xa9 \xc3\xa9\r\n") != std::string::npos);
        // our join: NAMES, then WHOX fills accounts, real names, away and bots, and none of it is shown
        vf(":me!u@h JOIN #v3 * :Me");
        Buffer* vb = core.find(v, "#v3");
        assert(vb && v->who_auto.count("#v3"));
        vs();
        vf(":srv 353 me = #v3 :@me Bob[x] bot1");
        vf(":srv 366 me #v3 :End of /NAMES list.");
        assert(vs() == "WHO #v3 %tcuhnfar,777\r\n");
        size_t n0 = vb->lines.size();
        vf(":srv 354 me 777 #v3 ~b b.host Bob[x] G acct1 :Bob Real");
        vf(":srv 354 me 777 #v3 ~t t.host bot1 HB 0 :I am a bot");
        vf(":srv 315 me #v3 :End of WHO list");
        assert(vb->lines.size() == n0 && v->who_auto.empty());
        NickEntry& bob = vb->nicks["bob[x]"];  // ascii casemapping: [ stays [
        assert(bob.away && bob.account == "acct1" && bob.realname == "Bob Real" && bob.host == "~b@b.host");
        assert(vb->nicks["bot1"].bot && vb->nicks["bot1"].account.empty());
        vf(":Bob[x]!b@h ACCOUNT *");
        assert(bob.account.empty());
        vf("@account=acct2 :Bob[x]!b@h PRIVMSG #v3 :hi");
        assert(bob.account == "acct2");
        vf(":Bob[x]!b@h SETNAME :New Name");
        assert(bob.realname == "New Name");
        vf(":carol!c@h JOIN #v3 cacct :Carol C");
        assert(vb->nicks["carol"].account == "cacct" && vb->nicks["carol"].realname == "Carol C");
        vf(":bob{x}!z@h JOIN #v3");  // ascii: someone else than Bob[x]
        assert(vb->nicks.count("bob{x}") && vb->nicks.count("bob[x]"));
        // +typing in, ended by their message
        vf("@+typing=active :carol!c@h TAGMSG #v3");
        assert(vb->typing.count("carol"));
        vf(":carol!c@h PRIVMSG #v3 :done typing");
        assert(!vb->typing.count("carol"));
        // msgid, replies (one to us hilights), reactions
        vf("@msgid=m1 :me!u@h PRIVMSG #v3 :my words here");
        assert(vb->lines.back().msgid == "m1" && vb->lines.back().self);
        vf("@msgid=m2;+draft/reply=m1 :carol!c@h PRIVMSG #v3 :agreed");
        assert(vb->lines.back().hilight && mirc_strip(vb->lines.back().text) == "↪ me: “my words here” agreed");
        vf("@+draft/react=\U0001F44D;+draft/reply=m2 :Bob[x]!b@h TAGMSG #v3");
        assert(vb->lines.back().text == "Bob[x] reacted \U0001F44D to carol's message “" "agreed”");
        core.exec(vb, "/reply carol sure");
        assert(vs() == "@+draft/reply=m2 PRIVMSG #v3 :sure\r\n");
        core.exec(vb, "/react \U0001F389 carol");
        assert(vs() == "@+draft/react=\U0001F389;+draft/reply=m2 TAGMSG #v3\r\n");
        // labeled-response: the answer goes to the tab the command was typed in, whatever is on screen
        core.select(v->status);
        auto label = [&](const std::string& out) { assert(out.rfind("@label=", 0) == 0); return out.substr(7, out.find(' ') - 7); };
        core.exec(vb, "/whois carol");
        std::string lab = label(vs());
        v->typed_at = 0;  // no 30s guess: only the label says where
        vf("@label=" + lab + " :srv 401 me carol :No such nick");
        assert(vb->lines.back().text.find("No such nick") != std::string::npos && v->labels.empty());
        core.exec(vb, "/quote CHATHISTORY LATEST #v3 * 10");
        lab = label(vs()), v->typed_at = 0;
        vf("@label=" + lab + " :srv FAIL CHATHISTORY INVALID_TARGET #v3 :No history here");
        assert(vb->lines.back().kind == LK::ERROR && vb->lines.back().text == "CHATHISTORY: No history here (INVALID_TARGET #v3)");
        core.exec(vb, "/quote WHO x");
        lab = label(vs()), v->typed_at = 0;
        vf("@label=" + lab + " :srv BATCH +lb labeled-response");
        vf("@batch=lb :srv 352 me * u h srv x H :0 X");
        vf(":srv BATCH -lb");
        assert(vb->lines.back().text.find("u h srv x") != std::string::npos && v->batch_labels.empty());
        core.select(vb);
        // redaction: theirs shows as deleted, /redact deletes our newest
        vf(":carol!c@h REDACT #v3 m2 :oops");
        Line* l2 = core.by_msgid(vb, "m2");
        assert(l2 && mirc_strip(l2->text) == "[message deleted: oops]" && !l2->hilight);
        core.exec(vb, "/redact");
        assert(vs().find(" REDACT #v3 m1\r\n") != std::string::npos);
        // read markers: another client read it all; we tell the server what's on screen, once
        vb->activity = 2, vb->unread = 3;
        vf(":srv MARKREAD #v3 timestamp=2099-01-01T00:00:00.000Z");
        assert(vb->activity == 0 && vb->unread == 0);
        Buffer* dq = core.open(v, BT::QUERY, "dave");
        core.add(dq, Line{1700000000, LK::MSG, "dave", "yo"});
        core.select(dq), core.attached = true;
        core.mark_read(v, time(nullptr));
        assert(vs() == "MARKREAD dave timestamp=2023-11-14T22:13:20.999Z\r\n");
        core.mark_read(v, time(nullptr));
        assert(vs().empty());
        // not logged in: the server refuses read markers; no error per line, none sent until 900
        size_t nl = dq->lines.size() + v->status->lines.size();
        vf(":srv FAIL MARKREAD ACCOUNT_REQUIRED dave :You must be logged in");
        core.add(dq, Line{1700000100, LK::MSG, "dave", "yo2"}), nl++;
        core.mark_read(v, time(nullptr));
        assert(vs().empty() && dq->lines.size() + v->status->lines.size() == nl);
        vf(":srv 900 me me!u@h me :You are now logged in as me");
        core.mark_read(v, time(nullptr));
        assert(vs().rfind("MARKREAD dave ", 0) == 0);
        // /topic #other text: that channel, not "#other text" as this one's topic
        core.exec(vb, "/topic #elsewhere new words");
        assert(vs().find("TOPIC #elsewhere :new words\r\n") != std::string::npos);
        core.exec(vb, "/topic #elsewhere");
        assert(vs().find("TOPIC #elsewhere\r\n") != std::string::npos);
        core.select(vb);
        vf(":srv RENAME #v3 #v4 :moved");
        assert(vb->name == "#v4" && core.find(v, "#v4") == vb && core.find(v, "#v3") == nullptr);
        // /notify: MONITOR from the end of the MOTD, within its limit
        core.exec(vb, "/notify add alice zed");
        assert(core.cfg["server.v3"]["notify"] == "alice zed" && vs().find("MONITOR") == std::string::npos);
        vf(":srv 376 me :End of /MOTD command.");
        assert(vs().find("MONITOR + alice,zed\r\n") != std::string::npos);
        vf(":srv 730 me :alice!a@a.host");
        assert(v->online["alice"] && v->status->lines.back().text.find("is online (a@a.host)") != std::string::npos && v->status->lines.back().hilight);
        vf(":srv 731 me :zed");  // offline from the start: nothing to say
        vf(":srv 731 me :alice");
        assert(v->status->lines.back().text == "alice went offline");
        core.exec(vb, "/notify del zed");
        assert(vs().find("MONITOR - zed") != std::string::npos);
        // ...and ISON where there's no MONITOR
        v->isupport.erase("MONITOR"), v->motd_done = false;
        vf(":srv 422 me :MOTD File is missing");
        assert(v->ison_at);
        vs();
        v->ison_at = 1, v->ping_at = time(nullptr) + 100, v->last_rx = time(nullptr);
        core.upd_at = time(nullptr) + 86400;  // no update check thread (its HTTPS would outlive the test)
        core.tick(time(nullptr));
        assert(vs().find("ISON alice\r\n") != std::string::npos);
        size_t sn = v->status->lines.size();
        vf(":srv 303 me :alice");
        assert(v->online["alice"] && v->ison_wait == 0 && v->status->lines.size() == sn + 1);
        // draft/multiline in: one message, its lines shown in turn, concat joined
        vf("@msgid=ml1 :carol!c@h BATCH +mm draft/multiline #v4");
        vf("@batch=mm :carol!c@h PRIVMSG #v4 :line one");
        vf("@batch=mm;draft/multiline-concat :carol!c@h PRIVMSG #v4 : continued");
        vf("@batch=mm :carol!c@h PRIVMSG #v4 :line two");
        vf(":carol!c@h BATCH -mm");
        size_t L = vb->lines.size();
        assert(vb->lines[L - 2].text == "line one continued" && vb->lines[L - 1].text == "line two" && vb->lines[L - 1].msgid == "ml1");
        // ...and out: a paste in one BATCH, unless it's over max-lines
        core.exec(vb, "a\nb");
        o = vs();
        std::string ref = o.substr(7, o.find(' ', 7) - 7);
        assert(o == "BATCH +" + ref + " draft/multiline #v4\r\n@batch=" + ref + " PRIVMSG #v4 :a\r\n@batch=" + ref + " PRIVMSG #v4 :b\r\nBATCH -" + ref + "\r\n");
        core.exec(vb, "1\n2\n3\n4");
        o = vs();
        assert(o.find("BATCH") == std::string::npos && o.find("PRIVMSG #v4 :4\r\n") != std::string::npos);
        // +typing out: active (not again within 3s), done when the line empties; not for a command
        core.typed(vb, "hel");
        assert(vs() == "@+typing=active TAGMSG #v4\r\n");
        core.typed(vb, "hell");
        assert(vs().empty());
        core.typed(vb, "");
        assert(vs() == "@+typing=done TAGMSG #v4\r\n");
        core.typed(vb, "/whois x");
        assert(vs().empty());
        // NICKLEN: refused here, not by the server
        vf(":srv 005 me NICKLEN=5 :are supported");
        core.exec(vb, "/nick waytoolong");
        assert(vs().empty() && vb->lines.back().kind == LK::ERROR);
        std::swap(core.cfg_path, path);
        remove(path.c_str()), remove("build/sts");
    }

    Server sv;  // token bucket: 2 at once, then one per second
    sv.flood_burst = 2, sv.flood_ms = 1000, sv.tokens = 2, sv.tokens_at = 0;
    for (int i = 0; i < 4; i++) sv.sendq.push_back("L" + std::to_string(i) + "\r\n");
    assert(sv.drain(0) == 1001 && sv.conn.out == "L0\r\nL1\r\n");
    assert(sv.drain(500) > 0 && sv.sendq.size() == 2);
    assert(sv.drain(1000) == 1001 && sv.sendq.size() == 1);
    assert(sv.drain(2000) == -1 && sv.conn.out == "L0\r\nL1\r\nL2\r\nL3\r\n");

    {  // Lua plugins: the sandbox, the step and memory caps, scoped options, the store, enable/disable, events
        char d[] = "/tmp/isekur-plugins-XXXXXX";
        assert(mkdtemp(d));
        std::string dir = d, pdir = dir + "/plugins";
        Core pc;
        pc.cfg_path = dir + "/isekur.conf";
        Buffer* ph = pc.open(nullptr, BT::STATUS, "isekur");
        auto last = [&](Buffer* b) { return b->lines.empty() ? std::string() : b->lines.back().text; };
        auto said = [&](const std::string& t) {
            for (auto& l : ph->lines) if (l.text.find(t) != std::string::npos) return true;
            return false;
        };
        pc.exec(ph, "/plugin");
        assert(last(ph) == "No plugins in " + pdir + ": /plugin available lists the catalog's");
        mkdir_p(pdir);
        auto put = [&](const std::string& f, const std::string& body) { FILE* o = fopen((pdir + "/" + f).c_str(), "w"); fputs(body.c_str(), o), fclose(o); };
        put("t.lua", R"LUA(
isekur.setting("greet", "hi", "c")
isekur.setting("loud", false, "n")
assert(io == nil and os == nil and package == nil and debug == nil and load == nil and dofile == nil and loadfile == nil)
isekur.command("ptest", "", function(args, srv, target)
  if args == "loop" then while true do end end
  if args == "mem" then local t = {} for i = 1, 1e8 do t[i] = string.rep("x", 1000) .. i end end
  if args == "err" then error("boom") end
  if args == "opt" then print(isekur.opt("greet"), isekur.opt("loud")) return end
  if args == "store" then isekur.store_set("k", "a\tb\nc\\") print(isekur.store_get("k") == "a\tb\nc\\") return end
  if args == "self" then isekur.exec(nil, nil, "/plugin disable t") return end
  if args == "usage" then return "Usage: /ptest thing" end
  print("ran " .. args .. " in " .. target)
end)
isekur.on("message", function(e) return e.text == "hideme" end)
print("loaded")
)LUA");
        put("bad.lua", "this is not lua");
        pc.exec(ph, "/plugins");
        assert(said("t  off"));
        pc.exec(ph, "/plugin enable ../t");
        assert(last(ph) == "No plugin named ../t in " + pdir);
        pc.exec(ph, "/plugin enable t");
        assert(said("loaded") && last(ph) == "t enabled" && ph->lines.back().badge == "plugin" && pc.plugins.size() == 1);
        Ini back;
        assert(ini_load(pc.cfg_path, back) && back["plugin.t"]["enabled"] == "on");
        pc.exec(ph, "/ptest x");
        assert(last(ph) == "ran x in isekur" && ph->lines.back().badge == "t" && ph->lines.back().badge_kind == 'p');
        pc.exec(ph, "/ptest opt");
        assert(last(ph) == "hi false");
        pc.exec(ph, "/plugin set t greet yo there");
        pc.exec(ph, "/ptest opt");
        assert(last(ph) == "yo there false");
        pc.exec(ph, "/ptest usage");
        assert(last(ph) == "Usage: /ptest thing" && ph->lines.back().kind == LK::ERROR);
        pc.exec(ph, "/ptest loop");  // killed by the step cap, still loaded
        assert(last(ph).find("error:") == 0 && ph->lines.back().badge == "t" && last(ph).find("took too long") != std::string::npos && pc.plugins.size() == 1);
        pc.exec(ph, "/ptest mem");
        assert(last(ph).find("not enough memory") != std::string::npos);
        pc.exec(ph, "/ptest store");
        assert(last(ph) == "true");
        {
            pc.plugins_poll();  // the tick writes the store (batched, at most every 5s)
            FILE* f = fopen((pdir + "/t.data").c_str(), "r");
            char buf[64] = {};
            assert(f && fread(buf, 1, sizeof buf - 1, f) > 0), fclose(f);
            assert(std::string(buf) == "k\ta\\tb\\nc\\\\\n");
        }
        pc.exec(ph, "/ptest self");
        assert(last(ph) == "/plugin can't be run by a plugin" && pc.plugins.size() == 1);
        // scoped options: channel, then network, then global; a 'n' key skips the channel's
        ServerCfg pcfg;
        pcfg.name = "sn", pcfg.nick = "me";
        Server* ps = pc.add_server(pcfg);
        ps->state = SS::CONNECTED, ps->flood_ms = 0;
        pc.exec(ph, "/plugin set t@sn/#Chan greet chan");
        pc.exec(ph, "/plugin set t@sn loud on");
        pc.exec(ph, "/plugin set t@sn/#chan loud off");
        assert(pc.plugin_opt("t", "greet", ps, "#CHAN", "d") == "chan" && pc.plugin_opt("t", "greet", ps, "#other", "d") == "yo there");
        assert(pc.plugin_opt("t", "greet", nullptr, "", "d") == "yo there" && pc.plugin_opt("t", "greet", ps, "#chan", "d", 'g') == "yo there");
        assert(pc.plugin_opt("t", "loud", ps, "#chan", "off", 'n') == "on" && pc.plugin_opt("t", "loud", ps, "#chan", "x", 'c') == "off");
        assert(pc.plugin_opt("t", "nope", ps, "#chan", "dflt") == "dflt");
        // a message handler returning true hides the line
        Buffer* pchan = pc.open(ps, BT::CHANNEL, "#chan");
        Msg pm;
        size_t n0 = pchan->lines.size();
        assert(irc_parse(":bob!u@h PRIVMSG #chan :hideme", pm)), pc.handle(ps, pm);
        assert(pchan->lines.size() == n0);
        assert(irc_parse(":bob!u@h PRIVMSG #chan :showme", pm)), pc.handle(ps, pm);
        assert(pchan->lines.size() == n0 + 1);
        // on only in sn/#chan: its events, not #other's
        pc.exec(ph, "/plugin disable t");
        assert(pc.plugins.empty() && last(ph) == "t disabled");
        pc.exec(ph, "/plugin enable t sn/#chan");
        assert(pc.plugins.size() == 1 && last(ph) == "t enabled in sn/#chan");
        assert(pc.plugin_emit("message", ps, "#Chan", {{"text", std::string("hideme")}}));
        assert(!pc.plugin_emit("message", ps, "#other", {{"text", std::string("hideme")}}));
        pc.exec(ph, "/plugins");
        assert(said("t  on  in: sn/#chan  commands: /ptest"));
        // 5 errors in a row: unloaded; reload brings it back
        for (int i = 0; i < 5; i++) pc.exec(ph, "/ptest err");
        assert(pc.plugins.empty() && said("unloaded after 5 errors"));
        pc.exec(ph, "/ptest x");  // no plugin has it now: off to the server, not to a dead plugin
        pc.exec(ph, "/plugin reload t");
        assert(pc.plugins.size() == 1 && last(ph) == "t reloaded");
        // a file that isn't Lua: not loaded, said so
        pc.exec(ph, "/plugin enable bad");
        assert(said("not loaded:") && pc.plugins.size() == 1);
        // /set and the settings window leave [plugin.*] to /plugin
        for (auto& st : pc.settings()) assert(st.key.rfind("plugin.", 0) != 0);
        for (auto& r : pc.setting_rows()) assert(r.sec.rfind("plugin.", 0) != 0);
        // the catalog's index: flat objects; bad JSON is refused
        bool jok;
        auto ix = plugin_index(R"([{"name": "a\"b", "api": 1, "x": true}, {}])", &jok);
        assert(jok && ix.size() == 2 && ix[0]["name"] == "a\"b" && ix[0]["api"] == "1" && ix[0]["x"] == "true");
        assert(plugin_index("[{\"name\": }]", &jok).empty() && !jok);
        assert(sha256_of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        // old keys (and the comments over them) out of a config's text
        assert(ini_drop_keys("[general]\n# about it\n# more\nauto_away = on\nnick = x\n[server.a]\n; auto_away = off ; c\nhost = h\n", {"auto_away"}) ==
               "[general]\nnick = x\n[server.a]\nhost = h\n");
        auto wait_catalog = [&] {
            for (int i = 0; i < 500 && pc.catalog; i++) usleep(10000), pc.plugins_poll();
            assert(!pc.catalog);
        };
        // a catalog whose checksum is wrong: refused, nothing written
        std::string cat = dir + "/cat";
        mkdir_p(cat + "/t2");
        {
            FILE* o = fopen((cat + "/index.json").c_str(), "w");
            fputs("[{\"name\": \"t2\", \"version\": \"1\", \"api\": 1, \"sha256\": \"00\"}, {\"name\": \"future\", \"api\": 99}]", o), fclose(o);
            o = fopen((cat + "/t2/t2.lua").c_str(), "w");
            fputs("print('t2')\n", o), fclose(o);
        }
        setenv("ISEKUR_PLUGIN_URL", ("file://" + cat + "/").c_str(), 1);
        pc.exec(ph, "/plugin install t2");
        wait_catalog();
        assert(last(ph) == "t2: refused, its checksum doesn't match the catalog's" && access((pdir + "/t2.lua").c_str(), F_OK) != 0);
        pc.exec(ph, "/plugin available");
        wait_catalog();
        assert(said("1 more need a newer iSekur") && !said("future"));
        // autoaway from the repo's own catalog (so its sha256 in index.json must be right), with the old
        // core settings moved into it and taken out of the config
        char cwd[4096];
        assert(getcwd(cwd, sizeof cwd));
        setenv("ISEKUR_PLUGIN_URL", (std::string("file://") + cwd + "/plugins/").c_str(), 1);
        {
            std::string err;
            Ini old{{"general", {{"auto_away", "on"}, {"away_message", "brb"}, {"auto_away_idle", "0"}}},
                    {"server.sn", {{"auto_away", "off"}, {"away_announce", "on"}}}};
            assert(ini_save(pc.cfg_path, old, err));
            for (auto& [sec, kv] : old) for (auto& [k, v] : kv) pc.cfg[sec][k] = v;
        }
        {  // the repo's catalog: every entry's file says the same version, and has the same sha256
            std::ifstream f(std::string(cwd) + "/plugins/index.json");
            std::stringstream ss;
            ss << f.rdbuf();
            bool iok;
            auto idx = plugin_index(ss.str(), &iok);
            assert(iok && !idx.empty());
            for (auto& e : idx) {
                std::ifstream lf(std::string(cwd) + "/plugins/" + e["name"] + "/" + e["name"] + ".lua");
                std::stringstream ls;
                ls << lf.rdbuf();
                assert(plugin_version(ls.str()) == e["version"] && sha256_of(ls.str()) == e["sha256"]);
            }
        }
        assert(old_away_keys(pc));
        pc.exec(ph, "/plugin install autoaway");
        wait_catalog();
        assert(said("autoaway: moved 6 auto-away settings") && said("autoaway 1.1 installed and on") && !old_away_keys(pc));
        {
            Ini back2;
            assert(ini_load(pc.cfg_path, back2));
            assert(!back2["general"].count("auto_away") && !back2["server.sn"].count("away_announce"));
            assert(back2["plugin.autoaway"]["message"] == "brb" && back2["plugin.autoaway"]["enabled"] == "on");
            assert(back2["plugin.autoaway@sn"]["enabled"] == "off" && back2["plugin.autoaway@sn"]["announce_away"] == "on" &&
                   back2["plugin.autoaway@sn"]["announce_back"] == "on");  // away_announce did both
        }
        // what core auto-away did: off on sn, then away when detached, /me in the channel, back when attached
        pc.exec(ph, "/plugin disable t");
        ps->nick = "me", pchan->joined = true, ps->conn.out.clear();
        auto psent = [&] { std::string o = ps->conn.out; ps->conn.out.clear(); return o; };
        pc.attached = false, pc.detached_at = 0;
        pc.plugin_emit("tick", nullptr, "");
        assert(psent().empty());  // off on sn
        pc.exec(ph, "/plugin enable autoaway@sn");
        pc.plugin_emit("tick", nullptr, "");
        assert(psent() == "AWAY :brb\r\nPRIVMSG #chan :\x01" "ACTION is away: brb\x01\r\n");
        pc.plugin_emit("tick", nullptr, "");
        assert(psent().empty());  // once
        assert(irc_parse(":srv 306 me :away", pm)), pc.handle(ps, pm);
        pc.set_attached(true);
        pc.plugin_emit("tick", nullptr, "");
        assert(psent() == "AWAY\r\nPRIVMSG #chan :\x01" "ACTION is back\x01\r\n");
        assert(irc_parse(":srv 305 me :back", pm)), pc.handle(ps, pm);
        // a manual /away while auto-away: yours, coming back leaves it alone
        // each one on its own: back announced, away not (and per channel: off in #chan wins over sn)
        pc.exec(ph, "/plugin set autoaway@sn announce_away off");
        pc.set_attached(false), pc.detached_at = 0;
        pc.plugin_emit("tick", nullptr, "");
        assert(psent() == "AWAY :brb\r\n");
        assert(irc_parse(":srv 306 me :away", pm)), pc.handle(ps, pm);
        pc.set_attached(true);
        pc.plugin_emit("tick", nullptr, "");
        assert(psent() == "AWAY\r\nPRIVMSG #chan :\x01" "ACTION is back\x01\r\n");
        assert(irc_parse(":srv 305 me :back", pm)), pc.handle(ps, pm);
        pc.exec(ph, "/plugin set autoaway@sn/#chan announce_back off");
        pc.set_attached(false), pc.detached_at = 0;
        pc.plugin_emit("tick", nullptr, "");
        assert(psent() == "AWAY :brb\r\n");
        assert(irc_parse(":srv 306 me :away", pm)), pc.handle(ps, pm);
        pc.exec(ps->status, "/away lunch");
        assert(psent() == "AWAY :lunch\r\n");
        pc.set_attached(true);
        pc.plugin_emit("tick", nullptr, "");
        assert(psent().empty());
        // what the plugin windows show: installed ones with their state, scopes and settings, the catalog, the scopes
        {
            auto info = pc.plugin_info();
            const PluginInfo* aw = nullptr;
            for (auto& pi : info) if (pi.name == "autoaway") aw = &pi;
            assert(aw && aw->state == "on" && aw->version == "1.1" && aw->local == "1.1" && !aw->update && !aw->edited && !aw->desc.empty());
            assert(std::find(aw->on_in.begin(), aw->on_in.end(), "") != aw->on_in.end() && std::find(aw->on_in.begin(), aw->on_in.end(), "sn") != aw->on_in.end());
            assert(aw->sets.size() == 4 && aw->sets[0].help.find("minutes") == 0 && aw->sets[0].key == "idle" && aw->sets[0].scope == 'n' && aw->sets[3].key == "announce_back" && aw->sets[3].flag);
            auto cat = pc.catalog_rows();
            assert(cat.size() >= 3 && cat[0].name == "autoaway" && cat[0].installed && !cat[0].update && !cat[0].edited && !cat[0].too_new);
            assert(!cat[1].installed && cat[1].version == "1.1");
            auto sc = pc.plugin_scopes();
            assert(sc.front().empty() && std::find(sc.begin(), sc.end(), "sn") != sc.end() && std::find(sc.begin(), sc.end(), "sn/#chan") != sc.end());
            pc.cfg["plugin.autoaway@sn/#chan"]["message"] = "chan msg";
            assert(pc.plugin_at("autoaway", "message", "sn") == "" && pc.plugin_eff("autoaway", "message", "sn", "d", 'c') == "brb");
            assert(pc.plugin_eff("autoaway", "message", "sn/#chan", "d", 'c') == "chan msg" && pc.plugin_eff("autoaway", "message", "sn/#chan", "d", 'n') == "brb");
            assert(pc.plugin_eff("autoaway", "nope", "sn/#chan", "d") == "d");
            pc.cfg.erase("plugin.autoaway@sn/#chan");
            pc.tty = true;  // /plugin alone in the terminal: its window; from the webchat, the list
            pc.exec(ph, "/plugin");
            assert(pc.plugin_win);
            pc.plugin_win = false, pc.from_web = true;
            pc.exec(ph, "/plugin");
            assert(!pc.plugin_win && said("autoaway  on"));
            pc.from_web = pc.tty = false;
            pc.exec(ph, "/plugin set autoaway@sn enabled");  // emptied: sn inherits the global on, and it's still loaded
            assert(pc.plugin_eff("autoaway", "enabled", "sn", "off") == "on" && said("autoaway enabled = (default) in sn"));
        }
        // the catalog is kept in .catalog.json: a new start shows it without fetching, and a refresh waits a minute
        {
            assert(access((pdir + "/.catalog.json").c_str(), F_OK) == 0);
            Core p2;
            p2.cfg_path = pc.cfg_path;
            Buffer* h2 = p2.open(nullptr, BT::STATUS, "isekur");
            p2.plugins_sync();
            assert(p2.catalog_have && p2.catalog_rows().size() >= 3);
            p2.plugin_refresh(h2);  // fresh: no fetch
            assert(!p2.catalog);
            p2.catalog_tried = time(nullptr) - 10;
            p2.exec(h2, "/plugin refresh");
            assert(!p2.catalog && h2->lines.back().text.find("try again in a minute") != std::string::npos);
            p2.exec(h2, "/plugin available");  // from the cache
            bool listed = false;
            for (auto& l : h2->lines) listed |= l.text.find("autoaway\x02 1.1") != std::string::npos && l.text.find("[installed]") != std::string::npos;
            assert(listed && !p2.catalog);
        }
        // versions: edited here (same version, other contents) is not an update, and only a named update restores it
        {
            std::string ap = pdir + "/autoaway.lua", orig;
            { std::ifstream f(ap); std::stringstream ss; ss << f.rdbuf(); orig = ss.str(); }
            { std::ofstream f(ap, std::ios::app); f << "-- my change\n"; }
            sleep(1);  // a new mtime for the sha cache
            { std::ofstream f(ap, std::ios::app); f << "\n"; }
            auto ri = pc.catalog_rows();  // sorted: autoaway first
            assert(ri[0].name == "autoaway" && ri[0].edited && !ri[0].update && ri[0].local == "1.1");
            pc.exec(ph, "/plugin update");
            wait_catalog();
            assert(said("Your plugins are up to date with the catalog"));
            pc.exec(ph, "/plugin update autoaway");
            wait_catalog();
            assert(said("autoaway 1.1 restored"));
            std::ifstream f(ap); std::stringstream ss; ss << f.rdbuf();
            assert(ss.str() == orig);
        }
        // a newer version in the catalog: shown as an update, and /plugin update takes it
        {
            assert(plugin_version("-- t2\n-- version: 2.1 \nprint(1)") == "2.1" && plugin_version("print(1)").empty());
            std::string v2 = "-- version: 2\nprint('t2 v2')\n";
            { std::ofstream f(cat + "/t2/t2.lua"); f << v2; }
            { std::ofstream f(cat + "/index.json"); f << "[{\"name\": \"t2\", \"version\": \"2\", \"api\": 1, \"sha256\": \"" + sha256_of(v2) + "\"}]"; }
            { std::ofstream f(pdir + "/t2.lua"); f << "-- version: 1\nprint('t2 v1')\n"; }
            setenv("ISEKUR_PLUGIN_URL", ("file://" + cat + "/").c_str(), 1);
            pc.catalog_tried = 0;
            pc.exec(ph, "/plugin refresh");
            wait_catalog();
            bool up = false;
            for (auto& r : pc.catalog_rows()) up |= r.name == "t2" && r.update && r.local == "1";
            assert(up && said("Catalog fetched: 1 plugin"));
            pc.exec(ph, "/plugin update");
            wait_catalog();
            assert(said("t2 2 updated from 1"));
            unlink((pdir + "/t2.lua").c_str()), pc.cfg.erase("plugin.t2");
            setenv("ISEKUR_PLUGIN_URL", (std::string("file://") + cwd + "/plugins/").c_str(), 1);
        }
        // remove: the file, its data and its sections
        pc.exec(ph, "/plugin remove autoaway");
        assert(last(ph) == "autoaway removed, with its settings and saved data" && access((pdir + "/autoaway.lua").c_str(), F_OK) != 0);
        assert(!pc.cfg.count("plugin.autoaway") && !pc.cfg.count("plugin.autoaway@sn"));
        // awaylog: highlights and PMs while detached, one summary (INFO badge) in the tab you're on once you're back
        pc.exec(ph, "/plugin install awaylog");
        wait_catalog();
        assert(said("awaylog 1.1 installed and on"));
        pc.select(pchan);
        assert(irc_parse(":srv 305 me :back", pm)), pc.handle(ps, pm);  // the /away lunch above: back first
        pc.set_attached(false), pc.detached_at = 0;
        pc.plugin_emit("tick", nullptr, "");  // it sees you go
        assert(irc_parse(":bob!u@h PRIVMSG #chan :me: are you there?", pm)), pc.handle(ps, pm);
        assert(irc_parse(":bob!u@h PRIVMSG #chan :nothing for you", pm)), pc.handle(ps, pm);
        assert(irc_parse(":carol!u@h PRIVMSG me :psst", pm)), pc.handle(ps, pm);
        assert(irc_parse(":carol!u@h PRIVMSG me :\x01" "ACTION waves\x01", pm)), pc.handle(ps, pm);
        pc.plugin_emit("tick", nullptr, "");  // still away: nothing shown
        size_t before_back = pchan->lines.size();
        pc.set_attached(true);
        pc.plugin_emit("tick", nullptr, "");
        {
            std::vector<std::string> got;
            for (size_t i = before_back; i < pchan->lines.size(); i++) got.push_back(pchan->lines[i].text);
            assert(got.size() == 4 && pchan->lines[before_back].badge == "awaylog" && pchan->lines[before_back].badge_kind == 'i');
            assert(pchan->lines[before_back + 1].badge == "awaylog" && pchan->lines[before_back + 1].badge_kind == 'p');
            assert(got[0].rfind("While you were away (", 0) == 0 && got[0].find("): 1 mention in #chan, 2 PMs from carol") != std::string::npos);
            assert(got[1].find("#chan <bob> me: are you there?") != std::string::npos && got[2].find("<carol> psst") != std::string::npos);
            assert(got[3].find("* carol waves") != std::string::npos);
        }
        pc.plugin_emit("tick", nullptr, "");  // once
        size_t n_after = pchan->lines.size();
        pc.exec(ps->status, "/awaylog");  // again, in full
        assert(ps->status->lines.back().text.find("* carol waves") != std::string::npos && pchan->lines.size() == n_after);
        pc.exec(ps->status, "/awaylog clear");
        pc.exec(ps->status, "/awaylog");
        assert(ps->status->lines.back().text == "awaylog: nothing came while you were away");
        // seen: what a nick did last; !seen answered where reply is on, once per 10s, telling only when for elsewhere
        pc.exec(ph, "/plugin install seen");
        wait_catalog();
        assert(said("seen 1.1 installed and on"));
        auto seen_q = [&](const std::string& who) { pc.exec(pchan, "/seen " + who); return pchan->lines.back().text; };
        assert(irc_parse(":dave!u@host JOIN #chan", pm)), pc.handle(ps, pm);
        assert(seen_q("dave") == "dave is here now, in #chan");
        assert(irc_parse(":dave!u@host PRIVMSG #chan :hello all", pm)), pc.handle(ps, pm);
        assert(irc_parse(":dave!u@host PART #chan :bye now", pm)), pc.handle(ps, pm);
        assert(seen_q("DAVE").rfind("dave was last seen 0s ago (", 0) == 0 && seen_q("dave").find("), leaving #chan (bye now)") != std::string::npos);
        assert(irc_parse(":erin!u@h PRIVMSG #chan :quick one", pm)), pc.handle(ps, pm);
        assert(seen_q("erin").find("saying: quick one in #chan") != std::string::npos);
        assert(irc_parse(":erin!u@h NICK erin_", pm)), pc.handle(ps, pm);
        assert(seen_q("erin").find("changing nick to erin_") != std::string::npos && seen_q("erin_").find("changing nick from erin") != std::string::npos);
        assert(irc_parse(":erin_!u@h QUIT :gone home", pm)), pc.handle(ps, pm);
        assert(seen_q("erin_").find("quitting (gone home)") != std::string::npos);
        assert(seen_q("nobody") == "I haven't seen nobody" && seen_q("me") == "That's you");
        ps->conn.out.clear();
        assert(irc_parse(":frank!u@h PRIVMSG #chan :!seen dave", pm)), pc.handle(ps, pm);
        assert(psent().empty());  // reply is off
        pc.exec(ph, "/plugin set seen@sn/#chan reply on");
        assert(irc_parse(":frank!u@h PRIVMSG #chan :!seen dave", pm)), pc.handle(ps, pm);
        {
            std::string a = psent();
            assert(a.rfind("PRIVMSG #chan :dave was last seen", 0) == 0 && a.find("leaving #chan (bye now)") != std::string::npos);
        }
        assert(irc_parse(":frank!u@h PRIVMSG #chan :!seen erin_", pm)), pc.handle(ps, pm);
        assert(psent().empty());  // 10s between answers in a channel
        pc.plugins_poll();  // the store reaches the disk on the tick
        {
            std::ifstream f(pdir + "/seen.data");
            std::stringstream ss;
            ss << f.rdbuf();
            assert(ss.str().find("sn\\tdave\t") != std::string::npos && ss.str().find("leaving #chan") != std::string::npos);
        }
        pc.exec(ph, "/plugin remove awaylog");
        pc.exec(ph, "/plugin remove seen");
        unsetenv("ISEKUR_PLUGIN_URL");
        unlink((cat + "/index.json").c_str()), unlink((cat + "/t2/t2.lua").c_str()), rmdir((cat + "/t2").c_str()), rmdir(cat.c_str());
        for (auto f : {"t.lua", "bad.lua", "t.data", ".catalog.json"}) unlink((pdir + "/" + f).c_str());
        rmdir(pdir.c_str()), unlink(pc.cfg_path.c_str()), rmdir(d);
    }

    puts("all tests passed");
}
