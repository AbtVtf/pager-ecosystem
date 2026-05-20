# Identity

Your pager has one persistent identity: a public/private keypair that was generated on first boot. Everything an app knows about "you" is keyed off this identity.

## The short version

- The device generates an **Ed25519** keypair the first time it boots.
- The private half lives in encrypted on-chip storage and never leaves the device.
- The public half is your identity — apps see it on every request.
- A 12-character **fingerprint** (e.g. `AB7Q-4ZK2-MX9P`) is the human-readable form. Find it under **Settings → Identity**.

## Why this matters

PagerOS does not use accounts, emails, or passwords. The device is the account. When you open an app, the request is signed with your private key; the app verifies it against your public key. To the app, you are whoever holds that private key.

A few practical consequences:
- **No account recovery.** If you lose the device (or its SD card — see below) you cannot prove you are the same user to an app that knew you before. There is no project-run "reset password" flow.
- **No remote sign-out.** Each app maintains its own session for your identity. To stop using an app, uninstall it on the device or revoke its session inside the app itself if it provides that.
- **Apps can't talk to each other about you.** Two unrelated apps both see your device pubkey, but they don't share data — that would require both apps to coordinate on a server you don't operate, and PagerOS doesn't provide a mechanism for it.

## Where the keys live

| Piece | Location | Backed up? |
|---|---|---|
| Private key | ESP32-S3 secure storage (NVS with flash encryption) | No |
| `identity.key` (encrypted private key, redundant) | `/system/identity.key` on SD card | Movable with the SD card |
| Public key + fingerprint | Derivable from private key; also written to `/system/identity.pub` | n/a |

The private key never leaves the device through any documented path. There is no export command.

## Moving an identity to another device

This is the **only** supported way to keep your identity if your device is broken or replaced:

1. Power off both devices.
2. Move the microSD card from the old device into the new one.
3. Power on the new device. It detects the existing `/system/identity.key`, decrypts it against the on-chip key, and re-uses your identity.

This works only when both devices' on-chip keys can decrypt the same `identity.key`. In v1, that means the same physical device's main board paired with a different SD card, or the same SD card moved between devices that have been pre-paired with a matching unlock secret (see **Settings → Identity → Pair another device**). For most users, in practice: keep the SD card, replace the device only if its mainboard is dead.

## What apps see

When an app receives a request, it gets:
- Your **device public key** (base64) in the `PagerOS-Device` header.
- A signature over the request in `PagerOS-Sig`.
- Optionally, anything the app itself has stored against your pubkey (its session store).

Apps do **not** see:
- Your fingerprint label, location, contacts, or any other personal field. Those are only available if the app holds a granted permission for them (see [Settings → Permissions](settings.md#permissions)).
- Other apps' data.

## The fingerprint, in detail

The 12-character fingerprint is a base32 encoding of a truncated SHA-256 of your public key, grouped as `XXXX-XXXX-XXXX` for readability. It's safe to share — knowing your fingerprint does not let anyone impersonate you. It's useful when:
- An app developer needs to allowlist you (e.g. for a private beta).
- You're reporting a bug and want a maintainer to find your device's log.
- Two pager users want to add each other to a chat app's group by reading fingerprints aloud.

The full public key is also visible (and copyable as a QR code on devices with NFC) under **Settings → Identity → Show full key**.

## Resetting identity

If you want a fresh identity — for example because your previous one was leaked or you're handing the device to someone else — go to **Settings → Identity → Reset**.

This:
1. Wipes the on-chip secure storage entry.
2. Deletes `/system/identity.key` and `/system/identity.pub`.
3. Clears `/cache/` and `/apps/` (otherwise old apps would still think your old identity was you).
4. Reboots into a first-boot flow, generating a new keypair.

There is no undo. Make sure you have nothing tied to the old identity that you care about.
