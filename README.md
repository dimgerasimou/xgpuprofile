# xgpuprofile

Chooses which GPU drives your X display — the low-power integrated one, or
the discrete one — from a saved mode, with an optional rule for deciding
automatically.

One C file, no dependencies beyond libc, no daemon.

## The problem

On a hybrid laptop the external video outputs are wired to the discrete GPU.
With the integrated GPU owning the X screen, those displays are driven by
copying every frame across PCIe for the dGPU to scan out — *reverse PRIME*.
At 2560x1440 @ 144Hz that's roughly 2.1 GB/s of copying that has to finish
inside each 6.9 ms frame, and it doesn't keep up. It feels like lag, and no
compositor tuning fixes it, because the copy is architectural.

Handing the screen to the dGPU removes the copy. The cost is that the dGPU
can then never reach its deepest idle power state — so this isn't something
to just leave on, which is what the modes and rules are for.

## Why nothing switches at runtime

Tools that switch GPU modes live have to unload the graphics kernel modules
and restart X while you're using it. That's the riskiest possible moment for
the operation, and it's where they hang or fail — especially with a display
actively attached to the GPU being unloaded.

This never does that. Xorg fixes its GPU assignment when the server starts,
so a change is only *staged* — written to a config fragment — and applies the
next time Xorg starts. Nothing is torn down behind your back.

## Install

```bash
make
sudo make install
sudo systemctl daemon-reload
sudo systemctl enable xgpuprofile.service
```

Then add the runtime half to `~/.xinitrc`, before exec'ing your window
manager (or to your display manager's session startup):

```sh
/usr/local/bin/xgpuprofile-xrandr
```

**This is not optional on a laptop.** The fragment can hand the screen to the
dGPU and keep the integrated one present, but connecting them — attaching the
integrated GPU's outputs as a scanout sink for the discrete one — is an
`xrandr` call, and `xrandr` needs a running X server. Without it the built-in
panel stays dark whenever the discrete layout is active. The script is safe
to run unconditionally: under the hybrid layout there's no pairing to make
and it does nothing.

## Modes

| mode | what it does |
|---|---|
| `hybrid` | keep the integrated GPU — the ordinary low-power setup |
| `auto` | let the rule below decide |
| `dgpu` | always hand the display to the discrete GPU |

```bash
sudo xgpuprofile --mode auto     # saved, persists across reboots
sudo xgpuprofile --restart-x     # apply now instead of at next login
```

## Rules

With `mode = auto`, `rule` decides when to use the discrete GPU:

| rule | uses the dGPU when |
|---|---|
| `both` | on mains **and** an external display attached (default) |
| `either` | on mains **or** an external display attached |
| `ac` | on mains, whatever is plugged in |
| `external` | an external display attached, whatever the power source |
| `always` | unconditionally — same as `mode = dgpu` |

## One-off overrides

To use the discrete GPU right now — say, gaming with no monitor attached —
without touching your saved configuration:

```bash
sudo xgpuprofile --once dgpu
sudo xgpuprofile --restart-x
```

`--once` writes nothing to `/etc/xgpuprofile.conf`, so there's nothing to
undo: the next `--refresh` decides again from your unchanged config, and the
one that runs at boot does it for you. `--once hybrid` works the same way in
the other direction.

## Checking state

```bash
xgpuprofile                      # mode, what was detected, in sync or not
xgpuprofile --refresh --dry-run  # the decision, no root, changes nothing
```

`--status` reports the running layout and the wanted layout separately, so a
mismatch is visible rather than silent — that's the case where the hardware
changed after you logged in.

## Detecting external displays

Two sources, in order of preference:

1. **A live X session via `xrandr`**, when `$DISPLAY` is reachable — what
   interactive commands use.
2. **Kernel sysfs connector status**, when no X session can be reached —
   the boot-time case, where the systemd service runs `--refresh`.

This split exists because of a real driver quirk: NVIDIA's DRM/KMS connector
status isn't reliably kept in sync once its own driver stack is managing the
display, so reading it from a running session can report a connected monitor
as absent. Before Xorg starts, nothing has taken that over and the kernel
probe is accurate — which is why boot-time decisions were always correct even
when interactive ones weren't.

## What else it detects

- **The discrete GPU** — the PCI display device the firmware didn't mark
  `boot_vga`. BusID is emitted in decimal (sysfs gives hex) and the driver
  comes from the bound kernel driver, so nothing is hardcoded.
- **The integrated GPU** — the one that *is* marked `boot_vga`. It stays in
  the generated layout as an `Inactive` device: the built-in panel is wired
  to it, so leaving it out turns the internal display off entirely.
- **External displays** — outputs that aren't internal panel types (`eDP`,
  `LVDS`, `DSI` excluded). That exclusion matters: the built-in panel is
  often reachable through the dGPU too, so counting it would leave the
  external-display condition permanently true.
- **The display manager** — the `display-manager.service` alias, then known
  units that are enabled. Only used by `--restart-x`.

All of these can be overridden in the config if detection guesses wrong.

## Limitation

A decision is only made when Xorg starts. Plugging a display in mid-session
changes nothing until you re-stage with `--refresh` and apply, or reboot.
`xgpuprofile --status` will tell you when you're in that state.
