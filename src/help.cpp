// Command reference: /help, /help <command>, and the list Tab completes from.
#include "core.h"

// mIRC colors, so the terminal and the webchat both show them
#define BOLD "\x02"
#define RESET "\x0f"
#define C_GROUP "\x02\x03" "08"  // bold yellow
#define C_CMD "\x03" "11"        // light cyan
#define C_ARGS "\x03" "15"       // light grey
#define C_EX "\x03" "09"         // light green
#define C_DIM "\x03" "14"        // grey

static const std::vector<CmdInfo> CMDS = {
    // ---- Connection
    {"server", "Connection", "host [port] [-tls] [-noverify] [-name name] [-nick nick] | rename [old] [new] | del name", "Connect to a new server, rename or delete one",
     "Opens a server tab and connects. With -tls the port defaults to 6697, otherwise 6667. "
     "-noverify skips the certificate check (self-signed servers). -name is its [server.<name>] "
     "(default: the host), -nick its own nick (default: [general] nick). An IPv6 address works bare or "
     "in brackets with its port: /server [2001:db8::1]:6697 -tls (named 2001-db8--1). The server is remembered "
     "like /set does, so /save keeps it for the next start. Naming a server you already have "
     "reconnects it. Twitch chat works too: /server irc.chat.twitch.tv 6697 -tls -name twitch -nick "
     "yourlogin, then /set server.twitch.password oauth:yourtoken and /connect twitch; /join #streamer "
     "(lowercase is done for you). Names, colors, badges, subs, raids, timeouts and room modes show up; "
     "WHOIS, DCC and Twitch's /ban /timeout don't exist over its IRC. A server that asks for TLS "
     "(IRCv3 STS) gets it: iSekur reconnects with TLS on the port it names, and remembers that for as long "
     "as the server says (the sts file next to the config; delete it to forget). /server rename gives a network a new name: "
     "its [server.<name>] in the config (saved at once, comments kept), its tab, its logs folder and kept history follow, "
     "no reconnect needed (the name is only yours). Without the names it asks, for the tab you're in; the settings window's "
     "name row and r do the same.",
     {"/server irc.sekurnet.org 6697 -tls -name sekurnet", "/server rename sekurnet sn", "/server irc.sekurnet.org", "/server 10.0.0.5 6697 -tls -noverify",
      "/server irc.chat.twitch.tv 6697 -tls -name twitch -nick mylogin"}},
    {"twitch", "Connection", "[token]", "Open the Twitch chat tab",
     "Opens the tab of your Twitch server (any [server.<name>] on twitch.tv; made as [server.twitch] if "
     "there's none). Already set up: asks to connect. Otherwise it says what's missing (your login and an "
     "OAuth token with chat:read and chat:edit) and asks for them, so the token never lands in your input "
     "history. They're kept like /set does: /save writes them to the config. /twitch token shows how to get "
     "a token: register an app at dev.twitch.tv/console (redirect http://localhost), open Twitch's sign-in "
     "link with its Client ID, approve, and copy access_token from the address bar. Once connected, "
     "/join streamer joins the chat of twitch.tv/streamer; /autojoin add #streamer and /save keep it.",
     {"/twitch", "/twitch token", "/join somestreamer"}},
    {"connect", "Connection", "[name]", "Reconnect a configured server",
     "Connects the server of the current tab, or the [server.<name>] you give.",
     {"/connect", "/connect sekurnet"}},
    {"servers", "Connection", "", "Manage your servers",
     "The terminal: the settings window at the first server, where c connects the selected one, r renames it, x deletes "
     "it (it asks first) and n adds one; its keys edit it. The webchat: the Servers window (the server-rack button). Elsewhere it "
     "lists them.",
     {"/servers"}},
    {"autoconnect", "Connection", "[on|off]", "Connect this server when iSekur starts",
     "Without arguments, says whether this tab's server connects at startup. on / off change it and write it to "
     "the config at once ([server.<name>] autoconnect). Also in a server tab's right-click menu.",
     {"/autoconnect", "/autoconnect off"}},
    {"reconnect", "Connection", "[message]", "Drop and connect again",
     "Quits this server (with the message, else quit_message) and connects again right away; "
     "the channels are joined again. Also in the menu of a right click on a server tab.",
     {"/reconnect", "/reconnect brb, new config"}},
    {"disconnect", "Connection", "[message]", "Quit this server, keep the tabs",
     "Sends QUIT to the server of the current tab and does not reconnect. Its tabs stay open; "
     "/connect brings it back and rejoins the channels.",
     {"/disconnect", "/disconnect brb, rebooting"}},
    {"quit", "Connection", "[message]", "Quit the server of this tab",
     "Sends QUIT to the server of the tab you're on, like /disconnect: its tabs stay and /connect comes back. "
     "In a tab with no server (the home tab, a shell) it does nothing. To close iSekur, use /exit.",
     {"/quit", "/quit see you tomorrow"}},
    {"exit", "Connection", "[-y] [message]", "Quit every server and close iSekur",
     "Sends QUIT everywhere (with the message, else quit_message) and exits. While a network is connected it "
     "asks first; -y doesn't ask. With a lock password (/lock on) it asks for that too, -y or not, so an "
     "unlocked terminal or an open webchat can't close iSekur. Alt+Q (Option+Q on a Mac) does the same. To keep "
     "iSekur running in the background instead, use /detach or Alt+D (Option+D on a Mac).",
     {"/exit", "/exit see you tomorrow", "/exit -y"}},
    {"nick", "Connection", "newnick", "Change your nickname",
     "Changes your nick on the server of the current tab; /nick alone shows it. When disconnected, it is used on the next connect. "
     "iSekur remembers it per network ([server.x] last_nick, written to the config at once), so "
     "reconnects and restarts use it; /set nick (or server.x.nick) goes back to the configured one.",
     {"/nick ethernet", "/nick ethernet_away"}},
    {"setname", "Connection", "real name", "Change your real name without reconnecting",
     "Sends SETNAME on servers with IRCv3 setname; others see it in WHOIS and the webchat nick list's tooltip. "
     "It lasts until you disconnect: /set realname keeps one for the next connect.",
     {"/setname Gerardo from SekurNet"}},
    {"register", "Connection", "[email]", "Register your nick as an account on this server",
     "On servers with IRCv3 account registration (draft/account-registration), asks for a password (hidden) "
     "and registers your current nick. Some servers need an email, and some send a code to it: /verify finishes "
     "then. Once it works, sasl_user and sasl_pass are set for this server: /save logs you in with SASL from then "
     "on. Elsewhere, use /msg NickServ REGISTER.",
     {"/register", "/register me@example.org"}},
    {"verify", "Connection", "account code", "Finish a /register with the emailed code",
     "Sends the code the server emailed after /register (IRCv3 VERIFY).",
     {"/verify ethernet 4f2a9c"}},

    // ---- Chat
    {"join", "Chat", "#channel [key]", "Join a channel",
     "The # is added when you leave it out. Give the key for channels with +k.",
     {"/join #sekurnet", "/j linux", "/join #private s3cret"}, "j"},
    {"autojoin", "Chat", "[add|del] [#channel [key]] | snap [here]", "Channels joined when the server connects",
     "Without arguments, lists this server's autojoin channels. add / del change the list, the "
     "current channel when you name none; add keeps the key you gave /join. Both are written to the config at once. "
     "snap makes every server's autojoin the channels open right now (here: only this server's) "
     "and writes it to the config at once.",
     {"/autojoin", "/autojoin add", "/autojoin add #private s3cret", "/autojoin del #old", "/autojoin snap"}},
    {"part", "Chat", "[#channel] [message]", "Leave a channel",
     "Leaves the current channel, or the one you name, and closes its tab, like /close and Ctrl+W. "
     "The message, or else part_message, goes with it (not on Twitch, which has none); the server's tab "
     "notes that you left.",
     {"/part", "/part #linux off to bed"}},
    {"msg", "Chat", "target text", "Send a private message",
     "Sends text to a nick or channel without opening a tab. If a tab for the target is "
     "already open the message shows there.",
     {"/msg NickServ IDENTIFY password", "/msg alice are you around?"}},
    {"query", "Chat", "nick [text]", "Open a private chat tab",
     "Opens (or switches to) a query tab with nick, and sends text if you give it.",
     {"/query alice", "/query bob hey, got a minute?"}},
    {"e2e", "Chat", "nick | #channel | off [nick|#channel]", "Encrypted chat with other iSekur users",
     "Asks for a password (typed hidden; an empty line cancels) and invites nick. They get a notice and "
     "accept with /e2e yournick and the same password. If the passwords match, both sides see "
     "\"Encrypted chat is on\" and the query is AES-256-GCM encrypted from then on; if not, "
     "both are told and nothing is encrypted. Keys live in memory only: after a restart, start "
     "again. /e2e off ends it, /e2e alone lists sessions. Other clients see +iSE lines. "
     "/e2e #channel asks for a password and encrypts what you say there; whoever typed the same "
     "password reads it (lines with 🔒), the rest see +iSE noise. No check: a wrong password just "
     "can't open the others' lines. Anyone with the password reads it all; to drop someone, change it. "
     "Also in the channel tab's right click menu. For an autojoin channel the key (not the password) "
     "is saved in the config as e2e_keys, so a restart doesn't ask; anyone who reads the file can read the channel.",
     {"/e2e alice", "/e2e #secret", "/e2e off", "/e2e"}, "sq"},
    {"me", "Chat", "action", "Send an action (/me waves)",
     "Shows as \"* yournick action\" in the channel or query.",
     {"/me waves", "/me is grabbing coffee"}},
    {"sv", "Chat", "", "Show off your iSekur version",
     "Sends \"* yournick is using iSekur <version> :: <OS> :: ... :: <website>\" as an action in the channel "
     "or query, the same text a CTCP VERSION reply gives.",
     {"/sv"}},
    {"notice", "Chat", "target text", "Send a notice",
     "Notices never trigger automatic replies, which is why bots use them.",
     {"/notice alice the build is green"}},
    {"topic", "Chat", "[#channel] [text|-delete]", "Show, set or clear the channel topic",
     "Without text, shows the topic. With text, sets it (channels with +t need ops); -delete clears it. "
     "A #channel first works on that one from any tab. A double click "
     "on the topic bar opens it in a box to edit, in the terminal and the webchat, when you may change it.",
     {"/topic", "/topic Welcome! Rules: be nice | https://example.org", "/topic -delete", "/topic #isekur"}},
    {"quiet", "Chat", "[on|off]", "Hide joins, parts, quits and nick changes in this tab",
     "Toggles for the current tab only. Hidden lines are still logged and come back with /quiet off. "
     "Your own joins and parts stay visible.",
     {"/quiet", "/quiet off"}},
    {"mute", "Chat", "[on|off]", "Mute hilights and alerts from this tab",
     "Toggles for the current tab and saves it in the network's mute setting right away. Your nick and hilight_words stop highlighting, "
     "ringing and being copied to window 1; messages still show and light up the tab. Muted tabs show 🔇. "
     "Also in the channel tab's right-click menu.",
     {"/mute", "/mute off"}},
    {"reply", "Chat", "nick text", "Reply to someone's last message here",
     "Sends text as an IRCv3 reply to nick's newest message in this tab: clients that know replies "
     "show it under that message, and iSekur shows it as \"↪ nick: “what they said”\" before your text. "
     "A reply to one of your messages hilights. On servers without message tags (or in an /e2e tab) it "
     "goes as \"nick: text\".",
     {"/reply alice yes, tonight", "/reply bob thanks!"}},
    {"react", "Chat", "emoji [nick]", "React to the last message here",
     "Sends an IRCv3 reaction to the newest message in this tab (or nick's newest). Reactions show as a "
     "line: \"bob reacted 👍 to your message “…”\". Needs message tags on the server.",
     {"/react 👍", "/react 😂 alice"}},
    {"redact", "Chat", "[reason]", "Delete your last message here",
     "On servers with IRCv3 message redaction, deletes your newest message in this tab for everyone: it "
     "shows as [message deleted]. Ops deleting others' messages show the same way.",
     {"/redact", "/redact wrong channel"}},
    {"notify", "Chat", "[add|del] nick [nick...]", "Be told when a nick comes online or goes",
     "Watches nicks on this tab's network: \"bob is online\" in the server tab (a hilight, so it rings and "
     "goes to window 1) and \"bob went offline\". It uses MONITOR, or ISON once a minute on servers "
     "without it. Alone it lists them, online first. add / del write the network's notify setting at once.",
     {"/notify", "/notify add alice bob", "/notify del bob"}},
    {"statusbar", "Chat", "[up|cpu|mem|load|bat] [on|off]", "Show uptime, CPU, memory, load or battery in the status bar",
     "Right of the status bar (under the nick list): up = how long iSekur has run, cpu = this "
     "machine's CPU use, mem = memory free/total, load = load average (1, 5, 15 min), bat = battery as 3 bars (green, yellow, orange, red "
     "below 15%) and its %, only on machines that have one. An item "
     "alone toggles it, /statusbar alone lists them. Kept until restart; /save writes it to "
     "status_info in the config. cpu and mem turn yellow, orange and red as they pass "
     "cpu_levels (default 50 75 90) and mem_levels (memory in use, default 70 85 95): "
     "/set cpu_levels 60 80 95. The theme sets the colors: status_info, status_ok, status_warn, "
     "status_high, status_crit and status_bat. What doesn't fit is left out from the end.",
     {"/statusbar cpu", "/statusbar bat on", "/statusbar up off"}},
    {"ctcp", "Chat", "target VERSION|TIME|PING", "Send a CTCP request",
     "PING shows the round trip time when the reply arrives.",
     {"/ctcp alice VERSION", "/ctcp alice PING"}},
    {"raw", "Chat", "line", "Send a raw IRC line",
     "Sends the line to the server exactly as typed. Unknown commands do this too.",
     {"/raw PRIVMSG #chan :hello", "/quote WHO #sekurnet"}, "quote"},
    {"rawlog", "Chat", "[on|off]", "Show every line to and from this server in a *raw tab",
     "For debugging: what the server sent (\xe2\x86\x90) and what iSekur sent (\xe2\x86\x92), as they are on the wire, "
     "numerics, CAP and replies iSekur doesn't know included. Per server, never logged; passwords and SASL payloads "
     "show as ***. /rawlog again, /rawlog off or closing the tab stops it.",
     {"/rawlog", "/rawlog off"}},

    // ---- Channel ops
    {"op", "Channel ops", "nick ...", "Give ops", "In the channel's tab; several nicks at once.", {"/op alice bob"}},
    {"deop", "Channel ops", "nick ...", "Take ops", "In the channel's tab.", {"/deop alice"}},
    {"halfop", "Channel ops", "nick ...", "Give halfop (+h)", "In the channel's tab, on servers with halfops.", {"/halfop alice"}},
    {"dehalfop", "Channel ops", "nick ...", "Take halfop", "In the channel's tab.", {"/dehalfop alice"}},
    {"voice", "Channel ops", "nick ...", "Give voice", "In the channel's tab.", {"/voice alice bob"}},
    {"devoice", "Channel ops", "nick ...", "Take voice", "In the channel's tab.", {"/devoice alice"}},
    {"ban", "Channel ops", "[nick|mask ...]", "Ban, or list the bans",
     "In the channel's tab. A bare nick bans nick!*@*; /kickban finds the host. /ban alone lists the bans.",
     {"/ban", "/ban troll", "/ban *!*@bad.host"}},
    {"unban", "Channel ops", "mask ...", "Lift a ban", "In the channel's tab; the mask as /ban lists it.", {"/unban *!*@bad.host"}},
    {"massop", "Channel ops", "", "Op everyone in the channel",
     "Gives +o to everyone who isn't an op yet, several per MODE line (as many as the server's "
     "MODES allows). Needs ops.",
     {"/massop"}},
    {"massdeop", "Channel ops", "", "Deop everyone except yourself",
     "Takes +o from every op but you. Needs ops.",
     {"/massdeop"}},
    {"massvoice", "Channel ops", "", "Voice everyone",
     "Gives +v to everyone below halfop who doesn't have it. Handy with /lockdown: the regulars can "
     "keep talking while the channel is moderated.",
     {"/massvoice"}},
    {"massdevoice", "Channel ops", "", "Remove voice from everyone",
     "Takes +v from everyone but you.",
     {"/massdevoice"}},
    {"masskick", "Channel ops", "[-yes] [reason]", "Kick everyone who isn't an op or halfop",
     "Asks first: without -yes it only says how many users it would kick. Ops, halfops and you stay. "
     "Kicks go through flood control, so a big channel takes a while.",
     {"/masskick spam wave", "/masskick -yes spam wave"}},
    {"lockdown", "Channel ops", "", "Lock the channel: +mntsi",
     "Moderated (only voiced and ops talk), no outside messages, topic lock, secret, invite only. "
     "Use it during a flood or raid; /unlock lifts it.",
     {"/lockdown", "/lockdown then /massvoice to let the regulars talk"}},
    {"unlock", "Channel ops", "", "Lift a lockdown: -msi",
     "Removes moderated, secret and invite only. +n and +t stay.",
     {"/unlock"}},
    {"kickban", "Channel ops", "nick [reason]", "Ban *!*@host and kick, in one go",
     "Bans the user's host (so a new nick doesn't get them back in) and kicks them. If the host isn't "
     "known yet iSekur asks the server for it first.",
     {"/kickban troll", "/kb troll flooding the channel"}, "kb"},
    {"tempban", "Channel ops", "nick minutes [reason]", "Kick + ban, unbanned after some minutes",
     "Like /kickban, then lifts the ban after the given minutes (1 to 10080). The unban is sent by "
     "iSekur, so it has to be running then. /chanstats lists the pending ones.",
     {"/tempban troll 30 spam", "/tempban flooder 1440 come back tomorrow"}},
    {"chanstats", "Channel ops", "", "Users, ops, voices, topic and modes",
     "Counts the nicklist (ops, halfops, voiced, regular, away) and shows the modes, the topic, "
     "the topic rotation and pending temp bans. Nothing is sent to the server.",
     {"/chanstats"}},
    {"topicrotate", "Channel ops", "minutes topic | topic ... | off", "Change the topic on a timer",
     "Sets the topics in turn, one every N minutes (1 to 1440), starting right away. Separate them "
     "with |. Without arguments shows what's rotating; off stops it. It runs while iSekur runs and "
     "is not saved.",
     {"/topicrotate 60 Welcome! | Rules: be nice | Meeting Friday 18:00 UTC", "/topicrotate", "/topicrotate off"}},

    // ---- Windows
    {"win", "Windows", "N", "Go to tab N",
     "Same as Alt+1..9 / Alt+0, but works for tabs past 10.",
     {"/win 3", "/win 12"}, "window"},
    {"move", "Windows", "N", "Move this tab to place N among its server's tabs (a server tab: among the servers)",
     "1 is right after the server tab. In a server tab it moves the server, with all its tabs. Dragging a tab onto "
     "another of its server (a server tab onto another server's), here or in the webchat, does the same. The order is "
     "kept: channels in the server's autojoin list, servers in [general] server_order.",
     {"/move 1", "/move 3"}, "window"},
    {"close", "Windows", "", "Close the current tab",
     "Parts the channel first, like /part (with part_message). On a server tab it disconnects and closes all of the server's tabs.",
     {"/close"}, "wc"},
    {"split", "Windows", "[side|stack] [N|name|off]", "Show two tabs at once",
     "Opens a second pane with tab N or the tab named so (this server's first), or the next tab "
     "when none is given. side puts them side by side, stack one over the other; without either it goes "
     "by the window's width. /split side or /split stack while split just changes the layout. "
     "Each channel pane has its own nicklist (hidden when its pane gets too narrow; F2 hides them all). "
     "Typing and the topic follow the focused pane: F6 or Alt+O switches (the "
     "[keys] action is other_pane), and so does a click in the other pane or picking its tab. "
     "Changing tabs changes the focused pane only. In the webchat it works the same, per browser. "
     "Alone, while split, it says what's in the other pane.",
     {"/split 3", "/split side #linux", "/split stack 2", "/split stack", "/split off"}},
    {"unsplit", "Windows", "", "Back to one tab at a time",
     "Closes the second pane from /split; the focused tab stays on screen. Closing the tab that's "
     "in the other pane does it too.",
     {"/unsplit"}, "merge"},
    {"fold", "Windows", "", "Hide or show a server's channels in the tab bar",
     "The server's tab shows +N for them, colored like their news; Ctrl+N/P, Ctrl+A and /win still go "
     "to them. The same as a click on the ▾ next to the network's name in the webchat, and in a server tab's right click menu.",
     {"/fold"}},
    {"clear", "Windows", "", "Clear the current tab's scrollback",
     "Only clears what is on screen; logs are untouched.",
     {"/clear"}},
    {"grep", "Windows", "[-all] text", "Search the scrollback",
     "Shows the lines of the current tab that contain text (nick or message, case insensitive), "
     "the last 50 at most. -all searches every tab.",
     {"/grep http", "/grep -all alice"}},
    {"logs", "Windows", "[text]", "Browse and search the chat logs",
     "Opens the log browser (terminal and webchat) at this tab's logs: a tree of networks, their channels and queries "
     "and each one's days (newest first), the picked day's lines on the right. Typing in the find line filters the day "
     "as you type; Enter searches every day of the channel or network picked in the tree. The find line takes words "
     "(all of them, any case), \"a phrase\" and nick:alice (only her lines, joins and quits too). Enter (or a double "
     "click) on a found line opens its day there. c opens a calendar of the channel's month with its logged days in "
     "color; pick one to read it. [ and ] go to the day before or after, e shows or hides joins, parts, quits and nick "
     "changes, Tab moves between the tree and the log, / goes to the find line, Esc closes. With text, it opens "
     "searching every day of this tab for it. Needs log = on (/set log on, /save); logs of networks no longer in the "
     "config are listed too.",
     {"/logs", "/logs release", "/logs nick:alice green"}},
    {"keys", "Windows", "", "Show every keyboard shortcut",
     "Opens a window with every key binding drawn as keycaps, by group (tabs, view, session, typing), in the terminal "
     "and the webchat; F1 or Alt+K open it too. Your own [keys] bindings show as well. Up/Down scrolls, Esc closes. "
     "Change a key with /set keys.<action> \"<key> [key...]\" (e.g. /set keys.next_tab \"M-Right C-n\").",
     {"/keys", "/set keys.find_tab \"C-g M-j\""}},
    {"theme", "Windows", "[name]", "Show or switch the color theme",
     "Alone: a picker in the terminal and the webchat; type to filter, Up/Down previews live (both), Enter keeps (as /set theme, so /save writes it), Esc goes back. "
     "Themes are files in themes/*.theme (also ~/.config/isekur/themes/); /set theme_dir ~/mythemes looks in your own folder first, "
     "the shipped ones still found after it. The switch is live; /set theme name + /save keeps it.",
     {"/theme", "/theme dracula", "/theme catppuccin-latte", "/set theme_dir ~/mythemes"}},

    // ---- Settings
    {"set", "Settings", "[[section.]key [value]]", "The settings window, or show or change one setting",
     "Alone it opens the settings window: All, then Global ([general], by topic), each network with its own topics "
     "(open the one you pick), then Web. A network lists its own keys and every Global one it can override: "
     "\"inherits\" shows Global's value, \"overrides Global\" its own (d gives Global's back), and the Global key "
     "names the networks overriding it. "
     "/ searches, Up/Down moves, Tab or Left/Right goes between categories and keys, e or Enter edits, "
     "Space switches on/off (and steps through the themes, applied at once), d or Del puts the default "
     "back, s or Ctrl+S saves, q or Esc closes; the mouse works too. The webchat's ⚙ is the same, with "
     "switches and a theme list. With a key it shows it; with a value it changes it right away. "
     "The section defaults to [general]. While typing the key, matching keys are listed and Tab completes them. "
     "/set key \"\" empties it, which means its default again. 0 turns off a key that counts: no nick column "
     "(nick_width), never (lock_idle, exec_timeout), no limit (paste_confirm), none (log_history, "
     "history_fetch); a port of 0 is the usual one.",
     {"/set", "/set theme neon", "/set server.sekurnet.autojoin #a,#b", "/set log on"}},
    {"reload", "Settings", "", "Re-read the config file",
     "Applies an edited config without restarting: theme, keys, mouse, colors, bell, logging and "
     "the rest right away; server settings on the next /connect; [web] after /web restart. "
     "Servers added to the file are opened (and connected when autoconnect is on); removed "
     "ones stay until you close their tab. /set changes not yet saved stay on top.",
     {"/reload"}},
    {"save", "Settings", "", "Write changed settings to the config file",
     "Saves what /set, /server, /autojoin and /ignore changed. Comments in the file are kept.",
     {"/save"}},
    {"hilight", "Settings", "[add|del] word", "Words highlighted besides your nick on this network",
     "Without arguments, lists them. Each network has its own list (hilight_words in its [server.name] "
     "section). Matching ignores case and takes whole words; * matches anything (isekur*). /save keeps the list.",
     {"/hilight", "/hilight add isekur", "/hilight add ger*", "/hilight del isekur"}},
    {"ignore", "Settings", "[mask]", "Ignore a nick or mask on this network, or list its ignores",
     "Each network has its own list (ignore in its [server.name] section): the same nick elsewhere is "
     "often someone else. A bare nick becomes nick!*@*. Ignored users' messages, notices, invites and "
     "CTCPs are dropped. /save keeps the list.",
     {"/ignore", "/ignore spammer", "/ignore *!*@*.evil.host"}},
    {"unignore", "Settings", "mask", "Stop ignoring a mask",
     "On this network. Use the mask exactly as /ignore lists it.",
     {"/unignore spammer!*@*"}},

    // ---- Session
    {"exec", "Session", "[-o] [-y] command", "Run a shell command",
     "The output shows up in the cmd tab as it comes, without blocking the chat. With -o it is "
     "said in the tab you typed it in once the command ends, unless it is longer than "
     "paste_confirm lines (0: no limit; -y sends it anyway). Commands are killed after exec_timeout "
     "seconds (default 30) or 1000 lines. /exec alone lists the running ones. Off in the "
     "webchat unless /set exec_web on, since it runs commands on this machine; with a /lock "
     "password set, turning exec_web on or off asks for it.",
     {"/exec ls -la", "/exec -o uptime", "/sh git log --oneline -5"}, "sh"},
    {"dcc", "Session", "[list] | get [number|nick] | send nick file | close number|nick", "Send and receive files (DCC)",
     "A file someone sends you is only offered: nothing is downloaded until you /dcc get it "
     "(the newest offer, the one with that number, or the newest from that nick). It goes into "
     "dcc_dir (default ~/Downloads, on the machine iSekur runs on) without overwriting anything; "
     "a smaller file there with the same name is taken as a broken download and resumed. /dcc send "
     "offers a file (a name alone is looked for in dcc_dir; from the webchat only those) and waits "
     "5 minutes for them to take it. Behind a router set dcc_ip to your public address and forward "
     "dcc_ports (e.g. 5000-5010) to this machine. Transfers run in the background with their "
     "progress in the status bar and the webchat. /dcc close refuses an offer, cancels a transfer "
     "or takes a finished one off the list. With dcc_web on, the webchat's Files window downloads "
     "what's in dcc_dir, like a seedbox. dcc = off (/set dcc off, every network) drops offers "
     "and turns off /dcc get and send. Passive DCC is not supported yet.",
     {"/dcc", "/dcc get", "/dcc get 3", "/dcc send bob ~/notes.txt", "/dcc close bob"}},
    {"shell", "Session", "", "Open a real terminal in a tab",
     "Asks for this machine's login and password first (the user iSekur runs as, checked by "
     "the system through PAM; pam_service sets the service, default login; three wrong ones "
     "lock it for a minute), then runs $SHELL as a login shell on a pty: vim, top, ssh, "
     "Ctrl+C all work. Every key goes to it except Alt+arrows and Alt+1-9 (other tabs); "
     "the mouse wheel scrolls back. exit ends it, Ctrl+W then closes the tab. Terminal only: "
     "never from the webchat.",
     {"/shell"}},
    {"attach", "Session", "[-p port] host [config]", "Another machine's iSekur in a tab, over ssh",
     "Runs ssh -t host there and isekur --attach on it (through a login shell, so its PATH is "
     "the usual one) in a tab like /shell's: the remote iSekur, live, inside this one. host is a "
     "name, user@host or a Host from ~/.ssh/config (keys go there); -p port is ssh's own -p, for a port other than 22. ssh asks for its own "
     "password or uses your keys; no system login is asked first. Every key goes to the remote "
     "except Alt+arrows and Alt+1-9, so Alt+D or /detach typed there detaches it and leaves it "
     "running; the tab then says so, and /attach host again reconnects. If nothing runs there, a "
     "new iSekur starts. Several iSekurs on one machine each run with their own config (isekur -c "
     "work.conf); config picks one (isekur -c config --attach there) and names the tab host:work; "
     "ssh host isekur --list shows which run there. "
     "The mouse isn't passed on. Terminal only: never from the webchat.",
     {"/attach vps", "/attach me@irc.example.org", "/attach -p 2222 me@irc.example.org", "/attach vps ~/.config/isekur/work.conf"}},
    {"lock", "Session", "[on|off]", "Lock the session (terminal and webchat) with a password",
     "/lock or Alt+S (Option+S on a Mac) locks now: the terminal and every webchat show only a password box, and the webchat "
     "gets nothing of the session until it's typed (in either one). The first time it asks for "
     "a password (twice) and keeps it: from then on iSekur also locks at startup and on every "
     "--attach. /lock on sets or changes it (the current one first) without locking; /lock off "
     "asks for it and turns locking off. lock_idle = N locks after N minutes without typing "
     "(0, the default: never). Forgot it? Stop iSekur, set lock = off in the config and start "
     "it again. Three wrong tries wait a minute. While it's set, /exit asks for it as well. The config and "
     "logs are not encrypted by it.",
     {"/lock", "/lock on", "/lock off", "/set lock_idle 15"}},
    {"plugin", "Session", "[list] | available | install|update|remove name | enable|disable name[@network[/#channel]] | set name[@network[/#channel]] key value | reload name",
     "Lua plugins: install, turn on and off, configure",
     "Plugins are Lua 5.4 files in the plugins folder next to the config (<name>.lua), sandboxed: no files, no programs, "
     "no network of their own. /plugin available lists the catalog (github.com/gcanosa/isekur-plugins), kept on disk and fetched again "
     "after 12 hours, or now with /plugin refresh (once a minute at most); /plugin install name fetches one, "
     "checks its sha256 against the catalog and turns it on; /plugin update installs every newer version (a plugin "
     "says its own in a \"-- version: 1.2\" line), /plugin update name also puts back a file you edited; "
     "/plugin remove name deletes it with its settings and saved data. Auto-away is a plugin: /plugin install autoaway "
     "(it moves your old auto_away settings into [plugin.autoaway]). "
     "/plugin (or /plugins) alone opens the plugin window: Installed and Available tabs, Space turns one on or off, "
     "c configures it (←→ picks everywhere, a network or a channel), i installs, d removes; the webchat's puzzle button "
     "opens the same window. /plugin list prints them instead. /plugin enable name turns one on everywhere; "
     "name@sekurnet only in that network, name@sekurnet/#chan only in that channel; disable the same way. "
     "/plugin set name key value sets one of its options ([plugin.name] in the config), name@network or "
     "name@network/#channel for that network or channel only; an empty value goes back to the default. "
     "Every change is written to the config at once. /plugin reload name loads it again after you edited the file. "
     "A plugin that errors 5 times in a row is unloaded until /plugin reload.",
     {"/plugin available", "/plugin install autoaway", "/plugin set autoaway@sekurnet idle 15", "/plugin enable seen@sekurnet/#isekur",
      "/plugin", "/plugin reload seen"}, "plugins"},
    {"web", "Session", "[start|stop|restart|status]", "Control the built-in webchat",
     "The webchat mirrors this session in a browser. It needs [web] user and password_hash "
     "(isekur --hash-password prints one). /web restart picks up changed [web] settings.",
     {"/web", "/web start", "/web stop", "/web restart"}},
    {"detach", "Session", "", "Leave iSekur running in the background",
     "Gives the terminal back; iSekur stays connected, in the channels, logging. isekur --attach "
     "(or just isekur) brings it back, from any terminal or SSH session. Alt+D (Option+D on a Mac) "
     "and Ctrl+Z do the same. Closing the terminal or losing SSH detaches too, instead of quitting.",
     {"/detach", "isekur --attach  (from your shell, later)"}},
    {"typing", "Session", "[on|off]", "Messages type themselves out",
     "Other people's messages, actions and notices appear letter by letter with a cursor, about "
     "30 a second (a long line still takes at most 2s), in the terminal and the webchat. Alone it "
     "toggles. Kept until restart; /save writes typing_effect to the config. Quiet mode comes on "
     "with it in every channel (joins and parts would be noise between typed lines) and goes off "
     "again with /typing off, except where you turned it on yourself.",
     {"/typing", "/typing off"}},
    {"about", "Session", "", "About iSekur",
     "Opens the *about tab: the version, the author, where to get iSekur, its sponsored network "
     "irc.sekurnet.org and the support channel #iSekur there. The webchat shows it in a dialog (the i button, bottom left).",
     {"/about"}},
    {"help", "Session", "[command]", "This help",
     "Without arguments lists every command. With a command name shows details and examples. "
     "Tab completes command names here and after \"/\". New here? /guide walks you through the first steps.",
     {"/help", "/help join", "/help set"}},
    {"guide", "Session", "[page|next|prev|on|off]", "The first steps guide, page by page",
     "Opens the *guide tab: the first steps (your nick, a server, channels, Twitch, keys, settings, the webchat, "
     "safety) on pages you turn with Left/Right (empty input line), a click on Prev/Next, or /guide next "
     "and /guide prev; q or Esc (empty input line) or Ctrl+W closes it. /guide 3 jumps to page 3. It opens by itself at startup while show_guide is on: "
     "the \"Don't show this at startup\" box on each page (or /guide off) turns that off, /guide on back on.",
     {"/guide", "/guide 5", "/guide next", "/guide off"}},

    // ---- Server (sent as typed)
    {"whois", "Server", "nick", "Who is this user",
     "Unknown commands are sent to the server as typed, so everything your server supports works.",
     {"/whois alice"}},
    {"away", "Server", "[message]", "Set or clear away",
     "With a message marks you away, without one (or /back) clears it. Away by itself when you're idle or detached: /plugin install autoaway (a manual /away is left alone). "
     "Its message, idle time and announcing it in your channels can be set per network (/help plugin). "
     "While you're away the status bar shows [away: network, ...] on every tab, the terminal's and the webchat's.",
     {"/away lunch", "/away", "/back"}},
    {"back", "Server", "", "Come back from away",
     "Clears your away on the server of this tab, like /away without a message.",
     {"/back"}},
    {"mode", "Server", "[target] modes [args]", "Change channel or user modes",
     "In a channel's tab the channel is the target when you start with + or -, and /mode "
     "alone shows its modes.", {"/mode +m", "/mode #chan +o alice", "/mode #chan +b *!*@bad.host"}},
    {"kick", "Server", "[#channel] nick [reason]", "Kick a user",
     "In a channel's tab the channel can be left out. /kickban kicks and bans in one go.",
     {"/kick troll bye", "/kick #chan troll bye"}},
    {"invite", "Server", "nick [#channel]", "Invite a user to a channel",
     "In a channel's tab the channel can be left out.", {"/invite alice", "/invite alice #private"}},
    {"list", "Server", "[pattern]", "List channels",
     "The server's answer opens in this server's *channels tab as a table, busiest first "
     "(up to 1900 rows; a new /list replaces it). The pattern is sent as typed: *word* "
     "filters by name; many servers also take >N (more than N users). /list sort name|users "
     "(or a click on the Channel or Users header) sorts it again, a second time reverses it; "
     "double click a channel to join it.", {"/list", "/list *linux*", "/list sort name"}},

    // ---- IRC operator (sent to the server; the reason/message needs no ':')
    {"oper", "IRC operator", "user [password]", "Log in as an IRC operator",
     "Without the password you are asked for it, hidden and kept out of the input history. "
     "While you are +o or +s, server notices (\"*** ...\") and WALLOPS go to a *snotices tab, "
     "and the nick menu gets Kill and G-line.",
     {"/oper gera", "/oper gera s3cret"}},
    {"kill", "IRC operator", "nick reason", "Disconnect a user from the network",
     "The user sees \"Killed (yournick (reason))\" and may reconnect; /gline keeps them out.", {"/kill troll flooding #help"}},
    {"wallops", "IRC operator", "message", "Message every user with +w (and the opers)",
     "/globops and /locops (opers only, network or this server) work the same way.",
     {"/wallops services restart in 5 minutes", "/globops checking the spam wave"}},
    {"gline", "IRC operator", "mask [duration] reason", "Ban a user@host from the whole network",
     "/kline (this server), /zline (an IP, before the connection registers) and /shun (connected "
     "but silenced) take the same arguments; /ungline, /unkline, /unzline and /unshun lift them. "
     "Durations: 0 (permanent), 3600, 30m, 1d, 1w. Without arguments lists the active lines.",
     {"/gline *@1.2.3.4 1d spam bots", "/kline *@bad.isp 0 abuse", "/ungline *@1.2.3.4", "/gline"}},
    {"sajoin", "IRC operator", "nick #channel", "Force a user into (or /sapart out of) a channel",
     "/sanick nick newnick and /samode #channel modes work the same way, ignoring channel rules.",
     {"/sajoin alice #help", "/sapart troll #help", "/samode #chan +o alice"}},
    {"chghost", "IRC operator", "nick host", "Change a user's host (/chgident: the ident)",
     "/sethost host changes your own.", {"/chghost alice staff.sekurnet.org"}},
    {"rehash", "IRC operator", "", "Reload the server configuration",
     "/die and /restart stop or restart the server. Server linking: /squit server reason, and "
     "/quote CONNECT server (iSekur's own /connect reconnects you).",
     {"/rehash", "/squit hub.example.org maintenance"}},
};

