# Changelog

One version number covers every edition and every download of a release (the app, the builder for each computer and
the source archive). Format: [Keep a Changelog](https://keepachangelog.com/); versions follow
[Semantic Versioning](https://semver.org/).

## [1.1.0-alpha.1] - unreleased

First public release. In this release the Android apps still report their own app versions: FivePocket shows
1.0.2-alpha-qol and FivePocket Thor Edition shows 1.0.3-alpha-thor (About page and Android's app info). Both are the apps
of release 1.1.0-alpha.1; the next update gives them the release's version number.

### Added
- Four editions, one version:
  - **FivePocket** (Android 8.0+, arm64, Vulkan; tested on a Galaxy Z Fold4) and **FivePocket Thor Edition** (AYN
    Thor, dual screen): apps with no game code inside; the app imports the package the builder makes from your discs.
  - **FivePocket iOS** (iOS / iPadOS 17.4 or later; tested on an iPad mini with A17 Pro, 8 GB) and **FivePocket Duo**
    (the Duo, iPhone19,4; tested on iOS 27.2): the Mac builder makes the app from your discs, you sign it with your
    own Apple ID, and the app imports the two disc images on the device (Files, Finder or a USB drive; about
    2 minutes on the iPad mini). The app contains no disc files.
- Builders: Windows (x86-64) for the Android editions, one per edition (`check`, `fetch`, `build`, `verify`), and
  macOS (Apple silicon, Xcode) for the iOS and Duo apps (`check`, `fetch`, `build --ios`/`--duo`, `sign`).
- Thor Edition: the game's HUD on the second screen (bigger minimap, wanted stars), the weapon wheel on the lower
  screen, a touch phone with Answer / Decline.
- Duo: the game on the top half, map, wheels and phone on the lower half; Top Screen setting Fit (default) / Zoom /
  Stretch / 4:3 (4:3 after a restart); landscape or Lower Screen Off for full screen.
- Touch controls laid out like mobile open-world games, hidden while a controller is connected; steering arrows in
  vehicles; icons instead of button prompts on touch screens without a controller.
- Side menu with settings, a trainer and script-mod support.
- Release checks: taint scan against your own game files, name check, key and secret scan, personal-detail scan.

### Fixed
- Leaving the app briefly no longer loses the game: it is held still and silent for 25 seconds in the background and
  carries on when you come back (Android).

### Changed
- New name (FivePocket) and a new signing key for all editions.

### Upgrade note
- Apps from the closed alpha were signed with a different key. Android only installs an update signed with the same
  key, so the alpha app must be uninstalled first, and **uninstalling deletes its saves**. Copy your saves out before
  you uninstall.
