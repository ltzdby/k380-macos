# Troubleshooting

Notes for diagnosing the two things that actually go wrong with this tool on
macOS: `k380` failing to open the keyboard, and the auto-reapply LaunchAgent not
firing. Ordered roughly by how often they bite.

## Background: how the pieces fit together

- `k380` — the configurator. `k380 -l` only *enumerates* HID devices (reads the
  IORegistry, opens nothing). `k380 -f on|off` *opens* the keyboard with an
  exclusive seize and writes the mode command. These are different code paths,
  which is why listing can succeed while `-f` fails.
- `libhidapi.dylib` — vendored hidapi (see `hidapi-src/VERSION`), loaded by
  `k380` from the same directory via an `@rpath`/`@loader_path` rpath.
- `k380-listener` — a small event consumer that handles
  `com.apple.iokit.matching` events and runs `sudo -n k380 -f on` whenever the
  keyboard connects.
- `com.local.k380-fnkeys` — the LaunchAgent that runs the listener.

The K380 does **not** persist the F-key mode: it resets on power-off, sleep, and
every Bluetooth reconnect. Re-applying on each connect is expected, not a bug.

## `k380 -f on` prints "unable to open device"

`hid_open` failed. `k380 -l` will still list the keyboard because enumeration
does not open it. Work through the causes below in order.

### 1. Missing Input Monitoring permission (most common)

hidapi opens the keyboard with `kIOHIDOptionsTypeSeizeDevice`. Seizing a keyboard
requires the process to hold the **Input Monitoring** TCC grant, and that grant
is keyed to the *specific binary* that calls `IOHIDDeviceOpen` and, for spawned
chains, to the process that launched it.

- Running `k380` yourself from a terminal: grant **Input Monitoring** to your
  terminal app (e.g. Ghostty) in
  **System Settings → Privacy & Security → Input Monitoring**, then fully quit
  and reopen the terminal (the grant is read at process start).
- Running under the LaunchAgent (`k380-listener → sudo → k380`): the responsible
  process is `k380-listener`, so it needs its own **Input Monitoring** grant.
  The prompt appears when the *listener* first tries to open the keyboard, i.e.
  on a keyboard connect. If the K380 is already connected when you run
  `make install`, `bootstrap` replays it as a connect event and the prompt for
  `k380-listener` appears right away; otherwise it appears on the next connect.
  Approve it, or add it manually: in
  **System Settings → Privacy & Security → Input Monitoring**, click `+`,
  navigate to `.../k380-macos/k380-listener`, enable it, then reload the agent
  (see below). (If you instead run `k380` yourself from a terminal, that is a
  separate case — the grant then applies to your terminal app, not the listener.)

A quick tell: if the log shows `k380` itself printing `unable to open device`
while `sudo` did not error, `sudo` succeeded and the failure is TCC, not sudoers.

### 2. Karabiner-Elements (or similar) is grabbing the keyboard

Karabiner's grabber opens keyboards with an exclusive seize, so `hid_open` gets
`kIOReturnExclusiveAccess` and returns NULL.

- Confirm it is grabbing: the grabber log
  (`/var/log/karabiner/core_service.log`) shows lines like
  `Keyboard K380 (device_id:...) hid queue value monitor is started (grabbed)`.
- Fix: open **Karabiner-Elements → Devices** and uncheck the K380 (this sets
  `"ignore": true` for it in `~/.config/karabiner/karabiner.json`). Note that in
  recent Karabiner versions the grabber runs as a system daemon; quitting only
  the menu-bar app does not release the device — unchecking the device does.
- After unchecking, the grabber log should no longer list the K380 as
  `(grabbed)`. Then re-apply with `k380 -f on`.

### 3. The keyboard is not actually connected

`k380 -l` should list `046d b342 Keyboard K380`. If it does not, reconnect over
Bluetooth first — there is nothing to open.

## The LaunchAgent doesn't re-apply on reconnect

### Check it is loaded and see recent activity

```sh
launchctl print gui/$(id -u)/com.local.k380-fnkeys | grep -E 'state|program'
tail -n 20 ~/Library/Logs/k380-fnkeys.log
```

The listener runs on demand and exits a few seconds after handling a connect, so
most of the time no process is running — that is expected, not a problem. The log
is the real signal of activity. Healthy log lines look like:

```
listener started (pid 12345, k380=/…/k380-macos/k380)
event k380-connect -> k380 -f on: rc=0 (F-n keys: ON)
```

### Reload after changing the listener, plist, or TCC grants

TCC grants and the listener binary are read at process start, so after granting
Input Monitoring or rebuilding, reload the agent:

```sh
launchctl bootout gui/$(id -u)/com.local.k380-fnkeys 2>/dev/null
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.local.k380-fnkeys.plist
```

`launchctl kickstart -k` only restarts the process; it does **not** redeliver a
connect event, so it won't trigger an apply. A fresh `bootstrap` does, because
launchd replays the currently-connected device as an event.

### `unable to open device` again after rebuilding the listener

Input Monitoring grants are keyed to the exact binary contents, so recompiling
`k380-listener` invalidates its existing grant, and macOS won't re-prompt while a
(now-stale) entry for that path is still listed. Fix: in
**System Settings → Privacy & Security → Input Monitoring**, select the old
`k380-listener` entry and remove it with `−`, then add it again with `+` and
enable it, and reload the agent. (Day-to-day use is unaffected — this only
happens when you rebuild the listener.)

### `rc=1` in the log

The listener ran `sudo -n k380 -f on` and it failed. Distinguish the cause from
the captured output on the same log line:

- `(unable to open device)` → an "unable to open device" cause above (usually
  Input Monitoring not granted to `k380-listener`).
- `(sudo: a password is required)` → the sudoers entry is missing or wrong; see
  below.

### It loops / re-applies every ~10s

IOKit match events must be consumed via `xpc_set_event_stream_handler()`;
otherwise launchd relaunches the job every ~10s while the device is present.
`k380-listener` consumes them, so make sure the loaded plist runs
`k380-listener` (not a script) and reload it.

## Passwordless sudo (sudoers)

The listener runs `sudo -n k380` (non-interactive). Seizing a keyboard needs
root, so a NOPASSWD sudoers entry for the `k380` binary is required:

```sh
echo "$(id -un) ALL=(ALL) NOPASSWD: $(pwd)/k380" | sudo tee /etc/sudoers.d/k380
sudo chmod 440 /etc/sudoers.d/k380
```

The path must match the installed `k380` location. Verify non-interactively:

```sh
sudo -n ./k380 -l >/dev/null && echo "sudoers OK" || echo "sudoers missing/wrong"
```

## dylib not found at runtime

If `k380` reports `Library not loaded: @rpath/libhidapi.dylib`, the dylib is not
in the same directory as the `k380` binary. `k380` resolves it via an rpath of
`@loader_path` (the directory of the binary itself), so `k380` and
`libhidapi.dylib` must ship together. Rebuild with `make`, or copy the dylib
next to `k380`.
