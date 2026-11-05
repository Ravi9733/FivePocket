FivePocket {version} for iPad and iPhone
=======================================

There is no ready-made iOS app to download: the FivePocket iOS Builder for macOS makes the app on your own Mac,
from the two Xbox 360 discs of the game that you own. The app it makes contains no disc files; those are imported on the device.
The Duo edition has its own README-duo.txt.

NEEDS
-----
- A Mac with Apple silicon and Xcode, for the builder (FivePocket-iOS-Builder-{version}-macos-arm64.zip; its
  README.txt names the tested Xcode).
- iOS / iPadOS 17.4 or later. Tested: iPad mini (A17 Pro, 8 GB), steady 30 fps at full resolution, the app uses
  1.9-2.3 GB. Other devices are untested; the game needs about 2-2.5 GB of memory, so 8 GB devices are the safe
  choice (on 6 GB devices iOS may close the app; an estimate).
- An Apple ID to sign the app. A free Apple ID allows at most 3 sideloaded apps on a device and 10 new app IDs per
  7 days, and the app must be re-signed within 7 days or it will not open.
- About 18 GB free on the device for the imported files and the app; about 36 GB at once if you copy the disc images
  onto the device first. Images on a USB drive picked with Choose A Disc Image need no device space. Offloading apps
  you don't need (Settings > General > iPad/iPhone Storage > an app > Offload App) frees space and keeps their data.

STEPS
-----
1. Run the Mac builder with --ios (its README.txt has the command) with your Disc 1 and Disc 2 images. It
   writes the unsigned .ipa.
2. Sign and install it with your own Apple ID (Xcode, Sideloadly or AltStore). ALWAYS RE-SIGN WITH THE SAME BUNDLE
   IDENTIFIER: a different one is a new app with empty storage, and the import must be done again. Never delete the
   app before reinstalling: deleting it removes the imported files and your saves.
3. Turn on Developer Mode: Settings > Privacy & Security > Developer Mode > On, then restart. At the first launch
   iOS says "Untrusted Developer": Settings > General > VPN & Device Management > your Apple ID > Trust.
4. Put your two disc images (any file names: the app recognises each disc by its contents) into the app's folder:
   the Files app (On My iPad/iPhone > FivePocket iOS) or Finder with the device connected. Or keep them on a USB
   drive and pick them on the import page with Choose A Disc Image.
5. Open the app. The Import Game Discs page checks every file against known-good hashes and copies the game files
   into the app's own storage: about 2 minutes with the images in the app's folder (iPad mini, A17 Pro), longer from
   a USB drive. If you leave the app during the import, it stops cleanly and keeps every file already copied; it
   does not continue by itself. Start it again (Import From The Files Folder, or Choose A Disc Image) and it copies
   only what is missing.
6. When the import says both discs are done, DELETE THE TWO DISC IMAGES from the app's folder in Files. The game no
   longer needs them; keep your own copies on your computer.

A game controller works; without one, touch controls appear on screen.

Re-signing (free Apple ID: every 7 days) or installing an update with the same bundle identifier replaces only the
app: your imported game files and your saves stay on the device.

NEVER SHARE WHAT YOU BUILD
--------------------------
Never share, upload or sell: disc images, files copied from your discs, the .ipa the builder makes, the builder's
work folder, the app's game files or caches, or your saves. They come from the game.

NOT AFFILIATED
--------------
FivePocket is a free, independent fan project. It is not affiliated with, endorsed by or sponsored by the game's
publisher or developer, Microsoft, Apple, Google, AYN, Samsung, Qualcomm or AMD. All trademarks belong to their
owners. See LEGAL.md.

LICENCE AND SOURCE
------------------
FivePocket is free software under the GNU General Public License, version 3 or later (COPYING). Third-party parts
and their licences: THIRD-PARTY-NOTICES.md and LICENSES/. The complete source code is published with every release
as FivePocket-{version}-source.tar.xz, next to this download on
https://github.com/marcm0de/FivePocket/releases. Check downloads against SHA256SUMS.
