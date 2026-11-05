# Code bundle, format 1 (`gta5-code-bundle/1`)

The desktop builder (`disc_builder.py`) writes one file, the code bundle; the phone app imports it with the file
picker. This page is the contract between the two. It contains no game data.

Both sides exist and follow this page:

* Writer and reference check: `write_bundle` and `verify_bundle` in `disc_builder.py` (`disc_builder.py verify FILE`
  runs the check).
* The app's reader: `CodeBundle.java` and `CodeBundleImporter.java` in
  `android/app/src/main/java/dev/recompbootstrap/smoke/`.

An earlier sketch of the app's side (a `bundle.json` manifest with flat entry names, in section 4.E of the app
design document) is not this format and is not what the app code reads; that section of the design document is out
of date.

## Container

* A ZIP file. Every entry is STORED (no compression) and not encrypted; no ZIP64, so the whole file is smaller than
  4 GiB.
* The app reads the file once, front to back, as a stream. It therefore goes by the ZIP *local file headers*, in file
  order, and does not consult the central directory at the end. Every local header carries the entry's CRC-32 and
  its size (written twice, as packed and unpacked size, with the same value); the general-purpose flags are 0 (bit
  11, "UTF-8 name", would be tolerated; an entry whose sizes follow its data, flag bit 3, is refused). The central
  directory is still written, so ordinary ZIP tools can list and unpack a bundle.
* The first entry is `manifest.json`, at most 1 MiB. The other entries are exactly the components the manifest
  lists, each once, sorted by name.
* Entry names match `[a-z0-9][a-z0-9._-]*(/[a-z0-9][a-z0-9._-]*)*` (lower case, forward slashes, no `..`).
* The writer is deterministic (fixed 1980-01-01 timestamps, fixed attributes): the same manifest and component bytes
  give the same file, byte for byte.
* Seal: the ZIP archive comment (the last 85 bytes of the file) is the ASCII text `own-disc-seal sha256=` followed by
  64 lower-case hex digits: the SHA-256 of every byte of the file before the comment (that range includes the
  end-of-central-directory record with its comment-length field). The 64 hex digits are the "bundle seal"; the builder
  prints it at the end of a build, so the app can show it for comparison.

## manifest.json

UTF-8 JSON object, keys sorted:

