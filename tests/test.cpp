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

    puts("all tests passed");
}
