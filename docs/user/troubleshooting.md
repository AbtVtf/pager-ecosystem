# Troubleshooting

If something isn't working, find the symptom below.

## Won't power on

1. **Battery dead.** Plug in USB-C and wait 30 seconds. A red charging LED should appear. The device may need ~5 minutes on charge before it'll turn on if it was fully drained.
2. **No SD card.** PagerOS refuses to boot past the recovery screen without an SD card. Insert one and try again. Any FAT32-formatted microSD ≥ 1 GB works.
3. **Held the wrong button.** Power is a *short hold* (≈ 2 s). Holding it longer than 10 s with no boot indicator means hardware not responding — try a forced reset by holding power + BACK for 15 s.

If after all this you still see nothing on screen, the device is bricked at a level a user can't recover from. File an issue on the project repo with your fingerprint (you may have written it down from first boot) and the device's hardware revision (printed on the back).

## Stuck on the self-test screen

The self-test prints, in order: `flash · psram · display · lora · wifi`. If any line says `fail`:

| Failure | Likely cause | What to try |
|---|---|---|
| `flash fail` | Corrupted firmware. | Recovery flash via `pagerctl flash <fw.bin>` over USB. Needs a computer. |
| `psram fail` | Hardware. | Nothing user-fixable. File an issue. |
| `display fail` | Loose ribbon cable. | Power off and gently re-seat the display ribbon if you're comfortable opening the device. Otherwise file an issue. |
| `lora fail` | SX1262 not responding. The device still works without LoRa, but you'll be Wi-Fi-only. | Try a reboot. If persistent, it's hardware. |
| `wifi fail` | Antenna disconnected. | Try a reboot. If persistent, it's hardware. |

## Can't connect to Wi-Fi

- **Captive portals don't work.** The device has no browser, so any network that requires accepting terms in a page (hotels, cafes, airport Wi-Fi) cannot be used directly. Use a different network, tether through a phone, or use LoRa.
- **Password wrong.** PagerOS doesn't tell you *why* a connection failed — it just retries. Try forgetting the network in **Settings → Network → Wi-Fi → Saved networks** and re-adding it.
- **5 GHz only.** The device is 2.4 GHz b/g/n only. If your router is configured for 5 GHz exclusively, it's invisible to the pager.
- **Hidden SSID.** Supported, but you must type the SSID exactly: **Settings → Network → Wi-Fi → Add hidden network**.

## "App requires internet"

The app you opened has `lora_compatible: false` in its manifest, and Wi-Fi isn't available. Either connect to Wi-Fi or pick a different app. There is no override; LoRa frames must fit a tiny budget that some apps can't be redesigned for.

## App opens but every screen says "Network error"

- Check the status bar: do you have `Wi-Fi`, `LoRa`, or nothing?
- If nothing: see the Wi-Fi/LoRa sections above.
- If you see `Wi-Fi` but every request fails, the app's server may be down. The Marketplace's app detail page sometimes shows recent uptime; otherwise wait and try later.
- If you see `LoRa` but requests time out: the nearest Exit Node may be saturated or offline. **Settings → Network → LoRa → Mesh status** will say which Exit Node you're routed through. If it's stuck, you can either wait for a fresh advert (~60 s) or pin a different Exit Node manually.

## Notifications never arrive

1. Confirm the app has the **notifications** permission: **Settings → Permissions**.
2. Check **global mute** isn't enabled (**Settings → Notifications**).
3. Confirm your pull schedule isn't `manual` — manual mode only fetches when you press **Notifications → Pull now**.
4. Notifications older than 7 days are dropped by the Push Relay. If the app sent one and you never woke up the device in time, you'll never see it.
5. Some apps' pushes are rate-limited (60/hour per device). If the app is misbehaving, you might be hitting that cap; the relay returns 429 to the app and drops the notification.

## Notifications arrive late

The pull schedule defaults to every 60 s during light sleep. To get faster delivery, keep the device awake or be on Wi-Fi (the device holds open connections more aggressively then). Switching to `aggressive` already is the fastest user-facing setting; lower than 60 s would shred the battery and isn't offered.

## "App opens slowly"

- A cached home frame loads instantly. If you uninstalled and reinstalled, the cache is empty — first open will be slow.
- Map widgets are heavy on LoRa because tiles have to come back over the mesh. The device caches tiles for areas you've viewed; subsequent visits to the same area are fast.
- Images load progressively. Big images (close to the 8 KB max) may take a few seconds.
- A persistent slowness across all apps usually means the SD card is failing. Try **Settings → Storage → Cache usage → Clear all caches**, then reboot. If that helps for a day or two and then returns, replace the SD card.

## Encoder feels wrong

- **Skips items**: try cleaning around the encoder shaft.
- **Reverse direction**: there's no software flip in v1. Sorry.
- **Dead**: the QWERTY keys' arrow combo (`Fn + ←/→`) and ENTER work as a fallback.

## Keyboard types the wrong character

- The first row above QWERTY is the **Fn layer** — make sure you don't have caps/num locked. **Settings → Display → Show keyboard state** turns on a small indicator.
- Some apps interpret keys directly (e.g. games subscribed to raw input). Those override the normal QWERTY behavior while focused.

## I lost my device

There is no remote wipe. Whoever finds it can use whatever was installed on it as you — that's the cost of accountless apps.

What you *can* do:
- For each app you care about, open its server side (web interface, etc.) and rotate the session if the app supports it.
- If you have a backup pager paired via **Settings → Identity → Pair another device**, you can recover your identity on it from the SD card; without that, the identity is gone with the device.

## I want to start over

**Settings → Identity → Reset** wipes the keypair, caches, and apps. The device boots into first-boot setup with a brand new identity. Make sure you've copied your fingerprint somewhere if you have anything tied to it.

## My problem isn't here

Open an issue at the project repository's issue tracker. Include:
- Your fingerprint.
- Firmware version (from **Settings → Updates**).
- Steps to reproduce.
- Recent log lines from **Settings → Storage → Logs** if relevant.

Don't include your full public key unless asked — fingerprint is enough.
