# First Boot

This page walks through powering on a new PagerOS pager for the first time.

## Before you start

Have ready:
- The pager, charged at least to 30% (a charge icon appears top-right).
- A Wi-Fi network name and password, if you want to use Wi-Fi. (Wi-Fi is optional — the device also works over LoRa if there's a mesh nearby.)
- A microSD card inserted in the device's slot. The first boot writes settings, the encrypted device key, and the frame cache to it.

If the SD card slot is empty the device will boot into a recovery screen asking you to insert one.

## Power on

Hold the power button for ~2 seconds. You'll see, in order:

1. **PagerOS logo + version** — the bootloader has handed off to the OS.
2. **Self-test** — a one-line status: `flash ok · psram ok · display ok · lora ok · wifi ok`. Any `fail` here means a hardware problem; see [Troubleshooting → Self-test failures](troubleshooting.md#self-test-failures).
3. **Identity setup** (only on the very first boot — see below).
4. **Welcome screen** — a short walkthrough you can step through with ENTER, or skip with BACK.
5. **Home Shell** — the launcher.

End-to-end this takes about 4 seconds on a clean device. Subsequent boots skip identity setup and the welcome screen.

## Identity setup (first boot only)

The first time the device boots, it generates an **Ed25519 keypair** and stores the private key in encrypted on-chip storage. You don't choose this; it's automatic.

After generation, the device shows your **fingerprint**: a 12-character base32 string like `AB7Q-4ZK2-MX9P`. Write this down somewhere — it's the public name of your device. Apps will see this fingerprint (or the underlying public key) when you interact with them.

Press ENTER to confirm and continue. See [Identity](identity.md) for what this means in practice.

## Welcome walkthrough

Five short screens introduce the basics:
1. **Controls.** ENTER, BACK, the encoder, the QWERTY keys.
2. **Home Shell.** Where your apps live.
3. **Marketplace.** How to find more apps.
4. **Notifications.** What the top-of-screen flash means.
5. **Where to learn more.** A pointer to `docs.pageros.org`.

You can revisit the walkthrough later from Settings → Help → "Show welcome again".

## Connecting to Wi-Fi (optional)

From the Shell, open **Settings → Network → Wi-Fi**.

1. The device scans and lists nearby networks, sorted by signal strength.
2. Select your network with the encoder and press ENTER.
3. Type the password using the QWERTY keys. Press ENTER to submit.
4. On success you'll see `Connected · 192.168.x.x`. The Wi-Fi indicator appears in the top bar.

If your network has a captive portal (hotels, cafes), the device cannot complete it — captive portals require a browser, which PagerOS does not have. Use a different network, tether to a phone that has completed the portal, or use the LoRa transport if an Exit Node is nearby.

The device remembers up to 4 networks and rejoins the strongest known one automatically.

## Setting the time

PagerOS uses NTP when Wi-Fi is available. The first successful sync happens silently in the background within ~30 s of joining a network.

If you're LoRa-only, the device asks an Exit Node for the current time during the first request, then drifts using the on-board RTC. You can also set the time manually in **Settings → Time → Manual**.

## What happens next

Once first boot is done, the device:
- Pulls the Marketplace's app directory (so you have something to install).
- Pulls any queued notifications from the Push Relay.
- Lights up `Online` in the status bar if Wi-Fi or LoRa is usable.

You're ready to [install apps](apps.md).

## Factory reset

If you ever need to start over, hold BACK + ENTER together for 10 seconds during boot — the device will offer to wipe the SD card and regenerate identity. This destroys your current keypair: any app that knew you by that identity will treat you as a new device.
