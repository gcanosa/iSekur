// Self-checks for the parsing code: run with `make test`.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <sys/stat.h>
#include <dirent.h>
#include <cstdio>
#include <filesystem>
#include <sys/time.h>
#include <fstream>
#include <regex>
#include <set>
#include <thread>
#include <sstream>
#include <csignal>
#include <cstring>
#include <functional>
#include <unistd.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "core.h"
#include "session.h"
#include "web.h"
#include "bnc.h"
#include "net.h"
#include <openssl/sha.h>

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

    {  // isekur.crash's breadcrumbs: the last 32, oldest first, with the loop stage and the last frame
        for (int n = 0; n < 40; n++) crumb("exec", ("/c" + std::to_string(n)).c_str());
        crumb_stage("drawing"), crumb_state("80x24 tab=cmd");
        char path[] = "/tmp/isekur-crumbs-XXXXXX";
        int fd = mkstemp(path);
        crumbs_write(fd), close(fd);
        std::ifstream cf(path);
        std::string t((std::istreambuf_iterator<char>(cf)), std::istreambuf_iterator<char>());
        unlink(path);
        assert(t.find("stage: drawing") != std::string::npos && t.find("80x24 tab=cmd") != std::string::npos);
        assert(t.find("/c7\n") == std::string::npos && t.find("/c8\n") != std::string::npos && t.find("/c8\n") < t.find("/c39\n"));
    }

    size_t i = 0;
    assert(utf8_next("\xc3\xa9", i) == 0xe9 && i == 2);
    i = 0;
    assert(utf8_next("\xe9x", i) == 0xe9 && i == 1);  // Latin-1 fallback
    i = 0;
    assert(utf8_next("\xc0\x80", i) == 0xc0);          // overlong rejected
    // legacy charsets (a network's encoding): decoded to UTF-8, encoded back, unmappable -> ?
    assert(from_legacy("\xdd\xfe\x41", "iso-8859-9") == "\xc4\xb0\xc5\x9f" "A");  // 0xDD = I-dot, 0xFE = s-cedilla
    assert(to_legacy("\xc5\x9f\xc4\xb1", "iso-8859-9") == "\xfe\xfd");              // and back: ş ı
    {  // each half on its own, with the bytes said on failure: iconv's answer for an unmappable character differs by system
        auto hex = [](const std::string& v) { std::string o; char h[4]; for (unsigned char ch : v) snprintf(h, sizeof h, "%02x ", ch), o += h; return o; };
        std::string eu = to_legacy("\xe2\x82\xac", "iso-8859-9"), ea = from_legacy("\xe9", "iso-8859-1");
        if (eu != "?") fprintf(stderr, "to_legacy(euro, iso-8859-9) = %s\n", hex(eu).c_str());
        if (ea != "\xc3\xa9") fprintf(stderr, "from_legacy(e9, iso-8859-1) = %s\n", hex(ea).c_str());
        assert(eu == "?");
        assert(ea == "\xc3\xa9");
    }
    if (legacy_charset_ok("iso-2022-jp")) {  // a stateful charset: each line ends back in ASCII, or the next one is read as Kanji
        std::string jp = to_legacy("\xe3\x81\x82", "iso-2022-jp");
        assert(jp.size() > 3 && jp.compare(jp.size() - 3, 3, "\x1b(B") == 0);
    }
    assert(legacy_charset_ok("iso-8859-9") && legacy_charset_ok("windows-1252") && !legacy_charset_ok("utf-16") && !legacy_charset_ok("nonsense"));

    Ini ini = ini_parse("# c\n[a]\nk = v ; note\nc = #chan\n[b]\nx=1\n");
    assert(ini["a"]["k"] == "v" && ini["a"]["c"] == "#chan" && ini_get(ini, "b", "x") == "1");
    assert(ini_get(ini, "b", "missing", "d") == "d");
    assert(ini_get(ini_parse("[a]\nk =    ; placeholder\n"), "a", "k", "d") == "d");  // empty = default
    // a tab before the ';' ends the value too: "user = Ident<TAB>; the ident part" once went out as the USER line
    assert(ini_get(ini_parse("[a]\nk = Ident\t; the ident (user@) part; default: the nick\n"), "a", "k") == "Ident");
    assert(ini_get(ini_parse("[a]\nk = Ident ; the ident (user@) part\n"), "a", "k") == "Ident");
    {   // /set keeps a tab-separated comment on the line, as it does a space-separated one
        Ini ch;
        ch["a"]["k"] = "Other";
        Ini back = ini_parse(ini_update("[a]\nk = Ident\t; note\n", ch));
        assert(back["a"]["k"] == "Other" && ini_update("[a]\nk = Ident\t; note\n", ch).find("; note") != std::string::npos);
    }
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

    {  // scrypt for LibreSSL: RFC 7914's vectors, iSekur's own parameters, and where OpenSSL has its own, the same answers as it
        auto hex = [](const unsigned char* d, size_t n) { std::string s; char b[3]; for (size_t i = 0; i < n; i++) snprintf(b, sizeof b, "%02x", d[i]), s += b; return s; };
        unsigned char o[64];
        assert(scrypt_rfc7914("", "", 16, 1, 1, o, 64) && hex(o, 64) == "77d6576238657b203b19ca42c18a0497f16b4844e3074ae8dfdffa3fede21442fcd0069ded0948f8326a753a0fc81f17e8d3e0fb2e0d3628cf35e20c38d18906");
        assert(scrypt_rfc7914("password", "NaCl", 1024, 8, 16, o, 64) && hex(o, 64) == "fdbabe1c9d3472007856e7190d01e9fe7c6ad7cbc8237830e77376634b3731622eaf30d92e22a3886ff109279d9830dac727afb94a83ee6d8360cbdfa2cc0640");
        assert(scrypt_rfc7914("pleaseletmein", "SodiumChloride", 16384, 8, 1, o, 64) && hex(o, 64) == "7023bdcb3afd7348461c06cd81fd38ebfda8fbba904f8e3ea9b543f6545da1f2d5432955613f0fcf62d49705242a9af9e61e85dc0d651e40dfcf017b45575887");
        assert(!scrypt_rfc7914("x", "y", 3, 1, 1, o, 64) && !scrypt_rfc7914("x", "y", 16, 0, 1, o, 64));  // N is a power of two, r and p aren't 0
        assert(scrypt_rfc7914("hunter2", "0123456789abcdef", 1 << 15, 8, 1, o, 32) && hex(o, 32) == "bb1c4a9190f062fa022fb5f9cf030aee479bbce7c0ea2f0a13a854e60fd31c83");
        assert(e2e_key("hunter2", "0123456789abcdef") == std::string((char*)o, 32));  // what /e2e really uses
#ifndef LIBRESSL_VERSION_NUMBER
        for (const std::string& pw : std::vector<std::string>{"", "a", "contraseña ñandú", std::string(200, 'z')}) {
            unsigned char a[40], b[40];
            assert(scrypt_rfc7914(pw, "isekur-chan:#x", 1024, 4, 2, a, 40));
            assert(EVP_PBE_scrypt(pw.data(), pw.size(), (const unsigned char*)"isekur-chan:#x", 14, 1024, 4, 2, 64 << 20, b, 40) == 1 && !memcmp(a, b, 40));
        }
#endif
    }

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
        assert(e2e_line_seal(k, std::string(259, 'x'), e2e_aad_c("#a", "Alice")).size() <= 400);  // say() chunk fits
        // channels: the AAD is the channel and the sender; a line moved to another channel or nick doesn't open
        std::string cs = e2e_seal(k, "hi", "#a");
        assert(e2e_open(k, cs, out, "#a") && out == "hi");
        assert(!e2e_open(k, cs, out, "#b") && !e2e_open(k, cs, out));
        std::string cl = e2e_line_seal(k, "hi", e2e_aad_c("#A", "Alice"));
        assert(e2e_line(cl) && cl.rfind("+iSE2 ", 0) == 0);
        assert(e2e_line_open(k, cl, out, e2e_aad_c("#a", "alice"), "#a") && out == "hi");  // case doesn't matter
        assert(!e2e_line_open(k, cl, out, e2e_aad_c("#a", "bob"), "#a") && out.empty());    // the server moved it to bob
        assert(!e2e_line_open(k, cl, out, e2e_aad_c("#b", "alice"), "#b"));
        // an old "+iSE " line opens with the old AAD; a v2 line given the old prefix doesn't (the AAD is in the tag)
        assert(e2e_line_open(k, "+iSE " + cs, out, e2e_aad_c("#a", "bob"), "#a") && out == "hi");
        assert(!e2e_line_open(k, "+iSE " + cl.substr(6), out, e2e_aad_c("#a", "alice"), "#a"));
        assert(!e2e_line("+iSEx y") && !e2e_line("iSE x") && e2e_line("+iSE x"));
        // queries: the AAD is the sender's role, so a line sent back to its author doesn't open as the peer's
        std::string ql = e2e_line_seal(k, "yes", e2e_aad_q(true));
        assert(e2e_line_open(k, ql, out, e2e_aad_q(true), "") && out == "yes");
        assert(!e2e_line_open(k, ql, out, e2e_aad_q(false), ""));
        assert(e2e_line_open(k, "+iSE " + e2e_seal(k, "old"), out, e2e_aad_q(false), "") && out == "old");  // before +iSE2
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
    {  // RSV bits (no extension agreed), reserved opcodes, a control frame over 125 bytes: refused
        std::string rsv = client_frame("x"), op3 = client_frame("x"), bigping = ws_frame(std::string(126, 'x'), 9, true);
        rsv[0] = (char)0xc1, op3[0] = (char)0x83;
        assert(ws_parse(rsv, op, p) == -1 && ws_parse(op3, op, p) == -1 && ws_parse(bigping, op, p) == -1);
        assert(ws_parse(ws_frame(std::string(125, 'x'), 9, true), op, p) > 0 && op == 9);
    }
    {  // a client's frames (masked) and a server's (not): each side refuses the other's
        std::string m = ws_frame("PING :x", 1, true), mb = ws_frame(big, 2, true), sf = ws_frame("hi");
        assert(ws_parse(m, op, p) == (long)m.size() && op == 1 && p == "PING :x");
        assert(ws_parse(mb, op, p) == (long)mb.size() && op == 2 && p == big);
        assert(ws_parse(m, op, p, nullptr, false) == -1 && ws_parse(sf, op, p) == -1);
        assert(ws_parse(sf, op, p, nullptr, false) == (long)sf.size() && p == "hi");
        std::string h, path;
        int port;
        bool wtls;
        assert(ws_url("wss://irc.sekurnet.org/ws", h, port, path, wtls) && h == "irc.sekurnet.org" && port == 443 && path == "/ws" && wtls);
        assert(ws_url("ws://[::1]:8080", h, port, path, wtls) && h == "::1" && port == 8080 && path == "/" && !wtls);
        assert(ws_url("WSS://h:7000/a?b=c#d", h, port, path, wtls) && h == "h" && port == 7000 && path == "/a?b=c" && wtls);
        for (const char* bad : {"irc.sekurnet.org", "wss://", "wss://h:99999/", "wss://h:/", "wss://u:p@h/", "wss://h/a b", "https://h/", "ws:/h"})
            assert(!ws_url(bad, h, port, path, wtls));
    }

    assert(json_str("a\"b\\\n\xe9") == "\"a\\\"b\\\\\\u000a\xc3\xa9\"");
    // attach / webchat audit lines
    assert(typing_text({"a"}) == "a is typing\u2026" && typing_text({"a", "b"}) == "a and b are typing\u2026");
    assert(typing_text({"a", "b", "c"}) == "a, b and c are typing\u2026" && typing_text({}) == "");
    assert(typing_text({"a", "b", "c", "d", "e", "f"}) == "a, b and 4 others are typing\u2026");
    assert(fmt_dur(9) == "9s" && fmt_dur(2832) == "47m12s" && fmt_dur(90061) == "1d1h1m");
    assert(attach_who("203.0.113.7 52144 10.0.0.2 22", "/dev/ttys004") == "203.0.113.7 (ssh) on /dev/ttys004");
    assert(attach_who("", "/dev/ttys001") == "local on /dev/ttys001" && attach_who("", "") == "local");
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

    {  // Face ID (WebAuthn): a P-256 key like a phone's, its answers signed here
        EVP_PKEY* k = nullptr;  // not EVP_EC_gen("P-256"): that is OpenSSL 3 only, LibreSSL and 1.1 have this
        EVP_PKEY_CTX* kc = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
        assert(kc && EVP_PKEY_keygen_init(kc) > 0 && EVP_PKEY_CTX_set_ec_paramgen_curve_nid(kc, NID_X9_62_prime256v1) > 0 && EVP_PKEY_keygen(kc, &k) > 0);
        EVP_PKEY_CTX_free(kc);
        unsigned char* der = nullptr;
        int dn = i2d_PUBKEY(k, &der);
        std::string spki((char*)der, dn);
        OPENSSL_free(der);
        std::string chal = "0123456789abcdef0123456789abcdef", origin = "https://irc.example.org:8443";
        auto cdj = [&](const std::string& type, const std::string& ch, const std::string& org) {
            return "{\"type\":\"" + type + "\",\"challenge\":\"" + b64url(ch) + "\",\"origin\":\"" + org + "\",\"crossOrigin\":false}";
        };
        auto ad = [](const std::string& host, unsigned char flags, uint32_t n) {
            unsigned char rp[32];
            SHA256((const unsigned char*)host.data(), host.size(), rp);
            std::string a((char*)rp, 32);
            a += (char)flags, a += (char)(n >> 24), a += (char)(n >> 16), a += (char)(n >> 8), a += (char)n;
            return a;
        };
        auto sign = [&](const std::string& a, const std::string& c) {
            unsigned char hc[32];
            SHA256((const unsigned char*)c.data(), c.size(), hc);
            std::string msg = a + std::string((char*)hc, 32), sig(80, '\0');
            size_t n = sig.size();
            EVP_MD_CTX* md = EVP_MD_CTX_new();
            EVP_DigestSignInit(md, nullptr, EVP_sha256(), nullptr, k);
            EVP_DigestSign(md, (unsigned char*)&sig[0], &n, (const unsigned char*)msg.data(), msg.size());
            EVP_MD_CTX_free(md);
            return sig.substr(0, n);
        };
        std::string g = cdj("webauthn.get", chal, origin), a = ad("irc.example.org", 0x05, 7);
        uint32_t count = 0;
        assert(webauthn_check("webauthn.get", g, a, chal, origin, spki, sign(a, g), &count) == "" && count == 7);
        assert(webauthn_check("webauthn.get", g, a, chal, origin, spki, sign(a, g + " "), nullptr) == "a bad signature");
        assert(webauthn_check("webauthn.get", g, a, "another challenge", origin, spki, sign(a, g), nullptr) == "an old or unknown challenge");
        assert(webauthn_check("webauthn.get", g, a, "", origin, spki, sign(a, g), nullptr) != "");
        std::string ev = cdj("webauthn.get", chal, "https://evil.example");
        assert(webauthn_check("webauthn.get", ev, a, chal, origin, spki, sign(a, ev), nullptr).rfind("from another page", 0) == 0);
        std::string other = ad("evil.example", 0x05, 7);
        assert(webauthn_check("webauthn.get", g, other, chal, origin, spki, sign(other, g), nullptr) == "for another host name");
        std::string nouv = ad("irc.example.org", 0x01, 7);  // touched, but no Face ID
        assert(webauthn_check("webauthn.get", g, nouv, chal, origin, spki, sign(nouv, g), nullptr) == "without Face ID (no user verification)");
        assert(webauthn_check("webauthn.create", g, a, chal, origin, spki, "", nullptr) == "not a webauthn.create");
        std::string cr = cdj("webauthn.create", chal, origin);
        assert(webauthn_check("webauthn.create", cr, a, chal, origin, spki, "", nullptr) == "");  // setting up: no signature
        assert(webauthn_check("webauthn.create", cr, a, chal, origin, "junk", "", nullptr) == "not an ES256 key");
        EVP_PKEY_free(k);
    }

    assert(safe_name("../../etc/passwd") == "_.._.._etc_passwd" && safe_name("#Chan[x]") == "#chan{x}");
    assert(safe_name(".hidden") == "_.hidden" && safe_name("") == "_");

    Line ll;
    assert(parse_log_line("2026-10-27 19:09:42 <nick> hi > there", ll) && ll.kind == LK::MSG && ll.nick == "nick" && ll.text == "hi > there");
    assert(parse_log_line("2026-10-27 19:09:42 * nick waves hi", ll) && ll.kind == LK::ACTION && ll.text == "waves hi");
    assert(parse_log_line("2026-10-27 19:09:42 -serv- note", ll) && ll.kind == LK::NOTICE && ll.nick == "serv");
    assert(parse_log_line("2026-10-27 19:09:42 -!- x joined", ll) && ll.kind == LK::INFO && ll.text == "x joined");
    assert(!parse_log_line("--- Log opened 2026-10-27 19:09:12", ll));
    {  // /logs: what's on disk, read back with events as their kinds, the find line's syntax and a search over days
        char d[] = "/tmp/isekur-logs-XXXXXX";
        assert(mkdtemp(d));
        std::string dir = d, net = dir + "/logs/gone";
        mkdir_p(net);
        auto put = [&](const std::string& f, const char* body) { FILE* o = fopen((net + "/" + f).c_str(), "w"); fputs(body, o), fclose(o); };
        put("#c.2026-11-01.log", "--- Log opened 2026-11-01 09:00:00\n2026-11-01 09:00:01 -!- alice (u@h) has joined #c\n"
                                 "2026-11-01 09:00:02 <alice> Green light\n2026-11-01 09:00:03 -!- mode/#c [+o bob] by alice\n");
        put("#c.2026-11-03.log", "2026-11-03 10:00:00 <bob> no green here? \"green light\"\n2026-11-03 10:00:01 -!- bob (u@h) has quit (bye)\n");
        put("#c.log", "2026-11-06 08:00:00 <carol> today\n");
        put("notes.txt", "not a log\n");
        Core lc;
        lc.cfg_path = dir + "/isekur.conf";
        auto files = lc.log_files();  // a network no longer configured: still under log_dir
        assert(files.size() == 3 && files[0].net == "gone" && files[0].name == "#c" && files[0].current && files[1].day == "2026-11-03" && files[2].day == "2026-11-01");
        auto v = log_read(files[2].path);
        assert(v.size() == 3 && v[0].kind == LK::JOIN && v[0].nick == "alice" && v[2].kind == LK::MODE && v[2].nick == "alice");
        assert(log_read(files[1].path)[1].kind == LK::QUIT);
        auto q = log_query("Green nick:Alice \"a phrase\"");
        assert(q.nick == "alice" && q.words.size() == 2 && q.words[1] == "a phrase");
        assert(log_match(v[1], log_query("green NICK:alice")) && !log_match(v[1], log_query("green nick:bob")) && log_match(v[0], log_query("nick:alice")));
        assert(!log_match(v[0], log_query("", false)) && log_match(v[1], log_query("", false)));  // events hidden: joins out, messages in
        bool more = true;
        auto hits = log_search(files, {0, 1, 2}, log_query("green"), 10, &more);
        assert(hits.size() == 2 && !more && hits[0].file == 1 && hits[1].file == 2 && hits[1].line == 1);
        assert(log_search(files, {1, 2}, log_query("\"green light\""), 1, &more).size() == 1 && more);
        // log_keep: rotated days older than it go, the current file stays; 0 or a bad value keeps all
        assert(keep_days("") == 0 && keep_days("30") == 30 && keep_days("30d") == 30 && keep_days("2w") == 14 && keep_days("6M") == 180 &&
               keep_days("1y") == 365 && keep_days("d") == -1 && keep_days("5x") == -1 && keep_days("1yy") == -1 && keep_days("-3") == -1);
        std::string recent = fmt_time(time(nullptr) - 86400, "%Y-%m-%d");
        put("#c." + recent + ".log", "x\n");
        log_prune(net, 0), log_prune(net, -1);
        assert(lc.log_files().size() == 4);
        log_prune(net, 7);
        files = lc.log_files();
        assert(files.size() == 2 && files[0].current && files[1].day == recent);
        for (auto& f : files) unlink(f.path.c_str());
        unlink((net + "/notes.txt").c_str()), rmdir(net.c_str()), rmdir((dir + "/logs").c_str()), rmdir(d);
    }

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
        { FILE* k = fopen((std::string(d) + "/secret.key").c_str(), "w"); fputs("short", k), fclose(k); }  // a damaged key: kept, not replaced
        Ini ch3;
        ch3["server.x"]["sasl_pass"] = "x";
        assert(!ini_save(path, ch3, err));
        { FILE* k = fopen((std::string(d) + "/secret.key").c_str(), "r"); char kb[16] = ""; assert(fread(kb, 1, sizeof kb, k) == 5); fclose(k); }
        unlink((std::string(d) + "/secret.key").c_str());
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

    {  // every shipped theme names every key some theme sets, live or as a "# key = ..." example: a new key reaches them all
        std::map<std::string, std::set<std::string>> named;  // theme -> its "section.key"s, commented ones too
        std::set<std::string> live;                          // "section.key" set for real by any theme
        for (auto& e : std::filesystem::directory_iterator("themes")) {
            if (e.path().extension() != ".theme") continue;
            std::ifstream in(e.path());
            std::string l, sec;
            std::smatch m;
            while (std::getline(in, l)) {
                if (std::regex_search(l, m, std::regex("^\\[(\\w+)\\]"))) sec = m[1];
                else if (std::regex_search(l, m, std::regex("^(#\\s*)?(\\w+)\\s*="))) {
                    named[e.path().stem().string()].insert(sec + "." + m[2].str());
                    if (!m[1].matched) live.insert(sec + "." + m[2].str());
                }
            }
        }
        assert(named.size() > 40 && !live.empty());
        for (auto& [t, ks] : named)
            for (auto& k : live)
                if (!ks.count(k)) fprintf(stderr, "theme %s doesn't name %s (set it, or add \"# key = ...\")\n", t.c_str(), k.c_str()), assert(false);
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
    {  // which words take a nick: completion offers them
        auto na = [](const char* c, size_t i, const char* w) { return nick_arg(find_command(c)->usage, i, w); };
        assert(na("msg", 0, "al") && !na("msg", 1, "hi") && na("query", 0, "b") && na("whois", 0, "x"));
        assert(na("op", 0, "a") && na("op", 3, "d") && na("ban", 1, "t") && na("react", 1, "a") && !na("react", 0, "a"));
        assert(na("kick", 0, "troll") && !na("kick", 0, "#chan") && na("kick", 1, "#chan") && !na("kick", 1, "troll") && !na("kick", 2, "#chan"));
        assert(na("notify", 1, "a") && na("notify", 4, "a") && na("invite", 0, "a") && !na("invite", 1, "a"));
        assert(!na("join", 0, "#a") && !na("set", 0, "nick") && !na("nick", 0, "x") && !na("tempban", 1, "30"));
        // a plugin's usage: /seen nick, /pmguard allow nick
        assert(nick_arg("nick", 0, "x") && nick_arg("[allow|deny|show|drop nick | allowed]", 1, "allow") && !nick_arg("send|retry", 0, "s") && !nick_arg("", 0, "x"));
    }
    size_t before = home->lines.size();
    core.exec(home, "/help /join");  // usage, aliases, details, "Examples", 3 examples
    assert(home->lines.size() == before + 7 && home->lines.back().nolog);
    assert(mirc_strip(home->lines[before].text).rfind("/join #channel [key]", 0) == 0);
    core.exec(home, "/help bogus");
    assert(mirc_strip(home->lines.back().text).find("No such command: /bogus") == 0);
    {  // a pasted "/close" with lines after it: the rest has no tab to run in (it used to run in the freed one)
        Buffer* gone = core.open(nullptr, BT::QUERY, "gone");
        size_t n = core.bufs.size();
        core.exec(gone, "/close\n/help /join\n/help bogus");
        assert(core.bufs.size() == n - 1 && core.index_of(gone) < 0);
        Buffer* was = core.active;  // the same through the FIFO: an escaped \n after /close runs in the tab active then
        Buffer* fgone = core.open(nullptr, BT::QUERY, "fgone");
        core.active = fgone;
        core.fifo_line("/close\\n/help /join");
        assert(core.index_of(fgone) < 0);
        core.active = was && core.index_of(was) >= 0 ? was : core.bufs.front().get();
    }
    {  // /set refuses what the key can't take, before it's in the config; the webchat's settings dialog hears why
        std::string was = ini_get(core.cfg, "general", "tab_bar_position");
        for (const char* bad : {"/set tab_bar_position middle", "/set mouse maybe", "/set scrollback 50", "/set scrollback lots", "/set tab_bar_width 200",
                                "/set nick_width x", "/set history_fetch 501", "/set log_keep 5x", "/set web_tree_width 90"}) {
            core.exec(home, bad);
            assert(home->lines.back().kind == LK::ERROR && home->lines.back().text.find("(unchanged)") != std::string::npos);
        }
        assert(ini_get(core.cfg, "general", "tab_bar_position") == was && !core.cfg["general"].count("tab_bar_width") && !core.cfg["general"].count("log_keep"));
        assert(bad_value("general", "tab_bar_width", "200") == "tab_bar_width = 200: takes 10-60" && bad_value("general", "scrollback", "50") == "scrollback = 50: takes 100 or more");
        for (const char* ok : {"/set log_keep 6m", "/set tab_bar_width 30", "/set nick_width 0", "/set history_fetch 500", "/set tab_bar_width \"\""})
            core.exec(home, ok), assert(home->lines.back().kind != LK::ERROR);
        assert(ini_get(core.cfg, "general", "log_keep") == "6m" && ini_get(core.cfg, "general", "tab_bar_width").empty());
        core.from_web = true;
        core.exec(home, "/set log_keep 1q"), core.exec(home, "/set tab_bar_position up");
        core.from_web = false;
        assert(core.set_refused == "log_keep = 1q: takes days (30), or 2w, 6m, 1y; 0 keeps all · tab_bar_position = up: takes top, left, right");
        core.set_refused.clear();
        for (const char* k : {"log_keep", "tab_bar_width", "nick_width", "history_fetch"}) core.cfg["general"].erase(k), core.unsaved["general"].erase(k), core.set_was["general"].erase(k);
        if (core.unsaved["general"].empty()) core.unsaved.erase("general");
    }
    {  // with a lock password, exec_web (and lock itself) change only after typing it
        core.cfg["general"]["lock"] = "on", core.cfg["general"]["lock_password_hash"] = hash_password("pw");
        core.cfg["general"]["lock_grace"] = "0";  // every guarded /set asks, as these tests expect; lock_grace has its own below
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
        // the folder the webchat serves files from, and the logins of the webchat and the bouncer, are as guarded
        for (const char* line : {"/set dcc_dir /etc", "/set server.t.dcc_dir /etc", "/set dcc_web on", "/set web.user x", "/set web.html /etc/passwd", "/set bnc.password_hash x", "/bnc password"}) {
            core.exec(home, line);
            assert(core.pass_buf == home);
            core.lock_fails = 0;  // each line's own first try; the throttle is checked below
            core.exec(home, "nope");
            assert(!core.pass_buf && home->lines.back().text == "Wrong password: it stays as it was");
        }
        assert(ini_get(core.cfg, "general", "dcc_dir").empty() && ini_get(core.cfg, "general", "dcc_web").empty() && ini_get(core.cfg, "web", "user").empty() &&
               ini_get(core.cfg, "bnc", "password_hash").empty());
        core.lock_fails = 0;
        for (int i = 0; i < 2; i++) core.exec(home, "/set dcc_dir /etc"), core.exec(home, "nope");
        core.exec(home, "/set dcc_dir /etc"), core.exec(home, "nope");  // the 3rd wrong one in a row: a minute with no tries
        assert(home->lines.back().text == "Wrong password. Too many tries: wait 60s");
        core.exec(home, "/set dcc_dir /etc"), core.exec(home, "pw");  // even the right one waits (a webchat can't guess it)
        assert(home->lines.back().text.rfind("Too many tries: wait ", 0) == 0 && ini_get(core.cfg, "general", "dcc_dir").empty());
        core.lock_wait = 0;
        core.exec(home, "/set dcc_dir /etc"), core.exec(home, "pw");  // the right one, and the line runs
        assert(core.cfg["general"]["dcc_dir"] == "/etc" && !core.pass_buf);
        core.exec(home, "/set dcc_dir"), core.exec(home, "/set nick_width 12");  // reading one, or a setting that isn't guarded, asks nothing
        assert(!core.pass_buf && core.cfg["general"]["nick_width"] == "12");
        core.cfg["general"].erase("dcc_dir"), core.cfg["general"].erase("nick_width"), core.unsaved.clear(), core.set_was.clear();
        {  // a guarded /set the lock password refuses is said where the settings window shows it (not only in the tab behind it)
            core.lock_fails = core.lock_wait = 0, core.set_denied.clear(), core.set_refused.clear();
            core.exec(home, "/set web.bind 10.0.0.9"), core.exec(home, "nope");  // the terminal's window
            assert(core.set_denied == "web.bind: Wrong password" && core.set_refused.empty() && ini_get(core.cfg, "web", "bind").empty());
            core.set_denied.clear();
            core.exec(home, "/set web.bind 10.0.0.9"), core.exec(home, "");  // Esc
            assert(core.set_denied == "web.bind: cancelled" && !core.pass_buf);
            core.set_denied.clear();
            // the webchat's Apply sends every changed row, and /save for Apply & save, before the password is typed: one password for all
            core.from_web = true;
            core.exec(home, "/set web.bind 10.0.0.9"), core.exec(home, "/set web.port 9"), core.exec(home, "/save"), core.exec(home, "nope");
            assert(core.set_refused == "web.bind, web.port: Wrong password" && core.set_denied.empty() && ini_get(core.cfg, "web", "bind").empty() && !core.pass_buf);
            core.set_refused.clear();
            std::string was = core.cfg_path;
            core.cfg_path = "build/test-gate.conf";
            core.exec(home, "/set web.bind 10.0.0.9"), core.exec(home, "/set web.port 9"), core.exec(home, "/save"), core.exec(home, "pw");
            core.from_web = false;
            assert(core.cfg["web"]["bind"] == "10.0.0.9" && core.cfg["web"]["port"] == "9" && core.set_refused.empty() && core.unsaved.empty());  // both applied, then saved
            std::ifstream saved("build/test-gate.conf");
            std::string text((std::istreambuf_iterator<char>(saved)), std::istreambuf_iterator<char>());
            assert(text.find("10.0.0.9") != std::string::npos);
            core.cfg_path = was, core.cfg["web"].erase("bind"), core.cfg["web"].erase("port"), core.unsaved.clear(), core.set_was.clear(), core.lock_fails = 0;
            std::remove("build/test-gate.conf");
        }
        {  // lock_grace: after the password was right, the next guarded /set on that side doesn't ask for a few minutes
            core.lock_fails = core.lock_wait = 0;
            core.cfg["general"]["lock_grace"] = "2";
            core.exec(home, "/set web.bind 10.0.0.9"), core.exec(home, "pw");
            assert(core.cfg["web"]["bind"] == "10.0.0.9" && core.lock_grace_until[0] > time(nullptr));
            core.exec(home, "/set web.port 9");  // no prompt
            assert(!core.pass_buf && core.cfg["web"]["port"] == "9");
            core.exec(home, "/set lock_grace 3"), core.exec(home, "/set lock_idle 0");  // not guarded either way
            core.exec(home, "/set lock off");  // the lock's own keys always ask
            assert(core.pass_buf == home);
            core.exec(home, "");
            core.from_web = true;  // the webchat has its own: the terminal's password doesn't cover it
            core.exec(home, "/set web.port 10");
            assert(core.pass_buf == home && core.cfg["web"]["port"] == "9");
            core.exec(home, "");
            core.from_web = false;
            core.set_lock(true, false);  // locking ends it
            core.set_lock(false, false);
            core.exec(home, "/set web.port 11");
            assert(core.pass_buf == home && core.cfg["web"]["port"] == "9");
            core.exec(home, "");
            core.cfg["general"]["lock_grace"] = "0", core.lock_grace_until[0] = core.lock_grace_until[1] = 0;
            core.exec(home, "/set web.bind 10.0.0.1"), core.exec(home, "pw");
            core.exec(home, "/set web.port 12");  // 0: asks every time
            assert(core.pass_buf == home && core.cfg["web"]["port"] == "9");
            core.exec(home, "");
            core.cfg["web"].erase("bind"), core.cfg["web"].erase("port"), core.unsaved.clear(), core.set_was.clear(), core.lock_fails = 0;
        }
        {  // web.tls: auto or true, and the settings list offers both
            assert(bad_value("web", "tls", "auto").empty() && bad_value("web", "tls", "true").empty());
            assert(bad_value("web", "tls", "false") == "tls = false: takes auto (HTTPS unless bound to localhost) or true (HTTPS on localhost too)");
            for (auto& r : core.setting_rows())
                if (r.sec == "web" && r.key == "tls") assert(r.choices == std::vector<std::string>({"auto", "true"}));
            assert(bad_value("general", "lock_grace", "5").empty() && !bad_value("general", "lock_grace", "61").empty());
            core.exec(home, "/set cursor block "), assert(core.cfg["general"]["cursor"] == "block"), core.exec(home, "/set cursor blink");  // a trailing space (a completed value had one) isn't part of it
            assert(core.setting_choices("general", "cursor") == std::vector<std::string>({"blink", "block", "off"}));  // /set cursor, Tab and the windows
            core.cfg["general"]["mouse"] = "on", core.cfg["general"]["scrollback"] = "2000";  // this Core hasn't loaded the template's defaults
            assert(core.setting_choices("general", "mouse") == std::vector<std::string>({"on", "off"}) && core.setting_choices("general", "scrollback").empty());
            assert(core.setting_choices("general", "colors").size() == 3 && core.setting_choices("server.x", "ip_version").size() == 3);
            assert(core.setting_hint("general", "cursor").find("block") != std::string::npos && core.setting_hint("server.x", "nick") == core.setting_hint("general", "nick"));
            assert(bad_value("general", "cursor", "block").empty() && bad_value("general", "cursor", "off").empty() && !bad_value("general", "cursor", "bar").empty());
            assert(bad_value("general", "web_font", "terminal").empty() && !bad_value("general", "web_font", "comic").empty());  // the presets
        }
        core.exec(home, "/exit -y"), core.exec(home, "nope");  // /exit too: the lock password, or iSekur stays
        assert(home->lines.back().text == "Wrong password: iSekur keeps running" && !core.quit);
        core.exec(home, "/exit -y");
        assert(core.pass_buf == home && !core.quit);
        core.exec(home, "");  // Esc / empty: cancelled
        assert(!core.quit && !core.pass_buf);
        core.self = "";  // /restart: the lock password too, and it quits nothing it can't start again
        core.exec(home, "/restart -y"), core.exec(home, "pw");
        assert(home->lines.back().text.rfind("Can't restart: ", 0) == 0 && !core.quit && !core.restart);
        core.self = "/bin/sh";
        core.exec(home, "/restart -y see you"), core.exec(home, "pw");
        assert(core.quit && core.restart && core.quit_msg == "see you");
        core.quit = core.restart = false, core.quit_msg.clear();
        {  // Face ID devices: kept in web.passkeys, lock_biometric guarded, /lock devices del takes the password
            std::string was = core.cfg_path;
            core.cfg_path = "build/test-bio.conf";
            std::string pk = "build/test-bio.web.passkeys";
            remove(pk.c_str());
            core.passkeys_read = false, core.passkeys_.clear();
            assert(core.passkeys().empty() && !core.bio_on());
            core.passkeys().push_back({"aWQx", b64("key"), "iPhone", true, 3, 1700000000});
            core.passkeys().push_back({"aWQy", b64("key"), "Mac", false, 0, 1700000000});
            assert(core.passkeys_save(home));
            core.passkeys_read = false, core.passkeys_.clear();
            auto& ks = core.passkeys();
            assert(ks.size() == 2 && ks[0].name == "iPhone" && ks[0].on && ks[0].count == 3 && !ks[1].on && ks[1].key == b64("key"));
            assert(!core.bio_on());  // lock_biometric off
            core.cfg["general"]["lock_biometric"] = "on";
            assert(core.bio_on());
            ks[0].on = false;
            assert(!core.bio_on());  // no device on
            ks[0].on = true;
            core.cfg["general"].erase("lock_biometric");
            core.exec(home, "/set lock_biometric on");  // turning it on asks for the password
            assert(core.pass_buf == home);
            core.exec(home, "");
            core.exec(home, "/lock devices");
            assert(home->lines.back().text.find("Mac (off), added ") == 2);
            core.exec(home, "/lock devices del nobody");
            assert(home->lines.back().text == "No Face ID device called nobody (/lock devices lists them)");
            core.exec(home, "/lock devices del iphone"), core.exec(home, "nope");
            assert(core.passkeys().size() == 2 && home->lines.back().text == "Wrong password: the device stays");
            core.exec(home, "/lock devices del iphone"), core.exec(home, "pw");
            assert(core.passkeys().size() == 1 && core.passkeys()[0].name == "Mac");
            core.passkeys_read = false, core.passkeys_.clear();
            assert(core.passkeys().size() == 1);  // saved
            remove(pk.c_str());
            core.passkeys_read = false, core.passkeys_.clear(), core.cfg_path = was, core.lock_fails = 0;
        }
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
        core.cfg["server.other"]["ignore"] = "spam*!*@*";
        assert(!core.ignored(ts, "SpamBot!u@h"));  // another network's list isn't used
        core.cfg["general"]["ignore"] = "spam*!*@*";  // /ignore -w's is
        assert(core.ignored(ts, "SpamBot!u@h"));
        core.cfg["server.t"]["ignore"] = "spambot!*@*,x";  // an exception wins, whichever list
        assert(!core.ignored(ts, "SpamBot!u@h") && core.ignored(ts, "spamz!u@h"));
        core.cfg.erase("server.other"), core.cfg["general"].erase("ignore"), core.cfg["server.t"].erase("ignore");
        std::string ty;
        time_t until;
        assert(ign_mask("a!*@*,pn=99", &until, &ty) == "a!*@*" && ty == "pn" && until == 99);
        assert(ign_mask("a!*@*", &until, &ty) == "a!*@*" && ty == "pcntid" && !until);
        core.cfg["server.t"]["ignore"] = "k!*@*,k j!*@*,j";  // colors only, joins only: not "ignored"
        assert(!core.ignored(ts, "k!u@h") && core.ignore_types(ts, "j!u@h") == "j" && core.ignored(ts, "j!u@h"));
        core.cfg["server.t"].erase("ignore");
        // mIRC's address types
        assert(ign_addr("Nick", "~user@a.b.example.com", 0) == "*!~user@a.b.example.com");
        assert(ign_addr("Nick", "~user@a.b.example.com", 1) == "*!*user@a.b.example.com");
        assert(ign_addr("Nick", "~user@a.b.example.com", 2) == "*!*@a.b.example.com");
        assert(ign_addr("Nick", "~user@a.b.example.com", 3) == "*!*user@*.b.example.com");
        assert(ign_addr("Nick", "user@1.2.3.4", 4) == "*!*@1.2.3.*");
        assert(ign_addr("Nick", "user@localhost", 4) == "*!*@localhost");
        assert(ign_addr("Nick", "user@a.example.com", 7) == "Nick!*@a.example.com");
        assert(ign_addr("Nick", "user@a.example.com", 9) == "Nick!*@*.example.com");
    }
    {  // /ignore -t: until then, then not; the list shows the time left
        assert(parse_span("90") == 5400 && parse_span("30m") == 1800 && parse_span("1d12h") == 129600 && parse_span("1w") == 604800);
        assert(!parse_span("") && !parse_span("h") && !parse_span("1x") && !parse_span("-5") && !parse_span("2h 3"));
        Buffer* sb = ts->status;
        core.exec(sb, "/ignore -t 2h timed");
        assert(core.ignored(ts, "timed!u@h") && ini_get(core.cfg, "server.t", "ignore").find("timed!*@*=") == 0);
        core.exec(sb, "/ignore -t nonsense other");  // -t alone: CTCPs, as in mIRC; "other" isn't a type
        assert(!core.ignored(ts, "other!u@h") && ini_get(core.cfg, "server.t", "ignore").find("nonsense") == std::string::npos);
        core.cfg["server.t"]["ignore"] = "timed!*@*=" + std::to_string((long long)time(nullptr) - 5) + " keep!*@*";
        assert(!core.ignored(ts, "timed!u@h") && core.ignored(ts, "keep!u@h"));  // ran out
        core.exec(sb, "/ignore -t 1h timed");
        core.exec(sb, "/ignore timed");  // again, no time: for good
        assert(ini_get(core.cfg, "server.t", "ignore") == "keep!*@* timed!*@*");
        core.exec(sb, "/unignore keep");
        assert(ini_get(core.cfg, "server.t", "ignore") == "timed!*@*");
        core.cfg["server.t"].erase("ignore");
        // mIRC's flags: types, -u seconds, -r, -w
        core.exec(sb, "/ignore -pn -u90 bot");
        std::string e = ini_get(core.cfg, "server.t", "ignore");
        assert(e.find("bot!*@*,pn=") == 0 && std::stoll(e.substr(e.find('=') + 1)) - time(nullptr) <= 90);
        core.exec(sb, "/ignore -rw bot");
        assert(sb->lines.back().kind == LK::ERROR);  // not on the every-network list
        core.exec(sb, "/ignore -r bot");
        assert(ini_get(core.cfg, "server.t", "ignore").empty());
        core.exec(sb, "/ignore -w *!*@*.evil.host");
        assert(ini_get(core.cfg, "general", "ignore") == "*!*@*.evil.host" && core.ignored(ts, "a!b@x.evil.host"));
        core.exec(sb, "/unignore -w *!*@*.evil.host");
        assert(ini_get(core.cfg, "general", "ignore").empty());
        core.exec(sb, "/ignore -q bot");
        core.exec(sb, "/ignore -u bot");
        core.exec(sb, "/ignore nobody 2");  // no address known
        assert(sb->lines.back().kind == LK::ERROR && ini_get(core.cfg, "server.t", "ignore").empty());
        core.cfg["general"].erase("ignore");
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
        {  // a private conversation comes back the same way (the incoming line opens the tab)
            core.hist_cache.clear(), core.hist_loaded = false;
            feed(":Pal!u@h PRIVMSG " + ts->nick + " :hi there");
            Buffer* q = core.find(ts, "Pal");
            assert(q && q->type == BT::QUERY && q->lines.size() == 1);
            core.add(q, Line{time(nullptr), LK::MSG, ts->nick, "hello Pal", true});
            core.save_history();
            core.close(q);
            core.hist_cache.clear(), core.hist_loaded = false;
            q = core.open(ts, BT::QUERY, "Pal");
            assert(q->lines.size() == 3 && q->lines[0].text == "hi there" && q->lines[1].self && q->lines[2].nolog);
            core.close(q);
            // and the way a restart does it: open_queries reopens the tab when the server is added
            q = core.open(ts, BT::QUERY, "Pal");
            core.hist_cache.clear(), core.hist_loaded = false;
            core.hist_loaded = false, q->lines.clear();
            core.add(q, Line{time(nullptr), LK::MSG, "Pal", "still here"});
            core.save_history();
            assert(ini_get(core.cfg, "server." + ts->cfg.name, "open_queries") == "Pal");
            core.hist_cache.clear(), core.hist_loaded = false;
            Server* s2 = core.add_server(ts->cfg);
            Buffer* q2 = core.find(s2, "Pal");
            assert(q2 && q2->lines.size() == 2 && q2->lines[0].text == "still here");
            core.close(s2->status), core.close(q);
        }
        core.cfg["general"]["keep_history"] = "off";
        core.hist_cache.clear(), core.hist_loaded = false;
        h = core.open(ts, BT::CHANNEL, "#hist");
        assert(h->lines.empty());
        core.close(h);
        core.cfg["general"].erase("keep_history"), core.cfg["general"].erase("log_history");
        core.hist_cache.clear(), core.hist_loaded = true;  // nothing to keep: no empty file left behind
        core.save_history();
        assert(access("build/test-hist.history", F_OK) != 0);
        {  // a folder it can't write to: said once (not every 5 minutes), and again only after a save worked
            core.cfg_path = "build/no-such-dir/test.conf";
            Buffer* h2 = core.open(ts, BT::CHANNEL, "#nosave");
            core.add(h2, Line{1000, LK::MSG, "bob", "x"});
            core.save_history();
            const Line& w = core.bufs.front()->lines.back();
            assert(core.hist_warned && w.kind == LK::ERROR && w.text.find("Can't save the chat history to build/no-such-dir/test.history: ") == 0);
            size_t n = core.bufs.front()->lines.size();
            core.save_history();
            assert(core.bufs.front()->lines.size() == n);
            core.cfg_path = "build/test-hist.conf";
            core.save_history();
            assert(!core.hist_warned);
            core.close(h2);
        }
        unlink("build/test-hist.history"), core.cfg_path.clear();
    }
    {  // /ignore types on the wire: -c drops channel lines only, -k strips colors, -j hides joins, nick 2 uses their host
        core.cfg["server.t"]["ignore"] = "chan!*@*,c pastel!*@*,k lurk!*@*,j";
        size_t n = chb->lines.size();
        feed(":chan!u@h PRIVMSG #c :hidden");
        feed(":chan!u@h PRIVMSG me :shown");
        assert(chb->lines.size() == n && core.find(ts, "chan"));
        feed(":pastel!u@h PRIVMSG #c :\x03" "04red\x0f text");
        assert(chb->lines.back().text == "red text");
        feed(":lurk!u@lurk.example.org JOIN #c");
        assert(chb->lines.back().text != "lurk (u@lurk.example.org) has joined #c" && chb->nicks.count("lurk"));
        core.cfg["server.t"].erase("ignore");
        core.exec(chb, "/ignore lurk 2");
        assert(ini_get(core.cfg, "server.t", "ignore") == "*!*@lurk.example.org");
        feed(":other!u@lurk.example.org PRIVMSG #c :gone");
        assert(chb->lines.back().text != "gone");
        core.cfg["server.t"].erase("ignore");
        chb->nicks.erase("lurk");
        if (Buffer* q = core.find(ts, "chan")) core.close(q);
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
        assert(ts->status->lines.back().text == "Your user mode is [+iwzT]");  // 221 at connect: status
        feed(":me!u@h MODE me :+x\x02-w");
        assert(ts->umodes == "izTx");
        assert(ts->status->lines.back().text == "Mode change [+x-w] for user me, your user mode is now [+izTx]");  // a change: there too
    }
    {  // /bans: the 367s fill the list the window shows, the 368 ends it; a list nobody asked for prints as before
        ts->chanmodes = "eIbq,k,l,imnpst";
        core.ban_list(chb, 'b', 2);
        assert(sent().find("MODE #c +b\r\n") != std::string::npos && ts->bl.loading);
        feed(":srv 367 me #c *!*@bad.host alice!u@h 1700000000");
        feed(":srv 367 me #c troll!*@* bob 1700000100");
        assert(ts->bl.v.size() == 2 && ts->bl.loading);
        feed(":srv 368 me #c :End of channel ban list");
        assert(!ts->bl.loading && ts->bl.v[1].setter == "bob" && ts->bl.v[0].ts == 1700000000);
        size_t nl = chb->lines.size();
        feed(":srv 367 me #c typed!*@* bob 5");  // nobody asked: the generic line
        assert(chb->lines.size() == nl + 1 && chb->lines.back().text.find("typed!*@*") != std::string::npos);
        core.ban_list(chb, 'q', 2), sent();  // 728/729 name the mode
        feed(":srv 728 me #c q m!*@* carol 1700000200");
        feed(":srv 729 me #c q :End of channel quiet list");
        assert(!ts->bl.loading && ts->bl.v.size() == 1 && ts->bl.v[0].mask == "m!*@*" && ts->bl.v[0].setter == "carol");
        core.ban_list(chb, 'q', 0), sent();  // no window: printed, newest first
        feed(":srv 728 me #c q old!*@* x 1000000000");
        feed(":srv 728 me #c q new!*@* y 1700000000");
        feed(":srv 729 me #c q :End");
        assert(chb->lines.back().text.find("old!*@*") != std::string::npos && chb->lines[chb->lines.size() - 2].text.find("new!*@*") != std::string::npos);
        core.ban_list(chb, 'b', 2), sent();  // no answer for 15 seconds: the window gets an error, not "Loading"
        ts->bl.at -= 20;
        core.chan_timers(ts, time(nullptr));
        assert(!ts->bl.loading && !ts->bl.err.empty());
        ts->chanmodes = "beI,k,l,imnpst";
        nl = chb->lines.size();
        core.ban_list(chb, 'q', 2);  // this server has no +q list
        assert(sent().empty() && chb->lines.size() == nl + 1 && chb->lines.back().kind == LK::ERROR);
        // changes take halfop or more
        NickEntry& me = chb->nicks[ts->lower(ts->nick)];
        me.nick = ts->nick, me.prefix = "";
        assert(!core.ban_rank(chb));
        core.exec(chb, "/ban troll");
        core.ban_edit(chb, 'b', true, "troll");
        assert(sent().empty());
        me.prefix = "@";
        assert(core.ban_rank(chb));
        core.exec(chb, "/ban troll");
        assert(sent() == "MODE #c +b troll!*@*\r\n");
        core.ban_edit(chb, 'b', false, "troll!*@*");
        assert(sent() == "MODE #c -b troll!*@*\r\nMODE #c +b\r\n" && ts->bl.loading);
        ts->bl.loading = false, me.prefix = "";
        chb->nicks.erase(ts->lower(ts->nick));
    }
    {  // 324/329: our join's MODE fills the title and prints, like a typed /mode; +k/+l track live
        feed(":me!u@h JOIN #mt");
        Buffer* mb = core.find(ts, "#mt");
        assert(mb && sent().find("MODE #mt\r\n") != std::string::npos);
        feed(":srv 324 me #mt +ntk sec");
        assert(mb->modes == "ntk" && mb->lines.back().text == "Channel #mt modes: [+ntk sec]");
        feed(":srv 329 me #mt 1700000000");
        assert(mb->lines.back().text.rfind("Channel #mt created 2023-11-", 0) == 0);
        feed(":srv 324 me #mt +ntk sec");  // the same reply to a typed /mode is shown
        assert(mb->lines.back().text == "Channel #mt modes: [+ntk sec]");
        feed(":op!u@h MODE #mt +lb 5 x!*@*");
        assert(mb->modes == "ntkl");
        feed(":op!u@h MODE #mt -lk sec");
        assert(mb->modes == "nt");
        core.close(mb);
        ts->umodes.clear();
        {  // server notices: to *snotices while +o/+s whatever the text says (Unreal's have no "***"); never from a nick
            size_t n = ts->status->lines.size();
            feed(":irc.t NOTICE me :connect.LOCAL_CLIENT_CONNECT Client connecting: x (u@h)");
            assert(ts->status->lines.size() == n + 1 && !core.find(ts, "*snotices"));  // not an oper: the server tab
            ts->umodes = "os";
            feed(":irc.t NOTICE me :connect.LOCAL_CLIENT_CONNECT Client connecting: x (u@h)");
            feed(":irc.t NOTICE me :*** Notice -- Client connecting: y (u@h)");
            Buffer* sn = core.find(ts, "*snotices");
            assert(sn && sn->lines.size() == 2 && sn->lines[0].text.rfind("connect.LOCAL", 0) == 0 && ts->status->lines.size() == n + 1);
            feed(":nobody NOTICE me :*** Notice -- not from a server");  // no dot: a nick, not a server
            assert(sn->lines.size() == 2 && ts->status->lines.size() == n + 2);
            feed(":bob!u@h NOTICE me :hi");  // an address: a user
            assert(sn->lines.size() == 2);
            core.close(sn), ts->umodes.clear();
        }
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
        // a pass past its time budget handles one line and leaves the rest for the next pass; none get lost
        ts->inbuf = ":me NICK :a1\r\n:a1 NICK :a2\r\n:a2 NICK :me\r\n";
        core.on_readable(ts, 0, 1);
        assert(ts->nick == "a1" && Core::rx_backlog(ts));
        core.on_readable(ts, 0, 1);
        assert(ts->nick == "a2" && Core::rx_backlog(ts));
        core.on_readable(ts, 0);
        assert(ts->nick == "me" && ts->inbuf.empty() && !Core::rx_backlog(ts));
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
    {  // /alias: [aliases] written at once, its variables, && for several, iSekur's own names kept
        assert((alias_expand("/msg NickServ $*", "identify pw", "me", "#c", "net") == std::vector<std::string>{"/msg NickServ identify pw"}));
        assert((alias_expand("/part && /join $channel", "", "me", "#c", "net") == std::vector<std::string>{"/part", "/join #c"}));
        assert((alias_expand("/msg $1 hi $2- ($nick on $server) $5 $HOME $nickname", "bob a b c", "me", "", "net") ==
                std::vector<std::string>{"/msg bob hi a b c (me on net)  $HOME $nickname"}));
        assert((alias_expand("/msg NickServ", "identify pw", "me", "", "") == std::vector<std::string>{"/msg NickServ identify pw"}));  // no $: at the end
        assert(!perform_secret("/msg NickServ identify $1") && perform_secret("/msg NickServ identify pw"));
        std::string path = "build/test-alias.conf";
        remove(path.c_str());
        std::swap(core.cfg_path, path);
        core.exec(chb, "/alias hi /msg $1 hello from $nick in $channel");
        core.exec(chb, "/alias two /hi bob && /quote PING :$server");
        core.exec(chb, "/alias join /part");  // iSekur's own: refused
        core.exec(chb, "/alias loop /loop");
        sent();
        core.exec(chb, "/two");
        assert(sent() == "PRIVMSG bob :hello from me in #c\r\nPING :t\r\n");
        core.exec(chb, "/loop");  // stops 10 deep
        assert(chb->lines.back().text.find("10 deep") != std::string::npos && ini_get(core.cfg, "aliases", "join").empty());
        core.exec(chb, "/alias ns /msg NickServ identify pw9z");
        std::ifstream f1(core.cfg_path);
        std::string file((std::istreambuf_iterator<char>(f1)), {});
        assert(file.find("hi = /msg $1 hello from $nick in $channel") != std::string::npos && file.find("ns = enc:") != std::string::npos && file.find("pw9z") == std::string::npos);
        core.exec(chb, "/alias");
        assert(chb->lines.back().text == "  /two = /hi bob && /quote PING :$server" && chb->lines[chb->lines.size() - 3].text == "  /loop = /loop");
        bool row = false;  // the settings windows: an Aliases category with the alias, its password hidden in the list
        for (auto& r : core.setting_rows()) row |= r.sec == "aliases" && r.key == "ns" && r.cat == "aliases";
        for (auto& st : core.settings()) if (st.key == "aliases.ns") assert(st.val == "/msg NickServ identify ********");
        assert(row);
        core.exec(chb, "/alias del /ns");
        core.exec(chb, "/alias del loop");
        Ini disk;
        assert(ini_load(core.cfg_path, disk) && ini_get(disk, "aliases", "ns").empty() && ini_get(disk, "aliases", "hi") == "/msg $1 hello from $nick in $channel");
        std::swap(core.cfg_path, path);
        remove(path.c_str());
        core.cfg.erase("aliases"), core.unsaved.erase("aliases");
        sent();
    }
    core.exec(ts->status, "/mode -x");  // outside a channel: your own modes, not the server tab's name
    assert(sent() == "MODE me -x\r\n");
    core.exec(chb, "/umode +i");  // from any tab
    core.exec(chb, "/umode");
    core.exec(ts->status, "/mode #c +t");  // a target given: as typed
    assert(sent() == "MODE me +i\r\nMODE me\r\nMODE #c +t\r\n");
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
        feed(":" + ts->nick + "!u@h JOIN #bg");  // autojoin (no /join of ours): opens in the background, typing stays here
        assert(core.find(ts, "#bg") && core.active == sp);
        feed(":srv 332 " + ts->nick + " #bg :a topic");  // the join's topic, names, modes: no activity
        assert(core.find(ts, "#bg")->activity == 0);
        feed(":irc.test MODE #bg +nt");  // a new channel's modes, set by the server itself: no activity
        assert(core.find(ts, "#bg")->activity == 0);
        ts->status->activity = 0;
        feed(":ChanServ!s@services MODE #bg +v " + ts->nick);  // the join burst: services' voice, their welcome notice
        feed(":ChanServ!s@services NOTICE " + ts->nick + " :Welcome to #bg!");
        assert(core.find(ts, "#bg")->activity == 0 && ts->status->activity == 0);
        feed(":srv BATCH +h1 chathistory #bg");  // the catch-up of what was said before: old news, no activity
        feed("@batch=h1;time=2020-01-01T00:00:00.000Z :carol!u@h PRIVMSG #bg :old");
        feed(":srv BATCH -h1");
        assert(std::any_of(core.find(ts, "#bg")->lines.begin(), core.find(ts, "#bg")->lines.end(), [](const Line& l) { return l.text == "old"; }));
        assert(core.find(ts, "#bg")->activity == 0 && core.find(ts, "#bg")->unread == 0);
        feed(":bob!u@h PRIVMSG #bg :hi");  // a message still lights the tab
        assert(core.find(ts, "#bg")->activity == 2);
        core.close(core.find(ts, "#bg")), sent();
        core.exec(sp, "/join #new"), sent();
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
        {  // a message to a query (a webchat tap on a nick, the terminal in #other): "no such nick" lands in the query
            Buffer* q = core.open(ts, BT::QUERY, "gone");
            core.exec(q, "hello?"), sent();
            feed(":srv 401 me gone :No such nick/channel");
            assert(q->lines.back().text.find("No such nick") != std::string::npos && other->lines.empty());
            core.close(q);
        }
        {  // an open query watches its nick (MONITOR): offline, it says so and doesn't send
            ts->motd_done = true, ts->isupport["MONITOR"] = "100";
            Buffer* q = core.open(ts, BT::QUERY, "gone");
            assert(sent() == "MONITOR + gone\r\n");
            feed(":srv 731 me :gone");
            assert(q->lines.back().text == "gone is offline" && ts->status->lines.back().text.find("gone") == std::string::npos);
            core.exec(q, "hello?");
            assert(sent().empty() && q->lines.back().text == "gone is offline: not sent");
            feed(":srv 730 me :gone!u@h");
            assert(q->lines.back().text == "gone is online");
            core.exec(q, "hello!");
            assert(sent() == "PRIVMSG gone :hello!\r\n");
            core.say(q, std::string(500, '\xa1'));  // not UTF-8, longer than a line: cut in two, no endless loop
            { std::string o = sent(); assert(std::count(o.begin(), o.end(), '\n') == 2); }
            feed(":gone!u@h QUIT :bye");  // its "has quit" is enough, then nothing goes
            feed(":srv 731 me :gone");
            assert(q->lines.back().text.find("has quit") != std::string::npos);
            core.exec(q, "x"), assert(sent().empty());
            feed(":gone!u@h PRIVMSG me :back");  // it speaks: online again before any 730 or ISON
            core.exec(q, "hi"), assert(sent() == "PRIVMSG gone :hi\r\n");
            core.close(q);
            assert(sent() == "MONITOR - gone\r\n" && !ts->online.count("gone"));
            ts->isupport.erase("MONITOR");  // no MONITOR: ISON asks, the reply says
            q = core.open(ts, BT::QUERY, "gone");
            assert(ts->ison_at);
            ts->send("ISON gone"), ts->ison_asked = " gone", ts->ison_wait++, sent();
            feed(":srv 303 me :");
            assert(q->lines.back().text == "gone is offline");
            core.close(q), sent(), ts->motd_done = false, ts->ison_at = 0;
        }
        ts->typed_at = 0;  // long ago (an autojoin): not the active #other
        feed(":srv 474 me #b :Cannot join channel (+b)");
        assert(ts->status->lines.back().text.find("(+b)") != std::string::npos && other->lines.empty());
        other->activity = 2, other->unread = 3, ts->status->activity = 1;
        core.mark_all_read();  // Ctrl+R / Cmd+R, the terminal's and the webchat's
        assert(!other->activity && !other->unread && !ts->status->activity);
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
    {  // logged in, and the server has an account extban: their account is banned too
        chb->nicks["r2"].host = "u@h9", chb->nicks["r2"].account = "R2acct";
        ts->isupport["EXTBAN"] = "~,amnrz";
        core.exec(chb, "/kickban r2");
        assert(sent() == "MODE #c +bb *!*@h9 ~a:R2acct\r\nKICK #c r2 :Banned\r\n");
        ts->isupport["EXTBAN"] = ",ABCRjm";  // InspIRCd: no prefix, letters of its own
        core.exec(chb, "/kickban r2");
        assert(sent() == "MODE #c +b *!*@h9\r\nKICK #c r2 :Banned\r\n");
        ts->isupport["EXTBAN"] = "$,ajrxz";  // Solanum
        core.exec(chb, "/tempban r2 5");
        assert(sent() == "MODE #c +bb *!*@h9 $a:R2acct\r\nKICK #c r2 :Banned for 5 minutes\r\n" && ts->tempbans.size() == 2);
        core.chan_timers(ts, time(nullptr) + 301);
        std::string lifted = sent();
        assert(lifted.find("-b *!*@h9") != std::string::npos && lifted.find("-b $a:R2acct") != std::string::npos && ts->tempbans.empty());
        ts->isupport.erase("EXTBAN"), chb->nicks["r2"].host.clear(), chb->nicks["r2"].account.clear();
    }
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
        assert(line.rfind("PRIVMSG #c :+iSE2 ", 0) == 0 && line.find("secret") == std::string::npos);
        std::string sealed = line.substr(0, line.size() - 2);
        feed(":" + ts->nick + "!u@h2 " + sealed);  // the AAD names the sender: it opens as ours...
        assert(chb->lines.back().text == "secret" && chb->lines.back().e2e);
        feed(":r1!u@h2 " + sealed);  // ...and not moved under another nick, same key and channel
        assert(chb->lines.back().text.find("wrong password?") != std::string::npos && !chb->lines.back().e2e);
        std::string theirs = "PRIVMSG #c :" + e2e_line_seal(ts->chan_e2e["#c"], "secret", e2e_aad_c("#c", "r1"));  // r1's line
        feed(":r1!u@h2 " + theirs);
        assert(chb->lines.back().text == "secret" && chb->lines.back().e2e);
        {  // e2e_effect takes /typing's place on encrypted lines only; off, they type out like the rest
            core.cfg["general"]["typing_effect"] = "on";
            assert(chb->lines.back().fx_at && !chb->lines.back().typed_at);
            feed(":r1!u@h2 PRIVMSG #c :plain");
            assert(!chb->lines.back().e2e && !chb->lines.back().fx_at && chb->lines.back().typed_at);
            core.cfg["general"]["e2e_effect"] = "off";
            feed(":r1!u@h2 " + theirs);
            assert(chb->lines.back().e2e && !chb->lines.back().fx_at && chb->lines.back().typed_at);
            feed(":r1!u@h2 PRIVMSG #c :\xf0\x9f\x94\x92 fake");  // a typed lock is text, not the flag
            assert(!chb->lines.back().e2e);
            Line l = chb->lines.back();
            l.e2e = true, l.fx_at = 1000, l.self = false;
            assert(e2e_glyph(l, 5, 6, 1000) && !e2e_glyph(l, 5, 6, 1000 + E2E_FX_MS));  // noise, then the text
            core.cfg["general"].erase("typing_effect"), core.cfg["general"].erase("e2e_effect");
        }
        ts->chan_e2e["#c"] = e2e_key("nope", "isekur-chan:#c");
        feed(":" + ts->nick + "!u@h2 " + sealed);
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
    {  // /e2e bob: a chat that is on isn't replaced, switched off or failed by a line anyone (the server too) could send; only its key does
        bool had = core.find(ts, "bob") != nullptr;
        Buffer* was = core.active;
        auto ctcp_in = [&](const std::string& verb, const std::string& arg) { feed(":bob!u@h PRIVMSG me :\x01" + verb + (arg.empty() ? "" : " " + arg) + "\x01"); };
        auto arg_of = [](const std::string& l, const std::string& verb) {  // "PRIVMSG bob :\x01VERB arg\x01" -> arg
            size_t a = l.find(verb + " ");
            assert(a != std::string::npos);
            a += verb.size() + 1;
            return l.substr(a, l.find('\x01', a) - a);
        };
        std::string plain;
        auto establish = [&](const std::string& pw) {  // we invite, bob answers with the same password, we prove it back
            core.exec(chb, "/e2e bob " + pw);
            std::string key = e2e_key(pw, unb64(arg_of(sent(), "ISEKUR-E2E")));
            ctcp_in("ISEKUR-E2E-OK", e2e_seal(key, "isekur-e2e-ok"));
            assert(arg_of(sent(), "ISEKUR-E2E-ACK").size() > 20 && ts->e2e["bob"].ok);
            return key;
        };
        std::string key = establish("pw1"), salt0 = ts->e2e["bob"].salt, forged(16, 'z');
        ctcp_in("ISEKUR-E2E", b64(forged));  // a new invitation: kept aside, the chat is as it was
        ctcp_in("ISEKUR-E2E-OFF", ""), ctcp_in("ISEKUR-E2E-OFF", e2e_seal(e2e_key("other", salt0), "isekur-e2e-off"));
        ctcp_in("ISEKUR-E2E-OFF", e2e_seal(key, "isekur-e2e-ok"));  // the right key, the wrong token
        ctcp_in("ISEKUR-E2E-BAD", ""), ctcp_in("ISEKUR-E2E-ACK", "junk"), ctcp_in("ISEKUR-E2E-OK", "junk");
        E2E& e = ts->e2e["bob"];
        assert(e.ok && e.key == key && e.salt == salt0 && e.pend == forged && e2e_on(core.find(ts, "bob")));
        core.exec(chb, "/e2e bob pw2");  // taking the new invitation up is the user's call
        assert(e.salt == forged && !e.ok && e.pend.empty() && sent().find("ISEKUR-E2E-OK ") != std::string::npos);
        std::string key2 = e2e_key("pw2", forged);
        ctcp_in("ISEKUR-E2E-ACK", e2e_seal(key2, "isekur-e2e-ack"));
        assert(ts->e2e["bob"].ok);
        ctcp_in("ISEKUR-E2E-OFF", e2e_seal(key2, "isekur-e2e-off"));  // the holder of the key ends it
        assert(!ts->e2e.count("bob"));
        std::string key3 = establish("pw3");
        core.exec(chb, "/e2e off bob");  // and so do we: the line we send proves it
        assert(!ts->e2e.count("bob") && e2e_open(key3, arg_of(sent(), "ISEKUR-E2E-OFF"), plain) && plain == "isekur-e2e-off");
        std::string key4 = establish("pw4");
        ctcp_in("ISEKUR-E2E-OFF", "");  // an old iSekur's bare OFF can't end a chat that is on
        assert(ts->e2e.count("bob"));
        {  // lines: we invited, so ours carry "i" and bob's "r"; our own line sent back as bob's doesn't open
            Buffer* bq = core.find(ts, "bob");
            core.say(bq, "hola", false);
            std::string l = sent(), body = l.substr(l.find(" :") + 2, l.size() - l.find(" :") - 4);
            assert(body.rfind("+iSE2 ", 0) == 0 && body.find("hola") == std::string::npos);
            feed(":bob!u@h PRIVMSG " + ts->nick + " :" + body);  // the server sends it back as bob's
            assert(bq->lines.back().text.find("wrong password?") != std::string::npos && !bq->lines.back().e2e);
            feed(":" + ts->nick + "!u@h PRIVMSG bob :" + body);  // an echo of ours is ours
            assert(bq->lines.back().text == "hola" && bq->lines.back().e2e);
            feed(":bob!u@h PRIVMSG " + ts->nick + " :" + e2e_line_seal(key4, "hi", e2e_aad_q(false)));
            assert(bq->lines.back().text == "hi" && bq->lines.back().e2e);
            feed(":bob!u@h PRIVMSG " + ts->nick + " :+iSE " + e2e_seal(key4, "old"));  // an old iSekur's line, no AAD
            assert(bq->lines.back().text == "old" && bq->lines.back().e2e);
        }
        core.exec(chb, "/e2e off bob");
        sent();
        if (!had) core.close(core.find(ts, "bob"));
        core.select(was);
    }
    {  // 433 while registering: altnick, then "_" added, and after ten tries it stops (a server cutting nicks short said 433 for ever)
        ts->state = SS::REGISTERING, ts->nick_tries = 0, ts->nick = "me";
        sent();
        for (int i = 0; i < 14; i++) feed(":srv 433 * me :Nickname is already in use");
        std::string out = sent();
        size_t nicks = 0;
        for (size_t at = 0; (at = out.find("NICK ", at)) != std::string::npos; at += 5) nicks++;
        assert(nicks == 10 && ts->nick_tries == 10 && ts->status->lines.back().text.find("No free nick after 10 tries") == 0);
        ts->state = SS::CONNECTED, ts->nick_tries = 0, ts->nick = "me";
    }
    {  // a config symlinked into a dotfiles repo is written through, not replaced by a regular file
        std::filesystem::remove_all("build/symtest");
        std::filesystem::create_directories("build/symtest/repo");
        { std::ofstream("build/symtest/repo/real.conf") << "[general]\nnick = a\n"; }
        std::filesystem::create_symlink("repo/real.conf", "build/symtest/isekur.conf");
        std::string serr;
        assert(ini_save("build/symtest/isekur.conf", Ini{{"general", {{"nick", "b"}}}}, serr));
        assert(std::filesystem::is_symlink("build/symtest/isekur.conf") && ini_get(ini_parse([&] { std::ifstream in("build/symtest/repo/real.conf"); std::stringstream ss; ss << in.rdbuf(); return ss.str(); }()), "general", "nick") == "b");
        std::filesystem::remove_all("build/symtest");
    }
    {  // log rotation: an earlier day's leftover goes to the dated file, which if already there is added to, never overwritten
        std::filesystem::remove_all("build/logtest");
        std::filesystem::create_directories("build/logtest/t");
        core.cfg["general"]["log"] = "on", core.cfg["general"]["log_dir"] = "build/logtest";
        std::string day = fmt_time(1577962800, "%Y-%m-%d");  // that file's own day, the local one
        auto put = [](const std::string& f, const std::string& text) { std::ofstream(f) << text; };
        auto get = [](const std::string& f) { std::ifstream in(f); std::stringstream ss; ss << in.rdbuf(); return ss.str(); };
        put("build/logtest/t/#lr." + day + ".log", "OLD\n"), put("build/logtest/t/#lr.log", "LEFT\n");
        timeval tv[2] = {{1577962800, 0}, {1577962800, 0}};
        utimes("build/logtest/t/#lr.log", tv);
        Buffer* lb = core.open(ts, BT::CHANNEL, "#lr");
        core.add(lb, Line{time(nullptr), LK::MSG, "bob", "hi"});
        assert(get("build/logtest/t/#lr." + day + ".log") == "OLD\nLEFT\n");
        assert(get("build/logtest/t/#lr.log").find("<bob> hi") != std::string::npos && get("build/logtest/t/#lr.log").find("LEFT") == std::string::npos);
        core.close(lb);
        core.cfg["general"].erase("log"), core.cfg["general"].erase("log_dir");
        std::filesystem::remove_all("build/logtest");
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
        assert(sm.size() == 9 && sm[0].second == "/reconnect" && sm[1].second == "/disconnect" && sm[2].first == "Hide its channels" &&
               sm[4] == std::make_pair(std::string("Edit network..."), std::string("/servers t")) &&
               sm[6].second.rfind("?Disconnect from ", 0) == 0 && sm[7].second == "/server rename t" && sm[8].second.rfind("?Delete t:", 0) == 0 &&
               sm[8].second.find("\n/server del t") != std::string::npos);
        // /servers name: the Edit network item's command; an unknown name is refused
        core.exec(ts->status, "/servers nosuch");
        assert(core.settings_srv.empty() && !core.settings_servers);
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

    // /server without -save is temporary: out of /servers (setting_rows), left out by /save, kept by -save or /autojoin snap
    {
        std::string path = "build/test-servertemp.conf";
        { std::ofstream f(path); f << "[general]\nnick = me\n"; }
        std::swap(core.cfg_path, path);
        auto listed = [&](const char* n) {
            for (auto& r : core.setting_rows())
                if (r.sec == std::string("server.") + n) return true;
            return false;
        };
        auto in_file = [&](const char* n) { Ini f; return ini_load(core.cfg_path, f) && f.count(std::string("server.") + n); };
        core.exec(home, "/server 127.0.0.1 1 -name tmp1");
        assert(core.temp("tmp1") && !listed("tmp1"));
        core.exec(home, "/save");
        assert(!in_file("tmp1") && core.temp("tmp1") && home->lines.back().text.find("not tmp1") != std::string::npos);
        core.exec(home, "/server tmp1 -save");  // a known one: written now
        assert(!core.temp("tmp1") && listed("tmp1") && in_file("tmp1"));
        core.exec(home, "/server 127.0.0.1 1 -name tmp2 -save");
        assert(!core.temp("tmp2") && listed("tmp2") && in_file("tmp2"));
        core.exec(home, "/server 127.0.0.1 1 -name tmp3");
        assert(core.temp("tmp3") && !in_file("tmp3"));
        for (auto& sv : core.servers)
            if (sv->cfg.name == "tmp3") core.exec(sv->status, "/autojoin snap here");  // what is open on it, with its channels
        assert(!core.temp("tmp3") && listed("tmp3") && in_file("tmp3"));
        core.exec(home, "/server 127.0.0.1 1 -name tmp4");
        core.exec(home, "/server del tmp4");
        assert(!core.temp("tmp4") && !in_file("tmp4"));
        // -noconnect: added (and kept with -save) without connecting, as the webchat's Save button for a new network does
        core.exec(home, "/server 127.0.0.1 1 -name tmp5 -save -noconnect");
        {
            Server* n5 = nullptr;
            for (auto& sv : core.servers)
                if (sv->cfg.name == "tmp5") n5 = sv.get();
            assert(n5 && n5->conn.fd < 0 && n5->state == SS::DISCONNECTED && !core.temp("tmp5") && listed("tmp5") && in_file("tmp5"));
            assert(home->lines.back().text.find("not connected") != std::string::npos || n5->status->lines.back().text.find("not connected") != std::string::npos);
        }
        core.exec(home, "/server 127.0.0.1 1 -name tmp5 -nocap -noconnect");  // a known one: its options, still not connected
        for (auto& sv : core.servers)
            if (sv->cfg.name == "tmp5") assert(sv->conn.fd < 0 && !sv->cfg.cap);
        for (auto n : {"tmp1", "tmp2", "tmp3", "tmp5"}) core.exec(home, std::string("/server del ") + n);
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

    // /login and /server -login: asked in dialogs, one login at a time; a new server asks before it connects (a password goes out with the connection)
    {
        std::string path = "build/test-login.conf";
        std::swap(core.cfg_path, path);
        size_t n = core.servers.size();
        core.exec(home, "/server irc.invalid 6667 -name lg -login nope");
        assert(core.servers.size() == n && home->lines.back().text.rfind("-login takes", 0) == 0);
        core.exec(home, "/login sasl");
        assert(home->lines.back().text.rfind("Not a server's tab", 0) == 0 && !core.pass_buf);
        core.exec(home, "/server irc.invalid 6667 -name lg -login sasl");
        Server* g = core.servers.back().get();
        assert(core.servers.size() == n + 1 && g->conn.fd < 0 && core.pass_buf == g->status);  // asking, not connected yet
        core.exec(g->status, "me");
        assert(core.pass_buf == g->status && g->conn.fd < 0);
        core.exec(g->status, "hunter2");
        assert(!core.pass_buf && g->conn.fd >= 0 && g->cfg.sasl_user == "me" && g->cfg.sasl_pass == "hunter2" && core.unsaved["server.lg"].count("sasl_pass"));
        // another login replaces it: SASL's keys are cleared, saved at once
        core.exec(g->status, "/login nickserv");
        assert(core.pass_buf == g->status);
        core.exec(g->status, "ns3cret");
        Ini f;
        assert(!core.pass_buf && g->cfg.nickserv_pass == "ns3cret" && g->cfg.sasl_user.empty() && g->cfg.sasl_pass.empty() &&
               ini_get(core.cfg, "server.lg", "sasl_user").empty() && !core.unsaved.count("server.lg") && ini_load(core.cfg_path, f) &&
               f["server.lg"]["nickserv_pass"] == "ns3cret" && f["server.lg"]["sasl_user"].empty());
        // an empty answer leaves it as it was
        core.exec(g->status, "/login sasl");
        core.exec(g->status, "");
        assert(!core.pass_buf && g->cfg.nickserv_pass == "ns3cret" && g->cfg.sasl_user.empty());
        // a certificate
        core.exec(g->status, "/login cert");
        core.exec(g->status, "/tmp/me.pem");
        assert(!core.pass_buf && g->cfg.cert == "/tmp/me.pem" && g->cfg.key.empty() && g->cfg.nickserv_pass.empty());
        core.exec(g->status, "/login none");
        assert(!core.pass_buf && g->cfg.cert.empty() && ini_get(core.cfg, "server.lg", "cert").empty());
        core.exec(g->status, "/login what");
        assert(g->status->lines.back().text.rfind("Usage: /login", 0) == 0);
        // cancelled on a new server: it's there, not connected
        core.exec(home, "/server irc.invalid 6667 -name lg2 -login password");
        Server* g2 = core.servers.back().get();
        core.exec(g2->status, "");
        assert(!core.pass_buf && g2->conn.fd < 0 && core.cfg.count("server.lg2") && ini_get(core.cfg, "server.lg2", "password").empty());
        // -save keeps the login with it, -noconnect leaves it not connected once answered
        core.exec(home, "/server irc.invalid 6667 -name lg3 -save -noconnect -login sasl");
        Server* g3 = core.servers.back().get();
        core.exec(g3->status, "me");
        core.exec(g3->status, "pw3");
        Ini f3;
        assert(!core.pass_buf && g3->conn.fd < 0 && !core.unsaved.count("server.lg3") && ini_load(core.cfg_path, f3) && f3["server.lg3"]["sasl_user"] == "me");
        // the name the terminal's New server box works out for /servers
        assert(new_server_name("irc.sekurnet.org 6697 -tls") == "irc.sekurnet.org" && new_server_name("irc.x.org -name sn") == "sn" &&
               new_server_name("-host vhost.example.org irc.x.org -tls") == "irc.x.org" && new_server_name("[2001:db8::1]:6697 -tls") == "2001-db8--1" &&
               new_server_name("wss://irc.x.org/ws") == "irc.x.org" && new_server_name("-tls").empty());
        for (auto nm : {"lg", "lg2", "lg3"}) core.exec(home, std::string("/server del ") + nm);
        remove(core.cfg_path.c_str());
        std::swap(core.cfg_path, path);
    }

    // /server -host: the vhost is kept as server.<name>.vhost (on a known server too), and Conn binds to it before it connects
    {
        std::string path = "build/test-vhost.conf";
        std::swap(core.cfg_path, path);
        core.exec(home, "/server 127.0.0.1 1 -name vh -host 127.0.0.1");
        assert(core.cfg["server.vh"]["vhost"] == "127.0.0.1" && core.server_cfg("vh").vhost == "127.0.0.1");
        core.exec(home, "/server 127.0.0.1 1 -name vh -host vhost.example.org");  // a known one: the new vhost, then the reconnect
        assert(core.cfg["server.vh"]["vhost"] == "vhost.example.org");
        for (auto& sv : core.servers)
            if (sv->cfg.name == "vh") assert(sv->cfg.vhost == "vhost.example.org");
        core.cfg["server.vh"]["vhost"] = "[2001:db8::1]";  // as /server takes an IPv6 address: the lookup takes it bare
        assert(core.server_cfg("vh").vhost == "2001:db8::1");
        core.exec(home, "/server del vh");
        assert(core.server_cfg("vh").vhost.empty());
        remove(core.cfg_path.c_str());
        std::swap(core.cfg_path, path);

        std::string derr;
        int lfd = listen_tcp("127.0.0.1", 0, derr);
        assert(lfd >= 0);
        sockaddr_in la{};
        socklen_t ll = sizeof la;
        getsockname(lfd, (sockaddr*)&la, &ll);
        // connects to the listener (target: where it listens, any family) from `vh`; err = why not, local = the address the connection came from
        auto via = [&](const std::string& target, const std::string& vh, std::string& err, std::string& local, int fam = 0) {
            Conn c;
            c.vhost = vh, c.family = fam;
            assert(c.connect(target, ntohs(la.sin_port), 0, "", ""));
            std::string in;
            bool ok = true;
            // pumped on an event only, like the main loop: a connect still in progress has SO_ERROR 0, which pump() reads as
            // connected (Windows takes seconds to refuse one on loopback, so the refused IPv6 try looked open there)
            for (int i = 0; i < 1500 && ok && c.st != Conn::OPEN; i++) {
                pollfd pf{c.fd, c.events(), 0};
                poll(&pf, 1, 10);
                if (pf.revents) ok = c.pump(pf.revents, in);
            }
            err = c.err, local.clear();
            if (ok && c.st == Conn::OPEN) {
                sockaddr_storage a{};
                socklen_t l = sizeof a;
                char ip[NI_MAXHOST] = "";
                getsockname(c.fd, (sockaddr*)&a, &l);
                getnameinfo((sockaddr*)&a, l, ip, sizeof ip, nullptr, 0, NI_NUMERICHOST);
                local = ip;
            }
            return ok && c.st == Conn::OPEN;
        };
        std::string err, local;
        assert(via("127.0.0.1", "", err, local) && local == "127.0.0.1");
        assert(via("127.0.0.1", "127.0.0.1", err, local) && local == "127.0.0.1");
        // /server -4, -6: only that family is looked up, so an address of the other one is "no IPv6 address", not a failed connect
        assert(via("127.0.0.1", "", err, local, AF_INET) && local == "127.0.0.1");
        assert(!via("127.0.0.1", "", err, local, AF_INET6) && err.rfind("127.0.0.1 has no IPv6 address", 0) == 0);
        assert(!via("127.0.0.1", "::1", err, local, AF_INET) && err.rfind("vhost ::1: ", 0) == 0);  // a vhost of the other family
        assert(!via("127.0.0.1", "::1", err, local) && err.rfind("vhost ::1 is IPv6 only, but 127.0.0.1 has no IPv6 address", 0) == 0);
        assert(!via("127.0.0.1", "::1", err, local) && err.rfind("vhost ::1 is IPv6 only, but 127.0.0.1 has no IPv6 address", 0) == 0);
        assert(!via("127.0.0.1", "192.0.2.1", err, local) && err.rfind("vhost 192.0.2.1: 192.0.2.1 (", 0) == 0 &&
               err.find(": it has to be an address of this machine") != std::string::npos);  // not one of this machine's addresses: which one, and why
        assert(!via("127.0.0.1", "no-such-host.invalid", err, local) && err.rfind("vhost no-such-host.invalid: ", 0) == 0);
        int probe = socket(AF_INET, SOCK_STREAM, 0);  // Linux has all of 127/8, macOS only 127.0.0.1
        sockaddr_in two{};
        two.sin_family = AF_INET, two.sin_addr.s_addr = htonl(0x7f000002);
        if (bind(probe, (sockaddr*)&two, sizeof two) == 0) assert(via("127.0.0.1", "127.0.0.2", err, local) && local == "127.0.0.2");
        close(probe);
        // IPv6 (where the machine has ::1): a v6 vhost to a v6 server, and the families that can't meet
        int l6 = listen_tcp("::1", ntohs(la.sin_port), derr);
        if (l6 >= 0) {
            assert(via("::1", "::1", err, local) && local == "::1");
            assert(via("::1", "", err, local) && local == "::1");
            assert(via("::1", "", err, local, AF_INET6) && local == "::1");
            assert(!via("::1", "", err, local, AF_INET) && err.rfind("::1 has no IPv4 address", 0) == 0);
            assert(!via("::1", "127.0.0.1", err, local) && err.rfind("vhost 127.0.0.1 is IPv4 only, but ::1 has no IPv4 address", 0) == 0);
            addrinfo hs{}, *lh = nullptr;  // a name with both: the vhost's family picks the address, in either order
            hs.ai_socktype = SOCK_STREAM;
            bool both4 = false, both6 = false;
            if (getaddrinfo("localhost", nullptr, &hs, &lh) == 0) {
                for (addrinfo* x = lh; x; x = x->ai_next) both4 |= x->ai_family == AF_INET, both6 |= x->ai_family == AF_INET6;
                freeaddrinfo(lh);
            }
            if (both4 && both6) {
                assert(via("localhost", "127.0.0.1", err, local) && local == "127.0.0.1");
                assert(via("localhost", "::1", err, local) && local == "::1");
                // a vhost name with both families, only the server's IPv4 listening: the IPv6 try is refused, the IPv4 one goes through
                close(l6), l6 = -1;
                assert(via("localhost", "localhost", err, local) && local == "127.0.0.1");
            }
            if (l6 >= 0) close(l6);
        }
        close(lfd);
    }

    // /server -cap -nocap -cmdspeed -cmdmax -noproxy -rawlog: kept as the network's keys, on a known one too, and used by its connection
    {
        std::string path = "build/test-srvopts.conf";
        std::swap(core.cfg_path, path);
        auto srv = [&](const std::string& n) {
            for (auto& sv : core.servers)
                if (sv->cfg.name == n) return sv.get();
            return (Server*)nullptr;
        };
        core.exec(home, "/server 127.0.0.1 1 -name so1");
        assert(srv("so1") && srv("so1")->cfg.cap && !srv("so1")->cfg.noproxy && srv("so1")->conn.out.find("CAP LS 302") != std::string::npos);
        assert(srv("so1")->flood_burst == 5 && srv("so1")->flood_ms == 2000 && !core.find(srv("so1"), "*raw"));
        // a user with spaces would be extra USER parameters (ngIRCd answers 461 and never registers): only its first word goes out
        core.cfg["general"]["user"] = "Ident the ident (user@) part";
        core.exec(home, "/server 127.0.0.1 1 -name so1u");
        assert(srv("so1u") && srv("so1u")->conn.out.find("USER Ident 0 * :") != std::string::npos);
        core.cfg["general"]["user"] = "Ident2; the ident part";  // a comment glued to the value, no space before the ';'
        core.exec(home, "/server 127.0.0.1 1 -name so1w");
        assert(srv("so1w") && srv("so1w")->conn.out.find("USER Ident2 0 * :") != std::string::npos);
        core.cfg["general"]["user"] = " ";
        core.exec(home, "/server 127.0.0.1 1 -name so1v");
        assert(srv("so1v") && srv("so1v")->conn.out.find("USER " + srv("so1v")->nick + " 0 * :") != std::string::npos);  // nothing left: the nick
        core.cfg["general"].erase("user");
        core.exec(home, "/server 127.0.0.1 1 -name so2 -nocap -noproxy -cmdspeed 500 -cmdmax 3 -rawlog");
        Server* t = srv("so2");
        assert(t && !t->cfg.cap && t->cfg.noproxy && core.cfg["server.so2"]["cap"] == "false" && core.cfg["server.so2"]["noproxy"] == "true");
        assert(t->flood_ms == 500 && t->flood_burst == 3 && core.cfg["server.so2"]["flood_interval_ms"] == "500");
        assert(t->conn.out.find("CAP") == std::string::npos && t->conn.out.find("NICK ") != std::string::npos);  // registers without CAP LS
        assert(core.find(t, "*raw"));  // opened before the connection: its first lines are in it
        core.exec(home, "/server 127.0.0.1 1 -name so2 -cap -cmdspeed 0");  // a known one: kept, then the reconnect with CAP
        assert(srv("so2") == t && t->cfg.cap && t->flood_ms == 0 && t->conn.out.find("CAP LS 302") != std::string::npos);
        // -4 and -6: kept as ip_version, handed to the connection, said in "Connecting to", and the last one wins; on a known server too
        core.exec(home, "/server 127.0.0.1 1 -name so7 -6");
        assert(srv("so7") && core.cfg["server.so7"]["ip_version"] == "6" && srv("so7")->cfg.ipv == 6 && srv("so7")->conn.family == AF_INET6);
        bool said = false;
        for (auto& l : srv("so7")->status->lines) said |= l.text.find("(IPv6 only)") != std::string::npos;
        assert(said);
        core.exec(home, "/server 127.0.0.1 1 -name so7 -6 -4");
        assert(core.cfg["server.so7"]["ip_version"] == "4" && srv("so7")->cfg.ipv == 4 && srv("so7")->conn.family == AF_INET);
        core.cfg["server.so7"]["ip_version"] = "auto";
        assert(core.server_cfg("so7").ipv == 0 && bad_value("server.so7", "ip_version", "auto").empty() && bad_value("server.so7", "ip_version", "6").empty());
        assert(!bad_value("server.so7", "ip_version", "5").empty());
        core.exec(home, "/server del so7");
        core.exec(home, "/server 127.0.0.1 1 -name so8 -wsproto Binary");
        assert(core.cfg["server.so8"]["ws_protocol"] == "binary" && srv("so8")->cfg.ws_proto == 2 && srv("so8")->conn.ws_proto == 2);
        assert(bad_value("server.so8", "ws_protocol", "text").empty() && !bad_value("server.so8", "ws_protocol", "blob").empty());
        core.exec(home, "/server del so8");
        core.exec(home, "/server 127.0.0.1 1 -name so9 -wsproto blob");
        assert(!srv("so9"));
        // the options before the address, like irssi's /server -host vhost irc... : same as after it
        core.exec(home, "/server -host 127.0.0.1 -nocap 127.0.0.1 -name so4 2 -tls");
        assert(core.cfg["server.so4"]["host"] == "127.0.0.1" && core.cfg["server.so4"]["port"] == "2" && core.cfg["server.so4"]["vhost"] == "127.0.0.1" &&
               core.cfg["server.so4"]["tls"] == "true" && core.cfg["server.so4"]["cap"] == "false" && !core.cfg.count("server.-host"));
        core.exec(home, "/server -tls 127.0.0.1 -name so5");
        assert(core.cfg["server.so5"]["host"] == "127.0.0.1" && core.cfg["server.so5"]["port"] == "6697");
        core.exec(home, "/server 127.0.0.1 -name so6 -bogus");
        assert(!srv("so6") && !core.cfg.count("server.so6") && home->lines.back().text == "No option -bogus (/help server)");
        for (auto n : {"so4", "so5"}) core.exec(home, std::string("/server del ") + n);
        for (auto bad : {"-cmdspeed x", "-cmdspeed -5", "-cmdmax 0", "-cmdmax 1.5", "-cmdmax"}) {  // nothing is made
            core.exec(home, std::string("/server 127.0.0.1 1 -name so3 ") + bad);
            assert(!srv("so3") && !core.cfg.count("server.so3"));
        }
        for (auto n : {"so1", "so2"}) core.exec(home, std::string("/server del ") + n);
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

    // conf_check: the template has no problems; each kind of mistake is found on its line, and only once
    {
        char d[] = "/tmp/isekur-cc-XXXXXX";
        assert(mkdtemp(d));
        std::string p = std::string(d) + "/c.conf";
        std::ofstream(p) << fresh_config();  // what a first start writes
        assert(conf_check(p).empty());
        std::ofstream(p) << "x = 1\n[general]\nautoconect = on\nmouse = maybe\nscrollback = lots\ntimestamp_align = middle\n"
                            "host = h\nmouse = on\njunk\n[server.a\n[server.ok]\ntls = on\nport = 66x7\nsasl_pass = enc:AAAA\n"
                            "[web]\nprot = 1\n[weird]\na = b\n[plugin.x]\nany = 1\n; comment = x\n";
        auto r = conf_check(p);
        auto has = [&](int n, const std::string& w) {
            for (auto& l : r) if (l.rfind("line " + std::to_string(n) + ":", 0) == 0 && l.find(w) != std::string::npos) return true;
            return false;
        };
        assert(has(1, "outside") && has(3, "did you mean autoconnect") && has(4, "on or off") && has(5, "number") && has(6, "left, right"));
        assert(has(7, "goes in a [server") && has(8, "set again") && has(9, "not a key") && has(10, "no closing ]"));
        assert(has(13, "number") && has(14, "decrypted") && has(16, "did you mean port") && has(17, "unknown section"));
        assert(r.size() == 13);  // tls = on, [plugin.x] and the comment are fine
        std::ofstream(p) << "[general]\ntab_bar_width = 99\nnick_width = wide\nlog_keep = 2w\nlog_keep = soon\n";
        r = conf_check(p);
        assert(has(2, "10-60") && has(3, "number") && !has(4, "takes") && has(5, "2w, 6m, 1y"));
        std::ofstream(p) << "[general]\ntab_bar = left\n";  // renamed to tab_bar_position: the old name says so
        r = conf_check(p);
        assert(r.size() == 1 && has(2, "tab_bar_position"));
        std::filesystem::remove_all(d);
    }

    // /server rename: the section in the file (comments kept), the tab, unsaved keys, server_order and the logs folder
    {
        assert(ini_rename("[a]\n# note\nk = 1\n[b]\n", "a", "z") == "[z]\n# note\nk = 1\n[b]\n");
        std::string path = "build/test-rename.conf", logs = "build/test-rename-logs";
        std::filesystem::remove_all(logs);
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
        std::filesystem::remove_all(logs);
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
    assert(sent() == "PART #c :iSekur IRC: v" ISEKUR_VERSION "\r\n");
    assert(core.bye(ts, "quit_message") == "iSekur IRC: v" ISEKUR_VERSION);
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
    {  // on connect: umodes, then perform (in order, $nick), then autojoin; +x with a login waits for 396
        assert((perform_lines(" /msg a b && msg c d &&") == std::vector<std::string>{"/msg a b", "/msg c d"}) && perform_join({"/a", "/b"}) == "/a && /b");
        assert(perform_show("/msg x@channels.undernet.org login me pw && /msg Bot ident pw2 me && /join #a") ==
               "/msg x@channels.undernet.org login me ******** && /msg Bot ident ******** me && /join #a");
        assert(mask_services("#c", "login me pw") == "login me pw" && mask_services("Q@CServe.quakenet.org", "AUTH me pw") == "AUTH me ********");
        assert(raw_redact("PRIVMSG x@channels.undernet.org :login me pw") == "PRIVMSG x@channels.undernet.org :login me ***");
        assert(input_secret("#c", "/perform add /msg Bot ident pw") == 28 && input_secret("#c", "/perform set 2 /msg Q AUTH me pw") == 30);
        assert(input_secret("#c", "/oper me opw") == 9 && input_secret("#c", "/oper me") == std::string::npos && perform_show("/oper me opw") == "/oper me ********");
        assert(umode_adds("+i-w+x", 'x') && !umode_adds("+i-x", 'x') && umode_adds("ix", 'x'));
        assert(cfg_secret("perform") && !secret_key("perform"));
        ts->sasl_ok = false, ts->cfg.nickserv_pass.clear();
        ts->cfg.umodes = "+ix", ts->cfg.perform = {"/msg x@channels.undernet.org login $nick pw", "/quote PING :p"};
        feed(":srv 001 me :Welcome");
        std::string o = sent();
        assert(o.rfind("MODE me +ix\r\nPRIVMSG x@channels.undernet.org :login me pw\r\nPING :p\r\n", 0) == 0 && o.find("JOIN") == std::string::npos && ts->join_hold);
        feed(":me!u@h PRIVMSG x@channels.undernet.org :login me pw");  // its echo: masked, in the server tab, no query
        assert(!core.find(ts, "x@channels.undernet.org") && ts->status->lines.back().text == "login me ********");
        feed(":srv 396 me hidden.users.undernet.org :is now your hidden host");
        assert(!ts->join_hold && sent().rfind("JOIN #c\r\n", 0) == 0);
        feed(":srv 001 me :Welcome"), sent();  // no 396: joined 10s on
        core.tick(time(nullptr) + 11);
        assert(!ts->join_hold && sent().find("JOIN #c\r\n") != std::string::npos);
        ts->cfg.join_wait = false;
        feed(":srv 001 me :Welcome");
        assert(!ts->join_hold && sent().find("JOIN #c\r\n") != std::string::npos);
        {  // the private chats still open ask the server for what came in meanwhile, after the JOINs
            int fl = ts->flood_ms;
            ts->flood_ms = 0, ts->caps.insert("draft/chathistory");
            Buffer* qa = core.open(ts, BT::QUERY, "Pal");
            core.add(qa, Line{1000, LK::MSG, "Pal", "before"});
            Buffer* qb = core.open(ts, BT::QUERY, "Bud");
            Buffer* qs = core.open(ts, BT::QUERY, "*snotices");
            sent(), feed(":srv 001 me :Welcome");
            std::string o2 = sent();
            assert(o2.find("JOIN #c\r\n") < o2.find("CHATHISTORY AFTER Pal timestamp=1970-01-01T00:16:40.000Z 100\r\n"));
            assert(o2.find("CHATHISTORY LATEST Bud * 100\r\n") != std::string::npos && o2.find("*snotices") == std::string::npos);
            core.cfg["general"]["history_fetch"] = "0";
            feed(":srv 001 me :Welcome");
            assert(sent().find("CHATHISTORY") == std::string::npos);
            // what comes back (theirs, and ours from another client) lands in the open tab as old news
            qa->unread = qa->activity = 0;  // as after a restart: nothing missed yet
            feed(":srv BATCH +hq chathistory Pal");
            feed("@batch=hq;time=2026-11-18T10:00:00.000Z :Pal!u@h PRIVMSG me :while away");
            feed("@batch=hq;time=2026-11-18T10:01:00.000Z :me!u@h PRIVMSG Pal :answered elsewhere");
            feed(":srv BATCH -hq");
            assert(qa->lines.size() == 3 && qa->lines[1].text == "while away" && !qa->lines[1].self && qa->lines[2].self);
            assert(!qa->unread && !qa->activity);
            assert(!qa->lines[1].hilight);
            assert(!core.find(ts, "me"));
            core.cfg["general"].erase("history_fetch"), ts->caps.erase("draft/chathistory"), ts->flood_ms = fl;
            core.close(qa), core.close(qb), core.close(qs);
        }
        ts->cfg.umodes.clear(), ts->cfg.perform.clear(), ts->cfg.join_wait = true;
        // /perform: the list in the config at once, and the echo of a login is masked
        std::string path = "build/test-perform.conf";
        bool had_key = access("build/secret.key", F_OK) == 0;
        remove(path.c_str());
        std::swap(core.cfg_path, path);
        core.exec(chb, "/perform add /msg MyBot ident hunter9");
        core.exec(chb, "/perform add join #extra");
        core.exec(chb, "/perform move 2 1");
        assert((ts->cfg.perform == std::vector<std::string>{"/join #extra", "/msg MyBot ident hunter9"}));
        Ini disk;
        assert(ini_load(core.cfg_path, disk) && ini_get(disk, "server.t", "perform") == "/join #extra && /msg MyBot ident hunter9");
        std::ifstream raw(core.cfg_path);
        std::string file((std::istreambuf_iterator<char>(raw)), {});
        assert(file.find("hunter9") == std::string::npos && file.find("perform = enc:") != std::string::npos);  // sealed on disk
        core.exec(chb, "/perform del 1");
        assert(ts->cfg.perform.size() == 1 && chb->lines.back().text.find("hunter9") == std::string::npos);
        core.exec(chb, "/perform");
        assert(chb->lines.back().text == "  1. /msg MyBot ident ********");
        core.exec(chb, "/perform del 1");
        {  // one perform = line per command; only a line with a password is sealed, and a list sealed whole (0.5.2) opens up again
            std::string err;
            auto text = [&]() { std::ifstream f(core.cfg_path); return std::string((std::istreambuf_iterator<char>(f)), {}); };
            assert(!perform_secret("/mode me +T && /msg me test test!") && perform_secret("/join #a && /msg Q AUTH me pw"));
            assert(!cfg_seal("perform", "/mode me +T") && cfg_seal("perform", "/oper me pw") && cfg_seal("sasl_pass", "x"));
            Ini plain;
            plain["server.t"]["perform"] = "/mode me +T && /msg me test test!";
            assert(ini_save(core.cfg_path, plain, err) && text().find("perform = /mode me +T\nperform = /msg me test test!\n") != std::string::npos);
            plain["server.t"]["perform"] = "/mode me +T && /msg Bot ident pw";
            assert(ini_save(core.cfg_path, plain, err) && text().find("perform = /mode me +T\nperform = enc:") != std::string::npos &&
                   text().find("ident pw") == std::string::npos && text().find("test test!") == std::string::npos);  // the old lines collapse into the new ones
            Ini two;
            assert(ini_load(core.cfg_path, two) && ini_get(two, "server.t", "perform") == "/mode me +T && /msg Bot ident pw");
            assert((ini_parse("[s]\nperform = /a\nperform =\nperform = /b ; note\n")["s"]["perform"] == "/a && /b"));
            // what the last build wrote for a perform without a password (Koragg's): readable again on load, in the file too
            std::ifstream kf("build/secret.key", std::ios::binary);
            std::string key((std::istreambuf_iterator<char>(kf)), {});
            assert(key.size() == 32);
            Ini old;
            old["server.t"]["perform"] = "enc:" + e2e_seal(key, "/mode me +T && /msg me test test!");
            std::ofstream(core.cfg_path) << "[server.t]\nperform = " << old["server.t"]["perform"] << "\n";
            Ini back;
            assert(ini_load(core.cfg_path, back) && ini_get(back, "server.t", "perform") == "/mode me +T && /msg me test test!");
            assert(text().find("perform = /mode me +T\nperform = /msg me test test!\n") != std::string::npos && text().find("enc:") == std::string::npos);
        }
        std::swap(core.cfg_path, path);
        remove(path.c_str());
        if (!had_key) remove("build/secret.key");
        core.cfg["server.t"].erase("perform"), core.unsaved.erase("server.t");
        sent();
    }
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
    wb = chb->lines.size();  // 275 (SSL) and the rest are framed too, with or without a nick parameter; the key values are bold
    feed(":srv 311 me Eth ~e h.example * :Eth");
    feed(":srv 275 me Eth :is using a secure connection");
    feed(":srv 275 me :Eth is connected via SSL (secure link)");
    feed(":srv 308 me Eth :is a Services Administrator");
    feed(":srv 312 me Eth kinetic.example :London");
    feed(":srv 318 me Eth :End of /WHOIS list.");
    assert(chb->lines.size() - wb == 7);
    assert(mirc_strip(chb->lines[wb + 2].text) == "│ is using a secure connection" && chb->lines[wb + 2].text.find("\x02is using") != std::string::npos);
    assert(mirc_strip(chb->lines[wb + 3].text) == "│ is connected via SSL (secure link)");
    assert(mirc_strip(chb->lines[wb + 4].text) == "│ is a Services Administrator");
    assert(mirc_strip(chb->lines[wb + 5].text) == "│ server kinetic.example (London)" && chb->lines[wb + 5].text.find("\x02kinetic.example\x02") != std::string::npos);
    wb = chb->lines.size();  // a realname of raw bytes, not UTF-8 (anyone can set one): framed, no endless loop
    feed(":srv 311 me Raw r h * :" + std::string(200, '\xa1'));
    feed(":srv 318 me Raw :End of /WHOIS list.");
    assert(chb->lines.size() - wb >= 4 && mirc_strip(chb->lines.back().text) == "└─────────");

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

    {  // /quiet: off | smart | all; alone it toggles off <-> all; kicks are never noise
        core.exec(chb, "/quiet smart");
        assert(chb->smart && !chb->quiet);
        feed(":lurk!u@h JOIN #c");
        assert(chb->lines.back().kind == LK::JOIN && chb->lines.back().idle && quieted(*chb, chb->lines.back()));
        feed(":talk!u@h JOIN #c");
        feed(":talk!u@h PRIVMSG #c :hi all");
        assert(!quieted(*chb, chb->lines.back()));
        feed(":talk!u@h NICK talk2");  // spoke under the old name: shown, and the new name inherits it
        assert(chb->lines.back().kind == LK::NICK && !chb->lines.back().idle);
        feed(":talk2!u@h QUIT :bye");
        assert(chb->lines.back().kind == LK::QUIT && !chb->lines.back().idle && !quieted(*chb, chb->lines.back()));
        feed(":lurk!u@h QUIT :bye");
        assert(chb->lines.back().idle && quieted(*chb, chb->lines.back()));
        feed("@time=2020-01-01T00:00:00.000Z :old!u@h PRIVMSG #c :hello");  // 4 minutes, then 6: past the window
        feed("@time=2020-01-01T00:04:00.000Z :old!u@h PART #c");
        assert(!chb->lines.back().idle);
        feed("@time=2020-01-01T00:06:00.000Z :old!u@h JOIN #c");
        assert(chb->lines.back().idle);
        feed(":op!u@h KICK #c victim :out");  // a kick shows in every mode
        assert(chb->lines.back().text.find("victim was kicked by op") != std::string::npos && !noise(chb->lines.back()));
        core.exec(chb, "/quiet all");
        assert(chb->quiet && !chb->smart && !quieted(*chb, chb->lines.back()));
        feed(":talk3!u@h JOIN #c");
        assert(quieted(*chb, chb->lines.back()));
        core.exec(chb, "/quiet");
        assert(!chb->quiet && !chb->smart);
        core.exec(chb, "/quiet");
        assert(chb->quiet && !chb->smart);
        core.exec(chb, "/quiet smart");
        core.exec(chb, "/quiet");  // smart toggles to off
        assert(!chb->quiet && !chb->smart);
        core.exec(chb, "/quiet bogus");
        assert(chb->lines.back().kind == LK::ERROR && !chb->quiet);
        core.exec(chb, "/set join_filter smart");  // the default of a channel tab opened later
        feed(":me!u@h JOIN #smartc");
        assert(core.find(ts, "#smartc")->smart && !core.find(ts, "#smartc")->quiet);
        core.exec(chb, "/set join_filter all");
        feed(":me!u@h JOIN #allc");
        assert(core.find(ts, "#allc")->quiet && !core.find(ts, "#allc")->smart);
        core.exec(chb, "/set join_filter off");
        feed(":me!u@h JOIN #offc");
        assert(!core.find(ts, "#offc")->quiet && !core.find(ts, "#offc")->smart);
        for (const char* c : {"#smartc", "#allc", "#offc"}) core.close(core.find(ts, c));
    }
    int flood_was = ts->flood_ms;  // the next blocks read what went out: no flood limit, and nothing left in the queue
    ts->flood_ms = 0, ts->sendq.clear();
    {  // /cycle: part and join again, with the key we know
        sent();
        core.exec(chb, "/cycle off for a bit");
        assert(sent() == "PART #c :off for a bit\r\nJOIN #c\r\n");
        ts->chan_keys["#c"] = "s3";
        core.exec(chb, "/cycle #c");
        assert(sent().find("JOIN #c s3\r\n") != std::string::npos);
        ts->chan_keys.erase("#c");
        core.exec(core.find(ts, "bob") ? core.find(ts, "bob") : ts->status, "/cycle");
        assert(sent().empty());  // not a channel
    }
    {  // invite_join: off by default; on joins what we're invited to, once
        sent();
        feed(":op!u@h INVITE me #inv");
        assert(sent().empty());
        core.exec(chb, "/set invite_join on");
        ts->flood_ms = 0;  // /set applies the server's flood settings again
        feed(":op!u@h INVITE me #inv");
        assert(sent() == "JOIN #inv\r\n");
        feed(":op!u@h INVITE someone #inv");  // invite-notify: for somebody else
        feed(":op!u@h INVITE me #c");          // a channel we're in
        assert(sent().empty());
        core.exec(chb, "/set invite_join off");
    }
    {  // draft/pre-away: AWAY * once the cap is on and nobody is attached, cleared when somebody attaches
        core.exec(chb, "/set pre_away on");
        ts->flood_ms = 0;
        core.attached = false, ts->state = SS::REGISTERING, ts->cap_done = false, sent();
        feed(":srv CAP * ACK :draft/pre-away");
        assert(sent().find("AWAY *\r\n") != std::string::npos && ts->pre_away);
        ts->state = SS::CONNECTED, ts->cap_done = true, sent();
        feed(":srv 306 me :You have been marked as being away");
        assert(ts->away && ts->pre_away);
        core.set_attached(true);
        assert(sent() == "AWAY\r\n" && !ts->pre_away);
        feed(":srv 305 me :You are no longer marked as being away");
        // autoaway (or /away) set its own away while detached: attaching leaves that to its owner
        core.attached = false, ts->pre_away = true;
        ts->send("AWAY :Not here right now");
        assert(!ts->pre_away);
        core.set_attached(true);
        assert(sent() == "AWAY :Not here right now\r\n");
        core.exec(chb, "/set pre_away off");
        ts->flood_ms = 0;
        core.attached = false, ts->state = SS::REGISTERING, ts->caps.erase("draft/pre-away"), sent();  // off: nothing sent
        feed(":srv CAP * ACK :draft/pre-away");
        assert(sent().find("AWAY") == std::string::npos && !ts->pre_away);
        core.set_attached(true), ts->state = SS::CONNECTED, ts->cap_done = true, ts->caps.erase("draft/pre-away"), sent();
    }
    ts->flood_ms = flood_was, ts->tokens = ts->flood_burst, ts->tokens_at = now_ms(), ts->sendq.clear(), sent();  // as the next tests found it

    // /set takes only keys iSekur reads, in the section that reads them; [plugin.*] are the plugins'
    for (const char* bad : {"/set gera asdasd", "/set cpu off", "/set host x", "/set server.t.theme x", "/set web.nope 1", "/set keys.nope C-x", "/set foo.bar 1"}) {
        core.exec(chb, bad);
        assert(chb->lines.back().kind == LK::ERROR);
    }
    assert(!ini_get(core.cfg, "general", "gera").size() && !core.unsaved["general"].count("gera"));
    core.exec(chb, "/set keys.find_tab C-g"), assert(chb->lines.back().kind != LK::ERROR);
    core.exec(chb, "/set web.port 8443"), assert(chb->lines.back().kind != LK::ERROR);
    core.exec(chb, "/set plugin.x.foo 1"), assert(chb->lines.back().kind != LK::ERROR);

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
        {  // open queries are remembered too, and a server added with them reopens them
            Buffer* q = core.open(ts, BT::QUERY, "Pal");
            core.save_history();
            assert(ini_get(core.cfg, "server." + ts->cfg.name, "open_queries") == "Pal");
            core.close(q);
            core.save_history();
            assert(ini_get(core.cfg, "server." + ts->cfg.name, "open_queries").empty());
            core.cfg["server.qtest"]["open_queries"] = "Pal Bud";
            ServerCfg qc = ts->cfg;
            qc.name = "qtest";
            Server* qs = core.add_server(qc);
            assert(core.find(qs, "Pal") && core.find(qs, "Bud") && core.find(qs, "Bud")->type == BT::QUERY);
            core.close(qs->status);
            core.cfg.erase("server.qtest");
        }
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
        // theme = takes the file name too: "dracula.theme" loads dracula, is kept as "dracula", and checks clean
        core.exec(home, "/set theme dracula.theme");
        assert(core.theme.name == "dracula" && ini_get(core.cfg, "general", "theme") == "dracula");
        {
            Theme t; std::string e;
            assert(load_theme("dracula.theme", t, e) && t.name == "dracula" && theme_name("x.theme") == "x" && theme_name(".theme") == ".theme");
        }
        core.exec(home, "/set theme \"\"");
        // every /set key takes "" and 0 without breaking what reads it; port 0 is the usual port
        {
            Ini keep = core.cfg;
            auto keep_unsaved = core.unsaved;
            std::string sv = "server." + ts->cfg.name;
            for (auto& k : setting_keys()) {
                std::string_view key = k.key;
                if (key == "name" || key == "lock" || key == "exec_web" || key == "lock_password_hash") continue;
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
            assert(inh && inh->shared && inh->val.empty() && inh->def == "77" && inh->cat == sv + "/Logs" && inh->unit == "lines");
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
            // an empty template default the code fills in says what it is (log_dir's folder, the quit text), not "(not set)"
            bool ldrow = false, qrow = false;
            for (auto& r : core.setting_rows()) {
                ldrow |= r.sec == "general" && r.key == "log_dir" && r.def == core.cfg_path.substr(0, core.cfg_path.rfind('/') + 1) + "logs";
                qrow |= r.sec == "general" && r.key == "quit_message" && r.def.rfind("iSekur IRC: v", 0) == 0;
            }
            assert(ldrow && qrow);
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
        core.select(home), core.notify(Ev::THEME, nullptr);  // redrawn while another tab is shown: not activity
        assert(!g->lines.empty() && !g->activity && !g->unread);
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
        assert(th.nicks.size() >= (n.rfind("mirc", 0) == 0 || n.rfind("mono", 0) == 0 || n == "nocolor" ? 1u : 6u));  // mirc, mono, nocolor: one nick color
        // a tab with activity (data, messages, a mention) is plainly another color than a quiet one, in every theme
        for (const char* k : {"tab_data", "tab_msg", "tab_hilight"})
            if (th.get("tabbar").fg >= 0 && th.get(k).fg >= 0) assert(color_distance(th.get(k).fg, th.get("tabbar").fg) >= 24.5);
        // own_nick_color: yours is a color none of the nick colors is (and clearly apart from each), unless nobody's are colored
        assert((th.nicks.size() < 2) == (th.self_nick < 0));
        for (RGB c : th.nicks) {
            if (th.self_nick < 0) break;
            int dr = (c >> 16 & 255) - (th.self_nick >> 16 & 255), dg = (c >> 8 & 255) - (th.self_nick >> 8 & 255), db = (c & 255) - (th.self_nick & 255);
            assert(dr * dr + dg * dg + db * db >= 30 * 30);
        }
    }
    {  // link_style: a colored link differs from the chat's text (underlined only under the pointer); the gray themes underline always; a theme's own `link` wins
        Theme th;
        std::string terr;
        for (const char* n : {"default", "dracula", "solarized-light", "github-dark", "contrast-light"}) {
            assert(load_theme(n, th, terr));
            Style l = link_style(th), d = th.get("default");
            assert(!(l.attr & A_UNDER) && l.fg >= 0 && (d.fg < 0 || color_distance(l.fg, d.fg) >= 10));
        }
        for (const char* n : {"mono", "mono-light"}) assert(load_theme(n, th, terr) && (link_style(th).attr & A_UNDER));
        th.el["link"] = Style{0x123456, -1, A_BOLD};
        assert(link_style(th).fg == 0x123456 && !(link_style(th).attr & A_UNDER));
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
    {  // NO_COLOR: the terminal draws nocolor for the default theme; a chosen theme and the webchat keep their colors
        std::string terr, was = core.theme.name;
        assert(core.use_theme("default", terr));
        setenv("NO_COLOR", "", 1), core.no_color_check();  // empty: as unset
        assert(!core.no_color && core.look(false).name == "default");
        setenv("NO_COLOR", "0", 1), core.no_color_check();  // any value
        assert(core.look(false).name == "nocolor" && core.look(true).name == "default" && core.theme.name == "default");
        const Theme& p = core.look(false);
        for (auto& [k, st] : p.el)
            if (st.fg >= 0 || st.bg >= 0) fprintf(stderr, "nocolor.theme colors %s\n", k.c_str()), assert(false);
        for (Style st : {note_style(p), plugin_style(p), link_style(p), fold_style(p)}) assert(st.fg < 0 && st.bg < 0);
        assert((note_style(p).attr & A_REV) && (link_style(p).attr & A_UNDER) && p.self_nick < 0 && p.lay("mirc_colors", "") == "off");
        for (RGB c : p.nicks) assert(c < 0);
        assert(core.use_theme("dracula", terr) && core.look(false).name == "dracula");
        unsetenv("NO_COLOR"), core.no_color_check();
        assert(core.use_theme(was, terr) && !core.no_color);
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
    core.exec(lb, "HI");  // typed in *channels: filters by name or topic, any case
    assert(lb->lines.size() == 4 && mirc_strip(lb->lines[2].text).rfind(" 1.  #small", 0) == 0 && mirc_strip(lb->lines[0].text).find("matching HI") != std::string::npos);
    core.exec(lb, "nope");
    assert(lb->lines.size() == 2);
    core.exec(lb, "/list filter");  // all again
    assert(lb->lines.size() == 5);
    core.exec(lb, "/list filter");  // nothing on (Esc again): no redraw, no error
    assert(lb->lines.size() == 5 && lb->lines.back().kind != LK::ERROR);
    {  // its tab menu: no nick behind it, so no CTCP or ignore
        auto tm = core.tab_menu(lb);
        assert(!tm.empty() && tm.front().second == "/list" && tm.back().second == "/close");
        for (auto& it : tm) assert(it.first.find("CTCP") == std::string::npos && it.first != "Ignore");
    }
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

    {  // wss://host/path as a server's host; and IRC over WebSocket through Conn, against a server written here
        core.cfg["server.w"]["host"] = "wss://irc.sekurnet.org:8443/ws";
        ServerCfg wc = core.server_cfg("w");
        assert(wc.host == "irc.sekurnet.org" && wc.port == 8443 && wc.tls && wc.ws == "/ws");
        core.cfg["server.w"]["host"] = "ws://irc.sekurnet.org";
        wc = core.server_cfg("w");
        assert(wc.port == 80 && !wc.tls && wc.ws == "/");
        core.cfg["server.w"]["host"] = "irc.sekurnet.org";
        wc = core.server_cfg("w");
        assert(wc.ws.empty() && wc.port == 6667);

        std::string derr;
        int lfd = listen_tcp("127.0.0.1", 0, derr);
        assert(lfd >= 0);
        sockaddr_in la{};
        socklen_t ll = sizeof la;
        getsockname(lfd, (sockaddr*)&la, &ll);
        Conn c;
        c.ws = "/irc";
        assert(c.connect("127.0.0.1", ntohs(la.sin_port), 0, "", ""));
        c.out = "NICK me\r\nUSER me 0 * :me\r\n";  // queued before the upgrade: waits for it
        int peer = -1;
        std::string in, said;  // in: the lines iSekur got; said: raw bytes the server got
        bool ok = true;
        auto step = [&] {
            std::vector<pollfd> pf{{c.fd, c.events(), 0}};
            poll(pf.data(), 1, 10);
            ok = c.pump(pf[0].revents, in);
            if (peer < 0 && (peer = accept(lfd, nullptr, nullptr)) >= 0) fcntl(peer, F_SETFL, O_NONBLOCK);
            char b[4096];
            ssize_t n = peer < 0 ? -1 : read(peer, b, sizeof b);
            if (n > 0) said.append(b, (size_t)n);
        };
        for (int i = 0; i < 500 && said.find("\r\n\r\n") == std::string::npos; i++) step();
        size_t he = said.find("\r\n\r\n");
        assert(he != std::string::npos && he + 4 == said.size());  // the request, and not a byte of IRC before the 101
        assert(said.rfind("GET /irc HTTP/1.1\r\nHost: 127.0.0.1:", 0) == 0 && said.find("Sec-WebSocket-Protocol: text.ircv3.net") != std::string::npos);
        size_t ks = said.find("Sec-WebSocket-Key: ") + 19;
        std::string k2 = said.substr(ks, said.find("\r\n", ks) - ks) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        unsigned char sha[20];
        SHA1((const unsigned char*)k2.data(), k2.size(), sha);
        std::string f1 = ws_frame("ab"), f2 = ws_frame("cd");
        f1[0] = 0x01, f2[0] = (char)0x80;  // text "ab" without FIN, then its continuation
        std::string reply = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                            b64(std::string((char*)sha, 20)) + "\r\nSec-WebSocket-Protocol: text.ircv3.net\r\n\r\n" +
                            ws_frame("PING :z") + ws_frame("pingdata", 9) + f1 + f2;  // lines right behind the headers
        said.clear();
        assert(write(peer, reply.data(), reply.size()) == (ssize_t)reply.size());
        for (int i = 0; i < 500 && (in.size() < 13 || said.size() < 60); i++) step();
        assert(ok && in == "PING :z\nabcd\n");
        std::vector<std::string> msgs;
        for (size_t at = 0; at < said.size();) {  // what iSekur sent: masked frames, one per line, no \r\n; the pong (queued on the read) first
            int op;
            std::string p;
            long n = ws_parse(said.substr(at), op, p);
            if (n <= 0) break;
            msgs.push_back(std::to_string(op) + p), at += (size_t)n;
        }
        assert(msgs == (std::vector<std::string>{"10pingdata", "1NICK me", "1USER me 0 * :me"}));
        c.out += "PRIVMSG #a :hi\r\n";
        said.clear();
        for (int i = 0; i < 500 && said.empty(); i++) step();
        int op;
        std::string p;
        assert(ws_parse(said, op, p) == (long)said.size() && op == 1 && p == "PRIVMSG #a :hi");
        std::string closeframe = ws_frame("", 8);
        assert(write(peer, closeframe.data(), closeframe.size()) == (ssize_t)closeframe.size());
        for (int i = 0; i < 500 && ok; i++) step();
        assert(!ok && c.err.find("WebSocket closed") != std::string::npos);
        c.close();
        close(peer);

        // ws_proto: only that subprotocol is offered, and its frame type is sent even if the server names none
        for (int want : {1, 2}) {
            c.ws_proto = want;
            assert(c.connect("127.0.0.1", ntohs(la.sin_port), 0, "", ""));
            c.out = "NICK me\r\n";
            peer = -1, ok = true, said.clear();
            for (int i = 0; i < 500 && said.find("\r\n\r\n") == std::string::npos; i++) step();
            assert(said.find(std::string("Sec-WebSocket-Protocol: ") + (want == 1 ? "text" : "binary") + ".ircv3.net\r\n") != std::string::npos);
            ks = said.find("Sec-WebSocket-Key: ") + 19;
            k2 = said.substr(ks, said.find("\r\n", ks) - ks) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
            SHA1((const unsigned char*)k2.data(), k2.size(), sha);
            reply = "HTTP/1.1 101 Switching Protocols\r\nSec-WebSocket-Accept: " + b64(std::string((char*)sha, 20)) + "\r\n\r\n";
            said.clear();
            assert(write(peer, reply.data(), reply.size()) == (ssize_t)reply.size());
            for (int i = 0; i < 500 && said.empty(); i++) step();
            assert(ws_parse(said, op, p) == (long)said.size() && op == want && p == "NICK me");
            c.close();
            close(peer);
        }
        close(lfd);
    }

    {  // /dcc: an offer, then a real download from a loopback sender
        std::string derr;
        int lfd = listen_tcp("127.0.0.1", 0, derr);
        assert(lfd >= 0);
        sockaddr_in la{};
        socklen_t ll = sizeof la;
        getsockname(lfd, (sockaddr*)&la, &ll);
        std::string port = std::to_string(ntohs(la.sin_port));
        std::filesystem::remove_all("build/dcctest");
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
        feed(":bob!u@h PRIVMSG me :\x01" "DCC SEND w.txt 2130706433 4294967297 5\x01");  // atoi made that port 1
        assert(core.dccs.size() == 1 && nb->lines.back().text.rfind("Bad DCC SEND", 0) == 0);
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

        {  // the network's vhost: a download connects from it (an address that isn't ours: it can't, and says so)
            std::string vp = "build/dcctest/v.txt";
            int vl = listen_tcp("127.0.0.1", 0, derr);
            ll = sizeof la, getsockname(vl, (sockaddr*)&la, &ll);
            ts->cfg.vhost = "192.0.2.1";
            feed(":bob!u@h PRIVMSG me :\x01" "DCC SEND v.txt 2130706433 " + std::to_string(ntohs(la.sin_port)) + " 5\x01");
            core.exec(chb, "/dcc get bob");
            Dcc* vd = core.dccs.back().get();
            for (int i = 0; i < 500 && vd->st == Dcc::RUN; i++) {
                std::vector<pollfd> pf{{vd->conn.fd, vd->conn.events(), 0}};
                poll(pf.data(), 1, 10);
                core.dccs_poll(pf, 0);
            }
            assert(vd->st == Dcc::FAILED && vd->err.rfind("vhost 192.0.2.1: ", 0) == 0);
            ts->cfg.vhost.clear(), close(vl);
            core.dccs.pop_back();
            unlink(vp.c_str());
        }

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
        fclose(fopen("build/dcctest/.r.txt.part", "wb"));  // what a broken download of ours leaves beside it
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
        assert(access("build/dcctest/.r.txt.part", F_OK) != 0);  // whole: the mark is gone
        {   // a file that merely has the name isn't a broken download: left alone, the new one takes another name
            Dcc* keep = d;
            f = fopen("build/dcctest/u.txt", "wb"), fputs("abc", f), fclose(f);
            feed(":bob!u@h PRIVMSG me :\x01" "DCC SEND u.txt 2130706433 9 11\x01");
            sent();
            core.exec(chb, "/dcc get");
            d = core.dccs.back().get();
            assert(d->path == "build/dcctest/u (1).txt" && d->pos == 0 && access("build/dcctest/.u (1).txt.part", F_OK) == 0);
            std::string id = std::to_string(d->id);
            core.exec(chb, "/dcc close " + id);  // cancelled before a byte came: nothing of it stays
            assert(access("build/dcctest/u (1).txt", F_OK) != 0 && access("build/dcctest/.u (1).txt.part", F_OK) != 0);
            core.exec(chb, "/dcc close " + id);  // off the list
            f = fopen("build/dcctest/u.txt", "rb");
            memset(fb, 0, sizeof fb);
            assert(fread(fb, 1, sizeof fb, f) == 3 && std::string(fb) == "abc");
            fclose(f);
            // cancelling a resume nobody answered keeps the part it was going to continue
            f = fopen("build/dcctest/p.txt", "wb"), fputs("hello", f), fclose(f), fclose(fopen("build/dcctest/.p.txt.part", "wb"));
            feed(":bob!u@h PRIVMSG me :\x01" "DCC SEND p.txt 2130706433 9 11\x01");
            sent();
            core.exec(chb, "/dcc get");
            d = core.dccs.back().get();
            assert(d->st == Dcc::RESUME && d->pos == 5);
            id = std::to_string(d->id);
            core.exec(chb, "/dcc close " + id);
            assert(access("build/dcctest/p.txt", F_OK) == 0 && access("build/dcctest/.p.txt.part", F_OK) == 0);
            core.exec(chb, "/dcc close " + id);
            sent();  // the RESUME it asked for
            d = keep;
        }

        // /dcc send: a name alone is in dcc_dir; we listen, they connect, read it all and ack it
        core.cfg["general"]["dcc_ip"] = "127.0.0.1";
        core.exec(chb, "/dcc send bob nope.txt");
        assert(core.dccs.back().get() == d && sent().empty());  // no such file: nothing offered
        core.cfg["general"]["dcc_ports"] = "5000-";  // half a range: said, not "no free port"
        core.exec(chb, "/dcc send bob r.txt");
        assert(core.dccs.back().get() == d && chb->lines.back().text.find("isn't a port or a range") != std::string::npos);
        core.cfg["general"].erase("dcc_ports");
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

    {  // the BNC: user/network:password, a welcome made from iSekur's state, lines both ways, plain IRC out
        core.cfg["bnc"] = {{"bind", "127.0.0.1"}, {"port", "18797"}, {"tls", "off"}, {"user", "u"}, {"password_hash", hash_password("pw")}};
        std::string nick0 = ts->nick, berr;
        ts->nick = "me", chb->joined = true, chb->topic = "the topic";
        chb->nicks["bob"] = NickEntry{"bob", "@+", false, "", "", "", false};
        core.add(chb, Line{time(nullptr), LK::MSG, "bob", "earlier"});
        Bnc bnc(core);
        assert(bnc.start(berr) && bnc.url == "irc://127.0.0.1:18797" && !bnc.attached());
        auto dial = [] {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in a{};
            a.sin_family = AF_INET, a.sin_port = htons(18797), a.sin_addr.s_addr = htonl(0x7f000001);
            assert(connect(fd, (sockaddr*)&a, sizeof a) == 0);
            return fd;
        };
        auto io = [&](int fd, const std::string& out, const std::string& until) {  // send, read until `until` shows up
            if (!out.empty()) assert(write(fd, out.data(), out.size()) == (ssize_t)out.size());
            std::string got;
            for (int i = 0; i < 200 && got.find(until) == std::string::npos; i++) {
                std::vector<pollfd> pf;
                bnc.add_fds(pf);
                poll(pf.data(), pf.size(), 5);
                bnc.process(pf);
                char rb[65536];
                ssize_t n = recv(fd, rb, sizeof rb, MSG_DONTWAIT);
                if (n > 0) got.append(rb, (size_t)n);
            }
            return got;
        };
        int bad = dial();
        assert(io(bad, "PASS u/t:nope\r\nNICK x\r\nUSER u 0 * :r\r\n", "ERROR").find(" 464 x ") != std::string::npos);
        close(bad);
        {  // no such network: logged in with none; *isekur says so and "use" goes to one
            std::string mf = core.cfg_path.substr(0, core.cfg_path.rfind('/') + 1) + "bnc.motd";
            FILE* mp = fopen(mf.c_str(), "w");
            fputs("hello there\r\nline two\n", mp), fclose(mp);
            int nn = dial();
            std::string g = io(nn, "PASS u/nope:pw\r\nNICK x\r\nUSER u 0 * :r\r\n", "isekur server");
            unlink(mf.c_str());
            assert(g.find(" 375 x :") != std::string::npos && g.find(" 372 x :- hello there\r\n") != std::string::npos &&
                   g.find(" 372 x :- line two\r\n") != std::string::npos && g.find(" 422 ") == std::string::npos);
            assert(g.find(":isekur 001 x :") != std::string::npos && g.find("PRIVMSG x :No network nope in iSekur. Networks: t") != std::string::npos);
            assert(io(nn, "JOIN #c\r\n", "Not on a network").find("*isekur") != std::string::npos);
            g = io(nn, "PRIVMSG *isekur :use t\r\n", "earlier");
            assert(g.find(":x NICK me") != std::string::npos && g.find(":me JOIN #c\r\n") != std::string::npos);
            close(nn);
            io(-1, "", "~");  // its hangup handled before the next login
        }
        int fd = dial();
        std::string w = io(fd, "CAP LS 302\r\nPASS u/t:pw\r\nNICK x\r\nUSER u 0 * :r\r\nCAP REQ :server-time\r\nCAP END\r\n", "earlier");
        assert(w.find(":isekur CAP * LS :account-notify account-tag away-notify batch chghost draft/chathistory echo-message extended-join "
                      "invite-notify labeled-response message-tags multi-prefix server-time setname userhost-in-names") != std::string::npos && w.find("CAP x ACK :server-time") != std::string::npos);
        assert(w.find(":isekur 001 me :") != std::string::npos && w.find(":me JOIN #c\r\n") != std::string::npos);
        assert(w.find(" 332 me #c :the topic") != std::string::npos && w.find(" 353 me = #c :@bob") != std::string::npos);
        assert(w.find(" :bob PRIVMSG #c :earlier") != std::string::npos && w.find("@time=") != std::string::npos && bnc.attached());
        // the network's lines: tags gone but the time, echoes and iSekur's own background replies kept back
        bnc.from_server(ts, "@account=c;time=2026-01-02T03:04:05.000Z :carol!c@h PRIVMSG #c :live");
        bnc.from_server(ts, ":carol!c@h JOIN #c carol :Carol C");
        std::string got = io(fd, "", "JOIN");
        assert(got.find("account=") == std::string::npos);
        assert(got.find("@time=2026-01-02T03:04:05.000Z :carol!c@h PRIVMSG #c :live") != std::string::npos);
        assert(got.find(":carol!c@h JOIN #c\r\n") != std::string::npos);
        // iSekur's own WHOX after a join is kept back, a client's /who of another channel isn't
        ts->who_auto.insert("#c");
        bnc.from_server(ts, ":srv 354 me 777 #c u h carol H 0 :Carol");
        bnc.from_server(ts, ":srv 315 me #c :End of /WHO list.");
        bnc.from_server(ts, ":srv 352 me #other u h srv dave H :0 Dave");
        bnc.from_server(ts, ":srv 315 me #other :End of /WHO list.");
        got = io(fd, "", "#other :End");
        ts->who_auto.clear();
        assert(got.find(" 777 ") == std::string::npos && got.find("315 me #c ") == std::string::npos && got.find(" 352 me #other ") != std::string::npos);
        // the network drops: the client's channels close, and open again with iSekur's rejoin
        ts->state = SS::REGISTERING;
        got = io(fd, "", "PART");
        assert(got.find("*** Disconnected from t") != std::string::npos && got.find(":me PART #c :Disconnected from t") != std::string::npos);
        ts->state = SS::CONNECTED;
        bnc.from_server(ts, ":me!m@h JOIN #c");
        got = io(fd, "", "JOIN");
        assert(got.find("*** Connected to t") != std::string::npos && got.find(":me!m@h JOIN #c") != std::string::npos);
        {  // MONITOR (WeeChat's /notify): the client's own list here, merged with iSekur's for the network's one list
            int flood = ts->flood_ms;
            ts->isupport["MONITOR"] = "100", ts->motd_done = true, ts->flood_ms = 0, ts->conn.out.clear();
            core.cfg["server.t"]["notify"] = "alice";  // iSekur's own /notify
            io(fd, "MONITOR + alice,bob\r\n", "~");
            assert(ts->conn.out == "MONITOR + alice,bob\r\n" && ts->bnc_watch.count("bob"));
            bnc.from_server(ts, ":srv 730 me :alice!a@h,bob!b@h,zed!z@h");  // zed is someone else's
            got = io(fd, "", "730");
            assert(got.find(":srv 730 me :alice!a@h,bob!b@h\r\n") != std::string::npos);
            bnc.from_server(ts, ":srv 731 me :zed");  // nobody's here: not sent on
            got = io(fd, "MONITOR L\r\n", " 733 ");
            assert(got.find(" 731 ") == std::string::npos && got.find(":isekur 732 me :alice,bob\r\n") != std::string::npos);
            ts->conn.out.clear(), ts->online["bob"] = true;
            got = io(fd, "MONITOR S\r\n", " 730 ");  // bob from what iSekur knows, alice asked again
            assert(got.find(":isekur 730 me :bob\r\n") != std::string::npos && ts->conn.out == "MONITOR + alice\r\n");
            ts->conn.out.clear();
            io(fd, "MONITOR C\r\n", "~");  // only bob leaves the network's list: alice is iSekur's
            assert(ts->conn.out == "MONITOR - bob\r\n" && ts->bnc_watch.empty() && !ts->online.count("bob"));
            core.cfg["server.t"].erase("notify"), ts->isupport.erase("MONITOR"), ts->motd_done = false, ts->online.clear(), ts->flood_ms = flood;
        }
        // what it says goes through iSekur: upstream, and into the tab
        ts->conn.out.clear();
        io(fd, "PRIVMSG #c :hi there\r\nMODE #c\r\n", "\n");
        std::string up = ts->conn.out;
        for (auto& q : ts->sendq) up += q;
        assert(up.find("PRIVMSG #c :hi there\r\n") != std::string::npos && up.find("MODE #c\r\n") != std::string::npos);
        assert(chb->lines.back().text == "hi there" && chb->lines.back().self);
        {  // a client with all of it: no backlog (it asks), CHATHISTORY from the tab, tags both ways, its own echoes
            for (auto& l : chb->lines) if (l.text == "earlier") l.msgid = "m1";
            int z = dial();
            std::string zw = io(z, "CAP LS 302\r\nPASS u/t:pw\r\nNICK z\r\nUSER u 0 * :r\r\n"
                                   "CAP REQ :server-time message-tags echo-message batch draft/chathistory\r\nCAP END\r\n", " 366 ");
            assert(zw.find("CHATHISTORY=500") != std::string::npos && zw.find("earlier") == std::string::npos);
            std::string h = io(z, "CHATHISTORY LATEST #c * 50\r\n", "BATCH -");
            assert(h.find(" BATCH +h1 chathistory #c") != std::string::npos && h.find("@batch=h1;time=") != std::string::npos);
            assert(h.find(";msgid=m1 :bob PRIVMSG #c :earlier") != std::string::npos);
            h = io(z, "CHATHISTORY BEFORE #c msgid=m1 10\r\n", "BATCH -");
            assert(h.find("earlier") == std::string::npos && h.find("BATCH -h2") != std::string::npos);
            h = io(z, "CHATHISTORY TARGETS timestamp=2000-01-01T00:00:00.000Z timestamp=2100-01-01T00:00:00.000Z 10\r\n", "BATCH -");
            assert(h.find("CHATHISTORY TARGETS #c ") != std::string::npos);
            // upstream does echo-message and message-tags: client tags go up, the echo comes back to it with its msgid
            ts->caps.insert("echo-message"), ts->caps.insert("message-tags"), ts->conn.out.clear(), ts->sendq.clear();
            io(z, "@+draft/reply=m1 PRIVMSG #c :tagged\r\n", "\n");
            std::string up2 = ts->conn.out;
            for (auto& q : ts->sendq) up2 += q;
            assert(up2.find("@+draft/reply=m1 PRIVMSG #c :tagged\r\n") != std::string::npos);
            bnc.from_server(ts, "@msgid=m2;+draft/reply=m1;time=2026-01-02T03:04:06.000Z :me!m@h PRIVMSG #c :tagged");
            bnc.from_server(ts, "@+typing=active :carol!c@h TAGMSG #c");
            h = io(z, "", "TAGMSG");
            assert(h.find("@time=2026-01-02T03:04:06.000Z;+draft/reply=m1;msgid=m2 :me!m@h PRIVMSG #c :tagged") != std::string::npos);
            assert(h.find("@+typing=active :carol!c@h TAGMSG #c") != std::string::npos);
            got = io(fd, "PING :x\r\n", "PONG");  // the other client: the line once, plain, and no TAGMSG
            assert(got.find(":me!m@h PRIVMSG #c :tagged") != std::string::npos && got.find("TAGMSG") == std::string::npos && got.find("msgid") == std::string::npos);
            // without echo-message upstream the BNC echoes it to the clients that asked, the sender too
            ts->caps.erase("echo-message");
            io(z, "PRIVMSG #c :plain one\r\n", "plain one");
            ts->caps.erase("message-tags");
            assert(io(fd, "", "plain one").find(":me PRIVMSG #c :plain one") != std::string::npos);
            io(z, "QUIT\r\n", "ERROR");
            close(z);
        }
        {  // the network's IRCv3 to the clients that ask; answers to a labeled command to that client alone
            int y = dial();
            std::string yw = io(y, "CAP LS 302\r\nPASS u/t:pw\r\nNICK y\r\nUSER u 0 * :r\r\n"
                                   "CAP REQ :extended-join multi-prefix away-notify labeled-response batch message-tags\r\nCAP END\r\n", " 366 ");
            assert(yw.find(" 353 me = #c :@+bob") != std::string::npos && yw.find(":me JOIN #c * :") != std::string::npos);
            bnc.from_server(ts, ":carol!c@h JOIN #c acct :Carol C");
            bnc.from_server(ts, ":carol!c@h AWAY :gone");
            bnc.from_server(ts, ":srv 353 me = #c :@+bob!b@h carol!c@h");
            std::string g = io(y, "", "carol!c@h\r\n");
            assert(g.find(":carol!c@h JOIN #c acct :Carol C") != std::string::npos && g.find(":carol!c@h AWAY :gone") != std::string::npos);
            assert(g.find(" 353 me = #c :@+bob carol") != std::string::npos);
            got = io(fd, "PING :a\r\n", "PONG");
            assert(got.find("AWAY") == std::string::npos && got.find(":carol!c@h JOIN #c\r\n") != std::string::npos && got.find(" 353 me = #c :@bob carol") != std::string::npos);
            ts->caps.insert("labeled-response"), ts->conn.out.clear(), ts->sendq.clear();
            io(y, "@label=L1 WHOIS bob\r\n@label=L2 MODE #c\r\n", "\n");
            std::string up3 = ts->conn.out;
            for (auto& q : ts->sendq) up3 += q;
            assert(up3.find("@label=b1 WHOIS bob\r\n") != std::string::npos && up3.find("@label=b2 MODE #c\r\n") != std::string::npos);
            bnc.from_server(ts, "@label=b1 :srv BATCH +X labeled-response");
            bnc.from_server(ts, "@batch=X :srv 311 me bob b h * :Bob");
            bnc.from_server(ts, ":srv BATCH -X");
            bnc.from_server(ts, "@label=b2 :srv 324 me #c +nt");
            g = io(y, "", " 324 ");
            assert(g.find("@label=L1 :srv BATCH +X labeled-response") != std::string::npos && g.find("@batch=X :srv 311 me bob") != std::string::npos);
            assert(g.find(":srv BATCH -X") != std::string::npos && g.find("@label=L2 :srv 324 me #c +nt") != std::string::npos);
            got = io(fd, "PING :b\r\n", "PONG");
            assert(got.find(" 311 ") == std::string::npos && got.find(" 324 ") == std::string::npos);
            ts->caps.erase("labeled-response");
            assert(io(y, "@label=L3 WHOIS bob\r\n", "ACK").find("@label=L3 :isekur ACK") != std::string::npos);  // can't tell: ACK now
            io(y, "QUIT\r\n", "ERROR");
            close(y);
        }
        // what the terminal says reaches the client as ours
        core.say(chb, "from the terminal");
        assert(io(fd, "", "terminal").find(":me PRIVMSG #c :from the terminal") != std::string::npos);
        assert(io(fd, "QUIT\r\n", "ERROR").find("ERROR :Detached") != std::string::npos);
        close(fd);
        io(-1, "", "never");
        assert(!bnc.attached());
        bnc.stop();
        assert(!Server::tap && !bnc.running());
        ts->nick = nick0, chb->topic.clear(), chb->nicks.erase("bob"), ts->conn.out.clear(), ts->sendq.clear();
        core.cfg.erase("bnc");
    }

    {  // SASL EXTERNAL: a client certificate in [bnc] certfp logs in with no password; another certificate, or none, is refused
        char d[] = "/tmp/isekur-sasl-XXXXXX";
        assert(mkdtemp(d));
        std::string dir = d, err;
        assert(gen_selfsigned(dir + "/srv.crt", dir + "/srv.key", err) && gen_selfsigned(dir + "/cl.crt", dir + "/cl.key", err) &&
               gen_selfsigned(dir + "/other.crt", dir + "/other.key", err));
        auto fp_of = [](const std::string& crt) {  // SHA-256 of the certificate's DER, as the config has it
            FILE* f = fopen(crt.c_str(), "r");
            X509* x = PEM_read_X509(f, nullptr, nullptr, nullptr);
            fclose(f);
            unsigned char md[EVP_MAX_MD_SIZE];
            unsigned int n = 0;
            assert(x && X509_digest(x, EVP_sha256(), md, &n) == 1);
            X509_free(x);
            std::string h;
            char b[3];
            for (unsigned i = 0; i < n; i++) snprintf(b, sizeof b, "%02x", md[i]), h += b;
            return h;
        };
        std::string colon;  // the client's fingerprint as openssl prints it: AB:CD:...
        for (size_t i = 0, cl_n = fp_of(dir + "/cl.crt").size(); i < cl_n; i += 2) colon += (colon.empty() ? "" : ":") + fp_of(dir + "/cl.crt").substr(i, 2);
        core.cfg["bnc"] = {{"bind", "127.0.0.1"}, {"port", "18798"}, {"tls", "on"}, {"cert", dir + "/srv.crt"}, {"key", dir + "/srv.key"},
                           {"user", "u"}, {"password_hash", hash_password("pw")}, {"certfp", colon + ", " + fp_of(dir + "/other.crt")}};
        Bnc bnc(core);
        std::string berr;
        assert(bnc.start(berr) && bnc.url == "ircs://127.0.0.1:18798");
        bool row = false;
        for (auto& r : core.setting_rows()) row |= r.sec == "bnc" && r.key == "certfp";
        assert(row);  // a setting: /set bnc.certfp, and the settings window
        // a TLS client: connects, presents cert_file (or none), and speaks lines until `until` shows up
        auto talk = [&](const char* cert_file, const std::string& lines, const std::string& until) {
            SSL_CTX* cx = SSL_CTX_new(TLS_client_method());
            if (cert_file) assert(SSL_CTX_use_certificate_file(cx, cert_file, SSL_FILETYPE_PEM) == 1 &&
                                  SSL_CTX_use_PrivateKey_file(cx, (std::string(cert_file).substr(0, std::string(cert_file).size() - 4) + ".key").c_str(), SSL_FILETYPE_PEM) == 1);
            SSL_CTX_set_verify(cx, SSL_VERIFY_NONE, nullptr);
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in a{};
            a.sin_family = AF_INET, a.sin_port = htons(18798), a.sin_addr.s_addr = htonl(0x7f000001);
            assert(connect(fd, (sockaddr*)&a, sizeof a) == 0);
            fcntl(fd, F_SETFL, O_NONBLOCK);
            SSL* ssl = SSL_new(cx);
            SSL_set_fd(ssl, fd);
            SSL_set_connect_state(ssl);
            auto pump = [&] {
                std::vector<pollfd> pf;
                bnc.add_fds(pf);
                poll(pf.data(), pf.size(), 5);
                bnc.process(pf);
            };
            for (int i = 0; i < 400 && SSL_do_handshake(ssl) != 1; i++) pump();
            assert(SSL_is_init_finished(ssl));
            std::string got;
            size_t sent = 0;
            for (int i = 0; i < 400 && got.find(until) == std::string::npos; i++) {
                if (sent < lines.size()) {
                    int w = SSL_write(ssl, lines.data() + sent, (int)(lines.size() - sent));
                    if (w > 0) sent += (size_t)w;
                }
                pump();
                char rb[65536];
                int n = SSL_read(ssl, rb, sizeof rb);
                if (n > 0) got.append(rb, (size_t)n);
            }
            SSL_shutdown(ssl);
            SSL_free(ssl), SSL_CTX_free(cx), close(fd);
            return got;
        };
        std::string ok = talk((dir + "/cl.crt").c_str(),
                              "CAP REQ :sasl\r\nAUTHENTICATE EXTERNAL\r\nAUTHENTICATE +\r\nCAP END\r\nNICK x\r\nUSER u 0 * :r\r\n", " 903 ");
        assert(ok.find("CAP * ACK :sasl") != std::string::npos && ok.find("AUTHENTICATE +") != std::string::npos && ok.find(" 903 * ") != std::string::npos);
        assert(ok.find(" 904 ") == std::string::npos && bnc.attached());
        assert(ok.find(" 900 ") != std::string::npos);
        // the certificate is the login: a client whose USER isn't [bnc] user gets in as it
        std::string who = talk((dir + "/cl.crt").c_str(),
                               "CAP REQ :sasl\r\nAUTHENTICATE EXTERNAL\r\nAUTHENTICATE +\r\nCAP END\r\nNICK z\r\nUSER somebody 0 * :r\r\n", " 001 ");
        assert(who.find(" 001 ") != std::string::npos && who.find(" 464 ") == std::string::npos);
        std::string old = talk(nullptr, "CAP LS\r\n", "sasl");  // no 302: a cap has no value
        assert(old.find(" sasl") != std::string::npos && old.find("sasl=") == std::string::npos);
        std::string bad = talk((dir + "/other.crt").c_str(),
                               "AUTHENTICATE EXTERNAL\r\nAUTHENTICATE +\r\nNICK y\r\n", " 904 ");
        assert(bad.find(" 904 ") != std::string::npos && bad.find(" 903 ") == std::string::npos);
        std::string none = talk(nullptr, "CAP LS 302\r\n", "sasl=EXTERNAL");
        assert(none.find("sasl=EXTERNAL") != std::string::npos);
        bnc.stop();
        core.cfg.erase("bnc");
        std::filesystem::remove_all(dir);
    }

    {  // FIFO escapes: \n splits, \xHH and \e are bytes, \\ is a backslash, anything else stays as typed
        auto v = fifo_lines("/me \\x02bold\\x0f \\e[0m\\x03" "4red\\n/msg bob \\\\n \\xZZ \\q\\x7");
        assert(v.size() == 2 && v[0] == "/me \x02" "bold\x0f \x1b[0m\x03" "4red" && v[1] == "/msg bob \\n \\xZZ \\q\\x7");
        auto f = fifo_lines("\\b\\i\\u\\s\\m\\v\\o");  // named formatting: bold italics underline strike mono reverse reset
        assert(f.size() == 1 && f[0] == "\x02\x1d\x1f\x1e\x11\x16\x0f");
    }
    {  // a target before the first |: a tab that isn't open is refused, a plain word too (never sent as chat); a prefix with a space is text
        core.fifo_line("nosuch/#x|/me hi");
        assert(core.bufs.front()->lines.back().text.find("no open tab nosuch/#x") != std::string::npos);
        size_t before = core.bufs.front()->lines.size();
        core.fifo_line("lol|ok");
        assert(core.bufs.front()->lines.back().text.find("no open tab lol") != std::string::npos);
        assert(core.bufs.front()->lines.size() > before);
        core.fifo_line("Build passed|ok");  // a space: not a target, plain text to the active tab
        assert(core.bufs.front()->lines.back().text.find("no open tab Build") == std::string::npos);
    }
    {  // [general] fifo: a line written to the pipe runs as if typed; a partial line waits; nothing runs while locked
        char d[] = "/tmp/isekur-fifo-XXXXXX";
        assert(mkdtemp(d));
        std::string p = std::string(d) + "/fifo";
        core.exec(home, "/set fifo_path " + p);  // a path alone opens nothing: fifo is still off
        assert(core.fifo_fd < 0 && access(p.c_str(), F_OK) != 0);
        core.exec(home, "/set fifo on");  // on: the pipe is created at once, at fifo_path
        assert(core.fifo_fd >= 0 && core.fifo_path == p);
        int w = open(p.c_str(), O_WRONLY | O_NONBLOCK);
        assert(w >= 0);
        auto pump = [&] {
            std::vector<pollfd> pf{{core.fifo_fd, POLLIN, 0}};
            poll(pf.data(), 1, 50);
            core.fifo_poll(pf, 0);
        };
        auto put = [&](const char* text) { assert(write(w, text, strlen(text)) == (ssize_t)strlen(text)); };
        put("/set nick_width 13\n/set nick_width 14");
        pump();
        assert(core.cfg["general"]["nick_width"] == "13");  // the complete line ran; the partial one waits
        put("\n");
        pump();
        assert(core.cfg["general"]["nick_width"] == "14");
        core.locked = true;
        put("/set nick_width 15\n");
        pump();
        assert(core.cfg["general"]["nick_width"] == "14");  // locked: dropped
        core.locked = false;
        std::string chunk(4000, 'x');  // over the limit: all of it goes, the end of the line too, and the next line still runs
        for (int i = 0; i < 3; i++) put(chunk.c_str()), pump();
        put("/set nick_width 16\n");
        pump();
        assert(core.cfg["general"]["nick_width"] == "14");
        put("/set nick_width 17\n");
        pump();
        assert(core.cfg["general"]["nick_width"] == "17");
        close(w);
        core.exec(home, "/set fifo off");  // off: the pipe iSekur made is gone
        assert(core.fifo_fd < 0 && access(p.c_str(), F_OK) != 0);
        core.cfg["general"].erase("fifo_path"), core.cfg["general"].erase("nick_width");
        {  // no fifo_path: the default ~/.config/isekur/fifo, with ~ taken from HOME
            const char* oldhome = getenv("HOME");
            std::string oh = oldhome ? oldhome : "";
            setenv("HOME", d, 1);
            std::filesystem::create_directories(std::string(d) + "/.config/isekur");
            std::string keep = core.cfg_path;
            core.cfg_path = default_config_path();  // the main config
            core.exec(home, "/set fifo on");
            assert(core.fifo_fd >= 0 && core.fifo_path == std::string(d) + "/.config/isekur/fifo");
            core.exec(home, "/set fifo off");
            assert(core.fifo_fd < 0);
            const char* oldxdg = getenv("XDG_CONFIG_HOME");  // $XDG_CONFIG_HOME moves the config, so the pipe with it
            std::string ox = oldxdg ? oldxdg : "";
            bool hadx = oldxdg != nullptr;
            setenv("XDG_CONFIG_HOME", (std::string(d) + "/xdg").c_str(), 1);
            std::filesystem::create_directories(std::string(d) + "/xdg/isekur");
            core.cfg_path = default_config_path();
            core.exec(home, "/set fifo on");
            assert(core.fifo_fd >= 0 && core.fifo_path == std::string(d) + "/xdg/isekur/fifo");
            core.exec(home, "/set fifo off");
            if (hadx) setenv("XDG_CONFIG_HOME", ox.c_str(), 1); else unsetenv("XDG_CONFIG_HOME");
            core.cfg_path = keep;
            setenv("HOME", oh.c_str(), 1);
        }
        {  // no fifo_path in another config: <name>.fifo beside it, so two iSekurs never share a pipe
            std::string keep = core.cfg_path;
            core.cfg_path = std::string(d) + "/work.conf";
            core.exec(home, "/set fifo on");
            assert(core.fifo_fd >= 0 && core.fifo_path == std::string(d) + "/work.fifo");
            core.exec(home, "/set fifo off");
            assert(core.fifo_fd < 0 && access((std::string(d) + "/work.fifo").c_str(), F_OK) != 0);
            core.cfg_path = keep;
        }
        core.cfg["general"].erase("fifo");
        bool row = false, prow = false;
        for (auto& r : core.setting_rows()) row |= r.sec == "general" && r.key == "fifo", prow |= r.sec == "general" && r.key == "fifo_path";
        assert(row && prow);  // settings window rows, next to the others
        std::filesystem::remove_all(d);
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
        {  // [web] html: served from disk, re-read on every load; unreadable falls back to the built-in page
            std::string hp = "build/test-web.html";
            auto get = [&] { auto [fd, r] = talk("GET / HTTP/1.1\r\nHost: x\r\n\r\n", 1 << 30); close(fd); return r; };
            core.cfg["web"]["html"] = hp;
            { std::ofstream(hp) << "one @ISEKUR_VERSION@"; }
            assert(get().find("one " ISEKUR_VERSION) != std::string::npos);
            { std::ofstream(hp) << "two"; }
            assert(get().find("\r\n\r\ntwo") != std::string::npos);
            unlink(hp.c_str());
            assert(get().find("<html") != std::string::npos);
            core.cfg["web"].erase("html");
        }
        {  // the favicon and the app icons: what the page's <link>s and the manifest name is served, as the kind and size they say
            auto fetch = [&](const std::string& path) {
                auto [fd, r] = talk("GET " + path + " HTTP/1.1\r\nHost: x\r\n\r\n", 1 << 30);
                close(fd);
                size_t b = r.find("\r\n\r\n");
                return std::make_pair(r.substr(0, b == std::string::npos ? 0 : b), b == std::string::npos ? std::string() : r.substr(b + 4));
            };
            auto u32 = [](const std::string& d, size_t at) { return (uint32_t)((uint8_t)d[at] << 24 | (uint8_t)d[at + 1] << 16 | (uint8_t)d[at + 2] << 8 | (uint8_t)d[at + 3]); };
            auto is_png = [&](const std::string& d, uint32_t w, uint32_t h) { return d.size() > 24 && d.compare(1, 3, "PNG") == 0 && u32(d, 16) == w && u32(d, 20) == h; };
            auto [mh, mj] = fetch("/manifest.json");
            assert(mh.find("application/manifest+json") != std::string::npos);
            std::set<std::string> purposes;
            int n_icons = 0;
            const std::regex manifest_icon("\\{\"src\":\"([^\"]+)\",\"sizes\":\"(\\d+)x(\\d+)\",\"type\":\"image/png\",\"purpose\":\"(\\w+)\"\\}");
            for (std::sregex_iterator it(mj.begin(), mj.end(), manifest_icon), end; it != end; ++it) {
                auto [h, body] = fetch((*it)[1]);
                assert(h.find("200 OK") != std::string::npos && h.find("image/png") != std::string::npos && is_png(body, (uint32_t)std::stoi((*it)[2]), (uint32_t)std::stoi((*it)[3])));
                purposes.insert((*it)[4]), n_icons++;
            }
            assert(n_icons == 3 && purposes == std::set<std::string>({"any", "maskable"}));
            auto [ph, page] = fetch("/");
            int n_links = 0;
            const std::regex page_icon("<link rel=\"(?:icon|apple-touch-icon)\"[^>]*?href=\"(/[^\"]+)\"");
            for (std::sregex_iterator it(page.begin(), page.end(), page_icon), end; it != end; ++it, n_links++) {
                auto [h, body] = fetch((*it)[1]);
                assert(h.find("200 OK") != std::string::npos && body.size() > 100);
            }
            assert(n_links == 4 && page.find("<link rel=\"apple-touch-icon\" sizes=\"180x180\"") != std::string::npos);
            assert(fetch("/session").first.find("X-Build: " + version_string() + "\r\n") != std::string::npos);  // the page reloads itself when a restart brings another one
            auto [ih, ico] = fetch("/favicon.ico");  // 16, 32 and 48 px, PNG inside
            assert(ih.find("image/x-icon") != std::string::npos && ico.size() > 22 && ico.compare(0, 6, std::string("\0\0\1\0\3\0", 6)) == 0);
            for (int i = 0; i < 3; i++) {
                uint8_t sz = (uint8_t)ico[6 + 16 * (size_t)i];
                uint32_t sl = (uint32_t)((uint8_t)ico[14 + 16 * (size_t)i] | (uint8_t)ico[15 + 16 * (size_t)i] << 8 | (uint8_t)ico[16 + 16 * (size_t)i] << 16);
                uint32_t off = (uint32_t)((uint8_t)ico[18 + 16 * (size_t)i] | (uint8_t)ico[19 + 16 * (size_t)i] << 8 | (uint8_t)ico[20 + 16 * (size_t)i] << 16);
                assert(sz == 16 || sz == 32 || sz == 48);
                assert(is_png(ico.substr(off, sl), sz, sz));
            }
            assert(is_png(fetch("/apple-touch-icon.png").second, 180, 180) && is_png(fetch("/icon.png").second, 512, 512) && is_png(fetch("/favicon-32.png").second, 32, 32));
            assert(fetch("/icon-nope.png").first.find("404") != std::string::npos);
        }
        auto [lfd2, login] = talk("POST /login HTTP/1.1\r\nHost: x\r\nContent-Length: 24\r\n\r\n{\"user\":\"u\",\"pass\":\"pw\"}", 1);
        close(lfd2);
        size_t ck = login.find("isekur_sid=");
        assert(login.rfind("HTTP/1.1 200", 0) == 0 && ck != std::string::npos);
        std::string cookie = login.substr(ck, login.find(';', ck) - ck);
        {  // POST /upload: a file for the plugin that takes dropped files. Not logged in: no; no plugin: told before the bytes come; a plugin: the
           // file lands in uploads/ (a random name, 0600) and its `drop` event has the name, size and kind, never the path
            std::string updir = "build/uploads", pdir2 = "build/plugins";
            auto count_up = [&] { int n = 0; if (DIR* d = opendir(updir.c_str())) { while (dirent* e = readdir(d)) n += e->d_name[0] != '.'; closedir(d); } return n; };
            auto post = [&](const std::string& q, const std::string& ck, const std::string& data, size_t claim = 0) {
                auto [fd, r] = talk("POST /upload" + q + " HTTP/1.1\r\nHost: x\r\n" + (ck.empty() ? "" : "Cookie: " + ck + "\r\n") + "Content-Length: " +
                                        std::to_string(claim ? claim : data.size()) + "\r\n\r\n" + data, 1 << 30);
                close(fd);
                return r;
            };
            std::string data(60000, 'x');
            for (size_t i = 0; i < data.size(); i++) data[i] = (char)('a' + i % 23);
            std::string q = "?buf=" + std::to_string(chb->id) + "&name=my%20pic.png";
            assert(post(q, "", data).rfind("HTTP/1.1 403", 0) == 0 && count_up() == 0);  // no session
            std::string r = post(q, cookie, data);
            assert(r.rfind("HTTP/1.1 422", 0) == 0 && r.find("no plugin takes files") != std::string::npos && count_up() == 0);  // no plugin: refused before any byte is kept
            mkdir_p(pdir2);
            { std::ofstream(pdir2 + "/dz.lua") << "isekur.on('drop', function(e) isekur.print(e.srv, e.target, 'dropped ' .. e.name .. '|' .. e.size .. '|' .. e.kind .. '|' .. e.type .. '|' .. tostring(e.srv) .. '|' .. tostring(e.target)) end)\n"; }
            core.exec(chb, "/plugin enable dz");
            assert(core.plugin_hooks("drop", ts, "#c"));
            assert(post("?buf=99999&name=a.png", cookie, data).rfind("HTTP/1.1 400", 0) == 0);  // a tab that isn't there
            assert(post(q, cookie, "").rfind("HTTP/1.1 400", 0) == 0);  // nothing in it
            assert(post(q, cookie, "", 2000000000).rfind("HTTP/1.1 413", 0) == 0 && count_up() == 0);  // over 1 GB: said at the headers
            core.locked = true;
            assert(post(q, cookie, data).rfind("HTTP/1.1 403", 0) == 0 && count_up() == 0);  // locked: no files either
            core.locked = false;
            r = post(q, cookie, data);
            assert(r.rfind("HTTP/1.1 200", 0) == 0 && r.find("{\"ok\":true}") != std::string::npos);
            assert(core.drops.size() == 1 && count_up() == 1);
            const Core::Drop& dd = core.drops[0];
            assert(dd.name == "my pic.png" && dd.size == 60000 && dd.kind == "image" && dd.mime == "image/png" && dd.own);
            assert(dd.path.find("pic") == std::string::npos && dd.path.rfind(updir + "/", 0) == 0 && dd.id.size() == 16);  // a random name on the disk
            struct stat ust;
            assert(stat(dd.path.c_str(), &ust) == 0 && (ust.st_mode & 0777) == 0600 && ust.st_size == 60000);
            { std::ifstream in(dd.path, std::ios::binary); std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); assert(got == data); }
            assert(chb->lines.back().text == "dropped my pic.png|60000|image|image/png|" + ts->cfg.name + "|#c");
            std::string id1 = dd.id, p1 = dd.path;
            core.drop_forget(id1);  // sent or discarded: the copy goes
            assert(core.drops.empty() && access(p1.c_str(), F_OK) != 0 && count_up() == 0);
            {  // a name is a name, not a path
                assert(post("?buf=" + std::to_string(chb->id) + "&name=..%2F..%2Fetc%2Fpasswd", cookie, "hi").rfind("HTTP/1.1 200", 0) == 0);
                assert(core.drops.size() == 1 && core.drops[0].name.find('/') == std::string::npos && core.drops[0].name[0] != '.');
                core.drop_forget(core.drops[0].id);
            }
            {  // a browser that goes away halfway leaves nothing
                auto [fd, half] = talk("POST /upload" + q + " HTTP/1.1\r\nHost: x\r\nCookie: " + cookie + "\r\nContent-Length: 50000\r\n\r\n" + data.substr(0, 10000), 1);
                assert(count_up() == 1);  // the half file is there while it comes
                close(fd);
                for (int i = 0; i < 20; i++) step();
                assert(count_up() == 0 && core.drops.empty());
            }
            core.exec(chb, "/plugin disable dz");
            unlink((pdir2 + "/dz.lua").c_str());
            assert(post(q, cookie, data).rfind("HTTP/1.1 422", 0) == 0);  // the plugin is off again
        }
        {  // a new X-Forwarded-For each try (an ssh tunnel passes the browser's as is): the 127.0.0.1 peer still locks, after 20
            auto bad = [&](int i) {
                std::string body = "{\"user\":\"u\",\"pass\":\"no\"}";
                auto [fd, r] = talk("POST /login HTTP/1.1\r\nHost: x\r\nX-Forwarded-For: 10.0.0." + std::to_string(i) +
                                        "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body, 1);
                close(fd);
                return r;
            };
            for (int i = 1; i <= 20; i++) assert(bad(i).rfind("HTTP/1.1 401", 0) == 0);
            assert(bad(21).rfind("HTTP/1.1 429", 0) == 0);
            web.fails.clear();
        }
        {  // a Cookie header with a segment of only spaces answered with a crash (substr at npos): it's just no session
            for (const char* odd : {"a=b; ;c=d", "a=b; ", ";", " ; ; ", "isekur_sid="}) {
                auto [fd, r] = talk(std::string("GET /session HTTP/1.1\r\nHost: x\r\nCookie: ") + odd + "\r\n\r\n", 1);
                close(fd);
                assert(r.rfind("HTTP/1.1 401", 0) == 0);
            }
            auto [fd, r] = talk("GET /session HTTP/1.1\r\nHost: x\r\nCookie: a=b; ;  " + cookie + "\r\n\r\n", 1);
            close(fd);
            assert(r.rfind("HTTP/1.1 200", 0) == 0);  // after the spaces, the real one still counts
        }
        auto [ws0, snap] = talk("GET /ws HTTP/1.1\r\nHost: x\r\nCookie: " + cookie + "\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n", 1 << 30);
        int ws = ws0;  // lambdas below capture it; capturing a structured binding is C++20
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
        {  // a plugin's question reaches the browser with its field, and the browser's answer (row and text) takes it off the list for everyone
            core.choices.push_back(Core::Choice{"none", "q1", "Send a file", {"pic.png"}, {"one", "two"}, 2, "Message (optional)", "hi"});
            core.notify(Ev::CHOICE, nullptr);
            std::string got = more("");
            assert(got.find("\"t\":\"choice\",\"open\":true,\"title\":\"Send a file\"") != std::string::npos);
            assert(got.find("\"items\":[\"one\",\"two\"],\"sel\":2,\"input\":\"Message (optional)\",\"value\":\"hi\"") != std::string::npos);
            got = more(client_frame("{\"t\":\"choice\",\"buf\":" + std::to_string(chb->id) + ",\"i\":2,\"text\":\"hello\"}"));
            assert(core.choices.empty() && got.find("{\"t\":\"choice\",\"open\":false}") != std::string::npos);
        }
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
        {  // restarted, unlocked at the terminal, then a tab left open reconnects: no second lock password
            Web w2(core);
            assert(w2.start(werr));
            core.cfg["general"]["lock"] = "on", core.cfg["general"]["lock_password_hash"] = hash_password("lk");
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in a{};
            a.sin_family = AF_INET, a.sin_port = htons(18799), a.sin_addr.s_addr = htonl(0x7f000001);
            std::string req = "GET /ws HTTP/1.1\r\nHost: x\r\nCookie: " + cookie + "\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
            assert(connect(fd, (sockaddr*)&a, sizeof a) == 0 && write(fd, req.data(), req.size()) == (ssize_t)req.size());
            for (int i = 0; i < 30; i++) {
                std::vector<pollfd> pf;
                w2.add_fds(pf);
                poll(pf.data(), pf.size(), 10);
                w2.process(pf), w2.flush();
            }
            assert(w2.attached() && !core.locked);
            close(fd), w2.stop();
            core.cfg["general"].erase("lock"), core.cfg["general"].erase("lock_password_hash");
        }
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
        // clearing: /topic -delete sends "TOPIC #c :"; the server's empty TOPIC and 331 say it's gone
        core.exec(vb, "/topic -delete");
        assert(vs().find("TOPIC " + vb->name + " :\r\n") != std::string::npos);
        vb->topic = "old words";
        vf(":bob!u@h TOPIC " + vb->name + " :");
        assert(vb->topic.empty() && vb->lines.back().text == "bob removed the topic");
        vb->topic = "stale";
        vf(":srv 331 me " + vb->name + " :No topic is set");
        assert(vb->topic.empty() && vb->lines.back().text == "No topic is set");
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
        // a server line with nothing after the command used to read the back() of an empty parameter list
        for (const char* bare : {":srv 730", ":srv 731", ":srv 734", ":srv REGISTER", ":srv VERIFY", "@time=2026-01-02T03:04:05.000Z :bob!u@h 734"}) vf(bare);
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
        assert(vs().find("ISON alice dave\r\n") != std::string::npos);  // dave's query is open: watched too
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

    {  // proxy URLs: kinds, default ports, user:pass, [v6], and nothing ssh could take for an option
        Proxy px;
        std::string e;
        assert(parse_proxy("socks5://127.0.0.1:9050", px, e) && px.kind == Proxy::SOCKS5 && px.host == "127.0.0.1" && px.port == 9050);
        assert(parse_proxy("SOCKS4a://p.lan", px, e) && px.kind == Proxy::SOCKS4 && px.port == 1080);
        assert(parse_proxy("http://me:se:cret@[::1]:3128/", px, e) && px.kind == Proxy::HTTP && px.user == "me" && px.pass == "se:cret" && px.host == "::1" && px.port == 3128);
        assert(parse_proxy("ssh://me@jump.example.org", px, e) && px.kind == Proxy::SSH && px.port == 22 && proxy_desc(px) == "ssh me@jump.example.org:22");
        for (auto bad : {"ftp://x", "socks5://", "socks5://h:0", "socks5://h:99999", "ssh://-oProxyCommand=x", "ssh://-l@h",
                         "ssh://me:pw@h", "socks5://h x", "http://@h"})
            assert(!parse_proxy(bad, px, e) && !e.empty());
    }
    {  // isekur.sort_tabs: a network's tabs together in its place, status first, the rest by name; the console stays first
        char d[] = "/tmp/isekur-sort-XXXXXX";
        assert(mkdtemp(d));
        std::string dir = d;
        Core sc;
        sc.cfg_path = dir + "/isekur.conf";
        mkdir_p(dir + "/plugins");
        FILE* o = fopen((dir + "/plugins/sorter.lua").c_str(), "w");
        fputs("isekur.command(\"srt\", \"\", function() print(isekur.sort_tabs()) end)\n", o), fclose(o);
        Buffer* ph = sc.open(nullptr, BT::STATUS, "isekur");
        auto last = [&](Buffer* b) { return b->lines.empty() ? std::string() : b->lines.back().text; };
        sc.exec(ph, "/plugin enable sorter");
        assert(sc.plugins.size() == 1);
        Server* zn = sc.add_server(sc.server_cfg("zn"));
        Server* an = sc.add_server(sc.server_cfg("an"));
        sc.open(zn, BT::QUERY, "bob");     // open() puts each tab at the end of its network: the order is out of sort
        sc.open(zn, BT::CHANNEL, "#zeta");
        sc.open(an, BT::CHANNEL, "#beta");
        sc.open(an, BT::CHANNEL, "#Alpha");
        auto names = [&] {
            std::string s;
            for (auto& b : sc.bufs) s += (s.empty() ? "" : " ") + (b->srv ? b->name : std::string("console"));
            return s;
        };
        assert(names() == "console zn bob #zeta an #beta #Alpha");
        sc.exec(ph, "/srt");
        assert(last(ph) == "true" && names() == "console zn #zeta bob an #Alpha #beta");  // networks keep their order
        sc.exec(ph, "/srt");
        assert(last(ph) == "false" && names() == "console zn #zeta bob an #Alpha #beta");
        Buffer* bob = nullptr;
        for (auto& b : sc.bufs) if (b->name == "bob") bob = b.get();
        sc.exec(bob, "/move 1");  // a tab moved by hand stays: the rest sort around it
        assert(names() == "console zn bob #zeta an #Alpha #beta");
        sc.open(zn, BT::CHANNEL, "#aaa");
        sc.exec(ph, "/srt");
        assert(last(ph) == "true" && names() == "console zn bob #aaa #zeta an #Alpha #beta");
        sc.plugins.clear();
        assert(system(("rm -rf '" + dir + "'").c_str()) == 0);
    }
    {  // API 8: the show event changes what the screens show (not what's sent or logged), isekur.theme() the theme's colors
        char d[] = "/tmp/isekur-show-XXXXXX";
        assert(mkdtemp(d));
        std::string dir = d;
        Core sy;
        sy.cfg_path = dir + "/isekur.conf";
        mkdir_p(dir + "/plugins");
        FILE* o = fopen((dir + "/plugins/up.lua").c_str(), "w");
        fputs(R"LUA(
isekur.on("show", function(e)
  if e.text ~= "keep" then return (e.self and "me:" or "") .. e.text:upper() end
end)
isekur.command("th", "", function() local t = isekur.theme(); print(t.name, #t.nicks, t.fg) end)
)LUA", o), fclose(o);
        sy.theme.name = "x", sy.theme.nicks = {0xff0000, 0x00ff00}, sy.theme.el["default"].fg = 0x123456;
        Buffer* ph = sy.open(nullptr, BT::STATUS, "isekur");
        sy.exec(ph, "/plugin enable up");
        sy.exec(ph, "/th");
        assert(ph->lines.back().text == "x 2 " + std::to_string(0x123456));
        ServerCfg yc;
        yc.name = "y", yc.nick = "me";
        Server* ys = sy.add_server(yc);
        ys->state = SS::CONNECTED, ys->flood_ms = 0, ys->nick = "me";
        Buffer* yb = sy.open(ys, BT::CHANNEL, "#y");
        yb->joined = true;
        ys->conn.out.clear();
        sy.exec(yb, "hello");
        assert(ys->conn.out == "PRIVMSG #y :hello\r\n" && yb->lines.back().text == "me:HELLO");  // sent as typed, shown changed
        Msg mm;
        assert(irc_parse(":bob!u@h PRIVMSG #y :hi there", mm));
        sy.handle(ys, mm);
        assert(yb->lines.back().text == "HI THERE");
        assert(irc_parse(":bob!u@h PRIVMSG #y :keep", mm));
        sy.handle(ys, mm);
        assert(yb->lines.back().text == "keep");
        // turned off and on: the backlog goes back to what was said, then is shown changed again
        auto texts = [&] { std::string t; for (auto& l : yb->lines) if (l.kind == LK::MSG) t += l.text + "|"; return t; };
        sy.exec(ph, "/plugin disable up");
        assert(texts() == "hello|hi there|keep|");
        sy.exec(ph, "/plugin enable up");
        assert(texts() == "me:HELLO|HI THERE|keep|");
        {  // API 9: nickmenu items go after a separator; /exec and the like, and non-commands, are dropped
            FILE* f = fopen((dir + "/plugins/nm.lua").c_str(), "w");
            fputs(R"LUA(
isekur.on("nickmenu", function(e)
  return {{"Allow " .. e.nick, "/pmguard allow " .. e.nick}, {"Run", "/exec rm -rf ~"}, {"Say", "hi"}, {"Two\nlines", "/whois x\n/quit"}}
end)
)LUA", f), fclose(f);
            sy.exec(ph, "/plugin enable nm");
            auto nm = sy.nick_menu(yb, "bob");
            auto it = std::find(nm.begin(), nm.end(), std::make_pair(std::string("Allow bob"), std::string("/pmguard allow bob")));
            assert(it != nm.end() && (it - 1)->first.empty());
            assert(nm.back() == std::make_pair(std::string("Two"), std::string("/whois x")) && nm.end() - it == 2);
        }
        {  // API 10: isekur.set writes the plugin's own declared settings, per network, to the config; never enabled
            FILE* f = fopen((dir + "/plugins/st.lua").c_str(), "w");
            fputs(R"LUA(
isekur.setting("allowed", "", "n", "nicks")
isekur.command("st", "", function(a, srv)
  isekur.set("allowed", "bob carol", srv)
  print(isekur.opt("allowed", srv), isekur.opt("allowed"), pcall(isekur.set, "enabled", "on"),
        pcall(isekur.set, "allowed", "x", "nonet"))
end)
)LUA", f), fclose(f);
            sy.exec(ph, "/plugin enable st");
            sy.exec(yb, "/st");
            std::string out;
            for (auto& l : sy.bufs) if (!l->lines.empty() && l->lines.back().text.find("bob carol") != std::string::npos) out = l->lines.back().text;
            assert(out.find("bob carol  false false") != std::string::npos && out.find("no network nonet") != std::string::npos);
            assert(ini_get(sy.cfg, "plugin.st@y", "allowed") == "bob carol");
            Ini disk;
            assert(ini_load(sy.cfg_path, disk) && ini_get(disk, "plugin.st@y", "allowed") == "bob carol");
        }
        {  // API 13: nick_color (yours is its own color), theme().self, nicklist_colors (the user list painted where the plugin is on)
            FILE* f = fopen((dir + "/plugins/nc.lua").c_str(), "w");
            fputs(R"LUA(
isekur.command("nc", "", function(a, srv)
  print(isekur.nick_color(srv, "bob"), isekur.nick_color(srv, "ME"), isekur.theme().self, isekur.nicklist_colors())
  isekur.nicklist_colors(true)
end)
)LUA", f), fclose(f);
            sy.theme.nicks = {0xff0000, 0x00ff00}, sy.theme.self_nick = 0x0000ff, sy.theme.el["msg_self"].fg = 0xabcdef;
            sy.exec(ph, "/plugin enable nc");
            sy.exec(yb, "/nc");
            assert(ph->lines.back().text == std::to_string(nick_rgb(sy.theme, "bob")) + " " + std::to_string(0x0000ff) + " " + std::to_string(0x0000ff) + " false");
            assert(sy.nicklist_by == "nc" && sy.nicklist_colored(yb) && !sy.nicklist_colored(ph));
            sy.exec(ph, "/set own_nick_color off");  // the theme's own msg_self again
            sy.exec(yb, "/nc");
            assert(ph->lines.back().text == std::to_string(nick_rgb(sy.theme, "bob")) + " " + std::to_string(0xabcdef) + " nil true");
            assert(sy.self_nick(sy.theme) == 0xabcdef);
            sy.exec(ph, "/set own_nick_color on");
            assert(sy.self_nick(sy.theme) == 0x0000ff);
            sy.exec(ph, "/plugin enable nc@other");  // on in another network only: loaded, but not painting here
            sy.exec(ph, "/plugin disable nc");
            assert(sy.nicklist_by == "nc" && !sy.nicklist_colored(yb));
            sy.exec(ph, "/plugin disable nc@other");
            assert(sy.nicklist_by.empty() && !sy.nicklist_colored(yb));  // it went with its plugin
        }
        {  // API 14: isekur.fold (eventshidemgr): on where the plugin is, its names / nicks / quiet settings per channel, gone with the plugin
            FILE* f = fopen((dir + "/plugins/fo.lua").c_str(), "w");
            fputs(R"LUA(
isekur.setting("names", false, "c", "n")
isekur.setting("nicks", true, "c", "k")
isekur.setting("quiet", "hide", "c", "q", {"hide", "fold"})
isekur.command("fo", "", function() print(isekur.fold(), isekur.api >= 14) isekur.fold(true) end)
)LUA", f), fclose(f);
            sy.exec(ph, "/plugin enable fo");
            sy.exec(yb, "/fo");
            assert(ph->lines.back().text == "false true" && sy.fold_by == "fo");
            auto fc = sy.fold_cfg(yb);
            assert(fc.on && !fc.names && fc.nicks && !fc.quiet);
            assert(!sy.fold_cfg(ph).on);  // the console has no network
            sy.exec(ph, "/plugin set fo names on");
            sy.exec(ph, "/plugin set fo@y quiet fold");
            sy.exec(ph, "/plugin set fo@y/#y nicks off");
            fc = sy.fold_cfg(yb);
            assert(fc.on && fc.names && !fc.nicks && fc.quiet);
            {  // /quiet tells the truth: with quiet = fold the plugin's rows stay in a quiet tab, with hide everything is hidden
                sy.exec(yb, "/quiet");
                assert(yb->quiet && yb->lines.back().text.find("except what fo folds") != std::string::npos);
                sy.exec(ph, "/plugin set fo@y quiet hide");
                sy.exec(yb, "/quiet off");
                sy.exec(yb, "/quiet");
                assert(yb->quiet && yb->lines.back().text.find("are hidden here") != std::string::npos && yb->lines.back().text.find("except") == std::string::npos);
                sy.exec(yb, "/quiet off");
                assert(!yb->quiet);
                sy.exec(ph, "/plugin set fo@y quiet fold");
            }
            {  // the kinds that fold: joins, leaves and nicks by default, the others only when asked, each its own setting
                sy.exec(ph, "/plugin set fo@y/#y nicks on");
                fc = sy.fold_cfg(yb);
                assert(fc.joins && fc.leaves && fc.nicks && !fc.hosts && !fc.modes && !fc.kicks && !fc.topics && fc.has(0) && fc.has(2) && !fc.has(4) && !fc.has(7) && !fc.has(-1));
                sy.exec(ph, "/plugin set fo modes on");
                sy.exec(ph, "/plugin set fo@y/#y hosts on");
                fc = sy.fold_cfg(yb);
                assert(fc.modes && fc.hosts && !fc.kicks && !fc.topics && fc.has(3) && fc.has(4) && !fc.has(5));
                // a host change was never a line: it is one now, only where `hosts` is on, and not for you
                Msg hm;
                yb->nicks[ys->lower("alice")] = {};
                size_t before = yb->lines.size();
                assert(irc_parse(":alice!a@h CHGHOST newu new.host", hm));
                sy.handle(ys, hm);
                assert(yb->lines.size() == before + 1 && yb->lines.back().kind == LK::HOST && yb->lines.back().text == "alice changed host to newu@new.host" &&
                       noise(yb->lines.back()) && fold_kind(yb->lines.back()) == 3);
                assert(irc_parse(":" + ys->nick + "!me@h CHGHOST mine my.host", hm));
                before = yb->lines.size();
                sy.handle(ys, hm);
                assert(yb->lines.size() == before);  // yours: just the nicklist's host
                sy.exec(ph, "/plugin set fo@y/#y hosts off");
                assert(irc_parse(":alice!a@h CHGHOST newu again.host", hm));
                sy.handle(ys, hm);
                assert(yb->lines.size() == before);  // off here: no line
                sy.exec(ph, "/plugin set fo@y/#y nicks off");
            }
            {  // fold_kind and noise: what is which kind of event; /quiet hides mode and host lines and leaves kicks and topic changes
                auto L = [](LK k, const std::string& n, const std::string& t, bool self = false) { return Line{0, k, n, t, self, false}; };
                assert(fold_kind(L(LK::JOIN, "a", "a has joined #c")) == 0 && fold_kind(L(LK::QUIT, "a", "a has quit")) == 1 && fold_kind(L(LK::PART, "a", "a has left #c")) == 1);
                assert(fold_kind(L(LK::PART, "a", "a was kicked by op (bye)")) == 5 && fold_kind(L(LK::NICK, "a", "a is now known as b")) == 2 && fold_kind(L(LK::HOST, "a", "a changed host to u@h")) == 3);
                assert(fold_kind(L(LK::MODE, "op", "mode/#c [+o bob] by op")) == 4 && fold_kind(L(LK::MODE, "", "Chat was cleared by a moderator")) == -1);  // Twitch's notices aren't mode changes
                assert(fold_kind(L(LK::TOPIC, "op", "op changed the topic to: x")) == 6 && fold_kind(L(LK::TOPIC, "op", "op removed the topic")) == 6);
                assert(fold_kind(L(LK::TOPIC, "srv", "Topic: op changed the topic to: x")) == -1 && fold_kind(L(LK::TOPIC, "", "No topic is set")) == -1);  // the topic shown on joining isn't a change
                assert(fold_kind(L(LK::JOIN, "me", "You have joined #c", true)) == -1 && fold_kind(L(LK::MODE, "me", "mode/#c [+o x] by me", true)) == -1 && fold_kind(L(LK::MSG, "a", "hi")) == -1);
                assert(noise(L(LK::MODE, "op", "mode/#c [+o bob] by op")) && noise(L(LK::HOST, "a", "a changed host to u@h")) && noise(L(LK::JOIN, "a", "a has joined #c")));
                assert(!noise(L(LK::PART, "a", "a was kicked by op")) && !noise(L(LK::TOPIC, "op", "op changed the topic to: x")) && !noise(L(LK::MODE, "me", "mode/#c [+o x] by me", true)));
            }
            Buffer* yq = sy.open(ys, BT::QUERY, "bob");
            assert(!sy.fold_cfg(yq).on);  // channels only
            sy.exec(ph, "/plugin enable fo@other");
            sy.exec(ph, "/plugin disable fo");  // on in another network only: loaded, not folding here
            assert(sy.fold_by == "fo" && !sy.fold_cfg(yb).on);
            sy.exec(ph, "/plugin disable fo@other");
            assert(sy.fold_by.empty() && !sy.fold_cfg(yb).on);  // it went with its plugin
        }
        {  // API 15: isekur.tint (twilight): multiplies every color on both screens, glides, and never jumps when its plugin goes
            FILE* f = fopen((dir + "/plugins/tn.lua").c_str(), "w");
            fputs(R"LUA(
isekur.command("tn", "", function(a)
  if a == "off" then isekur.tint(false)
  elseif a == "slow" then isekur.tint{r = 1, g = 0.5, b = 0.25, dim = 0.2, fade = 60, out = 30}
  else print(tostring(isekur.tint()), isekur.api >= 15) isekur.tint{r = 1, g = 0.5, b = 0.25, dim = 0.2} end
end)
isekur.command("pn", "", function(a)
  if a == "off" then isekur.panel(false)
  else isekur.panel{lines = {"one", "two\nx", "three", "four"}, sky = 0.52, actions = {{"Preview", "/pn go"}, {"Bad", "/exec ls"}, {"Other", "/tn x"}, {"No slash", "pn"}}} end
end)
)LUA", f), fclose(f);
            sy.exec(ph, "/plugin enable tn");
            sy.tint_instant_ms = 0;
            sy.exec(yb, "/tn");  // no fade asked: at once
            assert(ph->lines.back().text == "false true" && sy.tint.by == "tn" && sy.tint.on() && sy.tint.now[1] == 0.5 && sy.tint.now[3] == 0.2);
            {  // isekur.panel: up to 3 lines of one line each, a sky in steps of 1/20, buttons that run the plugin's own commands only
                sy.exec(yb, "/pn");
                auto infos = sy.plugin_info();
                const PluginInfo* tp = nullptr;
                for (auto& pi : infos) if (pi.name == "tn") tp = &pi;
                assert(tp && tp->has_panel() && tp->panel_lines.size() == 3 && tp->panel_lines[1] == "two x" && tp->panel_sky == 0.5);
                assert(tp->panel_actions.size() == 2 && tp->panel_actions[0].first == "Preview" && tp->panel_actions[1].second == "/tn x");  // /exec, and a word that isn't a command, are left out
                sy.exec(yb, "/pn off");
                for (auto& pi : sy.plugin_info()) if (pi.name == "tn") assert(!pi.has_panel());
            }
            Tint tt = sy.tint;
            assert(tint_color(0xffffff, tt) == 0xcc6633 && tint_color(-1, tt) == -1);  // 255 * (1, .5, .25) * (1 - .2)
            // blue on black had 2.4:1: the tint dims it to less, so it is moved toward white until it has the 2.4 again
            auto lum = [](RGB c) { auto f = [](int v) { double x = v / 255.0; return x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4); }; return 0.2126 * f(c >> 16 & 255) + 0.7152 * f(c >> 8 & 255) + 0.0722 * f(c & 255); };
            auto cr = [&](RGB a, RGB b) { double x = lum(a) + 0.05, y = lum(b) + 0.05; return x > y ? x / y : y / x; };
            RGB tf = tint_fg(0x0000ff, 0x000000, tt);
            assert(cr(0x0000ff, 0) < 2.5 && cr(tf, 0) >= cr(0x0000ff, 0) - 0.01 && tf != tint_color(0x0000ff, tt));
            assert(tint_fg(0xffffff, 0x000000, tt) == tint_color(0xffffff, tt));  // white on black still reads: just tinted
            sy.theme.el["default"] = Style{0xffffff, 0x101010, 0};
            Web tw(sy);
            std::string tj = tw.theme_json();  // the webchat gets the colors the terminal draws, and the tint for the mIRC ones
            assert(tj.find("\"tint\":[1.0000,0.5000,0.2500,0.2000,1]") != std::string::npos && tj.find("\"bg\":\"#0d0603\"") != std::string::npos);
            {  // the status bar's items (uptime) are painted by the page with the colors it is given: tinted here, and left alone for a terminal-only tint
                std::string si = status_info_json(sy);
                sy.tint.web = false;
                std::string si0 = status_info_json(sy);
                sy.tint.web = true;
                assert(si0.find("#101010") != std::string::npos && si.find("#101010") == std::string::npos && si.find("#0d0603") != std::string::npos);
            }
            sy.tint.media = false;
            assert(tw.theme_json().find("\"tint\":[1.0000,0.5000,0.2500,0.2000,0]") != std::string::npos);  // `media`: the pictures and videos stay as they are
            sy.tint.media = true;
            sy.tint.web = false;
            assert(tw.theme_json().find("\"tint\":null") != std::string::npos && tw.theme_json().find("#0d0603") == std::string::npos);  // `where`: terminal only
            sy.tint.web = true;
            // a fade: halfway through, half way there (smoothstep); at the end exactly there. The plugin going fades out the same way.
            // Time passes by moving the fade's start back, as the real clock would
            auto elapse = [&](long long ms) { sy.tint.t0 -= ms; sy.tint_step(now_ms()); };
            sy.exec(yb, "/tn off");
            sy.exec(yb, "/tn slow");  // fade 60, out 30, from none
            elapse(61000);
            assert(sy.tint.now[1] == 0.5 && sy.tint.now[3] == 0.2);  // there, exactly
            sy.exec(ph, "/plugin disable tn");  // out = 30 s
            assert(sy.tint.by.empty() && sy.tint.on() && std::fabs(sy.tint.now[1] - 0.5) < 0.02);  // not a jump
            elapse(15000);
            assert(std::fabs(sy.tint.now[1] - 0.75) < 0.02 && std::fabs(sy.tint.now[3] - 0.1) < 0.02 && sy.tint.on());
            elapse(16000);
            assert(!sy.tint.on() && sy.tint.now[1] == 1 && sy.tint.now[3] == 0);  // gone, exactly
            // a first tint from none, with a fade asked: it fades in
            sy.exec(ph, "/plugin enable tn");
            sy.exec(yb, "/tn slow");
            assert(sy.tint.by == "tn" && sy.tint.now[1] > 0.99);  // not there yet
            elapse(30000);
            assert(std::fabs(sy.tint.now[1] - 0.75) < 0.02);
            {  // a new fade starts from where this one is in time, not from the last step drawn: 59 s in, it is all but there
                double at[4];
                sy.tint_at(now_ms() + 29000, at);
                assert(at[1] < 0.505 && at[1] > 0.5 - 1e-9 && sy.tint.now[1] > 0.7);
            }
            sy.tint_instant_ms = 1LL << 40;  // "just started" (the test has run longer than the real window): a first tint shows at once, a fade in from bright would flash
            sy.exec(ph, "/plugin disable tn");
            elapse(31000);
            assert(!sy.tint.on());
            sy.exec(ph, "/plugin enable tn");
            sy.exec(yb, "/tn slow");
            assert(sy.tint.now[1] == 0.5);  // at once
            sy.exec(ph, "/plugin disable tn");
        }
        sy.plugins.clear();
        assert(system(("rm -rf '" + dir + "'").c_str()) == 0);
    }
    {  // speech host API: isekur.run only starts speech engines (no shell), which/platform, speak_web without a client
        char d[] = "/tmp/isekur-speech-XXXXXX";
        assert(mkdtemp(d));
        std::string dir = d;
        Core sp;
        sp.cfg_path = dir + "/isekur.conf";
        mkdir_p(dir + "/plugins");
        FILE* o = fopen((dir + "/plugins/spk.lua").c_str(), "w");
        fputs(R"LUA(
isekur.command("spk", "", function()
  print(select(2, pcall(isekur.run, {"festival"}, "(system)")))
  print(isekur.platform() ~= "", isekur.which("sh") ~= nil, isekur.which("no-such-program-xyz") == nil, isekur.which("a/b") == nil)
  print(select(2, pcall(isekur.run, {"sh", "-c", "true"}, "")))
  print(select(2, pcall(isekur.run, {}, "")))
  isekur.speak_web("hello", "es")
  print("spoke")
end)
isekur.command("sp", "", function() print(isekur.run({"espeak", "-v", "x"}, "hello there")) end)
)LUA", o), fclose(o);
        Buffer* ph = sp.open(nullptr, BT::STATUS, "isekur");
        sp.exec(ph, "/plugin enable spk");
        sp.exec(ph, "/spk");
        auto out = [&](size_t back) { return ph->lines[ph->lines.size() - back].text; };
        assert(out(5).find("festival runs only as") != std::string::npos);
        assert(out(4) == "true true true true");
        assert(out(3).find("sh isn't a speech engine") != std::string::npos);
        assert(out(2).find("(nothing) isn't a speech engine") != std::string::npos);
        assert(out(1) == "spoke");
        // a stand-in espeak on PATH: isekur.run starts it with the text on its stdin
        std::string fake = dir + "/bin", got = dir + "/got", oldpath = getenv("PATH") ? getenv("PATH") : "";
        mkdir_p(fake);
        FILE* fe = fopen((fake + "/espeak").c_str(), "w");
        fputs(("#!/bin/sh\ncat > '" + got + "'\n").c_str(), fe), fclose(fe);
        chmod((fake + "/espeak").c_str(), 0755);
        setenv("PATH", (fake + ":" + oldpath).c_str(), 1);
        sp.exec(ph, "/sp");
        assert(out(1) == "true");
        std::string text;
        for (int i = 0; i < 200 && text.empty(); i++) {
            usleep(10000);
            if (FILE* g = fopen(got.c_str(), "r")) { char b[256] = {0}; text = fgets(b, sizeof b, g) ? b : "", fclose(g); }
        }
        assert(text == "hello there");
        setenv("PATH", oldpath.c_str(), 1);
        sp.plugins.clear();
        assert(system(("rm -rf '" + dir + "'").c_str()) == 0);
    }
    {  // the plugin radio (API 12): what it refuses, ffmpeg's MP3 to the sink and a player, mute, direct, stop; isekur.window and its events
        char d[] = "/tmp/isekur-radio-XXXXXX";
        assert(mkdtemp(d));
        std::string dir = d, fake = dir + "/bin", oldpath = getenv("PATH") ? getenv("PATH") : "";
        mkdir_p(fake), mkdir_p(dir + "/plugins");
        auto script = [&](const std::string& name, const std::string& body) {  // the test's PATH holds only these; the script gets the system's too (OpenBSD's ksh has no printf builtin)
            FILE* f = fopen((fake + "/" + name).c_str(), "w");
            fputs(("#!/bin/sh\nPATH=$PATH:/usr/bin:/bin\n" + body + "\n").c_str(), f), fclose(f);
            chmod((fake + "/" + name).c_str(), 0755);
        };
        auto slurp = [](const std::string& path) { std::ifstream in(path, std::ios::binary); return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); };
        Core rc;
        rc.cfg_path = dir + "/isekur.conf";
        Buffer* rh = rc.open(nullptr, BT::STATUS, "isekur");
        setenv("PATH", fake.c_str(), 1);
        const std::string url = "https://example.org/a.mp3";
        assert(rc.radio_start("t", url, "A", false).find("ffmpeg isn't installed") == 0);
        // fd 3 keeps ffmpeg's stdin for the background cat: dash and FreeBSD sh point a background job's stdin at /dev/null before "<&0" runs
        script("ffmpeg", "printf '%s\\n' \"$@\" > '" + dir + "/args'\nexec 3<&0\n/bin/cat <&3 > '" + dir + "/stdin' &\nprintf 'AUDIO-1'\nexec /bin/sleep 30");
        // addresses it won't fetch: nothing local or private, no user:pass@, no spaces, only http(s)
        for (const char* bad : {"file:///etc/passwd", "ftp://example.org/x", "http://127.0.0.1/x", "http://10.1.2.3/x", "http://localhost/x",
                                "https://u:p@example.org/x", "https://example.org:80@127.0.0.1/x", "https://example.org:pw/x", "https://example.org:/x", "https://example.org/a b", "https://printer.local/x", "-i", ""})
            assert(!rc.radio_start("t", bad, "x", false).empty() && !rc.radio_on());
        assert(rc.radio_start("t", "https://www.youtube.com/@x/live", "Y", false).find("yt-dlp isn't installed") == 0);
        assert(rc.radio_start("t", url, "A", true).find("no player") == 0);
        auto pump = [&](int ms, const std::function<bool()>& until) {
            for (int i = 0; i < ms / 20 && !until(); i++) {
                std::vector<pollfd> fds;
                rc.radio_fds(fds);
                poll(fds.data(), (nfds_t)fds.size(), 20);
                rc.radio_poll(fds, 0);
            }
        };
        std::string got;
        int ended = 0;
        rc.radio_sink = [&](const char* b, size_t n) { if (b) got.append(b, n); else ended++; };
        // ffmpeg is told exactly what to do, and its MP3 reaches the sink
        assert(rc.radio_start("t", url, "A", false).empty() && rc.radio.st == Core::Radio::RESOLVING);
        pump(3000, [&] { return rc.radio.st == Core::Radio::PLAYING; });
        assert(rc.radio.st == Core::Radio::PLAYING && got == "AUDIO-1" && rc.radio.name == "A" && !rc.radio.direct);
        std::string args = slurp(dir + "/args");
        assert(args.find("-protocol_whitelist\nhttp,https,tcp,tls,crypto\n") != std::string::npos && args.find("-i\n" + url + "\n") != std::string::npos &&
               args.find("libmp3lame") != std::string::npos && args.find("volume@v=1.000:eval=frame,alimiter=limit=0.95\n") != std::string::npos &&
               args.find("-nostdin") == std::string::npos);  // stdin is ours: live volume commands
        // the volume changes while it plays: ffmpeg's stdin gets the command; a new station starts at the volume it was given
        rc.radio_volume(50);
        assert(rc.radio.volume == 50);
        pump(1500, [&] { return slurp(dir + "/stdin").find("cvolume@v -1 volume 0.500\n") != std::string::npos; });
        assert(slurp(dir + "/stdin") == "cvolume@v -1 volume 0.500\n");
        rc.radio_volume(999);
        assert(rc.radio.volume == 150);  // 0-150
        rc.radio_volume(-1);
        assert(rc.radio.volume == 0);
        rc.radio.volume = 80;
        // the player on this machine gets the same bytes, until it's muted
        script("mpv", "/bin/cat > '" + dir + "/played'");
        got.clear();
        assert(rc.radio_start("t", url, "A", true).empty());
        pump(3000, [&] { return rc.radio.st == Core::Radio::PLAYING; });
        pump(2000, [&] { return slurp(dir + "/played") == "AUDIO-1"; });
        assert(slurp(dir + "/played") == "AUDIO-1" && rc.radio.host);
        rc.radio_mute(true);
        assert(rc.radio.muted);
        rc.radio_mute(false);
        rc.radio_stop();
        assert(!rc.radio_on() && ended >= 1);
        pump(3000, [&] { return rc.radio.dying.empty(); });
        assert(rc.radio.dying.empty());  // ffmpeg and the player are gone, reaped
        // direct: nothing runs here, the browsers connect; a refused one leaves the one that plays alone
        assert(rc.radio_start("t", "https://example.org/live", "D", false, true).empty());
        assert(rc.radio.direct && rc.radio.st == Core::Radio::PLAYING && rc.radio.ff < 0 && rc.radio.res < 0);
        std::string si = status_info_json(rc);  // the status bar says it: R: ON (MUTE when muted), nothing when it's off
        assert(si.find("[\"R: \",") != std::string::npos && si.find("[\"ON\",") != std::string::npos);
        rc.radio_mute(true);
        assert(status_info_json(rc).find("[\"MUTE\",") != std::string::npos);
        rc.radio_mute(false);
        assert(!rc.radio_start("t", "https://www.youtube.com/@x/live", "Y", false, true).empty());
        assert(!rc.radio_start("t", "http://example.org/live", "H", false, true).empty() && rc.radio.direct && rc.radio.name == "D");
        rc.radio_stop();
        assert(!rc.radio.direct && !rc.radio_on());
        assert(status_info_json(rc).find("\"R: \"") == std::string::npos);
        // the plugin side: isekur.radio refuses and reports, isekur.window and its events (only to its plugin), going with the plugin
        auto wr = [&](const std::string& name, const std::string& body) {
            FILE* f = fopen((dir + "/plugins/" + name + ".lua").c_str(), "w");
            fputs(body.c_str(), f), fclose(f);
        };
        wr("rw", R"LUA(
isekur.command("w", "", function()
  isekur.window{title = "T", display = {"a", "b"}, items = {"x", "y", "z"}, sel = 2, mark = 1, star = 3, buttons = {"Go", "Quit"}, icons = {"1", "2"}, reels = true, slider = {value = 40, max = 150, step = 10, icon = "v"}}
end)
isekur.command("r", "", function()
  print(select(2, isekur.radio("file:///x")), isekur.radio().state, isekur.radio_mute(true), isekur.window())
end)
isekur.command("v", "", function() print(isekur.radio_volume(), isekur.radio_volume(200), isekur.radio_volume(-5), isekur.radio().volume) end)
isekur.command("dstart", "", function() print(isekur.radio("https://radio.example.com/live", {name = "D", direct = true})) end)
isekur.on("window", function(e) print("win", e.action, e.index, e.button, e.value) end)
)LUA");
        wr("ro", "isekur.on(\"window\", function(e) print(\"ro got it\") end)\n");
        rc.exec(rh, "/plugin enable rw");
        rc.exec(rh, "/plugin enable ro");
        {  // the suggestions (terminal and webchat) list the commands of the plugins that are on
            auto pc = rc.plugin_commands();
            assert(std::find(pc.begin(), pc.end(), std::array<std::string, 3>{"w", "", "rw"}) != pc.end());
            rc.exec(rh, "/help w");  // its usage and the plugin it comes from, not "No such command"
            assert(rh->lines[rh->lines.size() - 2].text.find("a command of the rw plugin") != std::string::npos);
            rc.exec(rh, "/plugin disable rw");
            pc = rc.plugin_commands();
            assert(std::find(pc.begin(), pc.end(), std::array<std::string, 3>{"w", "", "rw"}) == pc.end());
            rc.exec(rh, "/plugin enable rw");
        }
        auto last = [&] { return rh->lines.back().text; };
        rc.exec(rh, "/r");
        assert(last() == "not a stream address (http or https, a host on the internet) off false false");
        rc.radio.volume = 100;
        rc.exec(rh, "/v");
        assert(last() == "100 150 0 0");  // nothing plays: the volume is set for the next one
        {  // a direct stream runs in the browsers: only they can say it didn't play, and only about the stream that plays
            rc.exec(rh, "/dstart");
            assert(last() == "true" && rc.radio.direct && rc.radio.st == Core::Radio::PLAYING && rc.radio.url == "https://radio.example.com/live");
            assert(!rc.radio_client_fail("https://radio.example.com/another", "late") && rc.radio.st == Core::Radio::PLAYING);  // another station's report: ignored
            assert(rc.radio_client_fail("https://radio.example.com/live", "the browser can't play\tthis kind of stream"));
            assert(rc.radio.st == Core::Radio::OFF && rc.radio.err == "the browser can't play this kind of stream" && !rc.radio.direct);
            assert(!rc.radio_client_fail("https://radio.example.com/live", "again"));  // nothing plays now
            rc.exec(rh, "/dstart");
            assert(rc.radio_client_fail("https://radio.example.com/live", "") && rc.radio.err == "the browser couldn't play it");
            rc.exec(rh, "/dstart");
            rc.radio_stop();
            assert(!rc.radio_client_fail("https://radio.example.com/live", "after a stop"));
        }
        rc.exec(rh, "/w");
        const Core::PWin& w = rc.pwin;
        assert(w.open && w.by == "rw" && w.title == "T" && w.display.size() == 2 && w.items.size() == 3 && w.sel == 2 && w.mark == 1 && w.star == 3 && w.slider == 40 && w.slider_max == 150 && w.slider_step == 10 && w.slider_icon == "v" &&
               w.buttons == std::vector<std::string>({"Go", "Quit"}) && w.icons == std::vector<std::string>({"1", "2"}) && w.reels == 1);
        rc.pwin_act("button", 3, 2);
        assert(last() == "win button 3 Quit nil" && rc.pwin.sel == 3);
        rc.pwin_act("pick", 1);
        assert(last() == "win pick 1 nil nil");
        rc.pwin_act("button", 1, 9);  // no such button: nothing happens
        assert(last() == "win pick 1 nil nil");
        rc.pwin_act("slider", 65);
        assert(last() == "win slider nil nil 65" && rc.pwin.slider == 65);
        rc.pwin_act("slider", 9999);
        assert(last() == "win slider nil nil 150" && rc.pwin.slider == 150);  // up to its max
        for (auto& l : rh->lines) assert(l.text != "ro got it");  // the other plugin's handler never ran
        rc.pwin_act("close", 0);
        assert(last() == "win close nil nil nil" && !rc.pwin.open);
        rc.exec(rh, "/w");
        assert(rc.pwin.open);
        rc.pwin_act("back", 0);
        assert(last() == "win close nil nil nil");  // a window without tabs has no use for Backspace: nothing was sent
        rc.exec(rh, "/plugin disable rw");
        assert(!rc.pwin.open);  // its plugin went: the window closes
        {  // API 17: tabs, a second line under each row, a search field, the list's name; isekur.json_decode; isekur.http's headers
            wr("rv", R"LUA(
isekur.command("va", "", function()
  isekur.window{title = "V2", items = {"a", "b", "c"}, subs = {"one", "two"}, tabs = {"One", "Two", "Three"}, tab = 2, search = "Find stations", query = "ja\tzz", list = "L1", buttons = {"Go"}}
end)
isekur.command("vb", "", function() isekur.window{title = "V3", items = {"a"}, tabs = {"Only"}, tab = 9, search = ("x"):rep(100), list = "\xc3\xa9"} end)
isekur.on("window", function(e) print("v", e.action, e.index, e.text) end)
isekur.command("jsonchk", "", function()
  local v = isekur.json_decode('{"a":[1,2.5,"x\\u00e9\\ud83d\\ude00",null,true],"b":null,"c":{"d":-3e2,"e":"\\n\\"q\\""},"f":12345678901234}')
  assert(#v.a == 5 and v.a[1] == 1 and math.type(v.a[1]) == "integer" and v.a[2] == 2.5 and v.a[3] == "x\xc3\xa9\xf0\x9f\x98\x80" and v.a[4] == false and v.a[5] == true)
  assert(v.b == nil and v.c.d == -300 and v.c.e == '\n"q"' and v.f == 12345678901234)
  assert(#isekur.json_decode(" [] ") == 0 and isekur.json_decode("{}") ~= nil and isekur.json_decode('"\\ud800"') == "\xef\xbf\xbd" and isekur.json_decode("null") == nil)
  for _, bad in ipairs{"", "{", '{"a":}', "[1,]", "tru", '"x', "1 2", "[" .. ("["):rep(40), '{"a" 1}', "-", "1.", '"\\q"'} do
    local r, why = isekur.json_decode(bad)
    assert(r == nil and type(why) == "string", bad)
  end
  local ok, why = isekur.http("https://x.test/", function() end, nil, {headers = {["X-Foo"] = "1"}})
  assert(ok == false and why == "header X-Foo isn't allowed: only User-Agent and Accept, one line", why)
  ok, why = isekur.http("https://x.test/", function() end, nil, {headers = {["User-Agent"] = "a\r\nb: c"}})
  assert(ok == false and why:find("isn't allowed"))
  print("json ok")
end)
)LUA");
            rc.exec(rh, "/plugin enable rv");
            rc.exec(rh, "/va");
            assert(w.open && w.by == "rv" && w.tabs == std::vector<std::string>({"One", "Two", "Three"}) && w.tab == 2 && w.subs == std::vector<std::string>({"one", "two", ""}) &&
                   w.search == "Find stations" && w.query == "ja zz" && w.list == "L1");
            rc.pwin_act("tab", 3);
            assert(last() == "v tab 3 nil" && rc.pwin.tab == 3);
            rc.pwin_act("tab", 9);  // no such tab
            assert(last() == "v tab 3 nil" && rc.pwin.tab == 3);
            rc.pwin_act("search", 0, 0, "  smooth\tjazz ");  // trimmed, no control characters; both screens show it until the plugin says otherwise
            assert(last() == "v search nil smooth jazz" && rc.pwin.query == "smooth jazz");
            rc.pwin_act("back", 0);
            assert(last() == "v back nil nil");
            rc.pwin_act("search", 0, 0, std::string(119, 'a') + "\xc3\xa9");  // 121 bytes: cut before the e acute, not through it
            assert(last() == "v search nil " + std::string(119, 'a') && rc.pwin.query == std::string(119, 'a'));
            rc.exec(rh, "/vb");  // a tab past the last one is the last; the placeholder is cut
            assert(w.tab == 1 && w.search == std::string(60, 'x') && w.list == "\xc3\xa9" && w.subs.empty() && w.query.empty());
            rc.exec(rh, "/jsonchk");
            assert(last() == "json ok");
            rc.pwin_act("close", 0);
            rc.exec(rh, "/plugin disable rv");
        }
        rc.plugins.clear();
        setenv("PATH", oldpath.c_str(), 1);
        assert(system(("rm -rf '" + dir + "'").c_str()) == 0);
    }
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
  if args:sub(1, 4) == "run " then isekur.exec(nil, nil, "/" .. args:sub(5)) return end
  if args == "usage" then return "Usage: /ptest thing" end
  print("ran " .. args .. " in " .. target)
end)
isekur.on("message", function(e)
  if e.text:match("^embed") then  -- now (the line isn't added yet) and once a fetch answers
    assert(isekur.embed(e.srv, e.target, e.id, {type = "audio", url = "https://x.test/a.mp3", n = 3, skip = {}}))
    assert(not isekur.embed(e.srv, e.target, "999999", {type = "x"}))
    local ok, why = isekur.http("ftp://x.test/", function() end)
    assert(not ok and why:find("http(s)://", 1, true))
    assert(isekur.http(e.text:match("^embed (.*)"), function(r)
      isekur.embed(e.srv, e.target, e.id, {type = "link", url = r.url, title = r.error or "?"})
    end))
  end
  return e.text == "hideme"
end)
isekur.on("raw", function(e) return e.cmd == "999" and e.args == "me a :b c" and isekur.nick(e.srv) == "pnick" end)
isekur.on("paste", function(e) return e.text == "take\nthis" end)
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
        for (const char* blocked : {"exec", "sh", "alias", "perform"}) {  // no programs: the sandbox has no way around through a command
            size_t tabs = pc.bufs.size();
            pc.exec(ph, std::string("/ptest run ") + blocked + " true");
            assert(last(ph) == std::string("/") + blocked + " can't be run by a plugin" && pc.jobs.empty() && pc.bufs.size() == tabs);
        }
        for (const char* key : {"exec_web", "dcc_dir", "web.password_hash", "aliases.x", "server.sn.perform"}) {  // nor by switching them on
            std::string was = pc.cfg["general"]["exec_web"];
            pc.exec(ph, std::string("/ptest run set ") + key + " on");
            assert(last(ph) == std::string("/set ") + key + " can't be run by a plugin" && pc.cfg["general"]["exec_web"] == was);
        }
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
        // API 4: a hidden private message leaves no new query tab; raw numerics (args as on the wire); pastes; isekur.nick
        size_t nb = pc.bufs.size();
        assert(irc_parse(":carol!u@h PRIVMSG " + ps->nick + " :hideme", pm)), pc.handle(ps, pm);
        assert(pc.bufs.size() == nb && !pc.find(ps, "carol"));
        ps->cfg.nick = "pnick";
        size_t s0 = ps->status->lines.size();
        assert(irc_parse(":srv 999 me a :b c", pm)), pc.handle(ps, pm);
        assert(ps->status->lines.size() == s0);
        SS was = ps->state;
        ps->state = SS::CONNECTED;
        assert(pc.plugin_hooks("paste", ps, "#chan"));
        size_t c0 = pchan->lines.size();
        pc.exec(pchan, "take\nthis");
        assert(pchan->lines.size() == c0);
        ps->state = was;
        // isekur.embed: on the line its message event is for, then (isekur.http answered) one more. http refuses the LAN
        for (std::string url : {"https://127.0.0.1/", "http://192.168.1.1:3366/r/x", "https://[::1]:8443/x", "https://10.1.2.3/", "https://localhost/"}) {
            assert(irc_parse(":bob!u@h PRIVMSG #chan :embed " + url, pm)), pc.handle(ps, pm);
            Line& el = pchan->lines.back();
            assert(el.id && el.embeds == R"({"n":3,"type":"audio","url":"https://x.test/a.mp3"})");
            for (int i = 0; i < 500 && el.embeds.find("link") == std::string::npos; i++) usleep(10000), pc.plugins_poll();
            assert(el.embeds.find("private network") != std::string::npos);
        }
        auto pub = [](const char* ip) {
            sockaddr_storage ss{};
            if (inet_pton(AF_INET, ip, &((sockaddr_in*)&ss)->sin_addr) == 1) ss.ss_family = AF_INET;
            else assert(inet_pton(AF_INET6, ip, &((sockaddr_in6*)&ss)->sin6_addr) == 1), ss.ss_family = AF_INET6;
            return public_addr((sockaddr*)&ss);
        };
        assert(pub("8.8.8.8") && pub("172.32.0.1") && pub("100.128.0.1") && pub("2606:4700::1111"));
        for (auto ip : {"0.0.0.0", "127.0.0.1", "10.0.0.1", "172.16.0.1", "172.31.255.255", "192.168.1.1", "169.254.169.254",
                        "100.64.0.1", "224.0.0.1", "::", "::1", "fd00::1", "fe80::1", "::ffff:192.168.0.1", "ff02::1"})
            assert(!pub(ip));
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
        assert(plugin_header("-- version: 1.2\n-- author:  Ann B \r\n-- contact: a@b.c\nx = 1\n", "author") == "Ann B" &&
               plugin_header("-- version: 1.2\n-- contact: a@b.c\n", "contact") == "a@b.c" && plugin_header("x = 1\n-- author: late\n", "contact").empty());
        assert(sha256_of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        // old keys (and the comments over them) out of a config's text
        assert(ini_drop_keys("[general]\n# about it\n# more\nauto_away = on\nnick = x\n[server.a]\n; auto_away = off ; c\nhost = h\n", {"auto_away"}) ==
               "[general]\nnick = x\n[server.a]\nhost = h\n");
        // a comment line of nothing but #s and ;s used to throw out of the migration (substr at npos)
        assert(ini_drop_keys("[general]\n#\nnick = x\nauto_away = on\n;;\n########\n", {"auto_away"}) == "[general]\n#\nnick = x\n;;\n########\n");
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
        // the catalog's own plugins, from a checkout of github.com/gcanosa/isekur-plugins: isekur-plugins/ here
        // (CI puts it there) or next to iSekur. Without one this part is skipped; their versions come from its index
        std::string cdir;
        for (std::string d : {"isekur-plugins", "../isekur-plugins"}) {
            char rp[4096];
            if (access((d + "/index.json").c_str(), F_OK) == 0 && realpath(d.c_str(), rp)) { cdir = rp; break; }
        }
        if (cdir.empty()) puts("no isekur-plugins checkout here or next to iSekur: skipping the catalog plugins' tests");
        else {
        // autoaway from that catalog (so its sha256 in index.json must be right), with the old core settings
        // moved into it and taken out of the config
        setenv("ISEKUR_PLUGIN_URL", ("file://" + cdir + "/").c_str(), 1);
        std::map<std::string, std::string> ver;  // name -> its version in the catalog
        {
            std::string err;
            Ini old{{"general", {{"auto_away", "on"}, {"away_message", "brb"}, {"auto_away_idle", "0"}}},
                    {"server.sn", {{"auto_away", "off"}, {"away_announce", "on"}}}};
            assert(ini_save(pc.cfg_path, old, err));
            for (auto& [sec, kv] : old) for (auto& [k, v] : kv) pc.cfg[sec][k] = v;
        }
        {  // the repo's catalog: every entry's file says the same version, and has the same sha256
            std::ifstream f(cdir + "/index.json");
            std::stringstream ss;
            ss << f.rdbuf();
            bool iok;
            auto idx = plugin_index(ss.str(), &iok);
            assert(iok && !idx.empty());
            for (auto& e : idx) {
                ver[e["name"]] = e["version"];
                std::ifstream lf(cdir + "/" + e["name"] + "/" + e["name"] + ".lua");
                std::stringstream ls;
                ls << lf.rdbuf();
                assert(plugin_version(ls.str()) == e["version"] && sha256_of(ls.str()) == e["sha256"]);
            }
        }
        assert(old_away_keys(pc));
        pc.exec(ph, "/plugin install autoaway");
        wait_catalog();
        assert(said("autoaway: moved 6 auto-away settings") && said("autoaway " + ver["autoaway"] + " installed and on") && !old_away_keys(pc));
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
            assert(aw && aw->state == "on" && aw->version == ver["autoaway"] && aw->local == ver["autoaway"] && !aw->update && !aw->edited && !aw->desc.empty());
            assert(std::find(aw->on_in.begin(), aw->on_in.end(), "") != aw->on_in.end() && std::find(aw->on_in.begin(), aw->on_in.end(), "sn") != aw->on_in.end());
            assert(aw->sets.size() == 4 && aw->sets[0].help.find("minutes") == 0 && aw->sets[0].key == "idle" && aw->sets[0].scope == 'n' && aw->sets[3].key == "announce_back" && aw->sets[3].flag);
            auto cat = pc.catalog_rows();
            assert(cat.size() >= 3 && cat[0].name == "autoaway" && cat[0].installed && !cat[0].update && !cat[0].edited && !cat[0].too_new);
            assert(!cat[1].installed && cat[1].version == ver[cat[1].name]);
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
            for (auto& l : h2->lines) listed |= l.text.find("autoaway\x02 " + ver["autoaway"]) != std::string::npos && l.text.find("[installed]") != std::string::npos;
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
            assert(ri[0].name == "autoaway" && ri[0].edited && !ri[0].update && ri[0].local == ver["autoaway"]);
            pc.exec(ph, "/plugin update");
            wait_catalog();
            assert(said("Your plugins are up to date with the catalog"));
            pc.exec(ph, "/plugin update autoaway");
            wait_catalog();
            assert(said("autoaway " + ver["autoaway"] + " restored"));
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
            setenv("ISEKUR_PLUGIN_URL", ("file://" + cdir + "/").c_str(), 1);
        }
        // remove: the file, its data and its sections
        pc.exec(ph, "/plugin remove autoaway");
        assert(last(ph) == "autoaway removed, with its settings and saved data" && access((pdir + "/autoaway.lua").c_str(), F_OK) != 0);
        assert(!pc.cfg.count("plugin.autoaway") && !pc.cfg.count("plugin.autoaway@sn"));
        // awaylog: highlights and PMs while detached, one summary (INFO badge) in the tab you're on once you're back
        pc.exec(ph, "/plugin install awaylog");
        wait_catalog();
        assert(said("awaylog " + ver["awaylog"] + " installed and on"));
        pc.select(pchan);
        assert(irc_parse(":srv 305 me :back", pm)), pc.handle(ps, pm);  // the /away lunch above: back first
        pc.set_attached(false), pc.detached_at = 0;
        pc.plugin_emit("tick", nullptr, "");  // it sees you go
        assert(irc_parse(":bob!u@h PRIVMSG #chan :me: are you there?", pm)), pc.handle(ps, pm);
        assert(irc_parse(":bob!u@h PRIVMSG #chan :nothing for you", pm)), pc.handle(ps, pm);
        assert(irc_parse(":carol!u@h PRIVMSG me :psst", pm)), pc.handle(ps, pm);
        assert(irc_parse(":carol!u@h PRIVMSG me :\x01" "ACTION waves\x01", pm)), pc.handle(ps, pm);
        pc.plugin_emit("tick", nullptr, "");  // still away: nothing shown
        size_t before_back = ps->status->lines.size(), before_chan = pchan->lines.size();
        pc.set_attached(true);
        pc.plugin_emit("tick", nullptr, "");
        {  // the summary in the network's console, one pointer to it in the tab you're on
            auto& sl = ps->status->lines;
            std::vector<std::string> got;
            for (size_t i = before_back; i < sl.size(); i++) got.push_back(sl[i].text);
            assert(got.size() == 4 && sl[before_back].badge == "awaylog" && sl[before_back].badge_kind == 'i');
            assert(sl[before_back + 1].badge == "awaylog" && sl[before_back + 1].badge_kind == 'p');
            assert(pchan->lines.size() == before_chan + 1 && pchan->lines.back().badge_kind == 'i');
            assert(pchan->lines.back().text.rfind("3 messages for you while you were away, in the console of ", 0) == 0);
            assert(got[0].rfind("While you were away (", 0) == 0 && got[0].find("): 1 mention in #chan, 2 PMs from carol") != std::string::npos);
            assert(got[1].find("#chan <bob> me: are you there?") != std::string::npos && got[2].find("<carol> psst") != std::string::npos);
            assert(got[3].find("* carol waves") != std::string::npos);
        }
        pc.plugin_emit("tick", nullptr, "");  // once
        size_t n_after = pchan->lines.size();
        pc.exec(ps->status, "/awaylog");  // again, in full
        assert(ps->status->lines.back().text.find("* carol waves") != std::string::npos && pchan->lines.size() == n_after);
        assert(ps->status->lines[ps->status->lines.size() - 4].text.find("2 PMs from carol on " + ps->status->name) != std::string::npos);  // /awaylog names the network
        pc.exec(ps->status, "/awaylog clear");
        pc.exec(ps->status, "/awaylog");
        assert(ps->status->lines.back().text == "awaylog: nothing came while you were away");
        // seen: what a nick did last; !seen answered where reply is on, once per 10s, telling only when for elsewhere
        pc.exec(ph, "/plugin install seen");
        wait_catalog();
        assert(said("seen " + ver["seen"] + " installed and on"));
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
        {  // /plugin typed in a channel: its answer goes to the console tab, the channel stays as it was
            size_t nc = pchan->lines.size();
            pc.exec(pchan, "/plugin set seen max 6000");
            assert(pchan->lines.size() == nc && last(ph) == "seen max = 6000" && ph->lines.back().badge == "plugin");
        }
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
        // preview: audio, video and image links get their player or picture at once (no fetch); max, media off
        pc.exec(ph, "/plugin install preview");
        wait_catalog();
        assert(said("preview " + ver["preview"] + " installed and on"));
        for (auto& pi : pc.plugin_info())  // the About: author and contact, from the catalog (official) and the file alike
            if (pi.name == "preview") assert(pi.author == "Gerardo Canosa" && pi.contact == "gera.canosa@gmail.com" && pi.official && pi.in_catalog);
        for (auto& r : pc.catalog_rows()) assert(r.author == "Gerardo Canosa" && !r.contact.empty() && r.official);
        assert(irc_parse(":bob!u@h PRIVMSG #chan :listen \x02https://x.test/song.mp3?dl=1\x02 and https://x.test/Pic.PNG, "
                         "also (https://x.test/v.webm). https://x.test/four.ogg", pm)), pc.handle(ps, pm);
        assert(pchan->lines.back().embeds == R"({"size":"small","type":"audio","url":"https://x.test/song.mp3?dl=1"},)"
                                             R"({"size":"small","type":"image","url":"https://x.test/Pic.PNG"},{"size":"small","type":"video","url":"https://x.test/v.webm"})");
        assert(irc_parse(":bob!u@h PRIVMSG #chan :www.x.test/a.mp3 and (http://x.test/b.OGG) www.x", pm)), pc.handle(ps, pm);  // as https
        assert(pchan->lines.back().embeds == R"({"size":"small","type":"audio","url":"https://www.x.test/a.mp3"},)"
                                             R"({"size":"small","type":"audio","url":"https://x.test/b.OGG"})");
        pc.exec(ph, "/plugin set preview@sn/#chan media off");
        pc.exec(ph, "/plugin set preview ignore pics.test");
        pc.exec(ph, "/plugin set preview size Large");
        assert(irc_parse(":bob!u@h PRIVMSG #chan :https://x.test/a.mp3 https://cdn.pics.test/a.gif https://x.test/b.gif", pm)), pc.handle(ps, pm);
        assert(pchan->lines.back().embeds == R"({"size":"large","type":"image","url":"https://x.test/b.gif"})");
        pc.exec(ph, "/plugin remove preview");
        // zen: /zen draws the chosen side in mono, the configured theme stays and comes back on /zen, Esc or disable
        pc.exec(ph, "/plugin install zen");
        wait_catalog();
        assert(said("zen " + ver["zen"] + " installed and on"));
        std::string conf_theme = pc.theme.name;
        pc.exec(ph, "/plugin set zen where terminal");
        pc.exec(ph, "/zen");
        assert(pc.view_on(false) && !pc.view_on(true) && pc.look(false).name == "mono" && pc.look(true).name == conf_theme);
        pc.exec(ph, "/zen");
        assert(!pc.view_on(false) && pc.look(false).name == conf_theme);
        pc.exec(ph, "/zen");
        pc.exec(ph, "/plugin disable zen");
        assert(pc.view.by.empty() && pc.look(false).name == conf_theme && pc.theme.name == conf_theme);
        pc.exec(ph, "/plugin remove zen");
        // nickcolors: the channel's nicks said in a message get their color (shown only), but not inside a link, a #name or
        // a text with colors of its own, nor short ones; where picks the chat, the user list or both
        pc.theme.nicks = {0xff0000, 0x00ff00, 0x0000ff};
        pc.exec(ph, "/plugin install nickcolors");
        wait_catalog();
        assert(said("nickcolors " + ver["nickcolors"] + " installed and on") && pc.nicklist_by == "nickcolors" && pc.nicklist_colored(pchan));
        assert(irc_parse(":gary!u@h JOIN #chan", pm)), pc.handle(ps, pm);
        assert(irc_parse(":hank!u@h JOIN #chan", pm)), pc.handle(ps, pm);
        auto painted = [&](const std::string& n) { char b[16]; snprintf(b, sizeof b, "\x04%06x", (unsigned)nick_rgb(pc.theme, n)); return std::string(b) + n + "\x04"; };
        const std::string said_ = "hey gary, see https://gary.test/hank and #gary, hank.test zed ed";
        assert(irc_parse(":hank!u@h PRIVMSG #chan :" + said_, pm)), pc.handle(ps, pm);
        assert(pchan->lines.back().text == "hey " + painted("gary") + ", see https://gary.test/hank and #gary, hank.test zed ed" && pchan->lines.back().said == said_);
        assert(irc_parse(":hank!u@h PRIVMSG #chan :hi \x03" "4gary", pm)), pc.handle(ps, pm);
        assert(pchan->lines.back().text == "hi \x03" "4gary" && pchan->lines.back().said.empty());
        assert(irc_parse(":hank!u@h PRIVMSG #chan :gary: hank!", pm)), pc.handle(ps, pm);
        assert(pchan->lines.back().text == painted("gary") + ": " + painted("hank") + "!");
        pc.exec(ph, "/plugin set nickcolors where userlist");  // the user list only: the lines are as said again, the list still painted
        assert(pchan->lines.back().text == "gary: hank!" && pc.nicklist_by == "nickcolors");
        pc.exec(ph, "/plugin set nickcolors where chat");
        pc.plugin_emit("tick", nullptr, "");
        assert(pchan->lines.back().text == painted("gary") + ": " + painted("hank") + "!" && pc.nicklist_by.empty());
        pc.exec(ph, "/plugin remove nickcolors");
        pc.theme.nicks.clear();
        // screensaver: after idle minutes without a key (attached), each saver draws on any grid, a key ends it
        pc.exec(ph, "/plugin install screensaver");
        wait_catalog();
        assert(said("screensaver " + ver["screensaver"] + " installed and on"));
        pc.attached = true, pc.ui_on = false, pc.active_at = time(nullptr) - 3600;  // only a BNC client: none
        pc.plugin_emit("tick", nullptr, "");
        pc.exec(ph, "/screensaver");
        assert(pc.screen_by.empty() && !pc.locked);
        pc.ui_on = true, pc.active_at = time(nullptr) - 9 * 60;
        pc.plugin_emit("tick", nullptr, "");
        assert(pc.screen_by.empty());
        pc.active_at -= 60;
        pc.plugin_emit("tick", nullptr, "");
        assert(pc.screen_by == "screensaver" && !pc.locked);
        for (auto n : {"matrix", "particles", "stars"}) {
            pc.exec(ph, std::string("/screensaver ") + n);
            std::vector<Core::SCell> g;
            int lit = 0;
            for (int f = 0; f < 40; f++) {  // 8 seconds at the saver's step cap: matrix rain falls in from above first (3 frames left it to the seed)
                pc.screen_t0 -= 200;
                assert(pc.screen_frame(80, 24, "term", g) && pc.screen_frame(213, 60, "web", g) && g.size() == 213 * 60);
            }
            for (auto& sc : g) lit += sc.ch != 0;
            assert(lit > 20 && pc.screen_by == "screensaver");
        }
        pc.exec(ph, "/plugin set screensaver saver nope");  // only its choices, in any case
        assert(last(ph) == "screensaver saver is one of: random, matrix, particles, stars" && pc.plugin_at("screensaver", "saver", "").empty());
        pc.exec(ph, "/plugin set screensaver saver Stars");
        assert(pc.plugin_at("screensaver", "saver", "") == "stars");
        for (auto& pi : pc.plugin_info())
            if (pi.name == "screensaver")
                for (auto& st : pi.sets) if (st.key == "saver") assert(st.choices.size() == 4 && st.choices[3] == "stars");
        pc.exec(ph, "/screensaver nope");
        assert(last(ph) == "No such screensaver: nope (matrix, particles, stars)");
        assert(pc.wake() && pc.screen_by.empty() && !pc.wake() && time(nullptr) - pc.active_at < 2);
        pc.last_input = 0;  // a key, click or move is input for autoaway too, not only a sent line
        pc.wake();
        assert(time(nullptr) - pc.last_input < 2);
        // lock on: without a lock password it says so (once) and doesn't lock; with one, the screensaver locks
        pc.exec(ph, "/plugin set screensaver lock on");
        pc.exec(ph, "/screensaver");
        assert(said("there's no lock password yet") && !pc.locked);
        pc.wake();
        pc.cfg["general"]["lock"] = "on", pc.cfg["general"]["lock_password_hash"] = hash_password("pw");
        pc.exec(ph, "/screensaver stars");
        assert(pc.locked && pc.screen_by == "screensaver");
        assert(!pc.wake() && pc.locked && pc.screen_by == "screensaver");  // locked: it stays, the password box comes over it
        pc.unlock("nope");
        assert(pc.locked && pc.screen_by == "screensaver");
        pc.active_at = pc.last_input = time(nullptr) - 3600;  // the password box didn't count as input: unlocking idle restarted it
        pc.unlock("pw");
        assert(!pc.locked && pc.screen_by.empty());  // the right one ends both
        assert(time(nullptr) - pc.active_at < 2 && time(nullptr) - pc.last_input < 2);
        pc.exec(ph, "/plugin set screensaver lock off");  // locking (Alt+S, /lock, lock_idle): a screensaver comes up under the box
        pc.lock_now(ph);
        assert(pc.locked && pc.screen_by == "screensaver");
        pc.unlock("pw");
        assert(!pc.locked && pc.screen_by.empty());
        pc.set_lock(true, false);  // an attach: only the box; Esc on it starts one, once
        assert(pc.locked && pc.screen_by.empty());
        pc.lock_saver();
        assert(pc.locked && pc.screen_by == "screensaver");
        pc.lock_saver();
        assert(pc.screen_by == "screensaver");
        pc.unlock("pw");
        pc.exec(ph, "/plugin set screensaver on_lock off");  // only the password box
        pc.lock_now(ph);
        assert(pc.locked && pc.screen_by.empty());
        pc.unlock("pw");
        pc.cfg["general"].erase("lock"), pc.cfg["general"].erase("lock_password_hash"), pc.locked = false;
        pc.exec(ph, "/plugin remove screensaver");
        assert(pc.screen_by.empty());
        }  // the catalog's plugins
        unsetenv("ISEKUR_PLUGIN_URL");
        unlink((cat + "/index.json").c_str()), unlink((cat + "/t2/t2.lua").c_str()), rmdir((cat + "/t2").c_str()), rmdir(cat.c_str());
        for (auto f : {"t.lua", "bad.lua", "t.data", ".catalog.json"}) unlink((pdir + "/" + f).c_str());
        rmdir(pdir.c_str()), unlink(pc.cfg_path.c_str()), rmdir(d);
    }

    {  // /backup and /restore: the file is locked, a wrong passphrase or a changed byte opens nothing, /restore replaces
        auto rd = [](const std::string& p) { std::ifstream f(p, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), {}); };
        auto wr = [](const std::string& p, const std::string& s) { std::ofstream(p, std::ios::binary) << s; };
        std::vector<BackupFile> in = {{"isekur.conf", std::string(4000, 'x') + "\n"}, {"secret.key", std::string("\0\xff\n\t", 4)}, {"plugins/a.lua", "-- a\n"}};
        std::string bak = backup_pack(in, "correct horse"), err;
        assert(bak.compare(0, 7, "iSEBAK1") == 0 && bak.find("xxxxxxxx") == std::string::npos);  // sealed: nothing readable
        assert(bak.size() < 4100 + 120);  // the lock adds a header, a salt, a nonce and a tag, no more
        std::vector<BackupFile> out;
        assert(backup_unpack(bak, "correct horse", out, err) && out.size() == 3 && out[0].data == in[0].data && out[1].data == in[1].data && out[2].path == "plugins/a.lua");
        assert(!backup_unpack(bak, "wrong horse", out, err) && err.find("passphrase") != std::string::npos && out.empty());
        std::string bad = bak;
        bad[bad.size() / 2] ^= 1;
        assert(!backup_unpack(bad, "correct horse", out, err));  // a changed byte
        bad = bak, bad[8] ^= 1;
        assert(!backup_unpack(bad, "correct horse", out, err));  // the salt is part of the key
        bad = bak, bad[6] = '2';
        assert(!backup_unpack(bad, "correct horse", out, err) && err == "not an iSekur backup");  // another version
        assert(!backup_unpack("hello", "x", out, err) && err == "not an iSekur backup");
        // only these files, and no way out of the config folder
        for (const char* ok : {"isekur.conf", "secret.key", "bnc.key", "plugins/radio.lua", "plugins/radio.data", "themes/my-night.theme"}) assert(backup_path_ok(ok));
        for (const char* no : {"", "../isekur.conf", "/etc/passwd", "plugins/../x.lua", "plugins/a/b.lua", "plugins/.catalog.json", "plugins/.lua", "themes/.theme", "themes/../x.theme",
                               "themes/a/b.theme", "plugins/a.sh", "isekur.history", "web.sessions", "MANIFEST", "plugins/bad name.lua"})
            assert(!backup_path_ok(no));

        char d[] = "/tmp/isekur-bak-XXXXXX";
        assert(mkdtemp(d));
        std::string dir = std::string(d) + "/", conf = "[general]\nnick = alice\ntheme = default\n[server.test]\nhost = bak-host.example\nautoconnect = off\n";
        mkdir_p(dir + "plugins"), mkdir_p(dir + "themes");
        wr(dir + "isekur.conf", conf), wr(dir + "secret.key", std::string(32, '\x07')), wr(dir + "plugins/t.lua", "-- t v1\n"), wr(dir + "plugins/t.data", "k\tv\n");
        wr(dir + "plugins/.catalog.json", "{}"), wr(dir + "themes/mine.theme", "[colors]\n"), wr(dir + "notes.txt", "not part of it");
        Core bk;
        bk.cfg_path = dir + "isekur.conf";
        Buffer* bh = bk.open(nullptr, BT::STATUS, "isekur");
        std::string path = dir + "backups/first.isebak";
        {  // typed in another tab: the boxes and the lines are in CMD, and you are taken there; the other tab says nothing
            Buffer* other = bk.open(nullptr, BT::QUERY, "*elsewhere");
            bk.select(other);
            size_t n_other = other->lines.size(), n_home = bh->lines.size();
            bk.exec(other, "/backup elsewhere!");  // an error: in CMD
            assert(bk.active == bh && other->lines.size() == n_other && bh->lines.size() > n_home);
            bk.select(other), n_home = bh->lines.size();
            bk.exec(other, "/backup elsewhere");  // its passphrase box is CMD's
            assert(bk.pass_buf == bh && bk.active == bh && other->lines.size() == n_other);
            bk.exec(bh, "");  // cancelled
            bk.select(other);
            bk.exec(other, "/restore"), bk.exec(bh, "");  // the list and the question: CMD's too
            bk.select(other);
            bk.exec(other, "/restore nope");
            assert(!bk.pass_buf && bk.active == bh && other->lines.size() == n_other && bh->lines.back().kind == LK::ERROR);
            bk.select(bh);
        }
        bk.exec(bh, "/backup first!");  // a name takes letters, digits and . _ -
        assert(!bk.pass_buf && access(path.c_str(), F_OK) != 0);
        bk.exec(bh, "/backup first");
        assert(bk.pass_buf == bh);
        bk.exec(bh, "short");  // 8 characters or more
        assert(!bk.pass_buf && access(path.c_str(), F_OK) != 0);
        bk.exec(bh, "/backup first"), bk.exec(bh, "correct horse"), bk.exec(bh, "correct horsE");  // the two don't match
        assert(!bk.pass_buf && access(path.c_str(), F_OK) != 0);
        bk.exec(bh, "/backup first"), bk.exec(bh, "correct horse"), bk.exec(bh, "correct horse");
        assert(access(path.c_str(), F_OK) == 0 && !bk.pass_buf);
        struct stat sb;
        assert(stat(path.c_str(), &sb) == 0 && (sb.st_mode & 077) == 0);  // only you can read it
        std::string packed = rd(path);
        assert(packed.find("alice") == std::string::npos && packed.find("bak-host.example") == std::string::npos);
        assert(backup_unpack(packed, "correct horse", out, err) && out.size() == 6 && out[0].path == "MANIFEST" && out[0].data.find("created=") == 0);  // + the 5 files, not notes.txt or .catalog.json
        bk.exec(bh, "/backup first");  // never overwritten
        assert(!bk.pass_buf && rd(path) == packed);

        // the configuration changes: a new config, a changed plugin, one more plugin and theme, no secret.key
        wr(dir + "isekur.conf", "[general]\nnick = bob\n"), wr(dir + "plugins/t.lua", "-- t v2\n"), wr(dir + "plugins/extra.lua", "-- extra\n");
        wr(dir + "themes/extra.theme", "[colors]\n"), unlink((dir + "secret.key").c_str());
        bk.unsaved["general"]["nick"] = "carol";
        bk.exec(bh, "/restore nope");  // no such backup
        assert(!bk.pass_buf);
        bk.exec(bh, "/restore /dev/null");  // not a file to read
        assert(!bk.pass_buf);
        bk.exec(bh, "/restore");  // no name: lists them and asks which (the settings window's Restore button)
        assert(bk.pass_buf == bh && bk.pass_title == "Restore backup" && !bk.pass_hidden && bk.pass_info.size() >= 5 && bk.pass_info[0].find("WIPES") != std::string::npos);
        bool lists_first = false;
        for (auto& l : bk.pass_info) lists_first |= l == "  first";
        assert(lists_first);
        bk.exec(bh, "first");  // then its passphrase
        assert(bk.pass_buf == bh && bk.pass_label == "passphrase of the backup" && bk.pass_hidden);
        bk.exec(bh, "");  // cancelled
        assert(!bk.pass_buf && rd(dir + "isekur.conf") == "[general]\nnick = bob\n");
        wr(dir + "backups/plain.isebak", "not a backup at all");
        bk.exec(bh, "/restore plain");
        assert(!bk.pass_buf);
        bk.plugin_depth = 1;
        bk.exec(bh, "/restore first"), bk.exec(bh, "/backup other");  // a plugin can't
        bk.plugin_depth = 0;
        assert(!bk.pass_buf && rd(dir + "isekur.conf") == "[general]\nnick = bob\n");
        bk.exec(bh, "/restore first");
        assert(bk.pass_buf == bh && bk.pass_title == "Restore backup");
        bool says_wipe = false;
        for (auto& l : bk.pass_info) says_wipe |= l.find("WIPES") != std::string::npos;
        assert(says_wipe);  // the dialog says it replaces everything
        bk.exec(bh, "wrong horse");  // nothing changes
        assert(rd(dir + "isekur.conf") == "[general]\nnick = bob\n" && access((dir + "secret.key").c_str(), F_OK) != 0 && !bk.unsaved.empty());
        bk.exec(bh, "/restore first.isebak"), bk.exec(bh, "correct horse");
        assert(rd(dir + "isekur.conf") == conf && rd(dir + "secret.key") == std::string(32, '\x07') && rd(dir + "plugins/t.lua") == "-- t v1\n" && rd(dir + "plugins/t.data") == "k\tv\n");
        assert(access((dir + "plugins/extra.lua").c_str(), F_OK) != 0 && access((dir + "themes/extra.theme").c_str(), F_OK) != 0);  // not in the backup: gone
        assert(access((dir + "plugins/.catalog.json").c_str(), F_OK) == 0 && rd(dir + "notes.txt") == "not part of it" && access((dir + "themes/mine.theme").c_str(), F_OK) == 0);
        assert(bk.unsaved.empty() && ini_get(bk.cfg, "general", "nick") == "alice");  // the config is loaded
        std::string snap;  // what was there before is in before-restore-*.isebak, with the same passphrase
        if (DIR* dd = opendir((dir + "backups").c_str())) {
            while (dirent* e = readdir(dd))
                if (std::string(e->d_name).rfind("before-restore-", 0) == 0) snap = dir + "backups/" + e->d_name;
            closedir(dd);
        }
        assert(!snap.empty() && backup_unpack(rd(snap), "correct horse", out, err));
        bool had_bob = false, had_extra = false;
        for (auto& f : out) had_bob |= f.path == "isekur.conf" && f.data == "[general]\nnick = bob\n", had_extra |= f.path == "plugins/extra.lua";
        assert(had_bob && had_extra);
        // a backup holding a file that isn't one of ours is refused before anything is written
        wr(dir + "backups/evil.isebak", backup_pack({{"isekur.conf", "[general]\nnick = evil\n"}, {"../escape", "x"}}, "correct horse"));
        bk.exec(bh, "/restore evil"), bk.exec(bh, "correct horse");
        assert(rd(dir + "isekur.conf") == conf && access((std::string(d) + "/escape").c_str(), F_OK) != 0);
        std::filesystem::remove_all(d);
    }

    {  // API 16: files given to iSekur (/upload, a dropped path), isekur.upload, isekur.choose and isekur.copy
        char d[] = "/tmp/isekur-drop-XXXXXX";
        assert(mkdtemp(d));
        std::string dir = d, pdir = dir + "/plugins", pic = dir + "/a b.png";
        auto wr = [](const std::string& f, const std::string& body) { std::ofstream(f, std::ios::binary) << body; };
        wr(pic, "0123456789"), wr(dir + "/two.txt", "hello"), wr(dir + "/empty.txt", "");
        mkdir(dir.c_str(), 0700), mkdir((dir + "/sub").c_str(), 0700);
        // what a terminal types for a dropped file: quotes, backslashes, file:// URLs, several; anything else is no path at all
        assert((dropped_paths("'" + pic + "'") == std::vector<std::string>{pic}));
        assert((dropped_paths(dir + "/a\\ b.png ") == std::vector<std::string>{pic}));
        assert((dropped_paths("file://" + dir + "/a%20b.png") == std::vector<std::string>{pic}));
        assert((dropped_paths("\"" + pic + "\" " + dir + "/two.txt") == std::vector<std::string>{pic, dir + "/two.txt"}));
        assert(dropped_paths("").empty() && dropped_paths("hello").empty() && dropped_paths(dir + "/nope.png").empty() && dropped_paths(dir + "/sub").empty());
        assert(dropped_paths(pic + " hello").empty() && dropped_paths("'" + pic).empty() && dropped_paths("relative/a.png").empty());  // one word that isn't a file: a message

        Core dc;
        dc.cfg_path = dir + "/isekur.conf";
        Buffer* dh = dc.open(nullptr, BT::STATUS, "isekur");
        auto last = [&] { return dh->lines.back().text; };
        assert(dc.file_drop(dh, pic, "", false) == "no plugin takes files: /plugin install dropzone" && dc.drops.empty());
        mkdir_p(pdir);
        wr(pdir + "/dz.lua", R"LUA(
local rec
isekur.on("drop", function(e)
  rec = e.id
  isekur.print(nil, nil, string.format("drop %s %d %s %s %d", e.name, e.size, e.kind, e.type, #e.id))
  local q = {id = e.id, title = "Upload", text = {"a", "b"}, items = {"one", "two", "three"}, sel = 2}
  if e.name ~= "two.txt" then q.input, q.value = "Message", "hello" end  -- a text field, starting with "hello" (not for two.txt: none)
  assert(isekur.choose(q))
end)
isekur.on("choice", function(e) isekur.print(nil, nil, "choice " .. tostring(e.id == rec) .. " " .. e.index .. " [" .. e.text .. "]") end)
isekur.command("dzt", "", function(args)
  if args == "http" then local ok, why = isekur.upload(rec, {url = "http://x.test/"}, function() end) isekur.print(nil, nil, tostring(ok) .. " " .. why) end
  if args == "gone" then local ok, why = isekur.upload("nope", {url = "https://x.test/"}, function() end) isekur.print(nil, nil, tostring(ok) .. " " .. why) end
  if args == "lan" then assert(isekur.upload(rec, {url = "https://127.0.0.1:9/", field = "f"}, function(r) isekur.print(nil, nil, "up " .. r.status .. " " .. tostring(r.error)) end)) end
  if args == "forget" then isekur.print(nil, nil, "forget " .. tostring(isekur.upload(rec, false))) end
  if args == "copy" then isekur.print(nil, nil, "copy " .. tostring(isekur.copy("https://litter.test/x.png"))) end
  if args == "upload" then isekur.exec(nil, nil, "/upload /etc/hosts") end
  if args == "empty" then isekur.print(nil, nil, tostring(select(2, isekur.choose{items = {}}))) end
end)
)LUA");
        dc.exec(dh, "/plugin enable dz");
        // /upload gives the file to the plugin: what it is, never where it is
        dc.exec(dh, "/upload '" + pic + "'");
        assert(dh->lines[dh->lines.size() - 1].text == "drop a b.png 10 image image/png 16");
        dc.choices.clear(), dc.drops.clear();
        wr(dir + "/x\ty.png", "12345");
        dc.exec(dh, "/upload '" + dir + "/x\ty.png'");
        assert(dh->lines[dh->lines.size() - 1].text == "drop x_y.png 5 image image/png 16");  // no control character in a name
        dc.choices.clear(), dc.drops.clear();
        dc.exec(dh, "/upload '" + pic + "'");
        assert(dc.drops.size() == 1 && !dc.drops[0].own && dc.choices.size() == 1);
        const Core::Choice& ch = dc.choices.front();
        assert(ch.by == "dz" && ch.title == "Upload" && ch.text.size() == 2 && ch.items.size() == 3 && ch.sel == 2 && ch.id == dc.drops[0].id);
        assert(ch.input == "Message" && ch.value == "hello");  // a text field under the rows
        std::string id = dc.drops[0].id;
        dc.choice_act(3, "  typed\t text\n ");  // the third row: the plugin hears it, once, with the field's text as one trimmed line; nothing is left on screen
        assert(last() == "choice true 3 [typed  text]" && dc.choices.empty());
        dc.choice_act(1, "later");  // nothing asked: nothing happens
        assert(last() == "choice true 3 [typed  text]");
        dc.exec(dh, "/upload '" + pic + "'");  // cancelled: no text, whatever was typed
        dc.choice_act(0, "typed and then Esc");
        assert(last() == "choice true 0 []");
        dc.exec(dh, "/upload '" + pic + "'");  // 300 bytes at most, and never half of a character
        dc.choice_act(1, std::string(400, 'a'));
        assert(last() == "choice true 1 [" + std::string(300, 'a') + "]");
        dc.exec(dh, "/upload '" + pic + "'");
        dc.choice_act(1, std::string(299, 'a') + "\xc3\xa9");  // 301 bytes: the cut would fall inside the e acute
        assert(last() == "choice true 1 [" + std::string(299, 'a') + "]");
        dc.exec(dh, "/upload " + dir + "/two.txt");
        assert(last() == "drop two.txt 5 text text/plain 16" && dc.drops.size() == 5 && dc.choices.size() == 1);
        assert(dc.choices.front().input.empty());
        dc.choice_act(2, "a box with no field gets no text");
        assert(last() == "choice true 2 []");
        dc.exec(dh, "/upload " + dir + "/two.txt");
        dc.choice_act(99), assert(last() == "choice true 0 []");  // a row that isn't there: cancelled
        dc.exec(dh, "/upload " + dir + "/two.txt");
        dc.choice_act(0), assert(last() == "choice true 0 []");
        dc.exec(dh, "/upload " + dir + "/empty.txt");
        assert(last() == "/upload: the file is empty" && dc.drops.size() == 7);
        dc.exec(dh, "/upload " + dir + "/nope.png");
        assert(last().find("/upload: not a file") == 0);
        dc.exec(dh, "/upload");
        assert(last().find("/upload <file>") == 0);
        // two questions at once: one after the other
        dc.drops.clear(), dc.exec(dh, "/upload '" + pic + "'"), dc.exec(dh, "/upload " + dir + "/two.txt");
        assert(dc.choices.size() == 2 && dc.choices.front().id == dc.drops[0].id);
        dc.choice_act(1), assert(dc.choices.size() == 1 && dc.choices.front().id == dc.drops[1].id);
        dc.choice_act(2), assert(dc.choices.empty());
        // isekur.upload: https only, a file that exists, never the LAN, a failure keeps the file for another try
        id = dc.drops[1].id;
        dc.exec(dh, "/dzt http"), assert(last() == "nil the URL must be https://...");
        dc.exec(dh, "/dzt gone"), assert(last() == "nil that file is gone");
        dc.exec(dh, "/dzt lan");
        for (int i = 0; i < 500 && last().rfind("up ", 0) != 0; i++) usleep(10000), dc.plugins_poll();
        assert(last() == "up 0 127.0.0.1 is on a private network: not fetched" && dc.drop_find(id));
        dc.exec(dh, "/dzt forget"), assert(last() == "forget true" && !dc.drop_find(id));
        dc.exec(dh, "/dzt empty"), assert(last() == "no items");
        // a webchat's copy is deleted when forgotten or when nobody sent it; a path the user named never is
        wr(dir + "/own.bin", "xyz");
        assert(dc.file_drop(dh, dir + "/own.bin", "x.bin", true).empty() && dc.drops.back().name == "x.bin" && dc.drops.back().kind == "other");
        dc.choices.clear();
        std::string oid = dc.drops.back().id;
        dc.drop_forget(oid), assert(access((dir + "/own.bin").c_str(), F_OK) != 0 && access(pic.c_str(), F_OK) == 0);
        wr(dir + "/own.bin", "xyz");
        assert(dc.file_drop(dh, dir + "/own.bin", "x.bin", true).empty());
        dc.choices.clear();
        dc.drops_gc(time(nullptr) + 100);  // not yet
        assert(access((dir + "/own.bin").c_str(), F_OK) == 0);
        dc.drops_gc(time(nullptr) + 2000);  // forgotten after 30 minutes
        assert(access((dir + "/own.bin").c_str(), F_OK) != 0 && access(pic.c_str(), F_OK) == 0);
        wr(dir + "/empty2.bin", "");
        assert(dc.file_drop(dh, dir + "/empty2.bin", "", true) == "the file is empty" && access((dir + "/empty2.bin").c_str(), F_OK) != 0);  // refused: its copy goes too
        // a plugin can't name a file of yours
        size_t n0 = dc.drops.size();
        dc.exec(dh, "/dzt upload");
        assert(last() == "/upload can't be run by a plugin" && dc.drops.size() == n0);
        // isekur.copy: the terminal's clipboard on its next frame, the webchat's through its message
        std::string copied;
        dc.listener = [&](Ev e, Buffer*, const Line* l) { if (e == Ev::COPY) copied = l->text; };
        dc.tty = true;
        dc.exec(dh, "/dzt copy");
        assert(last() == "copy true" && copied == "https://litter.test/x.png" && dc.copy_out == copied);
        dc.listener = nullptr;
        // a plugin that goes takes its questions with it
        dc.exec(dh, "/upload '" + pic + "'");
        assert(dc.choices.size() == 1);
        dc.exec(dh, "/plugin disable dz");
        assert(dc.choices.empty());

        {  // the multipart POST: fields, then the file, streamed; the plugin's headers, one line each; a name can't break out of its header
            int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
            setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            sockaddr_in a{};
            a.sin_family = AF_INET, a.sin_addr.s_addr = htonl(0x7f000001);
            assert(bind(ls, (sockaddr*)&a, sizeof a) == 0 && listen(ls, 1) == 0);
            socklen_t al = sizeof a;
            getsockname(ls, (sockaddr*)&a, &al);
            std::string file(200000, 'x'), req;
            for (size_t i = 0; i < file.size(); i++) file[i] = (char)(i * 7 + i / 251);
            wr(dir + "/big.bin", file);
            std::thread srv([&] {
                int fd = accept(ls, nullptr, nullptr);
                size_t he;
                char buf[65536];
                while ((he = req.find("\r\n\r\n")) == std::string::npos) { ssize_t n = recv(fd, buf, sizeof buf, 0); if (n <= 0) break; req.append(buf, (size_t)n); }
                size_t cl = (size_t)atol(req.c_str() + req.find("Content-Length: ") + 16);
                while (req.size() < he + 4 + cl) { ssize_t n = recv(fd, buf, sizeof buf, 0); if (n <= 0) break; req.append(buf, (size_t)n); }
                const char* ans = "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nhttps://litter.test/abc.png";
                send(fd, ans, strlen(ans), 0);
                close(fd);
            });
            Upload up;
            up.url = "http://127.0.0.1:" + std::to_string(ntohs(a.sin_port)) + "/api.php", up.path = dir + "/big.bin", up.guard = false;
            up.field = "fileToUpload", up.filename = "a\"b\r\nc.png", up.ctype = "image/png";
            up.fields = {{"reqtype", "fileupload"}, {"time", "72h"}};
            up.headers = {{"Authorization", "Client-ID abc"}, {"X-Evil", "x\r\nInjected: yes"}, {"Bad\nName", "v"}, {"Content-Length", "1"}, {"host", "evil.test"}, {"Transfer-Encoding", "chunked"}, {"user-agent", "radio-test/1"}};
            int status = 0;
            std::string body, err;
            assert(https_upload(up, status, body, err) && status == 200 && body == "https://litter.test/abc.png" && err.empty());
            srv.join(), close(ls);
            size_t he = req.find("\r\n\r\n");
            std::string head = req.substr(0, he), rest = req.substr(he + 4);
            assert(head.rfind("POST /api.php HTTP/1.0\r\n", 0) == 0 && head.find("\r\nAuthorization: Client-ID abc") != std::string::npos);
            assert(head.find("Injected") == std::string::npos && head.find("X-Evil") == std::string::npos && head.find("Bad") == std::string::npos);
            assert(head.find("\r\nuser-agent: radio-test/1") != std::string::npos && head.find("User-Agent:") == std::string::npos);  // the plugin's User-Agent replaces ours, it isn't added to it
            assert(head.find("evil.test") == std::string::npos && head.find("chunked") == std::string::npos && head.find("Content-Length: 1\r\n") == std::string::npos);  // nothing that frames the request
            size_t bp = head.find("boundary=");
            std::string bnd = head.substr(bp + 9, head.find("\r\n", bp) - bp - 9);
            assert(head.find("Content-Type: multipart/form-data; boundary=----iSekur") != std::string::npos && head.find("Content-Length: " + std::to_string(rest.size())) != std::string::npos);
            std::string want = "--" + bnd + "\r\nContent-Disposition: form-data; name=\"reqtype\"\r\n\r\nfileupload\r\n"
                               "--" + bnd + "\r\nContent-Disposition: form-data; name=\"time\"\r\n\r\n72h\r\n"
                               "--" + bnd + "\r\nContent-Disposition: form-data; name=\"fileToUpload\"; filename=\"a_b__c.png\"\r\nContent-Type: image/png\r\n\r\n" +
                               file + "\r\n--" + bnd + "--\r\n";
            assert(rest == want && up.sent == req.size());  // every byte of the request went through the counter
            {  // what iSekur says when a host answers before it has the file, closes without a word, or is too slow: what happened
                struct Host { int ls; std::thread t; unsigned short port; std::atomic<bool> stop{false}; };
                auto host = [&](int mode) {  // 0: answers at the headers and sits on it, 1: takes the file and closes, 2: takes the file and thinks for 3 s, 3: reads nothing for 0.4 s, then answers
                    auto h = std::make_unique<Host>();
                    h->ls = socket(AF_INET, SOCK_STREAM, 0);
                    int on = 1;
                    setsockopt(h->ls, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
                    sockaddr_in sa{};
                    sa.sin_family = AF_INET, sa.sin_addr.s_addr = htonl(0x7f000001);
                    assert(bind(h->ls, (sockaddr*)&sa, sizeof sa) == 0 && listen(h->ls, 1) == 0);
                    socklen_t sl = sizeof sa;
                    getsockname(h->ls, (sockaddr*)&sa, &sl);
                    h->port = ntohs(sa.sin_port);
                    int lfd = h->ls;
                    std::atomic<bool>* stop = &h->stop;
                    h->t = std::thread([lfd, mode, stop] {
                        int fd = accept(lfd, nullptr, nullptr);
                        if (mode == 3) {  // reads nothing and says nothing for a while: the client fills its buffers and sits in send(); the answer comes then, to a client that must still hear it
                            usleep(400000);
                            const char* late = "HTTP/1.0 413 Payload Too Large\r\nContent-Length: 4\r\n\r\nnope";
                            send(fd, late, strlen(late), 0);
                            for (int i = 0; i < 2000 && !stop->load(); i++) usleep(10000);
                            close(fd), close(lfd);
                            return;
                        }
                        std::string rq;
                        char b[65536];
                        size_t e;
                        while ((e = rq.find("\r\n\r\n")) == std::string::npos) { ssize_t n = recv(fd, b, sizeof b, 0); if (n <= 0) break; rq.append(b, (size_t)n); }
                        if (mode == 0 || mode == 3) {
                            const char* ans = "HTTP/1.0 413 Payload Too Large\r\nContent-Length: 4\r\n\r\nnope";
                            send(fd, ans, strlen(ans), 0);
                            for (int i = 0; i < 2000 && !stop->load(); i++) usleep(10000);  // not reading, and not closing until the client is done (20 s at most): a client that goes on sending fills its buffers and stops
                        } else {
                            size_t cl = (size_t)atol(rq.c_str() + rq.find("Content-Length: ") + 16), got = rq.size() - e - 4;
                            while (got < cl) { ssize_t n = recv(fd, b, sizeof b, 0); if (n <= 0) break; got += (size_t)n; }
                            if (mode == 2) sleep(3);
                        }
                        close(fd), close(lfd);
                    });
                    return h;
                };
                auto go = [&](int mode, const std::string& path, int wait, std::string& answer, std::string& why, int& code) {
                    auto h = host(mode);
                    Upload u;
                    u.url = "http://127.0.0.1:" + std::to_string(h->port) + "/up", u.path = path, u.guard = false, u.wait = wait;
                    bool ok = https_upload(u, code, answer, why);
                    h->stop = true;
                    h->t.join();
                    return std::make_pair(ok, (unsigned long long)u.sent);
                };
                std::string big(20 << 20, 'z'), ans, why;
                wr(dir + "/20mb.bin", big);
                int code = 0;
                // "413 too big" at the headers: heard, and the rest of the 20 MB is not sent. The host reads no more than its first piece, so what got
                // written is what the two kernel buffers hold (a few hundred KB on some systems, several MB on others that grow them): the bound is
                // "far from all of it", not a number of one kernel
                for (int mode : {0, 3}) {  // 3: the buffers are full, the client is in send(), when the host answers
                    auto [okm, sentm] = go(mode, dir + "/20mb.bin", 120, ans, why, code);
                    if (!(okm && code == 413 && ans == "nope" && sentm < big.size() * 3 / 4))
                        fprintf(stderr, "early answer, mode %d: ok=%d code=%d answer=[%s] why=[%s] sent=%llu\n", mode, okm, code, ans.c_str(), why.c_str(), sentm);
                    assert(okm && code == 413 && ans == "nope" && sentm < big.size() * 3 / 4);
                }
                auto [ok1, sent1] = go(1, dir + "/big.bin", 120, ans, why, code);
                assert(!ok1 && why == "127.0.0.1 closed the connection without answering" && sent1 > 200000);
                auto [ok2, sent2] = go(2, dir + "/big.bin", 1, ans, why, code);
                assert(!ok2 && why == "127.0.0.1 didn't answer within 1 seconds" && sent2 > 200000);
            }
            // the guard: https only, and never a private address
            Upload bad;
            bad.url = "http://example.org/", bad.path = pic;
            assert(!https_upload(bad, status, body, err) && err == "uploads go over https:// only");
            bad.url = "https://127.0.0.1/";
            assert(!https_upload(bad, status, body, err) && err == "127.0.0.1 is on a private network: not fetched");
        }
        std::filesystem::remove_all(d);
    }

    puts("all tests passed");
}
