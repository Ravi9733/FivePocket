# Releasing

A release is made from a clean checkout of a tagged commit. Every step must pass; nothing is published until all do.

1. `python3 tools/release/run_checks.py` passes on the clean checkout.
2. If the builder's `XenonRecomp` or `xex_image` programs were rebuilt, check they come from upstream XenonRecomp
   `16ee5918` plus our patch (`diff_sha256` in the builder's `sources/sources.json`), then add their SHA-256 to
   `tools/release/upstream-key-allow.txt`; without that the key scan fails on them, as it should.
3. Make the downloads: `python3 tools/release/package_release.py --version $(cat VERSION) --out dist ...`
   (one archive per platform, the source archive, `SHA256SUMS`).
4. Unpack every archive into a new folder and run on each:
   - `tools/release/scan_names.py`, `scan_secrets.py`, `scan_owner.py`;
   - `tools/release/taint_scan.py --published <unpacked> --retail-list <your own game files>`; the report must end
     `TAINT PASS` with every control found.
5. Install the app download on a test device from the unpacked archive, run the builder download on a clean machine,
   import, start the game.
6. Create the release as a **draft** first, with the hashes, the commit and the scan results in its notes. Publish
   only after reading the draft page.
