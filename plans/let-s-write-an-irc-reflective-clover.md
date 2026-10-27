# iSekur —  A modern and lightweight terminal IRC client with steroids

## Context
Greenfield. Goal: a small, fast, very colorful IRC client for the terminal, built on macOS and
compiling on Linux and the BSDs. WSL works from day one, and Cygwin almost does too (see Portability). It handles several servers
with tabs, has a topic bar and status bar, uses themes from `.theme` files, supports SSL/TLS and IRCv3,
and has a built-in webchat. The webchat is password protected and mirrors the live session, which
makes it work like a bouncer (ZNC style). A `--daemon` mode runs it with no terminal at all.

Decisions made: **C++17**, **termbox2 TUI**, **headless `--daemon` mode**, **HTTPS required unless bound to localhost**.

## Stack (no exotic dependencies)
| Need | Choice | Why |
|---|---|---|
| TUI | **termbox2**, a single header copied into `vendor/termbox2.h` (MIT) | 24-bit true color (`TB_OPT_ATTR_W=32`, `TB_OUTPUT_TRUECOLOR`), with an automatic fallback to 256 colors. Has its own terminfo handling. `tb_get_fds()` gives the tty and resize file descriptors so they join our `poll()` set. No system dependency. We considered ncurses (true color is awkward), FTXUI (runs its own loop, so it needs a second thread) and notcurses (heavy). |
| TLS (IRC + web) | **OpenSSL / LibreSSL** (libssl+libcrypto) | Present on every target. Also provides SHA1, base64, PBKDF2 and RNG for the WebSocket handshake and password hashing. |
| Event loop | **`poll()`**, single thread | POSIX everywhere, including Cygwin. No epoll/kqueue, no libuv. |
| HTTP/WebSocket | Hand-written minimal server, about 400 lines | Only handles GET of one embedded page plus one WS endpoint. A framework would be overkill. |
| Config/themes | Small INI parser | Readable for users and trivial to parse. |
| Build | **GNU Makefile + pkg-config** | `make` on Linux/macOS/Cygwin, `gmake` on BSD. |

Portability rules: POSIX only, no Linux-only syscalls (`epoll`, `accept4`, `signalfd`), use
termbox2 handles resize through its resize file descriptor, and `getaddrinfo` handles IPv4/IPv6. With these rules, Cygwin should
build without changes (it has openssl and poll, and termbox2 is copied into the repo). A native Windows port (Win32 console + Winsock) is out of scope.

## Architecture
The core is UI-agnostic. The TUI and the web are two **frontends** that subscribe to the same event bus.
```
          ┌──────────── Core (state + event bus) ────────────┐
IRC conns │ Server[] → Buffer[] (status/channel/query)       │
(TLS)  ⇄  │ each Buffer: lines ring (N=2000), nicks, topic   │
          └───────┬───────────────────────────┬──────────────┘
                  │ events (line/topic/nick)  │
            TUI frontend               Web frontend (0..n WS clients)
         (absent in --daemon)          same input path: "/cmd" or text
```
User input from either frontend goes through the same `Commands::exec(buffer, line)`. Typing in the
browser therefore behaves exactly like typing in the console, and both frontends see every line.

## File layout (as few files as reasonable)
```
Makefile
src/main.cpp        args (--daemon, -c config), poll loop, signals
src/net.{h,cpp}     socket + TLS wrapper (connect/accept, nonblocking read/write, cert verify)
src/irc.{h,cpp}     message parse (tags/prefix/params), Server state, CAP/SASL, reconnect
src/core.{h,cpp}    Buffer, Line, event bus, Commands (/join /part /msg /me /nick /query /server /connect /quit /close /topic /raw /theme /set)
vendor/termbox2.h   pinned upstream release; the only TU defining TB_IMPL is tui.cpp
src/tui.{h,cpp}     termbox2 cell layout (bars/tabs/nicklist drawn by us), mIRC color rendering, input line + history + tab-complete, keybinds
src/web.{h,cpp}     HTTP/WS server, auth, JSON (hand-escaped) event protocol
src/web_assets.h    generated from web/index.html via `xxd -i` at build (single-file HTML/CSS/JS)
src/config.{h,cpp}  INI parser for config + themes
web/index.html      webchat UI (vanilla JS, no framework)
themes/default.theme, themes/neon.theme, themes/solarized.theme
iSekur.conf.example
tests/test_parse.cpp  asserts for IRC parser, mIRC color stripping, INI, WS frame
```

## Features by phase
**Phase 1: core + TUI (usable client)**
- Parse IRC messages including IRCv3 tags. PING/PONG, registration, nick-in-use fallback,
  auto-reconnect with backoff, autojoin and per-server config.
- TLS through OpenSSL. Certificates are verified by default, `tls_verify=false` turns that off per server, and client certificates are supported for SASL EXTERNAL.
- Screen layout from top to bottom: **topic bar**, a **tab bar** with colored activity markers (none/data/msg/highlight),
  the chat area with optional right-side **nick list**, the **status bar** (nick, modes, server, lag, tab#,
  time), and the input line.
