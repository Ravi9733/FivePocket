FivePocket iOS Builder {version} for macOS (Apple silicon)
=========================================================

This builder makes the FivePocket iOS and FivePocket Duo apps on your Mac from YOUR OWN Xbox 360 discs of the game.
It contains no game code or data and sends nothing anywhere except to download Google's Android NDK (its shader tools)
when you ask it to. The apps for Android phones and the AYN Thor are made by the Windows builders.

NEEDS
-----
- A Mac with Apple silicon and Xcode. Tested with Xcode 26.5. The build checks every compiled unit against the
  kit's record, so another Xcode version may stop with "REFUSED".
- 16 GiB of free space, plus about 4 GB for the NDK (an 836 MB download).
- Disc 1 ("Install") and Disc 2 ("Play") of the game as ISO images, from discs you own.
- About an hour for the first app; the second app reuses most of the first one's work.
- Your own Apple ID to sign the app.

STEPS
-----
1. Unpack this folder. If macOS blocks a builder program, allow it in System Settings > Privacy & Security (Open
   Anyway), then run the command again.
2. In Terminal, inside the builder folder, in this order (./builder with no word prints these steps):
     ./builder fetch --accept-ndk-license
     ./builder build --duo or --ios --play-disc PLAY-DISC.iso --install-disc INSTALL-DISC.iso
     ./builder sign --ipa UNSIGNED.ipa --identity "Apple Development: you@example.com" --profile-file YOUR.mobileprovision --out SIGNED.ipa
   ./builder check checks the package and your Mac without building.
3. The build writes an unsigned .ipa with no disc files inside to local-analysis/builder/work/out in the builder
   folder. Your app's bundle identifier is made once and printed at the end: ALWAYS SIGN WITH THAT IDENTIFIER
   (another one makes a new app with empty storage); --bundle-id ID chooses your own for one build.
4. Sign it with the sign step or with your usual tools, install it, then import the discs on the device:
   README-ios.txt (iPad and iPhone) and README-duo.txt (the Duo), next to this file.

A build that was interrupted goes on where it stopped when the build step is run again. If the builder stops with
"REFUSED", nothing was changed: fix the input it names and run check again.

The complete source of the builder's programs is in builder/sources/.

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
