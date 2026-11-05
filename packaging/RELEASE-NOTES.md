# FivePocket {version} — first public release

**Play the 2013 open-world classic natively on Android, the AYN Thor, iPad and iPhone, from your own Xbox 360 discs.**
FivePocket is a static recompilation with a new renderer, not an emulator. No game code, data or shaders are in any
download: the FivePocket Builder makes everything from your own discs, on your own computer.

## Editions

| Edition | Device | How you get it |
|---|---|---|
| **FivePocket** | Android 8.0+, arm64 + Vulkan, Snapdragon 8-class or better | `FivePocket-{version}-android.zip` + the builder |
| **FivePocket Thor Edition** | AYN Thor (dual screen) | `FivePocket-Thor-Edition-{version}-android.zip` + the builder |
| **FivePocket iOS** | iPhone / iPad, iOS 17.4+ (8 GB devices recommended) | the Mac builder makes the app; you sign it with your Apple ID |
| **FivePocket Duo** | the Duo (iPhone19,4), iOS 27.2 tested | the Mac builder makes the app; you sign it with your Apple ID |

Builders: the Android editions build on Windows only in this release (x86-64, one builder per edition); the iOS and Duo
apps build on a Mac (Apple silicon, Xcode). A Mac builder for the Android editions is planned.

## Highlights

- Touch controls laid out like the mobile classics (hidden when a controller is connected), with steering arrows in vehicles.
- Menus in the style of the console game's; icons instead of button prompts on touch screens without a controller.
- Thor Edition: the game's own HUD on the second screen (bigger minimap, wanted stars), weapon wheel on the lower screen,
  touch phone with Answer/Decline.
- Duo: game on the top screen, map, wheels and phone below; Top Screen setting Fit (default) / Zoom / Stretch / 4:3
  (4:3 after a restart).
- Side menu with settings, a trainer and script-mod support.
- Leaving the app for a moment keeps your game (25 s), held still and silent.
- iOS: the app imports your two disc images on the device (Files app, Finder or a USB drive), about 2 minutes.

## Performance (measured)

- iPad mini (A17 Pro): steady 30 fps at full resolution.
- AYN Thor: steady 30 fps in our tests (frame generation shows about 60).
- Duo: steady 30 fps on foot; occasional hitches while driving.
- Galaxy Z Fold4: around 25 fps; it slows down when hot.
- The first launch builds shader caches and stutters briefly.

## Before you start

- You need your own Xbox 360 discs (Disc 1 and Disc 2) as images. FivePocket never downloads game files.
- **Never share what you build** (disc images, the bundle, the .ipa/.apk you build, game files, saves). See LEGAL.md.
- FivePocket is free and has no affiliation with the game's publisher or developer, Microsoft, Apple, Google or AYN.
- Closed-alpha users on Android: uninstall the old app first, which **deletes its saves**, so copy them out.

Source: `FivePocket-{version}-source.tar.xz` (GPL-3.0-or-later). Checksums: `SHA256SUMS` and the table below.
Community and help: the FivePocket Discord, https://discord.gg/mZvqCsxgxT
