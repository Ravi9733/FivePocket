FivePocket Duo {version}
=========================

FivePocket Duo is the iOS edition laid out for the Duo's inner display: the game on the top half, the map, wheels
and phone on the lower half. There is no ready-made app to download: the FivePocket iOS Builder makes it on your
own Mac from the two Xbox 360 discs of the game that you own. The app contains no disc files; they are imported on the
device.

NEEDS
-----
- The Duo (iPhone19,4). Tested on iOS 27.2. The builder labels the Duo app for the iOS 27.1 SDK so that it fills
  the Duo's screen (an app labelled for an older SDK runs in a small compatibility window).
- A Mac with Apple silicon and Xcode, an Apple ID to sign the app, and about 18 GB free on the Duo (more while disc
  images are on it). The free Apple ID limits and every install step are the same as for iOS: see README-ios.txt.
- A gamepad is the main way to play; touch covers the lower screen and the menus.

INSTALL
-------
Follow README-ios.txt, building with --duo instead of --ios (the builder's README.txt has the command). Put the
disc images into the Duo app's own folder: Files app > On My iPhone > FivePocket Duo.

SCREENS
-------
- Portrait, split (the default): the top half is the game's picture; the bottom half is the lower screen with the
  minimap or map, the weapon, radio and character wheels, the phone and help text. The divider stays at the middle.
- Landscape, or Lower Screen Off: full screen, the regular layout, no lower screen.
- The status bar is hidden.

TOP SCREEN (side menu > Dual Screens > Lower Screen > Top Screen)
---------------------------------------------------------------
- Fit (default): the whole picture, its shape kept, with thin bars where the screen's shape differs.
- Zoom: enlarged to fill the top screen, its shape kept, with the edges cut off.
- Stretch: the whole picture stretched to fill; no bars, nothing cut off.
- 4:3: the game's own non-widescreen layout (menus, loading screens and HUD placed for 4:3, a narrower view), drawn
  like Fit with thin bars at the sides. It needs a restart: the game reads its display mode once at start; the row
  says "Restart the game to apply." until then.
Fit, Zoom and Stretch apply at once. The choice is kept across launches.

LOWER-SCREEN CONTROLS (in this build and covered by tests; not yet checked by touch on the Duo)
-------------------------------------------------------------------------------------------
- Wheels: hold LB, D-pad Left or D-pad Down, then pick on the lower screen.
- Map: tap the lower screen's map to set a waypoint; with the pause map open in split, drag on the top half pans,
  pinch zooms and a tap sets a waypoint.
- Phone: swipe or tap on the lower screen; while it rings or a call is on, a quick tap on the left half answers and
  on the right half declines.
- Quick Save button; a left-edge swipe on the lower screen opens the side menu.
- Full screen: the small button on the lower screen, or a two-finger double tap.
- With a gamepad connected, the on-screen touch controls hide by themselves.

Saves live in the app's own storage and survive reinstalls of the same app (same bundle identifier).

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
