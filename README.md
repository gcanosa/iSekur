# iSekur 0.2

A small, very colorful terminal IRC client in C++17 with a built-in webchat
that mirrors your live session in a browser, bouncer style.

![iSekur in a terminal: topic bar, tabs with activity colors, nicklist, a "new" marker above missed messages](docs/screenshot.png)

- Multiple servers and tabs, a topic bar, a status bar, a nicklist and an activity list
- 24-bit color themes in `themes/*.theme`, with a 256-color fallback (`/theme neon` switches live)
- TLS with certificate verification, plus IRCv3: CAP 302, SASL PLAIN / SCRAM-SHA-256 / EXTERNAL,
  server-time, echo-message, multi-prefix, away/account-notify, extended-join and more
- Webchat over HTTPS + WebSocket with login, PBKDF2 password hash and login lockout.
  Off unless you ask: `[web] autostart = on`, `isekur --web`, or `/web start` / `/web stop` while running
- `--daemon` runs headless and keeps you on IRC; attach from the browser
- Mouse: click tabs, activity numbers and nicks, wheel scrolls chat and nicklist (F3 toggles)
- Flood-safe sending (pastes are queued so servers don't kick you; long pastes ask first) and `/ignore`
- `/grep [-all] text` searches the scrollback
- Auto-away when nothing is attached, and catch-up from IRCv3 chathistory on join
- Webchat notifications for highlights and private messages (the bell button)
- A "new" marker above the first message you missed, shared by the terminal and the webchat
- Optional chat logs (`log = on`, rotated daily) and CTCP VERSION/TIME/PING replies you can switch on or off, globally or per server

## Build

Needs a C++17 compiler, GNU make and OpenSSL 1.1+/LibreSSL. termbox2 is vendored.

    make            # gmake on *BSD
    make test
    ./isekur        # or: ./isekur -c path/to/isekur.conf
    sudo make install            # /usr/local; PREFIX=... must match the build
    man isekur

Copy `iSekur.conf.example` to `~/.config/isekur/isekur.conf`, or change
settings from inside the client with `/set theme neon`, `/set server.libera.autojoin #a,#b`
and write them back with `/save` (comments in the file are kept). Changes apply
right away; server settings are used on the next `/connect`, `[web]` after `/web stop` + `/web start`. Run
`./isekur --hash-password` to get the `[web] password_hash` value.

## Keys

| Key | Action |
|---|---|
| Alt+1..9, Alt+0 | go to tab N |
| Alt+←/→, Ctrl+P/N | previous / next tab |
| Alt+A | next tab with activity |
| Ctrl+X | next server |
| Ctrl+W | close tab (parts the channel) |
| PgUp / PgDn | scroll |
| F2 | toggle nicklist |
| F3 | toggle mouse (Shift+drag selects text while it's on) |
| Tab | complete nick |
| Ctrl+B / C / O / V / ] / _ | bold, color, reset, reverse, italic, underline |

Rebind any of the actions in the `[keys]` section.

## Commands

`/server host [port] [-tls] [-noverify]`, `/connect`, `/disconnect`, `/join`, `/part`,
`/msg`, `/query`, `/me`, `/notice`, `/nick`, `/topic`, `/close`, `/win N`, `/raw`, `/ctcp`,
`/theme`, `/set [section.]key [value]`, `/save`, `/ignore [mask]`, `/unignore mask`, `/grep [-all] text`, `/web [start|stop|status]`,
`/clear`, `/quit`. Any other command goes to the server unchanged
(`/whois`, `/mode`, `/away`, ...).