| Key | Meaning |
|---|---|
| `schema` | `"gta5-code-bundle/1"`. Any other value is refused. |
| `profile` | Name of the pipeline profile (the recipe of this build), for display and bug reports. |
| `flavour` | Build flavour, e.g. `snapdragon` or `generic`. |
| `game` | `{"id", "executable_sha256", "executable_size"}`: the game executable (default.xex of the play disc) this code was built from. `executable_sha256` is 64 lower-case hex digits. |
| `runtime_abi` | A non-empty string naming the interface between the app's runtime and the game library. The app refuses a value it does not implement. |
| `versions` | Free-form strings for display: base build, recipe, generator and compiler versions, ABI, and `builder_sha256` (the builder script). |
| `stages` | `[{"id", "key", "provenance"}]`: the builder stages in order with their receipt keys (SHA-256 over each stage's definition, tools, inputs and the previous key). |
| `components` | `[{"name", "role", "size", "sha256", "stage", "provenance"}]`, at least one, sorted by name. `name` is the ZIP entry; `size` is a number of bytes; `sha256` is 64 lower-case hex digits. |

`provenance` is `built` (produced by a builder stage on this machine), `disc` (copied unchanged from the disc) or
`supplied` (a file handed to the builder because no stage builds it yet; the app should show this).

Roles: `game-library` (the native library, loaded by file descriptor), `title-xex` (the game executable, unchanged),
`decoded-image` (the executable decrypted and expanded to its memory layout), `shader-pack`. Any other role is an
error in format 1: a new role means a new `runtime_abi`. Planned, not yet produced by any stage and not yet roles:
boot shader modules, file catalogs.

## What a reader must check

`verify_bundle` makes checks 1 to 5 and, when it is told the expected executable hash, the first half of check 6.
The app makes all of them (it compares the seal last, when the stream has ended).

1. File size at least 107 bytes and below 4 GiB; the 22 bytes before the last 85 are an end-of-central-directory
   record (`PK\5\6`) whose comment length is 85; the comment starts with `own-disc-seal sha256=`.
2. SHA-256 of the file without its last 85 bytes equals the 64 hex digits. This detects any single changed, missing or
   added byte anywhere in the file (tested exhaustively on a small bundle).
3. The entries, read through their local headers from the start of the file up to the central directory: every one
   stored, with flags 0 (or only bit 11) and two equal sizes, and not running past the end of the file. The first is
   `manifest.json`, at most 1 MiB, and its bytes match the CRC-32 of its header.
4. The manifest: `schema` as above; `game.executable_sha256` a SHA-256; `runtime_abi` present (the builder checks only
   that; the app also requires the value it implements); at least one component; every component with a name of
   the allowed form that is not `manifest.json` and not repeated, a size, a SHA-256 and a known role; a `title-xex`
   component has the SHA-256 that `game.executable_sha256` names.
5. Every further entry is a listed component, none twice, none missing; its size equals the manifest's, its bytes
   match the CRC-32 of its header and their SHA-256 equals the manifest's.
6. `game.executable_sha256` equals the SHA-256 of the default.xex of the supported release, and the app has already
   copied that file from the player's own play disc.
7. (App.) Copy the components into app-private storage (owner read-only) and keep the manifest beside them; re-hash
   the installed components against the kept manifest before use, not against anything passed in from outside.

The seal and the hashes detect damage and mix-ups. They are not a signature: the bundle is made on the player's own
computer and nothing in it proves who made it.

## How the two sides were checked against each other

* `tools/own_disc/tests/test_disc_builder.py` packs a bundle field by field from this page (no ZIP library) and
  requires the builder's file to be byte for byte the same; it also makes one complete, correctly sealed bundle for
  every rule above with that one rule broken and requires `verify_bundle` to refuse it for that reason.
* On 2026-10-04 a bundle written by the builder from synthetic files (the four roles, the component names and the
  `runtime_abi` of the private v458 profile) was handed to the app's `CodeBundleImporter.importBundle`, compiled on
  the host with the JDK alone: imported, and the installed files had the sizes and hashes of the manifest. The same
  bundle with one changed byte, with its last 100 bytes missing, and for another executable was refused by the app.
  Nothing was run on a phone.

## Format 2 (2026-10-07): what the public app reads

Schema `gta5-code-bundle/2`. The container, the seal and every check above are unchanged. Three differences:

1. **Runtime data.** A component may have the role `runtime-data`: a small file the runtime reads beside the game
   data (the six catalogs, the start-up shader modules and their guest-code companions). Such a component is named
   under `runtime/`, and nothing else is.
2. **Exactly one** component of each of the four roles of format 1 (`game-library`, `title-xex`, `decoded-image`,
   `shader-pack`).
3. **Nothing of the machine that built it.** The manifest has no `stages` and no `versions`: stage keys cover local
   paths and the host's own tool builds, so two machines never wrote the same format-1 manifest. In their place is
   `portable_provenance`: the texts the profile states under `bundle.provenance` (what the build is made from: source
   revisions, the compiler release, the recipe, parameter tables) and `tools`, the files the profile lists under
   `bundle.portable_tools`, each named as the profile writes it (`${repo}/...`, unexpanded) with its SHA-256.

So two machines that build from the same discs with the same profile and tools write the **same file**, and
comparing the 64 characters of the seal compares everything. The full receipts of a build stay in its work folder.

Profile: `"bundle": {"format": 2, "provenance": {...}, "portable_tools": [...], ...}`. Format 1 stays the default
and keeps refusing `runtime-data`.

Checked: `test_disc_builder.py`, class `BundleFormat2` (the manifest's content; one fixture built in two places gives
one file in format 2 and two files in format 1; every rule broken once in a resealed bundle); the app's reader for
format 2 is the builder-app lane's `CodeBundle.java` (not yet in the tracked app).

## Open points

* The value of `runtime_abi`: the private profile for v458 and the app both use `private-trace-fd-dlopen/1`. It has
  to change together on both sides whenever the interface between the app's runtime and the library changes; no
  rule for that exists yet.
* Whether the decoded image and the executable travel in the bundle (as here, following the design's P1) or are
  produced on the phone from the disc. The app accepts both roles today.
* Where the app shows the seal and the `supplied` marks.
* The app starts an imported install only from a later version on (its design says so); what the launcher needs from
  the manifest for that is not settled.
