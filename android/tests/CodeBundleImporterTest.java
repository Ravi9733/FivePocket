package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SetupTestKit.bytes;
import static dev.recompbootstrap.smoke.SetupTestKit.check;
import static dev.recompbootstrap.smoke.SetupTestKit.expect;
import static dev.recompbootstrap.smoke.SetupTestKit.expectContains;
import static dev.recompbootstrap.smoke.SetupTestKit.expectEquals;
import static dev.recompbootstrap.smoke.SetupTestKit.item;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.Map;
import java.util.TreeMap;

/**
 * Run with java -ea. Bundles are built by the test itself (SetupTestKit.Bundle) from synthetic bytes.
 * Two optional system properties connect the test with the real desktop builder (set by run_tests.sh):
 *   -Dsetup.pybundle.dir=DIR   a bundle written by tools/own_disc/disc_builder.py write_bundle (DIR/test.bundle, with
 *                              DIR/executable.sha256 and the component sources under DIR/src) is imported here;
 *   -Dsetup.kitbundle.out=FILE this test's own good bundle is written to FILE (FILE.sha256 holds the executable hash)
 *                              so that the builder's verify_bundle can check it.
 */
public final class CodeBundleImporterTest {
    private static final byte[] EXECUTABLE = bytes(70000, 61), LIBRARY = bytes((1 << 20) + 54321, 62), IMAGE = bytes(90000, 63),
            SHADERS = bytes(33333, 64);
    private static final String EXE_SHA = SetupTestKit.sha256(EXECUTABLE);
    private static final SetupCatalog.Item I_EXE = item("default.xex", EXECUTABLE);
    private static final SetupCatalog.Spec SPEC = SetupTestKit.spec(EXECUTABLE, Arrays.asList(I_EXE), true);
    private static final String LIB = "lib/arm64-v8a/libgame.so", SHADER_NAME = "shaders/generic-shaders.bin";

    private static SetupTestKit.Bundle bundle() {
        return new SetupTestKit.Bundle(EXE_SHA).add(LIB, "game-library", LIBRARY).add("title.xex", "title-xex", EXECUTABLE)
                .add("default.image.bin", "decoded-image", IMAGE).add(SHADER_NAME, "shader-pack", SHADERS);
    }

    private static File root(File files) {
        return new File(files, "game-install");
    }

    /** files/ with the player's own executable already copied from the disc, and a save that must stay as it is. */
    private static File prepared(String label) throws IOException {
        File files = SetupTestKit.newFilesDir(label);
        SetupTestKit.write(new File(files, "game-install/inputs/" + EXE_SHA + ".bin"), EXECUTABLE);
        SetupTestKit.write(new File(files, "saves/slot0"), bytes(64, 65));
        return files;
    }

    private static VerifiedExtractor.Outcome run(InputStream zip, File files) {
        return CodeBundleImporter.importBundle(zip, root(files), new SetupTestKit.Recorder(), SPEC, SetupTestKit.PLENTY);
    }

    private static VerifiedExtractor.Outcome run(byte[] zip, File files) {
        return run(new ByteArrayInputStream(zip), files);
    }

    private static TreeMap<String, String> snapshotWithoutEmptyStaging(File files) throws IOException {
        TreeMap<String, String> s = SetupTestKit.snapshot(files);
        if ("dir".equals(s.get("game-install/staging")) && new File(files, "game-install/staging").list().length == 0) {
            s.remove("game-install/staging");
        }
        return s;
    }

    private static final VerifiedExtractor.Problem NOT_ONE = VerifiedExtractor.Problem.NOT_A_BUNDLE,
            DAMAGED = VerifiedExtractor.Problem.BUNDLE_DAMAGED, OTHER_BUILD = VerifiedExtractor.Problem.BUNDLE_OTHER;

    /**
     * Imports zip into a fresh prepared files/ and requires: refused with this reason and this kind (the kind decides
     * which next step the page gives), and nothing imported or left behind.
     */
    private static void expectRefused(String label, byte[] zip, VerifiedExtractor.Problem kind, String reason) throws IOException {
        File files = prepared("refused");
        TreeMap<String, String> before = SetupTestKit.snapshot(files);
        VerifiedExtractor.Outcome o = run(zip, files);
        expect(!o.complete && !o.cancelled, label + ": refused");
        expectContains(o.error, reason, label);
        expectEquals(kind, o.problem, label + ": the kind");
        expect(!new File(root(files), "bundle").exists(), label + ": no bundle directory");
        expectEquals(before, snapshotWithoutEmptyStaging(files), label + ": nothing written or left behind");
    }