- mIRC formatting: ^B bold, ^C fg,bg colors, ^I italics, ^U underline, ^R reverse, ^O reset, and hex ^D colors rendered as true color.
  Nicks are colored by a hash of the nick into the theme's nick palette.
- Scrollback with PgUp/PgDn, input history, Tab nick completion, UTF-8 throughout.
- Default keybinds, configurable under `[keys]`:
  `Alt+1..9,0` jumps to tab N · `Alt+←/→` or `Ctrl+P/N` goes to the previous/next tab · `Alt+A` goes to the next active tab ·
  `Ctrl+X` cycles servers · `Ctrl+W` closes the tab · `PgUp/PgDn` scrolls · `F2` toggles the nick list.

**Phase 2: themes**
- `themes/*.theme` is INI. Sections: `[colors]` (each UI element as fg/bg/attrs, using names, 0-255, or `#rrggbb`
  in true color, mapped to the nearest 256 color only if `COLORTERM` doesn't advertise truecolor), `[nicks]` (palette), `[layout]` (nicklist side/width, show tabbar,
  timestamp format, separators, Unicode or ASCII borders).
- `/theme name` reloads live. Themes are searched in `./themes` first, then `~/.config/isekur/themes`. The web UI gets the
  same colors as CSS variables, so both frontends look alike.

**Phase 3: IRCv3**
- `CAP LS 302` negotiation, `cap-notify`, SASL PLAIN/EXTERNAL/SCRAM-SHA-256 (SCRAM uses OpenSSL's PBKDF2/HMAC, and SekurIRCd supports it), `server-time` (used for timestamps and backlog),
  `message-tags`, `multi-prefix`, `away-notify`, `account-notify`, `extended-join`, `chghost`,
  `echo-message`, `batch` (netsplit/chathistory), `userhost-in-names`, `invite-notify`, `setname`.

**Phase 4: webchat + daemon**
- Config `[web] enabled, bind, port, user, password_hash, cert, key`. `isekur --hash-password`
  prints a PBKDF2-SHA256 hash, so the plaintext password never lives in the config.
- If bind is not loopback and cert/key are missing, generate a self-signed cert on the first run. Fall back to
  HTTP only on 127.0.0.1/::1.
- Login is a form POST that issues a random session cookie (HttpOnly, Secure, SameSite=Strict).
  Five failed logins from the same IP trigger a lockout with backoff. Comparisons are constant-time.
- WS protocol is JSON. On connect, the server sends a snapshot: buffers, topics, nick lists, the last N lines each.
  After that it sends live events. The client sends `{buf, text}`. Read markers stay in sync between the console and the web.
- `--daemon` never calls `tb_init`, detaches with `fork`/`setsid` and logs to a file. Only the web frontend is active.

**Later / skipped:** DCC, scripting/plugins, logging to disk beyond a simple optional per-buffer log, native
Win32. These get added when someone asks for them.

## Config example (`~/.config/isekur/isekur.conf`)
```ini
[general]  nick=foo  theme=neon  timestamp=%H:%M
[server.libera]  host=irc.libera.chat  port=6697  tls=true  sasl_user=foo  sasl_pass=...  autojoin=#c,#linux
[web]  enabled=true  bind=0.0.0.0  port=8443  user=me  password_hash=pbkdf2$...
[keys]  next_tab=M-Right  prev_tab=M-Left
```

## Verification
- `make && make test`. The tests cover the IRC parser (tags, trailing params), mIRC color parsing, INI, and WS frame encode/decode.
- Local end-to-end: use **our own SekurIRCd** at `/Users/ethernet/coding/sekurircd` (`bin/sekurircd`, configured by
  `config/sekurircd.toml`, with plaintext and `[tls]` listeners, CAP 302, SASL PLAIN/SCRAM/EXTERNAL, and services). **Don't download any
  other ircd.** Run a local instance on its plaintext port and on its TLS port, connect to both as two "servers", and open several channels. Check tab switching, the topic and status bars, theme switching, and reconnect after killing the server.
- Real network: `irc.sekurnet.org` (6667 plain, 6697 TLS) as a second server alongside localhost. Use only throwaway
  test channels (e.g. `#isekur-test`). **Never join `#sekurnet` or `#news`.** These are production channels, so
  autojoin in test configs must not include them.
- Web: open `https://host:8443` in the Playwright MCP browser, log in, send a message, and confirm it shows in the TUI and that
  TUI messages show in the browser. Check that a wrong password is locked out after 5 attempts.
- `--daemon`: run it, connect from the web, and confirm IRC stays connected after the terminal closes.
- Portability: build on macOS (Homebrew openssl via pkg-config), Linux, WSL, and FreeBSD (`gmake`) where available.