const std::vector<CmdInfo>& commands() { return CMDS; }

const CmdInfo* find_command(const std::string& name) {
    std::string n = irc_lower(name);
    for (auto& c : CMDS) {
        if (n == c.name) return &c;
        for (auto& a : split(c.aliases, ' '))
            if (n == a) return &c;
    }
    return nullptr;
}

static std::string usage(const CmdInfo& c) {
    return C_CMD BOLD "/" + std::string(c.name) + RESET + (*c.usage ? " " C_ARGS + std::string(c.usage) + RESET : "");
}

void Core::help(Buffer* b, const std::string& topic) {
    auto out = [&](const std::string& t) {
        Line l{time(nullptr), LK::INFO, "", t};
        l.nolog = true;
        add(b, l);
    };
    std::string q = topic;
    if (!q.empty() && q[0] == '/') q.erase(0, 1);
    if (q.empty()) {
        out(BOLD "iSekur commands" RESET C_DIM "  -  /help <command> for details and examples, Tab completes" RESET);
        out("\x03" "01,09" BOLD " NEW HERE? " RESET " " C_CMD BOLD "/guide" RESET " walks you through the first steps, page by page");
        const char* group = "";
        for (auto& c : CMDS) {
            if (std::string(group) != c.group) {
                group = c.group;
                out(C_GROUP + std::string(group) + (std::string(group) == "Server" ? RESET C_DIM " (sent to the server as typed)" : "") + RESET);
            }
            std::string plain = "/" + std::string(c.name) + (*c.usage ? " " + std::string(c.usage) : "");
            std::string pad(plain.size() < 34 ? 34 - plain.size() : 2, ' ');
            out("  " + usage(c) + pad + c.summary);
        }
        out(C_DIM "Anything else starting with / goes to the server unchanged. Type // to send a line that starts with /." RESET);
        return;
    }
    const CmdInfo* c = find_command(q);
    if (!c) {
        std::string near;
        for (auto& k : CMDS)
            if (std::string(k.name).find(irc_lower(q)) != std::string::npos) near += " /" + std::string(k.name);
        out("\x03" "04No such command: /" + q + RESET +
            (near.empty() ? " - /help lists them all. Unknown commands go to the server as typed." : ". Did you mean:" C_CMD + near + RESET "?"));
        return;
    }
    out(usage(*c) + C_DIM "  -  " RESET + c->summary);
    if (*c->aliases) {
        std::string al;
        for (auto& a : split(c->aliases, ' ')) al += (al.empty() ? "/" : ", /") + a;
        out("  " C_DIM "Also: " RESET C_CMD + al + RESET);
    }
    out("  " + std::string(c->details));
    out("  " C_GROUP "Examples" RESET);
    for (auto& e : c->examples) out("    " C_EX + std::string(e) + RESET);
}

