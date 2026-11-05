# Contributing

Thank you for helping. Three rules come before everything else:

1. **No game content, ever.** Do not commit, attach, paste or link anything from the never-share table in
   [LEGAL.md](LEGAL.md): disc files, the decoded executable, generated code, shader packs or caches, bundles, saves,
   memory dumps. Not in code, tests, issues, pull requests or screenshots of hex views.
2. **No keys or personal details.** No console keys, signing keys, tokens, home folder paths, device serials or
   private network addresses.
3. **No game names in product names.** Apps, app labels, downloads, folders and anything the player sees use
   FivePocket names. Internal source files may keep a descriptive name (`tools/release/scan_names.py` says exactly
   what is checked).

Before a pull request, run:

```
python3 tools/release/run_checks.py
```

It runs the name, secret and personal-detail checks and the release-check tests. If you have your own discs and a
builder work folder, also run the taint scan on your change:

```
python3 tools/release/taint_scan.py --published <changed files> --retail <your disc images and builder outputs>
```

## Code

- C++20 for native code, Java 17 for the Android app, Python 3.12 for tools.
- Keep the style of the file you are editing. New files carry `SPDX-License-Identifier: GPL-3.0-or-later` unless they
  adapt code under another licence, which then must be named in the file and in THIRD-PARTY-NOTICES.md.
- Add a host test that fails without your change when you fix a bug.
- By sending a contribution you agree that it is published under the GPL, version 3 or later.

## Reporting a bug

Post in the `#bug-reports` forum on the [FivePocket Discord](https://discord.gg/mZvqCsxgxT) (pick your edition's tag) or use the
[bug report form](https://github.com/marcm0de/FivePocket/issues/new/choose) here. Say which edition, version and
device you use and what you did before the problem, and add the log:

- **Builder:** the report from `builder build ... --report report.json`;
- **Android, Thor:** `adb logcat -d > fivepocket-log.txt` right after the problem (USB debugging on);
- **iOS, Duo:** after a crash, Settings → Privacy & Security → Analytics & Improvements → Analytics Data, the
  entry with the app's name; otherwise a description and a screenshot.

**Never attach built files**: disc images, bundles, the `.apk`/`.ipa` you built, the app's game folder, caches,
saves or memory dumps. Details: [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md#logs).
