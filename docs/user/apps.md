# Installing Apps

Apps on PagerOS are not stored on your device — they run on the developer's server and your pager just renders them. "Installing" means adding a manifest to your home Shell so the app appears in the launcher.

## Two ways to install

1. **From the Marketplace** — the project-operated public directory. Most users get apps this way.
2. **Sideload by URL** — add any compatible app directly by entering its URL. Useful for private apps, in-development apps, or apps that haven't (or won't) be listed in the Marketplace.

## From the Marketplace

The Marketplace is itself a PagerOS app — when you open it you're just rendering a list of available apps from `market.pageros.org`.

1. From the home Shell, open **Marketplace** (it ships pre-installed).
2. Browse by category, or press the action shortcut **Search** and type a name.
3. Select an app to see its detail screen:
   - Name, icon, one-line description.
   - Maintainer and contact link.
   - Required permissions (see below).
   - `LoRa-compatible` flag — whether the app works without Wi-Fi.
   - `Multi-device` flag — whether the app supports shared sessions with other pagers.
   - A "Tip developer" action if the maintainer added a donation link.
   - Trust tag: `unverified`, `verified`, `featured`, or `flagged`. You can choose to hide unverified apps under **Settings → Marketplace → Show unverified apps**.
4. Press **Install** to add the app to your home Shell. The pager fetches the manifest and the icon and writes them to `/apps/<app_id>/`.

Installing is free and reversible. It does not create an account with the app; the first time you actually open it, your pager will send a signed request and the app may show a permission prompt if it needs anything.

## Sideloading by URL

You can install any app whose developer has published a PagerOS manifest at a well-known path.

1. **Settings → Apps → Add by URL**.
2. Type the app's root URL (e.g. `https://notes.example.com/`). Press ENTER.
3. The pager fetches `<url>/.pageros/manifest.cbor`. If it parses, you'll see the manifest preview screen.
4. Press **Install**. The app is added to your home Shell, tagged `sideloaded`.

Sideloaded apps work exactly like Marketplace apps. They just aren't in the public directory and are not subject to Marketplace moderation.

## Permissions

An app may request permissions when you first install it (or the first time it needs them):

| Permission | What it lets the app do |
|---|---|
| **Location** | Receive your GPS position as `location` events. |
| **NFC** | Receive NFC tag scans you initiate while the app is open. |
| **Notifications** | Push notifications to your device via the Push Relay. |
| **Groups** | Add your device to multi-device sessions (chat, multiplayer). |
| **LoRa send** | Send raw mesh messages on your behalf — rare, comes with a warning. |
| **Contacts** | Read your local contacts (where supported). |

You see a single prompt per app, per permission. Granted permissions persist until you revoke them in **Settings → Permissions** (see [Settings](settings.md#permissions)).

## Opening an app

Select it on the home Shell and press ENTER. The pager:
1. Looks for a cached "home" Frame for the app and renders it instantly (if recent enough).
2. Fires a background request for a fresh Frame.
3. Re-renders when the fresh Frame arrives.

This is why apps usually feel snappy even when the network is slow.

## Working over LoRa (no Wi-Fi)

If your app has the `lora_compatible` flag set, the pager will fall back to the LoRa mesh when Wi-Fi isn't available. The status bar will show `LoRa` instead of `Wi-Fi`. Requests will take several seconds; large screens take longer because they're split across multiple LoRa packets. This is expected.

Apps without `lora_compatible` will show an error: **"App requires internet."** Connect to Wi-Fi or wait until it's available.

## Notifications

If you grant an app the **notifications** permission, it can send you push notifications even when the app isn't open. PagerOS pulls pending notifications:
- On wake (any key press or encoder turn).
- Every 60 seconds during light sleep, if you have at least one push-subscribed app installed.

A notification appears as a top-of-screen flash plus a short tone. Press ENTER to open the originating app at its notification handler. Notifications you ignore stay in the inbox for 7 days.

You can mute notifications per app or globally — see [Settings → Notifications](settings.md#notifications).

## Removing an app

1. From the home Shell, hold ENTER on the app's icon for ~1 second.
2. Select **Remove**.
3. Confirm.

This deletes the manifest from `/apps/<app_id>/`, the app's cached Frames, and revokes its permissions. Anything the app stored on its own server keyed by your device pubkey is untouched — to truly forget you, you have to ask the app's server, if it provides that option.

## Recent apps

The last 5 apps you opened are remembered with their last screen. They appear in a "Recent" row at the top of the home Shell so you can resume them quickly. This row is only ever local; it isn't reported anywhere.

## A note on app safety

Because apps run on their own servers, you should think about installing an app roughly the way you'd think about visiting a website: a malicious app can send you junk through its own UI, but it cannot run arbitrary code on your device (there is no on-device code execution in v1). It cannot read other apps' data. It can only see what its permissions grant it.

That said: be skeptical of unverified apps that request `location`, `nfc`, or `lora_send`. If something feels off, revoke its permissions or remove it.
