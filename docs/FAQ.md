# FAQ

**What is FivePocket?**
A free fan project that runs your own Xbox 360 copy of a 2013 open-world game natively on Android phones, the AYN
Thor, iPads and iPhones. It is a static recompilation with its own renderer, not an emulator. The apps and the builder
contain no game code, data or shaders.

**Do I need my own discs?**
Yes. You need the two Xbox 360 discs of the game (Disc 1 "Install" and Disc 2 "Play") that you own, as disc images.
FivePocket never downloads game files and nothing you build may be shared ([LEGAL.md](../LEGAL.md)).

**Which devices are supported?**
See [Supported devices](../README.md#supported-devices): Android 8.0+ (arm64, Vulkan, Snapdragon 8-class), the AYN
Thor, iOS / iPadOS 17.4 or later (tested on an iPad mini with A17 Pro; 8 GB devices are the safe choice) and the Duo.

**Why does the build take so long?**
The builder translates the game's whole PowerPC program and its shaders into code for your device, once. For the Android
editions it takes 2 to 4 hours on a 16 GB Windows PC (our measured builds took about 3 and 3.5 hours); the iOS and Duo
apps take about an hour on an Apple-silicon Mac. You only rebuild when you update to a new FivePocket version.

**Where do I get help?**
On the [FivePocket Discord](https://discord.gg/mZvqCsxgxT). Look at [TROUBLESHOOTING.md](TROUBLESHOOTING.md) first.

**How do I report a bug?**
Post in the Discord's #bug-reports forum or open a [bug report](https://github.com/marcm0de/FivePocket/issues/new/choose)
on GitHub. Say which edition, version and device you use and what you did, and add the log
([where to find it](TROUBLESHOOTING.md#logs)). Never attach disc images, bundles, the app you built, caches, saves or
memory dumps.