// ---- /guide: the first steps, one page at a time in the *guide tab ---------
// mIRC codes like the rest, so the webchat shows the same colors
#define G_TITLE "\x03" "00,02" BOLD          // white on blue
#define G_KEY(k) "\x03" "01,15" " " k " " RESET  // a keycap
#define G_CMD(x) "\x03" "11" BOLD x RESET     // something to type
#define G_VAL(x) "\x03" "13" "\x1d" x RESET   // your value: replace it
#define G_HEAD(x) "\x03" "08" BOLD "▸ " x RESET
#define G_TIP "\x03" "01,09" BOLD " TIP " RESET " "
#define G_NOTE "\x03" "01,08" BOLD " NOTE " RESET " "
#define G_WARN "\x03" "00,04" BOLD " CAREFUL " RESET " "
#define G_OK "\x03" "09" "✔ " RESET

struct GuidePage { const char* title; std::vector<const char*> lines; };
static const std::vector<GuidePage> GUIDE = {
    {"Welcome to iSekur", {
        "iSekur is a colorful IRC client for the terminal, with " BOLD "many servers at once" RESET " and a",
        BOLD "built-in webchat" RESET " that mirrors your session in a browser. This guide takes ~5 minutes.",
        "",
        G_HEAD("How the screen works"),
        "  " "\x03" "12" "Top" RESET "     the topic bar, then the " BOLD "tabs" RESET ": one per server, channel and private chat",
        "  " "\x03" "12" "Middle" RESET "  the chat, and the " BOLD "nick list" RESET " on the right in channels",
        "  " "\x03" "12" "Bottom" RESET "  the status bar and the " BOLD "input line" RESET ": type text to talk, " G_CMD("/commands") " to act",
        "",
        G_HEAD("Turning pages"),
        "  " G_KEY("←") " " G_KEY("→") "  with the input line empty, or click " "\x03" "00,12" BOLD " ◀ Prev " RESET " and " "\x03" "00,03" BOLD " Next ▶ " RESET " below",
        "  " G_CMD("/guide 4") " jumps to a page, " G_KEY("q") ", " G_KEY("Esc") " or " G_KEY("Ctrl+W") " closes it, " G_CMD("/guide") " brings it back",
        "",
        G_TIP "Text in " "\x03" "13" "\x1d" "pink italics" RESET " is yours to replace, e.g. " G_CMD("/nick ") G_VAL("yournick") ".",
    }},
    {"Who you are", {
        "Before connecting, pick the name people will see. These go in " BOLD "[general]" RESET ", for every server:",
        "",
        "  " G_CMD("/set nick ") G_VAL("yournick") "          your nickname",
        "  " G_CMD("/set altnick ") G_VAL("yournick_") "      used if the first one is taken",
        "  " G_CMD("/set realname ") G_VAL("Your Name") "    shown in /whois",
        "  " G_CMD("/save") "                       write them to the config file",
        "",
        G_HEAD("Saved or not?"),
        "  " G_CMD("/set") " changes apply " BOLD "right away" RESET " but only for this run. " G_CMD("/save") " keeps them for the",
        "  next start. Forgot? The settings window marks unsaved keys, and " G_CMD("/save") " any time.",
        "",
        G_TIP "Already connected? " G_CMD("/nick ") G_VAL("newnick") " changes it now, and iSekur remembers it.",
    }},
    {"Your first server", {
        "One command connects, opens a tab for the server and remembers it:",
        "",
        "  " G_CMD("/server irc.SekurNet.org 6697 -tls -name sekurnet"),
        "  " G_CMD("/save") "         so it connects by itself next time",
        "  " G_CMD("/join #iSekur") " say hi: iSekur's home, with help, news and feedback",
        "",
        G_HEAD("The pieces"),
        "  " "\x03" "10" "6697 -tls" RESET "      an encrypted connection (use it whenever the network offers it)",
        "  " "\x03" "10" "-name sekurnet" RESET " a short name: its settings live in " BOLD "[server.sekurnet]" RESET,
        "  " "\x03" "10" "-nick " RESET G_VAL("other") "    a different nick on this network only",
        "",
        G_HEAD("Registered nick? Log in with SASL"),
        "  " G_CMD("/set server.sekurnet.sasl_user ") G_VAL("yournick"),
        "  " G_CMD("/set server.sekurnet.sasl_pass ") G_VAL("secret") "   then " G_CMD("/save") " and " G_CMD("/connect"),
        "",
        "  " G_CMD("/disconnect") " leaves a server (tabs stay), " G_CMD("/connect") " comes back, " G_CMD("/quit") " leaves this tab's server, " G_CMD("/exit") " closes iSekur.",
        "  " G_CMD("/servers") " manages them: " G_KEY("c") " connects, " G_KEY("x") " deletes, " G_KEY("n") " adds one.",
        "  Right-click a server tab: " BOLD "Connect at startup" RESET " or not, " BOLD "Delete server" RESET ".",
        G_NOTE BOLD "irc.SekurNet.org" RESET " is iSekur's own network: any other works the same way.",
    }},
    {"Channels and talking", {
        G_HEAD("Channels"),
        "  " G_CMD("/join #channel") "      opens a tab and joins (or " G_CMD("/list") " to browse, double click to join)",
        "  " G_CMD("/part") "               leaves the channel and closes its tab (so do " G_CMD("/close") " and " G_KEY("Ctrl+W") ")",
        "  " G_CMD("/topic ") G_VAL("text") "         sets the topic (or double click the topic bar)",
        "",
        G_HEAD("Talking"),
        "  Just type and press " G_KEY("Enter") ". " G_CMD("/me waves") " is an action, " G_CMD("/msg ") G_VAL("nick text") " a private message.",
        "  " G_CMD("/query ") G_VAL("nick") " opens a private tab. " G_KEY("Tab") " completes nicks, commands and settings.",
        "",
        G_HEAD("People"),
        "  Click a nick to talk privately, " BOLD "right click" RESET " it for a menu (whois, op, kick, ignore...).",
        "  " G_CMD("/whois ") G_VAL("nick") " shows who someone is, " G_CMD("/ignore ") G_VAL("nick") " hides them.",
        "",
        G_TIP "While you type a command, the list under the input line shows what fits: " G_KEY("↑") G_KEY("↓") " pick, " G_KEY("Tab") " fills.",
    }},
    {"Your channels, every time", {
        "Each server keeps an " BOLD "autojoin" RESET " list: the channels it joins as soon as it connects.",
        "",
        G_HEAD("One by one"),
        "  " G_CMD("/autojoin") "                    this server's list",
        "  " G_CMD("/autojoin add") "                add the channel of this tab",
        "  " G_CMD("/autojoin add #private ") G_VAL("key") "   a channel with a key (the one you gave " G_CMD("/join") " is kept)",
        "  " G_CMD("/autojoin del #old") "           take one out",
        "  written to the config at once; a channel tab's right-click menu does the same",
        "",
        G_HEAD("The snapshot: everything at once"),
        "  Joined all the channels you want? Take a picture of them:",
        "  " G_CMD("/autojoin snap") "        every server's autojoin = the channels open " BOLD "right now" RESET,
        "  " G_CMD("/autojoin snap here") "   the same, only for this server",
        "  It's " BOLD "written to the config at once" RESET ": no " G_CMD("/save") " needed.",
        "",
        G_TIP "Rearrange your channels for a while, then " G_CMD("/autojoin snap") " again to keep the new set.",
    }},
    {"Twitch chat", {
        "Twitch's chat speaks IRC, so iSekur joins it like any other server, in a tab of its own:",
        "",
        "  " G_CMD("/twitch") "               opens the Twitch tab and asks for your login and token",
        "  " G_CMD("/join ") G_VAL("streamer") "       the chat of " "\x03" "12" "twitch.tv/" RESET G_VAL("streamer") " (# and lowercase are added)",
        "  " G_CMD("/autojoin add") " + " G_CMD("/save") "  in that channel's tab: rejoined on every start",
        "",
        G_HEAD("Getting a token, once"),
        "  " "\x03" "14" "1." RESET " " "\x03" "12" "\x1f" "dev.twitch.tv/console" RESET ": register an app, OAuth Redirect URL " BOLD "http://localhost" RESET,
        "  " "\x03" "14" "2." RESET " " G_CMD("/twitch token") " shows the sign-in link: your app's " BOLD "Client ID" RESET " goes in it",
        "  " "\x03" "14" "3." RESET " approve, then copy what follows " BOLD "access_token=" RESET " in the address bar",
        "  " "\x03" "14" "4." RESET " " G_CMD("/twitch") " and paste it when it asks (it's typed hidden)",
        "",
        G_HEAD("What you'll see"),
        "  names in their own colors, " BOLD "@" RESET " mods and " BOLD "+" RESET " VIPs, ★ subs and raids, timeouts, slow mode...",
        "",
        G_WARN "The token is a password to your Twitch account: never paste it in a chat.",
    }},
    {"Getting around", {
        G_HEAD("Tabs"),
        "  " G_KEY("Alt+1") ".." G_KEY("Alt+0") "    go to tab 1-10        " G_KEY("Ctrl+N") " " G_KEY("Ctrl+P") "  next / previous tab",
        "  " G_KEY("Ctrl+A") "           next tab with news    " G_KEY("Ctrl+X") "          next server",
        "  " G_KEY("Ctrl+W") "           close this tab        " G_KEY("PgUp") " " G_KEY("PgDn") "     scroll the chat",
        "  " G_KEY("Ctrl+G") " " G_KEY("Alt+J") "    find a tab by name (channel, query, network)",
        "  " G_KEY("Alt+S") "            lock the screen now (with a " G_CMD("/lock") " password)",
        "  " G_KEY("Alt+Q") "            quit iSekur (asks first; " G_KEY("Alt+D") " detaches instead)",
        "",
        G_HEAD("Showing and hiding"),
        "  " G_KEY("Ctrl+L") " or " G_KEY("F2") "  the nick list     " G_KEY("Ctrl+T") "  the topic bar     " G_KEY("F3") "  the mouse",
        "",
        G_HEAD("Two tabs at once"),
        "  " G_CMD("/split ") G_VAL("3") "   tab 3 too (or " G_CMD("/split ") G_VAL("#chan") ")     " G_KEY("F6") " or " G_KEY("Alt+O") "  switch panes",
        "  " G_CMD("/split side") " / " G_CMD("stack") "   beside or one over the other     " G_CMD("/unsplit") "  one tab again",
        "",
        G_HEAD("The mouse"),
        "  click tabs and the " "\x03" "08" "[Act: 3]" RESET " numbers, wheel scrolls, " BOLD "drag to copy" RESET ", right click a",
        "  tab for its menu (autojoin, reconnect, fold). Tab colors: " "\x03" "10" "news" RESET ", " "\x03" "09" "messages" RESET ", " "\x03" "04" "your nick" RESET ".",
        "",
        G_NOTE "On a Mac, Alt is " BOLD "Option" RESET ". If Option keys type symbols, turn on \"Option as Meta\"",
        "  in your terminal's settings. Keys are yours to change in " BOLD "[keys]" RESET " (" G_CMD("/help set") ").",
        "  " G_CMD("/keys") ", " G_KEY("F1") " or " G_KEY("Alt+K") " shows every key in a window, anytime.",
    }},
    {"Make it yours", {
        G_HEAD("The settings window"),
        "  " G_CMD("/set") " alone opens every setting by category: " G_KEY("/") " searches, " G_KEY("e") " edits,",
        "  " G_KEY("Space") " flips on/off or steps through a list, " G_KEY("d") " puts the default back, " G_KEY("s") " saves, " G_KEY("Esc") " closes.",
        "  Each network has Global's keys too: set one there and it overrides Global for that network only.",
        "",
        G_HEAD("Colors"),
        "  " G_CMD("/theme") " lists the themes, " G_CMD("/theme dracula") " tries one live. Some to try:",
        "  " "\x03" "13" "dracula" RESET "  " "\x03" "12" "tokyonight" RESET "  " "\x03" "06" "catppuccin-mocha" RESET "  " "\x03" "07" "gruvbox" RESET "  " "\x03" "10" "nord" RESET "  " "\x03" "15" "github-light" RESET,
        "  The one you pick is " BOLD "kept" RESET ": the next start opens with it, no " G_CMD("/save") " needed.",
        "  " BOLD "mirc" RESET " draws its windows like Windows 95; " BOLD "ayu" RESET ", " BOLD "github-light" RESET " add Mac buttons.",
        "",
        G_HEAD("Little extras"),
        "  " G_CMD("/typing on") "       messages type themselves out, letter by letter",
        "  " G_CMD("/quiet") "           hide joins, parts and quits in this channel",
        "  " G_CMD("/statusbar cpu on") " CPU, memory, load or battery in the status bar",
        "  " G_CMD("/set timestamp %H:%M:%S") "   seconds in the time stamps",
        "  " G_CMD("/set timestamp_align right") " the time at the right edge of the chat",
        "  " G_CMD("/set nick_width 16") "     room for longer nicks before they're cut (0: no column)",
    }},
    {"Never miss a thing", {
        G_HEAD("Highlights"),
        "  Lines with your nick light up, ring the bell and are copied to tab 1 (" BOLD "hilight_window" RESET ").",
        "  " G_CMD("/hilight add ") G_VAL("word") " + " G_CMD("/save") " lights up other words too.",
        "",
        G_HEAD("Logs"),
        "  After a restart every channel and query shows its last lines again (" BOLD "keep_history" RESET ").",
        "  " G_CMD("/set log on") " + " G_CMD("/save") " also writes them all to disk. " G_CMD("/grep ") G_VAL("text") " searches this tab.",
        "  " G_CMD("/logs") " browses them by network, channel and day, with a calendar and a search over every day.",
        "",
        G_HEAD("Stay online when you leave"),
        "  " G_KEY("Ctrl+Z") ", " G_KEY("Alt+D") " or " G_CMD("/detach") " gives the terminal back and " BOLD "keeps iSekur connected" RESET ".",
        "  Later, from any terminal or SSH session: " G_CMD("isekur --attach") " (or just " G_CMD("isekur") ").",
        "  On another computer: " G_CMD("/attach ") G_VAL("host") " shows that machine's iSekur in a tab (over ssh).",
        "  Several at once: one config each, " G_CMD("isekur -c ") G_VAL("work.conf") "; " G_CMD("isekur --list") " shows them,",
        "  " G_CMD("isekur -c ") G_VAL("work.conf") G_CMD(" --attach") " or " G_CMD("/attach ") G_VAL("host work.conf") " picks one.",
        "  Away while detached: you are marked " BOLD "away" RESET " by yourself, and back when you type.",
        "",
        G_TIP "Closing the terminal or losing SSH " BOLD "detaches" RESET " too: nothing is lost.",
    }},
    {"The webchat", {
        "The same session, live, in a browser (phones too). Set a login once:",
        "",
        "  " "\x03" "14" "1." RESET " in your shell: " G_CMD("isekur --hash-password") " and copy what it prints",
        "  " "\x03" "14" "2." RESET " " G_CMD("/set web.user ") G_VAL("me"),
        "  " "\x03" "14" "3." RESET " " G_CMD("/set web.password_hash ") G_VAL("the-hash") "   then " G_CMD("/save"),
        "  " "\x03" "14" "4." RESET " " G_CMD("/web start") " and open " "\x03" "12" "\x1f" "http://127.0.0.1:8443" RESET,
        "",
        G_HEAD("From another machine"),
        "  " G_CMD("/set web.bind 0.0.0.0") ": then it's " BOLD "HTTPS only" RESET " (a self-signed certificate is made,",
        "  or give " BOLD "web.cert" RESET " and " BOLD "web.key" RESET "). " G_CMD("/set web.autostart on") " starts it with iSekur.",
        "",
        G_HEAD("Tabs in the browser"),
        "  " BOLD "‹" RESET " at the top of the tab list folds it to a strip, " BOLD "›" RESET " opens it. " BOLD "👥" RESET " hides the users.",
        "  " G_CMD("/split ") G_VAL("3") " and " G_CMD("/unsplit") " work there too, for that browser only.",
        "  The server-rack button lists your servers: edit, connect, delete or add one.",
        "  " G_CMD("/set web_font sans") " and " G_CMD("/set web_font_size 16") ": the browser's font and size.",
        "  " G_CMD("/set web_tree_lines on") ": faint lines join each network to its channels.",
        "  " G_CMD("/set web_tree_width 280") ": the tab list's width in px (120 to 600).",
        "",
        G_TIP "Run it on a server with " G_CMD("isekur --daemon") ": always online, chat from anywhere.",
    }},
    {"Privacy and safety", {
        "  " G_CMD("/lock on") "        a password before anything shows (startup, attach, " G_CMD("/lock") ")",
        "  " G_CMD("/e2e ") G_VAL("nick") "       end-to-end encrypted private chat with another iSekur user",
        "  " G_CMD("/ignore ") G_VAL("nick") "    never see them again (" G_CMD("/save") " to keep it)",
        "  " G_CMD("/dcc") "            files sent to you wait for " G_CMD("/dcc get") ": nothing downloads by itself",
        "",
        G_WARN "The config file holds your passwords: it is created private (only you can read",
        "  it). Don't paste it anywhere. Passwords never show in " G_CMD("/set") " or the settings window.",
        "",
        G_NOTE "Self-signed server? " G_CMD("/server ") G_VAL("host") " 6697 -tls -noverify" ", only if you trust it.",
    }},
    {"You're ready", {
        G_OK "Set your nick:  " G_CMD("/set nick ") G_VAL("yournick"),
        G_OK "Connect:        " G_CMD("/server irc.SekurNet.org 6697 -tls -name sekurnet"),
        G_OK "Join:           " G_CMD("/join #iSekur"),
        G_OK "Every time:     " G_CMD("/autojoin snap"),
        G_OK "Keep the rest:  " G_CMD("/save") "  (themes and autojoin keep themselves)",
        "",
        G_HEAD("Where to look next"),
        "  " G_CMD("/help") "            every command, by group",
        "  " G_CMD("/help ") G_VAL("command") "    details and examples (" G_CMD("/help server") ", " G_CMD("/help set") ")",
        "  " G_CMD("/set") "             every setting, explained in the config file too",
        "  " G_CMD("/guide") "           this guide, any time",
        "",
        "  Questions, ideas, bugs? " G_CMD("#iSekur") " on " BOLD "irc.SekurNet.org" RESET ": see you there. Have fun!",
    }},
};

