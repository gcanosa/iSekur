# Writing iSekur plugins

A plugin is one Lua 5.4 file, `<name>.lua` (letters, digits, `_`, `-`), in the `plugins` folder next
to your config (`~/.config/isekur/plugins/`). It runs once when it loads: there it declares its
settings, commands and event handlers. `/plugin enable <name>` loads it, `/plugin reload <name>`
loads it again after you edit it.

Put its version in a `-- version:` line near the top. iSekur reads it to tell an update in the
catalog (a newer version) from a file you edited yourself (same version, other contents).

```lua
-- version: 1.0
-- hello.lua: /hello [nick], and a wave back at anyone who says "hi isekur"
isekur.setting("greeting", "Hello", "c")  -- per channel, network or global; "Hello" by default

isekur.command("hello", "[nick]", function(args, srv, target)
  if not srv then return "Use /hello in a channel" end  -- a returned string is shown as an error
  local who = args ~= "" and args or "everyone"
  isekur.say(srv, target, isekur.opt("greeting", srv, target) .. ", " .. who .. "!")
end)

isekur.on("message", function(e)
  if not e.self and e.text:lower():find("hi isekur", 1, true) then
    isekur.say(e.srv, e.target, "\x01ACTION waves at " .. e.nick .. "\x01")
  end
end)
```

## The sandbox

Only Lua's base library (without `load`, `loadfile`, `dofile`), `string`, `table`, `math` and `utf8`.
No `io`, `os`, `package`, `require` or `debug`. A call that runs too long (about 10 million Lua
instructions) is stopped with an error, and a plugin gets 32 MB of memory. Errors show in the main
window as `[name] error: ...`; after 5 in a row the plugin is unloaded until `/plugin reload`.
`print(...)` writes `[name] ...` to the main window.

## Settings

Options live in the config, one section per scope, and are looked up channel → network → global →
the default the plugin declared:

```ini
[plugin.hello]                  ; enabled = on: everywhere
greeting = Hi
[plugin.hello@sekurnet]         ; this network
greeting = Hey
[plugin.hello@sekurnet/#isekur] ; this channel (lowercase)
greeting = Welcome
```

`/plugin set hello@sekurnet/#isekur greeting Welcome` writes that last one.
`enabled` works the same way: a plugin is loaded when any of its sections has `enabled = on`, and its
events only come from the networks and channels where `enabled` resolves to on.

- `isekur.setting(key, default, scope)`: declare an option. `scope` is `"g"` (global only), `"n"`
  (up to the network) or `"c"` (up to the channel, the default). The default's type is what
  `opt` returns: a boolean (`on`/`true`/`yes`/`1` are true), a number or a string.
- `isekur.opt(key, srv, target)`: the option's value there; `srv`/`target` may be nil. `enabled` is
  always a boolean: whether the plugin is on there.

## Events

`isekur.on(event, function(e) ... end)`. `e.srv` (the network's name) and `e.target` (channel or
query nick) are set when the event has them.

| event | fields |
|---|---|
| `message` | `nick`, `text`, `self`, `hilight`, `notice`, `action`. Return `true` to hide the line. Your own lines only arrive when the server has echo-message |
| `join` | `nick`, `host`, `self` |
| `part` | `nick`, `reason`, `self`, `kick`, `by` (who kicked) |
| `quit` | `nick`, `host`, `reason` (no target: it's the whole network) |
| `nick` | `nick` (the old one), `new`, `self` (no target) |
| `away` | `away`: the server says you're away (306) or back (305) |
| `command` | `name`, `args`: you typed a /command (not one a plugin runs). Return `true` to take it over |
| `tick` | every second, no fields: check `isekur.opt("enabled", srv)` for the networks you act on |
| `attach`, `detach` | a terminal or a browser came back / nobody is attached any more |

## Calls

| call | what it does |
|---|---|
| `isekur.command(name, usage, fn)` | adds `/name`; `fn(args, srv, target)` gets the rest of the line, the tab's network (nil in the main window) and the tab's name. iSekur's own commands can't be replaced |
| `isekur.say(srv, target, text)` | says text (each line of it) in a channel or to a nick |
| `isekur.send(srv, line)` | a raw IRC line (one line) |
| `isekur.print(srv, target, text, note)` | shows text in that tab only (else the network's tab, else the main window); `note` true puts the INFO badge first |
| `isekur.exec(srv, target, "/cmd ...")` | runs a command as if typed in that tab (not `/plugin`) |
| `isekur.store_get(key)`, `isekur.store_set(key, value)` | the plugin's own saved strings (`<name>.data`); `nil` removes a key. Written within 5 seconds, and when it's unloaded or iSekur exits |
| `isekur.store_keys()` | every key in the store |
| `isekur.active()` | the tab you're on: its network (nil for the main window) and its name |
| `isekur.date(t, fmt)` | `t` (default now) as local time with strftime's `fmt` (default: your timestamp format) |
| `isekur.is_chan(srv, name)` | a channel name by that network's own prefixes |
| `isekur.servers()` | names of the connected networks |
| `isekur.channels(srv)` | the channels joined there |
| `isekur.me(srv)` | your nick there, nil when not connected |
| `isekur.rank(srv, chan, nick)` | the nick's prefix there (`"@"`, `"+"`, `""`), nil if it's not in the channel |
| `isekur.away(srv)`, `isekur.attached()` | booleans |
| `isekur.idle()`, `isekur.detached_for()`, `isekur.time()` | seconds since you typed, seconds nobody's been attached (0 while attached), the Unix time |
| `isekur.name`, `isekur.api` | the plugin's name, the API version (1) |

## The catalog

`/plugin available` and `/plugin install` read this folder from GitHub: `index.json` and
`<name>/<name>.lua`. To add a plugin, put it in `plugins/<name>/<name>.lua` and an entry in
`index.json`:

```json
{"name": "hello", "version": "1.0", "api": 1, "description": "Says hello",
 "sha256": "<shasum -a 256 plugins/hello/hello.lua>"}
```

The sha256 must match the file byte for byte: iSekur refuses the install otherwise. `version` must
be the file's own `-- version:` line; raise both with every change, or nobody is offered the
update (`make test` checks both for every entry). iSekur keeps the index it fetched in
`.catalog.json` in the user's plugins folder and fetches it again after 12 hours (`/plugin refresh`
sooner), so a new version can take that long to show up. `api` is the `isekur.api` the plugin needs; older iSekurs hide it.
`ISEKUR_PLUGIN_URL=file:///path/to/plugins/ isekur` installs from a local folder for testing.
