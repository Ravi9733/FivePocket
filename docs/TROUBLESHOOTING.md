# Troubleshooting

## Known issues

- **Fold-class phones slow down when hot.** On a Galaxy Z Fold4 (Snapdragon 8+ Gen 1) expect around 25 fps once the
  phone is warm: it lowers its clock to cool down. Playing without a case and not charging at the same time helps.
- **The Duo hitches while driving.** On foot it holds a steady 30 fps; driving shows occasional hitches.
- **The first launch stutters briefly** while the shader caches are built; it settles once they are.
- **iOS with a free Apple ID:** re-sign the app within 7 days or it will not open, and always re-sign with the **same
  bundle identifier** (a different one is a new app with empty storage). One free Apple ID can sideload at most 3 apps
  on a device and create 10 new app IDs per 7 days. Developer Mode must be on (Settings → Privacy & Security →
  Developer Mode), and the first launch needs Settings → General → VPN & Device Management → your Apple ID → Trust.
- **Android editions build on Windows only in this release;** a Mac builder for them is planned.
- **The iOS builder is tested with Xcode 26.5.** It checks every compiled unit against its record, so another Xcode
  version may stop with "REFUSED".
- **Closed-alpha Android users** must uninstall the old app first (it was signed with a different key), and
  **uninstalling deletes its saves**. Copy your saves out before you uninstall.

## Builder

| Message | Meaning |
|---|---|
| `REFUSED: ...` | the builder stopped before changing anything: an input is missing, a file of the download does not match its inventory, or there is not enough space. Correct the input and run `builder check` again. It never retries on its own. |
| refused disc | the image is not one of the two discs the profile knows (wrong disc, a different release of the game, or a damaged rip) |
| out of space | the build needs about 20 GB free in the work folder |

The builder writes a report (`--report FILE`) with every stage, its time and its result. It contains file names,
sizes and hashes, never game data, so it is safe to attach to a bug report.

## App

- **First-run import refuses the bundle**: the bundle was made by a builder of a different version. Use the builder
  from the same release as the app.
- **Black screen after loading**: note the edition, the device and the last screen you saw, and report it with the log.
- **Low frame rate after some minutes**: the phone is hot and has lowered its clock (see Known issues).

## Logs

| Where | How to get the log |
|---|---|
| Builder (Mac, Windows) | `builder build ... --report report.json` writes every stage, its time and its result: file names, sizes and hashes, never game data. |
| Android, Thor | with USB debugging on, run `adb logcat -d > fivepocket-log.txt` on your computer right after the problem. |
| iOS, Duo | after a crash: Settings → Privacy & Security → Analytics & Improvements → Analytics Data, the entry with the app's name and the time of the crash. Otherwise describe what happened and add a screenshot. |

Never attach memory dumps, caches, saves, bundles, the `.ipa` you built or anything from the app's game folder. Ask
on the [FivePocket Discord](https://discord.gg/mZvqCsxgxT) if you are unsure what is safe to share.

## Device requirements

See [Supported devices](../README.md#supported-devices) in the README.
