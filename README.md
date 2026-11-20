# iSekur 0.5.2

[![Latest release](https://img.shields.io/github/v/release/gcanosa/iSekur?color=c792ea)](https://github.com/gcanosa/iSekur/releases/latest)
[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-ff5fd7)](LICENSE)
[![Platforms](https://img.shields.io/badge/platforms-macOS%20%7C%20Linux%20%7C%20FreeBSD%20%7C%20OpenBSD%20%7C%20NetBSD-7dcfff)](#install)
[![Website](https://img.shields.io/badge/website-isekur--site-9ece6a)](https://gcanosa.github.io/isekur-site/)
[![IRC](https://img.shields.io/badge/IRC-%23iSekur%20on%20irc.sekurnet.org-ffcb6b)](#support)

A small, very colorful terminal IRC client in C++17. One program is the client, the bouncer and
the webchat: it stays connected when you detach, other IRC clients can connect to it, and
your browser or phone shows the same live session.

**[Website](https://gcanosa.github.io/isekur-site/)** · **[Docs](https://gcanosa.github.io/isekur-site/docs.html)** · `man isekur` · `/help` inside

![iSekur in the Neon theme: topic bar, tabs with activity colors, nicklist, mIRC colors and a "new" marker above missed messages](https://gcanosa.github.io/isekur-site/img/chat-neon.png)

## Why iSekur

- **No screen, tmux or ZNC needed.** Alt+D detaches and `isekur --attach` brings it back from any
  terminal or SSH login. A closed window or a dropped connection detaches; it doesn't quit.
- **A bouncer built in.** irssi, weechat, HexChat, mIRC or a phone app connect to your iSekur and
  share its networks, channels and recent lines. Clients that support IRCv3 also get chathistory and echoes.
- **A webchat built in.** It shows the same live session over HTTPS in a browser and installs
  as an app on a phone, with notifications and Face ID unlock. You don't need a separate relay or web client.
- **45 24-bit themes** (dark, light, soft contrast, the classic mIRC, high contrast, mono)
  with a live picker, a 256-color fallback, and theme files that work as templates for your own.
- **Modern IRC.** TLS, WebSocket, SASL (PLAIN, SCRAM-SHA-256, EXTERNAL), and most of IRCv3:
  server-time, chathistory, MONITOR, replies, reactions, typing, redaction, read markers synced between your clients, and more.
- **End-to-end encryption** for private chats and channels between iSekur users (AES-256-GCM).
- **A TUI you can use with the mouse.** Click tabs and nicks, right-click menus, drag to move or
  resize, `/set` settings window, tab completion with live suggestions, `/split` panes, tabs on top
  or as a side tree.
- **Lua plugins** run in a sandbox and install from a catalog checked by sha256 (`/plugin install autoaway`).
- **And more:** a real shell in a tab (`/shell`, behind a system login), another machine's iSekur in
  a tab (`/attach host`), Twitch chat, DCC with resume, a log browser, a master lock password, channel op tools.

| | |
|---|---|
| ![The theme picker previewing themes live](https://gcanosa.github.io/isekur-site/img/theme-picker.png) | ![Tabs as a tree down the left side](https://gcanosa.github.io/isekur-site/img/doc-layout.png) |
| ![The webchat in the Rosé Pine theme](https://gcanosa.github.io/isekur-site/img/web-rose.png) | ![The webchat in the mIRC theme with its Windows 95 dialogs](https://gcanosa.github.io/isekur-site/img/web-mirc.png) |
| ![An encrypted channel: 🔒 on the tab and every line](https://gcanosa.github.io/isekur-site/img/doc-e2e.png) | ![The settings window](https://gcanosa.github.io/isekur-site/img/settings.png) |

## Install

macOS (Homebrew):

    brew install gcanosa/isekur/isekur

Prebuilt packages of the latest release:
[macOS arm64](https://github.com/gcanosa/iSekur/releases/latest/download/isekur-macos-arm64.tar.gz),
[Linux x86_64](https://github.com/gcanosa/iSekur/releases/latest/download/isekur-linux-x86_64.tar.gz),
[Linux arm64](https://github.com/gcanosa/iSekur/releases/latest/download/isekur-linux-arm64.tar.gz) (libssl3),
[Windows x86_64](https://github.com/gcanosa/iSekur/releases/latest/download/isekur-windows-x86_64.zip) (Cygwin build, without `/shell` or detach; WSL runs the Linux one),
[source](https://github.com/gcanosa/iSekur/releases/latest/download/isekur-src.tar.gz).

From source (C++17, GNU make, OpenSSL 1.1+/LibreSSL, PAM; termbox2 and libvterm are vendored):

    make && sudo make install     # gmake on *BSD

Every push is built and tested on macOS, Linux (glibc and Alpine's musl), FreeBSD, OpenBSD, NetBSD and Cygwin.
On the BSDs install GNU make first: `pkg install gmake` (FreeBSD), `pkg_add gmake` (NetBSD),
`pkg_add gmake libiconv` (OpenBSD, which has no PAM, so no `/shell` login there).

Run `isekur`. The first start writes `~/.config/isekur/isekur.conf` and opens a short guide.
After that, `/server irc.sekurnet.org 6697 -tls` and `/join #iSekur`. All the rest is in the
[docs](https://gcanosa.github.io/isekur-site/docs.html), the man page and `/help`.

## Support

Questions, bugs and ideas: **#iSekur** on **irc.sekurnet.org**, iSekur's sponsored network, or a GitHub issue.

## License

GPL-3.0-or-later, see [LICENSE](LICENSE). Copyright (C) 2026 Gerardo Canosa. The vendored termbox2 is MIT.
