FivePocket Builder {version} for Windows (x86-64)
==============================================

The builder turns YOUR OWN Xbox 360 discs of the game into a package for the FivePocket app (Android phones with
one screen). It contains no game code or data and sends nothing anywhere except to download Google's Android NDK
and Microsoft's DirectX Shader Compiler when you ask it to. The AYN Thor has its own builder (FivePocket Thor
Edition Builder); the iOS and Duo apps are made by the Mac builder.

NEEDS
-----
- 64-bit Windows 10 or 11, 16 GB of memory. 20 GB free space, plus about 3 GB for the NDK.
- Disc 1 ("Install") and Disc 2 ("Play") of the game as ISO images, from discs you own.
- About 2 to 4 hours.

STEPS
-----
1. Unpack this folder. Windows SmartScreen may warn about an unknown publisher: choose "More info", "Run anyway".
2. In a Command Prompt, inside the download's builder folder (cd builder):
     builder fetch --accept-ndk-license --accept-dxc-license
     builder check
     builder build --play-disc D:\Disc2.iso --install-disc D:\Disc1.iso --out %USERPROFILE%\FivePocket-out
     builder verify %USERPROFILE%\FivePocket-out\<name>.bundle
3. Copy the .bundle to your phone as the app's first-run screen says.

If the builder stops with "REFUSED", nothing was changed: fix the input it names and run check again.

The complete source of the builder's programs is in builder\sources\.

NEVER SHARE WHAT YOU BUILD
--------------------------
Never share, upload or sell: disc images, files copied from your discs, the .bundle the builder makes, the
builder's work folder, the app's game files or caches, or your saves. They come from the game.

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