// the guide's mIRC colors in the theme's: each code stands for a role (title, key, command...);
// a role the theme leaves at the terminal default keeps the mIRC color
static std::string themed(const Theme& t, const std::string& s) {
    auto fg = [&](const char* k) { return t.get(k).fg; };
    auto bg = [&](const char* k) { RGB c = t.get(k).bg; return c < 0 ? t.get("default").bg : c; };
    RGB dark = bg("default") < 0 ? 0x000000 : bg("default");  // text on a colored label
    auto nick = [&](size_t i) { return t.nicks.empty() ? -1 : t.nicks[i % t.nicks.size()]; };
    const std::map<std::string, std::pair<RGB, RGB>> role = {
        {"00,02", {fg("tab_active"), bg("tab_active")}},   // the page title
        {"01,15", {fg("topic"), bg("topic")}},             // a key
        {"11", {fg("input_prompt"), -1}},                  // a command
        {"13", {fg("set_value"), -1}},                     // your value
        {"08", {fg("set_key"), -1}},                       // a heading
        {"01,09", {dark, fg("join")}},                     // TIP
        {"01,08", {dark, fg("notice")}},                   // NOTE
        {"00,04", {dark, fg("error")}},                    // CAREFUL
        {"00,12", {fg("menu_sel"), bg("menu_sel")}},       // Prev
        {"00,03", {dark, fg("join")}},                     // Next / Done
        {"09", {fg("join"), -1}}, {"03", {fg("nicklist_voice"), -1}},
        {"14", {fg("timestamp"), -1}}, {"15", {fg("nicklist"), -1}},
        {"12", {fg("info"), -1}}, {"10", {fg("action"), -1}},
        {"06", {nick(6), -1}}, {"07", {nick(1), -1}},
    };
    std::string out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] != '\x03' || i + 1 >= s.size() || !isdigit((unsigned char)s[i + 1])) { out += s[i++]; continue; }
        size_t j = i + 1;
        while (j < s.size() && j < i + 3 && isdigit((unsigned char)s[j])) j++;
        if (j + 1 < s.size() && s[j] == ',' && isdigit((unsigned char)s[j + 1])) {
            j += 2;
            if (j < s.size() && isdigit((unsigned char)s[j])) j++;
        }
        std::string code = s.substr(i + 1, j - i - 1);
        auto r = role.find(code);
        if (r == role.end() || r->second.first < 0 || (code.find(',') != std::string::npos && r->second.second < 0)) out += s.substr(i, j - i);
        else out += "\x04" + rgb_hex(r->second.first).substr(1) + (code.find(',') != std::string::npos ? "," + rgb_hex(r->second.second).substr(1) : "");
        i = j;
    }
    return out;
}

