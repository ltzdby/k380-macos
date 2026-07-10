### Logitech K380 Bluetooth Keyboard F-n keys mode switcher for macOS

F-n keys work in a non-standard (media) mode by default on this keyboard. Rather
than installing "Logi Options" just to switch them, this little tool flips the
mode, and can keep it flipped automatically across reconnects.

The K380 does **not** persist the setting — it resets on power-off, sleep, and
every Bluetooth reconnect — so an optional LaunchAgent re-applies it whenever the
keyboard connects.

#### Build

    $ make

This builds three things in place:

- `libhidapi.dylib` — vendored [hidapi](https://github.com/libusb/hidapi)
  (version pinned in `hidapi-src/VERSION`), built from `hidapi-src/mac`.
- `k380` — the configurator, dynamically linked against `libhidapi.dylib` (found
  at runtime via an `@loader_path` rpath, so the two must stay in the same
  directory).
- `k380-listener` — the auto-reapply event consumer (see below).

The committed binaries are built for `arm64` (Apple Silicon / aarch64). On an
Intel Mac, rebuild them with `make`.

#### One-off use

    $ sudo ./k380 -f on|off      # enable / disable direct F-key access
    $ ./k380 -l                  # list HID devices

If `-f` prints `unable to open device`, see `TROUBLESHOOTING.md` (most often the
terminal needs Input Monitoring permission, or Karabiner-Elements is grabbing the
keyboard).

#### Auto-reapply on every connect

`make install` renders a per-user LaunchAgent for this checkout and loads it.
launchd then starts `k380-listener` on demand whenever the keyboard connects; the
listener re-applies the F-key mode and exits.

    $ make install

`make install` itself does not talk to the keyboard — it just installs the agent
and prints the two grants required for it to work (see `TROUBLESHOOTING.md` for
details):

1. A passwordless sudoers entry for the `k380` binary — the listener runs
   `sudo -n k380`:

        echo "$(id -un) ALL=(ALL) NOPASSWD: $(pwd)/k380" | sudo tee /etc/sudoers.d/k380
        sudo chmod 440 /etc/sudoers.d/k380

2. **Input Monitoring** granted to `k380-listener` (not your terminal): macOS
   prompts when the listener first opens the keyboard on a connect — approve it,
   or add `k380-listener` manually under
   **System Settings → Privacy & Security → Input Monitoring**. If the K380 is
   already connected when you run `make install`, `bootstrap` replays it as a
   connect event, so the prompt appears right away.

Verify with the log after the next connect:

    $ tail ~/Library/Logs/k380-fnkeys.log   # look for: rc=0 (F-n keys: ON)

To remove it:

    $ make uninstall

This unloads and deletes the LaunchAgent; it prints a reminder to remove the
sudoers entry (that step needs root).

#### Credits

Based on prior work referenced in the source headers (k480/k810 configurators and
Benjamin Tissoires' pairing tool).
