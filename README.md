# xgpuprofile

Automatically choose which GPU drives the X display by starting X either
on the integrated one (as normally) or the discrete one.

## The problem

On most laptops with hybrid graphics the external video output is wired to
the discrete GPU. On the current state of NVIDIA drivers, when the
integrated GPU owns X, on some setups *reverse PRIME* doesn't work properly,
leading to percived lag on high resolution - high refresh rate external
monitors.

Handing the screen to the dGPU resolves this problem, however the GPU can't
reach lower power states, thus draining laptop batteries fast.

This utility fixes it by automatically starting X on the appropriate GPU
based on the current state of the laptop (AC power, external monitors).

## Why not optimus-manager

This utility is practically a dumbed down `optimus-manager`.

`xgpuprofile` doesn't provide an integrated mode, nor all the different
switching modes. It is also lacks other features `optimus-manager` provides.
`xgpuprofile` doesn't aim to be an optimus-manager successor. It aims to be
a simple daemon-less utility forcing X on the gpu without changing the
graphics setup itself at all.


## Install

To install, simply run:

```bash
make
sudo make install
sudo systemctl daemon-reload
sudo systemctl enable xgpuprofile.service
```

## Modes

| mode | what it does |
|---|---|
| `hybrid` | keep the integrated GPU — the ordinary out of the box, low-power setup |
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

## Limitation

A decision is only made when Xorg starts. Plugging a display in mid-session
changes nothing until you re-stage with `--refresh` and apply, or reboot.
`xgpuprofile --status` will tell you when you're in that state.