void Core::guide(Buffer* from, const std::string& arg) {
    int n = (int)GUIDE.size();
    if (arg == "on" || arg == "off") {
        cfg["general"]["show_guide"] = arg;
        unsaved["general"].erase("show_guide");
        std::string err;
        if (!ini_save(cfg_path, Ini{{"general", {{"show_guide", arg}}}}, err)) print(from, LK::ERROR, err);
        Buffer* g = find(nullptr, "*guide");
        if (g) guide(g, "page");  // redraw the box
        if (g != from) print(from, LK::INFO, arg == "on" ? "The guide opens at startup again" : "The guide won't open at startup (/guide shows it)");
        return;
    }
    if (arg == "close") { if (Buffer* g = find(nullptr, "*guide")) close(g); return; }
    if (arg == "next") guide_page++;
    else if (arg == "prev") guide_page--;
    else if (!arg.empty() && isdigit((unsigned char)arg[0])) guide_page = atoi(arg.c_str()) - 1;
    else if (arg != "page" && !arg.empty()) { print(from, LK::ERROR, "/guide [page|next|prev|on|off]"); return; }
    guide_page = std::max(0, std::min(guide_page, n - 1));
    Buffer* b = open(nullptr, BT::QUERY, "*guide");
    b->lines.clear(), b->scroll = b->unread = 0;
    notify(Ev::CLEAR, b);
    auto out = [&](const std::string& t) {
        Line l{time(nullptr), LK::INFO, "", themed(theme, t)};
        l.nolog = true;
        add(b, l);
    };
    const GuidePage& p = GUIDE[(size_t)guide_page];
    std::string dots;
    for (int i = 0; i < n; i++) dots += i == guide_page ? "\x03" "09●" : i < guide_page ? "\x03" "03●" : "\x03" "14○";
    out(G_TITLE "  " + std::to_string(guide_page + 1) + ". " + p.title + "  " RESET "  " + dots + RESET);
    out("");
    for (auto& l : p.lines) out(l);
    out("");
    bool first = guide_page == 0, last = guide_page == n - 1;
    std::string prev = first ? "\x03" "14" " ◀ Prev " RESET : "\x03" "00,12" BOLD " ◀ Prev " RESET;
    std::string next = last ? "\x03" "00,03" BOLD " ✔ Done " RESET : "\x03" "00,03" BOLD " Next ▶ " RESET;
    std::string box = opt_on(nullptr, "show_guide", false) ? "\x03" "15" "☐ Don't show this at startup" RESET
                                                           : "\x03" "09" "☑" RESET "\x03" "15" " Don't show this at startup" RESET;
    out(prev + C_DIM "  page " + std::to_string(guide_page + 1) + " of " + std::to_string(n) + "  " RESET + next + "      " + box);
    if (from != b) select(b);
    dirty = true;
}