    private static void expectImported(File files, Map<String, byte[]> components) throws IOException {
        File bundle = new File(root(files), "bundle");
        for (Map.Entry<String, byte[]> e : components.entrySet()) {
            File f = new File(bundle, e.getKey());
            expect(f.isFile(), e.getKey() + " is in place");
            expect(Arrays.equals(e.getValue(), SetupTestKit.read(f)), "bytes of " + e.getKey());
            expectEquals("r--------", SetupTestKit.mode(f), "mode of " + e.getKey());
        }
        expect(!new File(root(files), "staging/bundle.part").exists(), "the staging copy is gone");
    }

    public static void main(String[] args) throws Exception {
        SetupTestKit.requireAssertions();
        final byte[] good = bundle().build();

        check("import: a good bundle is imported: every component byte for byte, the manifest kept, the record complete", () -> {
            File files = prepared("import-good");
            SetupTestKit.Recorder progress = new SetupTestKit.Recorder();
            VerifiedExtractor.Outcome o = CodeBundleImporter.importBundle(new ByteArrayInputStream(good), root(files), progress, SPEC,
                    SetupTestKit.PLENTY);
            expect(o.complete && o.error == null, "complete: " + o.error);
            SetupTestKit.Bundle b = bundle();
            expectImported(files, b.components);
            expectEquals(b.manifest(), new String(SetupTestKit.read(new File(root(files), "bundle/manifest.json")), StandardCharsets.UTF_8),
                    "the kept manifest is the bundle's manifest");
            expectEquals("{\"version\":1,\"executableSha256\":\"" + EXE_SHA
                    + "\",\"discFilesComplete\":true,\"bundleComplete\":true,\"complete\":true}\n",
                    new String(SetupTestKit.read(new File(root(files), "install.json")), StandardCharsets.UTF_8), "install.json");
            long total = LIBRARY.length + EXECUTABLE.length + IMAGE.length + SHADERS.length;
            expect(progress.bytesDone == total && progress.bytesTotal == total && !progress.decreasing, "progress ends at the total");
            expectEquals(SetupTestKit.sha256(bytes(64, 65)), SetupTestKit.sha256(SetupTestKit.read(new File(files, "saves/slot0"))), "the save");
        });

        check("import: the result does not depend on how the stream hands out its bytes", () -> {
            int[][] patterns = {{1}, {7, 100, 1, 3000}, {106}, {107}, {108}, {85, 22}, {1 << 16}};
            for (int[] pattern : patterns) {
                File files = prepared("import-chunks");
                VerifiedExtractor.Outcome o = run(SetupTestKit.chunked(good, pattern), files);
                expect(o.complete, "chunks " + Arrays.toString(pattern) + ": " + o.error);
                expectImported(files, bundle().components);
            }
        });

        check("import: a bundle written by the real desktop builder (disc_builder.py write_bundle) is imported", () -> {
            String dir = System.getProperty("setup.pybundle.dir");
            if (dir == null || !new File(dir, "test.bundle").isFile()) {
                SetupTestKit.note("skipped: no builder-made bundle was supplied (-Dsetup.pybundle.dir)");
                return;
            }
            String sha = new String(SetupTestKit.read(new File(dir, "executable.sha256")), StandardCharsets.US_ASCII).trim();
            byte[] executable = SetupTestKit.read(new File(dir, "src/title.xex"));
            expectEquals(sha, SetupTestKit.sha256(executable), "the supplied executable");
            SetupCatalog.Spec spec = SetupTestKit.spec(executable, Arrays.asList(item("default.xex", executable)), true);
            File files = SetupTestKit.newFilesDir("import-builder");
            SetupTestKit.write(new File(files, "game-install/inputs/" + sha + ".bin"), executable);
            byte[] zip = SetupTestKit.read(new File(dir, "test.bundle"));
            VerifiedExtractor.Outcome o = CodeBundleImporter.importBundle(new ByteArrayInputStream(zip), root(files),
                    new SetupTestKit.Recorder(), spec, SetupTestKit.PLENTY);
            expect(o.complete, "complete: " + o.error);
            Map<String, byte[]> sources = new TreeMap<String, byte[]>();
            for (String name : new String[] {"lib/arm64-v8a/libgame.so", "title.xex", "default.image.bin", "shaders/generic-shaders.bin"}) {
                sources.put(name, SetupTestKit.read(new File(dir, "src/" + name)));
            }
            expectImported(files, sources);
            // and the builder's bundle with one byte changed is refused (the check above is not a formality)
            zip[zip.length / 2] ^= 1;
            File files2 = SetupTestKit.newFilesDir("import-builder-damaged");
            SetupTestKit.write(new File(files2, "game-install/inputs/" + sha + ".bin"), executable);
            VerifiedExtractor.Outcome damaged = CodeBundleImporter.importBundle(new ByteArrayInputStream(zip), root(files2),
                    new SetupTestKit.Recorder(), spec, SetupTestKit.PLENTY);
            expect(!damaged.complete && !new File(root(files2), "bundle").exists(), "a damaged builder bundle is refused");
        });

        check("import: a bundle built from another game executable is refused", () -> {
            SetupTestKit.Bundle b = bundle();
            b.executableSha256 = SetupTestKit.sha256(bytes(70000, 66));
            b.components.remove("title.xex");          // so that the manifest is consistent in itself
            expectRefused("other executable", b.build(), OTHER_BUILD, "built from another game program (check code " + b.executableSha256.substring(0, 12) + ") than the one on the play disc of Synthetic test release");
        });

        check("import: a component whose bytes differ from the manifest is refused and named", () -> {
            SetupTestKit.Bundle b = bundle();
            byte[] changed = LIBRARY.clone();
            changed[changed.length - 1] ^= 1;          // same size; the ZIP's own CRC and the seal are consistent with it
            b.storedInstead.put(LIB, changed);
            expectRefused("wrong hash", b.build(), DAMAGED, "The code bundle is damaged: " + LIB + " does not match its list of contents.");
            SetupTestKit.Bundle shorter = bundle();
            shorter.storedInstead.put(SHADER_NAME, Arrays.copyOf(SHADERS, SHADERS.length - 1));
            expectRefused("wrong size", shorter.build(), DAMAGED, "The code bundle is damaged: " + SHADER_NAME + " has another size than its list of contents says.");
        });

        check("import: one changed byte outside the components (here: in the ZIP directory) breaks the seal", () -> {
            SetupTestKit.Bundle b = bundle();
            byte[] zip = b.build();
            zip[b.centralDirectoryAt + 20] ^= 1;
            expectRefused("seal", zip, DAMAGED, "seal does not match its contents");
            byte[] noSeal = Arrays.copyOf(good, good.length);
            noSeal[noSeal.length - 85] = 'X';          // the comment no longer starts with the seal's text
            expectRefused("seal text", noSeal, DAMAGED, "seal record is missing or damaged");
        });

        check("import: a file that was cut off is refused wherever the cut is", () -> {
            SetupTestKit.Bundle b = bundle();
            byte[] zip = b.build();
            expectRefused("cut inside a component", Arrays.copyOf(zip, b.dataAt.get(LIB) + 5000), DAMAGED, "ends inside " + LIB);
            expectRefused("cut inside the ZIP directory", Arrays.copyOf(zip, b.centralDirectoryAt + 30), DAMAGED, "seal record is missing");
            expectRefused("cut inside the first header", Arrays.copyOf(zip, 10), DAMAGED, "incomplete");
            expectRefused("empty file", new byte[0], DAMAGED, "incomplete");
        });

        check("import: entry names that could leave the bundle directory are refused", () -> {
            // the rule itself: one or more parts of [a-z0-9][a-z0-9._-]*, joined by '/'
            String[] valid = {"a", "title.xex", "lib/arm64-v8a/libgame.so", "0-9_x.y/z"};
            String[] invalid = {"", "..", "../a", "a/..", "a/../b", "/a", "a/", "a//b", "A", "a b", "a\\b", ".hidden", "a/.b", "-a"};
            for (String name : valid) expect(CodeBundle.validName(name), "valid: " + name);
            for (String name : invalid) expect(!CodeBundle.validName(name), "invalid: " + name);
            // a ZIP entry with such a name stops the import
            for (String bad : new String[] {"../evil.bin", "/abs.bin", "shaders/../../evil.bin", "Upper.bin", "a\\b.bin", "shaders//x.bin"}) {
                SetupTestKit.Bundle b = bundle();
                b.renamedInZip.put(SHADER_NAME, bad);
                expectRefused("zip entry " + bad, b.build(), NOT_ONE, "badly named entry");
            }
            SetupTestKit.Bundle listed = bundle();
            listed.add("../evil.bin", "shader-pack", bytes(10, 67));
            expectRefused("manifest lists a bad name", listed.build(), NOT_ONE, "component name not allowed");
        });

        check("import: entries and manifest must list exactly the same components, once each, manifest first", () -> {
            SetupTestKit.Bundle extra = bundle();
            extra.unlisted.put("extra.bin", bytes(10, 68));
            expectRefused("unlisted entry", extra.build(), NOT_ONE, "entry extra.bin is not listed in its manifest");
            SetupTestKit.Bundle missing = bundle();
            missing.leftOut.add("default.image.bin");
            expectRefused("missing component", missing.build(), NOT_ONE, "component default.image.bin is missing");
            SetupTestKit.Bundle twice = bundle();
            twice.repeated.add("title.xex");
            expectRefused("repeated entry", twice.build(), NOT_ONE, "entry title.xex appears twice");
            SetupTestKit.Bundle late = bundle();
            late.manifestFirst = false;
            expectRefused("manifest not first", late.build(), NOT_ONE, "its first entry is not manifest.json");
        });

        check("import: compressed, encrypted or streamed entries are refused", () -> {
            SetupTestKit.Bundle compressed = bundle();
            compressed.method = 8;
            expectRefused("compressed", compressed.build(), NOT_ONE, "compressed entry");
            SetupTestKit.Bundle encrypted = bundle();
            encrypted.flags = 1;
            expectRefused("encrypted", encrypted.build(), NOT_ONE, "encrypted or streamed entry");
            SetupTestKit.Bundle streamed = bundle();
            streamed.flags = 8;
            expectRefused("streamed", streamed.build(), NOT_ONE, "encrypted or streamed entry");
        });

        check("import: other formats, other runtime interfaces, unknown roles and broken manifests are refused", () -> {
            SetupTestKit.Bundle other = bundle();
            other.schema = "some-other-file/1";
            expectRefused("other schema", other.build(), NOT_ONE, "this is not a code bundle");
            SetupTestKit.Bundle newer = bundle();
            newer.schema = "gta5-code-bundle/2";
            expectRefused("format 2", newer.build(), OTHER_BUILD, "has format 2");
            SetupTestKit.Bundle abi = bundle();
            abi.runtimeAbi = "private-trace-fd-dlopen/2";
            expectRefused("runtime interface", abi.build(), OTHER_BUILD, "built for another app version (runtime interface private-trace-fd-dlopen/2)");
            SetupTestKit.Bundle role = bundle();
            role.add("boot/module.spv", "boot-module", bytes(10, 69));
            expectRefused("unknown role", role.build(), NOT_ONE, "a role this app does not know: boot-module");
            SetupTestKit.Bundle broken = bundle();
            broken.manifestOverride = "{\"schema\": ";
            expectRefused("broken manifest", broken.build(), NOT_ONE, "its manifest cannot be read");
            SetupTestKit.Bundle otherExe = bundle();
            otherExe.components.put("title.xex", bytes(70000, 70));   // the bundled executable is not the one the manifest names
            expectRefused("bundled executable", otherExe.build(), NOT_ONE, "the bundled executable is not the one the manifest names");
        });

        check("import: what an interrupted import left in staging is replaced, not mixed in", () -> {
            File files = prepared("import-leftover");
            SetupTestKit.write(new File(root(files), "staging/bundle.part/junk.bin"), bytes(100, 71));
            SetupTestKit.write(new File(root(files), "staging/bundle.part/lib/arm64-v8a/libgame.so"), bytes(100, 72));
            VerifiedExtractor.Outcome o = run(good, files);
            expect(o.complete, "complete: " + o.error);
            expectImported(files, bundle().components);
            expect(!new File(root(files), "bundle/junk.bin").exists(), "no left-over file in the bundle");
        });

        check("import: links left in staging are removed or refused, never followed", () -> {
            // (a) links inside the left-over staging copy, to the saves folder and to one save
            File files = prepared("import-links");
            File part = new File(root(files), "staging/bundle.part");
            expect(part.mkdirs(), "mkdir");
            Files.createSymbolicLink(new File(part, "to-the-saves").toPath(), new File(files, "saves").toPath());
            Files.createSymbolicLink(new File(part, "to-a-save").toPath(), new File(files, "saves/slot0").toPath());
            TreeMap<String, String> saves = SetupTestKit.snapshot(new File(files, "saves"));
            VerifiedExtractor.Outcome o = run(good, files);
            expectEquals(saves, SetupTestKit.snapshot(new File(files, "saves")), "the saves are untouched");
            expect(o.complete, "the left-over was cleared and the import went through: " + o.error);
            expectImported(files, bundle().components);
            expect(!new File(root(files), "bundle/to-a-save").exists(), "no link ends up in the bundle");
            // (b) the staging copy itself is a link to the saves folder
            File files2 = prepared("import-link-part");
            expect(new File(root(files2), "staging").mkdirs(), "mkdir");
            Files.createSymbolicLink(new File(root(files2), "staging/bundle.part").toPath(), new File(files2, "saves").toPath());
            TreeMap<String, String> saves2 = SetupTestKit.snapshot(new File(files2, "saves"));
            VerifiedExtractor.Outcome o2 = run(good, files2);
            expectEquals(saves2, SetupTestKit.snapshot(new File(files2, "saves")), "the saves are untouched");
            expect(!o2.complete && !new File(root(files2), "bundle").exists(), "and nothing is imported");
            expectContains(o2.error, "leaves the setup directory", "with the reason");
            // (c) the staging copy is a link to a place that does not exist yet
            File files3 = prepared("import-link-dangling");
            expect(new File(root(files3), "staging").mkdirs(), "mkdir");
            Files.createSymbolicLink(new File(root(files3), "staging/bundle.part").toPath(), new File(files3, "saves/made-through-a-link").toPath());
            TreeMap<String, String> saves3 = SetupTestKit.snapshot(new File(files3, "saves"));
            VerifiedExtractor.Outcome o3 = run(good, files3);
            expectEquals(saves3, SetupTestKit.snapshot(new File(files3, "saves")), "nothing appeared among the saves");
            expect(!o3.complete && !new File(root(files3), "bundle").exists(), "and nothing is imported");
        });

        check("import: refused, without reading the file, when an install already exists", () -> {
            for (int variant = 0; variant < 3; ++variant) {
                File files = prepared("import-existing-" + variant);
                if (variant == 0) SetupTestKit.write(new File(files, "private-trace/inputs/abc.bin"), bytes(50, 73));
                if (variant == 1) SetupTestKit.write(new File(files, "private-trace/private-trace-live.so"), bytes(80, 74));
                if (variant == 2) SetupTestKit.write(new File(root(files), "install.json"), new InstallGuard.Record(EXE_SHA, true, true).encode());
                TreeMap<String, String> before = SetupTestKit.snapshot(files);
                VerifiedExtractor.Outcome o = run(SetupTestKit.untouchable(), files);
                expectContains(o.error, "An install already exists and was not changed", "variant " + variant);
                expectEquals(VerifiedExtractor.Problem.EXISTING_INSTALL, o.problem, "kind (variant " + variant + ")");
                expectEquals(before, SetupTestKit.snapshot(files), "files/ is untouched (variant " + variant + ")");
            }
        });

        check("import: game code already in place is never replaced: recorded when it verifies, reported when it does not", () -> {
            File files = prepared("import-in-place");
            expect(run(good, files).complete, "first import");
            // the state after a crash between the rename and the record: the directory is there, the record is not
            expect(new File(root(files), "install.json").delete(), "remove the record");
            TreeMap<String, String> bundleBefore = SetupTestKit.snapshot(new File(root(files), "bundle"));
            VerifiedExtractor.Outcome again = run(SetupTestKit.untouchable(), files);
            expect(again.complete, "recorded without reading any file: " + again.error);
            expectEquals(bundleBefore, SetupTestKit.snapshot(new File(root(files), "bundle")), "the bundle directory is untouched");
            expect(InstallGuard.readRecord(root(files)).bundleComplete, "the record now says so");

            File damaged = prepared("import-in-place-damaged");
            expect(run(good, damaged).complete, "first import");
            expect(new File(root(damaged), "install.json").delete(), "remove the record");
            File shader = new File(root(damaged), "bundle/" + SHADER_NAME);
            expect(shader.setWritable(true, true), "make it writable for the test");
            byte[] changed = SHADERS.clone();
            changed[0] ^= 1;
            SetupTestKit.write(shader, changed);
            TreeMap<String, String> before = SetupTestKit.snapshot(damaged);
            VerifiedExtractor.Outcome o = run(good, damaged);
            expectContains(o.error, "already on this device but does not pass its check (" + SHADER_NAME + " has changed); it was not changed.", "damaged in place");
            expectEquals(VerifiedExtractor.Problem.EXISTING_INSTALL, o.problem, "the kind: what is in place is not replaced");
            expectEquals(before, SetupTestKit.snapshot(damaged), "nothing was changed");
        });

        check("import: refused until the player's own executable was copied from the disc", () -> {
            File files = SetupTestKit.newFilesDir("import-no-disc");
            SetupTestKit.write(new File(files, "saves/slot0"), bytes(64, 75));
            TreeMap<String, String> before = SetupTestKit.snapshot(files);
            VerifiedExtractor.Outcome o = run(SetupTestKit.untouchable(), files);
            expectContains(o.error, "Copy the game files from your play disc first", "reason");
            expectEquals(before, SetupTestKit.snapshot(files), "nothing written");
        });

        check("import: too little free space stops before anything is written; one byte more is enough", () -> {
            long need = LIBRARY.length + EXECUTABLE.length + IMAGE.length + SHADERS.length + VerifiedExtractor.RESERVE_BYTES;
            File files = prepared("import-space");
            TreeMap<String, String> before = SetupTestKit.snapshot(files);
            VerifiedExtractor.Outcome o = CodeBundleImporter.importBundle(new ByteArrayInputStream(good), root(files),
                    new SetupTestKit.Recorder(), SPEC, SetupTestKit.spaceOf(need - 1));
            expectContains(o.error, "Not enough space: needs 0.6 GB, free 0.5 GB", "reason");
            expect(o.problem == VerifiedExtractor.Problem.NO_SPACE && o.needBytes == need && o.freeBytes == need - 1,
                    "the outcome carries both amounts exactly: " + o.needBytes + ", " + o.freeBytes);
            expectEquals(before, SetupTestKit.snapshot(files), "not even a staging directory was created");
            VerifiedExtractor.Outcome o2 = CodeBundleImporter.importBundle(new ByteArrayInputStream(good), root(files),
                    new SetupTestKit.Recorder(), SPEC, SetupTestKit.spaceOf(need));
            expect(o2.complete, "exactly enough: " + o2.error);
        });

        check("import: stopping in the middle leaves no bundle and no staging copy", () -> {
            File files = prepared("import-cancel");
            TreeMap<String, String> before = SetupTestKit.snapshot(files);
            SetupTestKit.Recorder progress = new SetupTestKit.Recorder();
            progress.cancelAtBytes = 1;
            VerifiedExtractor.Outcome o = CodeBundleImporter.importBundle(new ByteArrayInputStream(good), root(files), progress, SPEC,
                    SetupTestKit.PLENTY);
            expect(o.cancelled && !o.complete && o.error == null, "stopped");
            expectEquals(before, snapshotWithoutEmptyStaging(files), "nothing left behind");
        });

        check("import: a file that is no bundle at all (a picture, a disc image) is said to be none, not a storage failure", () -> {
            expectRefused("random bytes", bytes(5000, 76), NOT_ONE, "This file is not a usable code bundle: it does not begin like one.");
        });

        check("import: a file that stops being readable half-way is told apart from a damaged bundle, and leaves nothing", () -> {
            File files = prepared("import-read-fails");
            TreeMap<String, String> before = SetupTestKit.snapshot(files);
            final int stopAt = bundle().build().length / 2;
            InputStream failing = new InputStream() {
                private int at;
                @Override public int read() throws IOException { throw new IOException("card removed"); }
                @Override public int read(byte[] b, int off, int len) throws IOException {
                    if (at >= stopAt) throw new IOException("card removed");
                    int n = Math.min(len, stopAt - at);
                    System.arraycopy(good, at, b, off, n);
                    at += n;
                    return n;
                }
            };
            VerifiedExtractor.Outcome o = run(failing, files);
            expectEquals("The bundle file could not be read (card removed).", o.error, "the sentence");
            expectEquals(VerifiedExtractor.Problem.SOURCE_UNREADABLE, o.problem, "the kind: it may work when chosen again");
            expectEquals(before, snapshotWithoutEmptyStaging(files), "nothing left behind");
        });

        check("import: what an interrupted run left in staging counts as free space and is removed before the import", () -> {
            File files = prepared("import-leftovers");
            long need = LIBRARY.length + EXECUTABLE.length + IMAGE.length + SHADERS.length + VerifiedExtractor.RESERVE_BYTES;
            SetupTestKit.write(new File(root(files), "staging/bundle.part/" + LIB), bytes(400000, 77));       // a killed import
            SetupTestKit.write(new File(root(files), "staging/" + SetupTestKit.sha256(bytes(3, 78)) + ".part"), bytes(60000, 79));   // a killed copy
            long leftovers = 460000, used = SetupTestKit.regularBytes(root(files)), free = need - leftovers;   // free: what the storage reports now
            TreeMap<String, String> before = SetupTestKit.snapshot(files);
            VerifiedExtractor.Outcome refused = CodeBundleImporter.importBundle(new ByteArrayInputStream(good), root(files),
                    new SetupTestKit.Recorder(), SPEC, SetupTestKit.quota(root(files), used + free - 1));
            expect(refused.problem == VerifiedExtractor.Problem.NO_SPACE && refused.freeBytes == need - 1, "one byte short with them: refused: " + refused.error);
            expectEquals(before, SetupTestKit.snapshot(files), "nothing was changed by the refusal");
            VerifiedExtractor.Outcome o = CodeBundleImporter.importBundle(new ByteArrayInputStream(good), root(files),
                    new SetupTestKit.Recorder(), SPEC, SetupTestKit.quota(root(files), used + free));
            expect(o.complete, "exactly enough with them (not without): " + o.error);
            expectImported(files, bundle().components);
            expectEquals(0, new File(root(files), "staging").list().length, "both leftovers are gone");
        });

        check("import: a write that fails is reported as a space problem only when the device is short of space", () -> {
            // The host cannot fill a disk. The failure is provoked by taking the write permission from the staging copy
            // when the shader pack is about to be written; what the storage then reports decides the kind.
            for (int variant = 0; variant < 2; ++variant) {
                final File files = prepared("import-write-fails-" + variant);
                final long total = LIBRARY.length + EXECUTABLE.length + IMAGE.length + SHADERS.length;
                final SetupTestKit.Budget space = new SetupTestKit.Budget(total + VerifiedExtractor.RESERVE_BYTES);
                final boolean full = variant == 0;
                final File part = new File(root(files), "staging/bundle.part");
                SetupTestKit.Recorder progress = new SetupTestKit.Recorder();
                progress.atPath = SHADER_NAME;
                progress.atPathDo = () -> {
                    expect(part.setWritable(false, false), "take the write permission");
                    if (full) space.free = 1000;
                };
                VerifiedExtractor.Outcome o;
                try {
                    o = CodeBundleImporter.importBundle(new ByteArrayInputStream(good), root(files), progress, SPEC, space);
                } finally {
                    part.setWritable(true, true);
                }
                expect(!o.complete && !o.cancelled && !new File(root(files), "bundle").exists(), "variant " + variant + ": nothing imported");
                if (full) {
                    expectEquals(VerifiedExtractor.Problem.NO_SPACE, o.problem, "short of space: the kind");
                    expectContains(o.error, "This device ran out of space during the import: needs ", "short of space: the sentence");
                    expect(o.needBytes == total + VerifiedExtractor.RESERVE_BYTES && o.freeBytes == 1000, "short of space: the whole bundle plus the reserve, and what is free");
                } else {
                    expectEquals(VerifiedExtractor.Problem.STORAGE, o.problem, "room enough: the kind");
                    expectContains(o.error, "Could not import the game code: ", "room enough: the sentence");
                }
            }
        });

        String out = System.getProperty("setup.kitbundle.out");
        if (out != null) {
            SetupTestKit.write(new File(out), good);
            SetupTestKit.write(new File(out + ".sha256"), EXE_SHA.getBytes(StandardCharsets.US_ASCII));
            SetupTestKit.note("this test's own good bundle was written for the builder's verify_bundle");
        }
        SetupTestKit.finish("CodeBundleImporterTest");
    }
}
