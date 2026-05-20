# PagerOS User Manual

Welcome. This manual explains how to use a PagerOS pager day-to-day. It assumes you have a LILYGO T-LoRa Pager running PagerOS firmware.

PagerOS is an operating system for a small device with a 480×222 screen, a QWERTY keyboard, a rotary encoder, Wi-Fi, and LoRa. Apps live on the internet; your pager talks to them through Wi-Fi when available, or over the LoRa mesh when it isn't.

## What's in this manual

- [First boot](first-boot.md) — turning the device on, language, Wi-Fi, time.
- [Identity](identity.md) — what your device's identity is and where it's stored.
- [Installing apps](apps.md) — the Marketplace, sideloading by URL, removing apps.
- [Settings](settings.md) — every setting you can change and what it does.
- [Troubleshooting](troubleshooting.md) — when things go wrong.

## At a glance — controls

| Control | What it does |
|---|---|
| **QWERTY keys** | Type into inputs, trigger action shortcuts (the underlined letter in an action label). |
| **Rotary encoder** | Move selection up/down in lists; rotate to scroll long screens. |
| **ENTER** | Activate the selected list item, button, or form submit. |
| **BACK** (short press) | Go back one screen. From an app's root, returns to the home Shell. |
| **BACK** (long press) | Force-quit the current app and return to home. |
| **Power button** | Short press: wake/sleep. Long press (≥ 3 s): power off / deep sleep. |

## What "Shell" means

The **Shell** is the built-in screen you see when no app is open: a launcher with installed apps, a notification tray, and a settings entry. The Shell is itself rendered with the same UI protocol your apps use — there is no special privileged surface.

## A typical session

1. Wake the device (press any key or rotate the encoder).
2. The home Shell shows your installed apps. The newest notifications, if any, flash at the top.
3. Select an app with the encoder and press ENTER.
4. Use the app. Press BACK to return to the Shell.
5. The screen dims after 30 s of inactivity, turns off after 60 s, and the device light-sleeps after 5 minutes. Notifications can still wake it.

## Where things are stored

Everything user-visible lives on the microSD card: app icons, the frame cache, notification inbox, settings, and your encrypted device key. If you replace the SD card, your identity changes (see [Identity](identity.md)).

## When you need help

- See [Troubleshooting](troubleshooting.md) for common problems.
- If your problem isn't listed, the project repository's issue tracker is the best place to ask.
- Your device key fingerprint (Settings → Identity) is useful when reporting problems — it lets a maintainer correlate logs without revealing your private key.
