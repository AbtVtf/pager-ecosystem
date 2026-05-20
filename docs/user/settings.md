# Settings

Open **Settings** from the home Shell. The Settings app is structured as sections; this page walks each one.

## Network

### Wi-Fi
- **Status** — current state (`disconnected`, `connecting`, `connected: <ssid>`) and IP address when connected.
- **Scan / connect** — pick a network and enter a password.
- **Saved networks** — up to 4 networks the device will rejoin automatically. Remove any from here.
- **Auto-rejoin** — toggle. Default on.

PagerOS does not support captive-portal networks (no browser available).

### LoRa
- **Mesh status** — `idle`, `searching`, or `connected via <exit-node-fingerprint>`.
- **Preferred Exit Node** — pin a specific Exit Node by fingerprint (advanced).
- **TX power** — `low` / `medium` / `high`. Default `medium`. Higher uses more battery.
- **Region** — required for legal RF compliance. Set on first boot; change if you travel.

### Selection preference
The transport selector chooses Wi-Fi → LoRa → Push automatically. Override defaults here if you need to force a particular path for testing.

## Identity

- **Fingerprint** — your 12-char fingerprint (read-only).
- **Show full key** — full base64 public key plus a QR code.
- **Pair another device** — generate a one-time secret so a second device can later decrypt a moved SD card. The secret is shown once.
- **Reset identity** — wipes the keypair and forces a first-boot flow. Irreversible. See [Identity → Resetting identity](identity.md#resetting-identity).

## Apps

- **Installed apps** — list of every app currently on your home Shell. Open an entry for:
  - The app's manifest details.
  - **Permissions** — see and toggle granted permissions for that app.
  - **Clear cache** — drop the app's cached Frames (the next open will be slower but fresh).
  - **Remove** — uninstall.
- **Add by URL** — sideload. See [Installing apps → Sideloading by URL](apps.md#sideloading-by-url).
- **App permissions overview** — see the next section.

## Permissions

A grid view: rows are installed apps, columns are permission types. Each cell shows `granted`, `denied`, or `not requested`.

- Tap a cell to toggle. A denied permission can be re-granted later, but the app won't see the change until you re-open it.
- Revoking **notifications** removes that app from the Push Relay subscription list — the relay will drop further pushes silently.
- Revoking **groups** kicks your device out of any current multi-device sessions for that app.

## Notifications

- **Global mute** — silences all notification tones device-wide. Notifications still arrive in the inbox; they just don't beep or flash.
- **Per-app mute / tone** — pick a tone (`default`, `low_priority`, `alert`, `success`, `error`) or mute a specific app.
- **Inbox** — view recent notifications. Each entry has a timestamp, app, and the action it would route to. Dismissed notifications are removed; ignored ones expire after 7 days.
- **Quiet hours** — set a daily start/end where the device won't tone (notifications still flash on the screen if you wake the device).
- **Pull schedule** — `aggressive` (60 s, default), `relaxed` (5 min), or `manual`. Aggressive uses more battery.

## Display

- **Brightness** — 1–10. The device auto-dims after 30 s of inactivity.
- **Dim timeout** — 15 s / 30 s / 60 s / never.
- **Off timeout** — 30 s / 60 s / 2 min / never.
- **Theme** — `light`, `dark`, `auto` (`auto` follows the time of day if you've enabled NTP).
- **Font scale** — `small`, `normal`, `large`. Large reduces what fits on a Frame; some apps may scroll more.

## Time

- **Source** — `NTP (Wi-Fi)`, `Exit Node`, `Manual`.
- **Timezone** — picker; defaults from your region setting.
- **24-hour clock** — toggle.

If you change to **Manual**, the on-board RTC keeps time within ~2 minutes/month.

## Power

- **Battery level** and estimated runtime.
- **Light-sleep timeout** — minutes of idle before light sleep. Default 5.
- **Deep sleep** — manually enter deep sleep until the power button is pressed.
- **Power off** — full shutdown.

## Storage

- **SD card** — capacity, free space, format option (destructive: wipes the card).
- **Cache usage** — frames, images, OSM tiles. Buttons to clear each.
- **Logs** — view or clear `/logs/pageros.log` (1 MB rolling log).

## Updates

- **Current version** — firmware version and build hash.
- **Check for updates** — query `updates.pageros.org`.
- **Auto-update over Wi-Fi** — toggle. Default on. LoRa-only devices never auto-update.
- **Rollback to previous** — if you just installed an update that misbehaves, this re-boots into the inactive partition.

## Marketplace

- **Show unverified apps** — toggle. Default off.
- **Source URL** — pinned to `market.pageros.org` by default; can be changed to a mirror.
- **Refresh directory** — pull the latest app list now.

## Help

- **Show welcome again** — replays the first-boot walkthrough.
- **About** — credits, license, links.
- **Report a problem** — opens a screen that bundles your fingerprint, firmware version, and last 200 log lines as a Frame you can read or forward (Wi-Fi only — sends to `https://reports.pageros.org` after you confirm).

## Developer

Hidden unless you tap **About → Build hash** seven times.

- **Live link** — pair the device with a `pagerctl dev` session over USB for app development.
- **Verbose logging** — increases log detail. Burns through the log buffer quickly.
- **Network capture** — log raw CBOR exchanges to `/logs/wire.cbor` for inspection.
- **Reset shell layout** — restores the default home Shell ordering.

If you're not a developer, leave this section closed.
