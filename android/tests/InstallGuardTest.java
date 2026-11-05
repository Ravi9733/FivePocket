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
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

/** Run with java -ea. Every case works in its own temporary files/ directory. */
public final class InstallGuardTest {
    private static final byte[] EXECUTABLE = bytes(70000, 81), ARCHIVE = bytes(200000, 82), LIBRARY = bytes(300000, 83);
    private static final List<SetupCatalog.Item> ITEMS = Arrays.asList(item("default.xex", EXECUTABLE), item("common.rpf", ARCHIVE));
    private static final SetupCatalog.Spec SPEC = SetupTestKit.spec(EXECUTABLE, ITEMS, true);
    private static final String SHA = SetupTestKit.sha256(EXECUTABLE);

    private static String refusal(File root, String relative) {
        try {
            InstallGuard.under(root, relative);
            return "allowed";
        } catch (IOException e) {
            return "refused: " + e.getMessage();
        }
    }

    public static void main(String[] args) {
        SetupTestKit.requireAssertions();

        check("guard: a setup path lies under files/game-install and nothing is created by asking", () -> {
            File files = SetupTestKit.newFilesDir("guard-ok");
            File f = InstallGuard.resolve(files, "inputs/abc.bin");
            expectEquals(new File(files, "game-install/inputs/abc.bin"), f, "the path");
            expectEquals(new File(files, "game-install"), InstallGuard.root(files), "the root");
            expectEquals(0, files.list().length, "nothing was created");
            expect(InstallGuard.rootProblem(InstallGuard.root(files)) == null, "the root is acceptable before it exists");
        });

        check("guard: paths that leave the directory or name saves / private-trace are refused", () -> {
            File files = SetupTestKit.newFilesDir("guard-paths");
            File root = InstallGuard.root(files);
            String[] bad = {"", "..", "../saves/slot0", "inputs/../../saves/slot0", "/etc/passwd", "inputs//x", "inputs/", "./x",
                    "saves/slot0", "inputs/saves/x", "private-trace/diag-fps-overlay", "x/private-trace", "SAVES/slot0",
                    "Private-Trace/x", "inputs\\x", "a\0b"};
            for (String relative : bad) expectContains(refusal(root, relative), "refused", "path <" + relative + ">");
            expectEquals("refused: path not allowed for setup files", refusal(root, null), "null");
            expectEquals("allowed", refusal(root, "inputs/saves.bin"), "a name that merely contains the word is fine");
        });

        check("guard: a link inside the setup directory that points at the saves is not followed", () -> {
            File files = SetupTestKit.newFilesDir("guard-link");
            File root = InstallGuard.root(files);
            SetupTestKit.write(new File(files, "saves/slot0"), bytes(64, 84));
            SetupTestKit.write(new File(files, "elsewhere/x"), bytes(4, 85));
            expect(root.mkdir(), "mkdir");
            Files.createSymbolicLink(new File(root, "staging").toPath(), new File(files, "saves").toPath());
            Files.createSymbolicLink(new File(root, "inputs").toPath(), new File(files, "elsewhere").toPath());
            expectContains(refusal(root, "staging/new.part"), "refused: path leaves the setup directory", "link to saves");
            expectContains(refusal(root, "inputs/new.bin"), "refused: path leaves the setup directory", "link to another place");
        });

        check("guard: only a real directory named game-install, outside saves and private-trace, is a setup directory", () -> {
            File files = SetupTestKit.newFilesDir("guard-root");
            SetupTestKit.write(new File(files, "saves/slot0"), bytes(64, 86));
            expectContains(InstallGuard.rootProblem(new File(files, "saves")), "must be named game-install", "another name");
            expectContains(InstallGuard.rootProblem(new File(files, "saves/game-install")), "lies inside saves", "inside saves");
            expectContains(InstallGuard.rootProblem(new File(files, "private-trace/game-install")), "lies inside private-trace", "inside private-trace");
            File linked = new File(files, "game-install");
            Files.createSymbolicLink(linked.toPath(), new File(files, "saves").toPath());
            expectContains(InstallGuard.rootProblem(linked), "is a link to another place", "a link named game-install");
            expectContains(refusal(linked, "inputs/x.bin"), "refused", "and no path is built under it");
            File files2 = SetupTestKit.newFilesDir("guard-root-file");
            SetupTestKit.write(new File(files2, "game-install"), bytes(4, 87));
            expectContains(InstallGuard.rootProblem(new File(files2, "game-install")), "is not a directory", "a file of that name");
            expect(InstallGuard.rootProblem(null) != null, "null");
        });

        check("guard: copying and importing refuse a directory that is not files/game-install", () -> {
            File files = SetupTestKit.newFilesDir("guard-wrong-root");
            SetupTestKit.write(new File(files, "saves/slot0"), bytes(64, 88));
            TreeMap<String, String> before = SetupTestKit.snapshot(files);
            SetupTestKit.MemoryFolder disc = new SetupTestKit.MemoryFolder().put("default.xex", EXECUTABLE).put("common.rpf", ARCHIVE);
            VerifiedExtractor.Outcome o = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(disc), new File(files, "saves"),
                    new SetupTestKit.Recorder(), SPEC, SetupTestKit.PLENTY);
            expectContains(o.error, "must be named game-install", "extract into saves");
            VerifiedExtractor.Outcome i = CodeBundleImporter.importBundle(SetupTestKit.untouchable(), new File(files, "saves"),
                    new SetupTestKit.Recorder(), SPEC, SetupTestKit.PLENTY);
            expectContains(i.error, "must be named game-install", "import into saves");
            expectEquals(before, SetupTestKit.snapshot(files), "nothing changed");
            expect(disc.opened.isEmpty(), "nothing was read");
        });

        check("guard: what counts as an existing install", () -> {
            File files = SetupTestKit.newFilesDir("guard-existing");
            expect(InstallGuard.existingInstall(files) == null, "an empty files/ has none");
            SetupTestKit.write(new File(files, "private-trace/diag-fps-overlay"), new byte[0]);
            SetupTestKit.write(new File(files, "private-trace/run-0123/arguments.bin"), bytes(10, 89));
            SetupTestKit.write(new File(files, "saves/slot0"), bytes(64, 90));
            expect(new File(files, "private-trace/inputs").mkdir(), "mkdir inputs");
            expect(InstallGuard.existingInstall(files) == null, "switch files, run folders, saves and an empty inputs folder are not an install");
            SetupTestKit.write(new File(files, "private-trace/inputs/abc.bin"), bytes(5, 91));
            expectContains(InstallGuard.existingInstall(files), "files/private-trace/inputs", "developer game data");

            InstallGuard.Existing dataOnly = InstallGuard.existing(files);
            expect(dataOnly.any() && dataOnly.computerData && !dataOnly.computerCode && !dataOnly.recordUnreadable && !dataOnly.recordComplete,
                    "the structured answer: data from a computer, nothing else");
            SetupTestKit.write(new File(files, "private-trace/private-trace-live.so"), bytes(5, 97));
            InstallGuard.Existing both = InstallGuard.existing(files);
            expect(both.computerData && both.computerCode, "data and code are both reported, not only the first one found");

            File code = SetupTestKit.newFilesDir("guard-existing-code");
            expect(!InstallGuard.existing(code).any(), "an empty files/ closes nothing");
            SetupTestKit.write(new File(code, "private-trace/private-trace-live.so"), bytes(5, 92));
            expectContains(InstallGuard.existingInstall(code), "private-trace-live.so", "developer game code");
            InstallGuard.Existing codeOnly = InstallGuard.existing(code);
            expect(codeOnly.any() && codeOnly.computerCode && !codeOnly.computerData, "the structured answer: closed by code from a computer, no data");

            File recorded = SetupTestKit.newFilesDir("guard-existing-record");
            File record = new File(recorded, "game-install/install.json");
            SetupTestKit.write(record, new InstallGuard.Record(SHA, true, false).encode());
            expect(InstallGuard.existingInstall(recorded) == null, "an unfinished install may be continued");
            expect(!InstallGuard.existing(recorded).any(), "the structured answer agrees");
            SetupTestKit.write(record, new InstallGuard.Record(SHA, true, true).encode());
            expectContains(InstallGuard.existingInstall(recorded), "complete install", "a complete record");
            InstallGuard.Existing complete = InstallGuard.existing(recorded);
            expect(complete.any() && complete.recordComplete && !complete.recordUnreadable && !complete.computerData && !complete.computerCode,
                    "the structured answer: a complete record, readable, nothing from a computer");
            String[] unreadable = {"", "not json", "[]", "{\"version\":2,\"executableSha256\":\"" + SHA
                    + "\",\"discFilesComplete\":false,\"bundleComplete\":false,\"complete\":false}",
                    "{\"version\":1,\"executableSha256\":\"" + SHA + "\",\"discFilesComplete\":true,\"bundleComplete\":false,\"complete\":true}",
                    "{\"version\":1,\"executableSha256\":\"xyz\",\"discFilesComplete\":false,\"bundleComplete\":false,\"complete\":false}",
                    "{\"version\":1,\"executableSha256\":\"" + SHA + "\",\"discFilesComplete\":\"yes\",\"bundleComplete\":false,\"complete\":false}"};
            for (String text : unreadable) {
                SetupTestKit.write(record, text.getBytes(StandardCharsets.UTF_8));
                expectContains(InstallGuard.existingInstall(recorded), "cannot be read", "a record that cannot be trusted: " + text);
                InstallGuard.Existing found = InstallGuard.existing(recorded);
                expect(found.any() && found.recordUnreadable && !found.recordComplete, "the structured answer: closed, unreadable, not complete: " + text);
            }
        });

        check("guard: install.json has exactly the agreed content and is derived from the files", () -> {
            expectEquals("{\"version\":1,\"executableSha256\":\"" + SHA + "\",\"discFilesComplete\":true,\"bundleComplete\":false,\"complete\":false}\n",
                    new String(new InstallGuard.Record(SHA, true, false).encode(), StandardCharsets.UTF_8), "the agreed text");
            File files = SetupTestKit.newFilesDir("guard-record");
            File root = InstallGuard.root(files);
            SetupTestKit.write(SetupTestKit.input(root, ITEMS.get(0)), EXECUTABLE);
            InstallGuard.Record r1 = InstallGuard.refreshRecord(root, SPEC);
            expect(!r1.discFilesComplete && !r1.bundleComplete && !r1.complete, "one of two files");
            SetupTestKit.write(SetupTestKit.input(root, ITEMS.get(1)), ARCHIVE);
            InstallGuard.Record r2 = InstallGuard.refreshRecord(root, SPEC);
            expect(r2.discFilesComplete && !r2.bundleComplete && !r2.complete, "both files");
            InstallGuard.Record partial = InstallGuard.refreshRecord(root, SetupTestKit.spec(EXECUTABLE, ITEMS, false));
            expect(!partial.discFilesComplete, "a catalog that does not cover every disc never gives 'disc files complete'");
            InstallGuard.refreshRecord(root, SPEC);
            SetupTestKit.write(new File(root, "bundle/manifest.json"), bytes(10, 93));
            InstallGuard.Record r3 = InstallGuard.refreshRecord(root, SPEC);
            expect(r3.discFilesComplete && r3.bundleComplete && r3.complete, "with the bundle");
            InstallGuard.Record read = InstallGuard.readRecord(root);
            expect(read != null && read.complete && read.executableSha256.equals(SHA), "what was written can be read");
            TreeMap<String, String> before = SetupTestKit.snapshot(files);
            InstallGuard.refreshRecord(root, SPEC);
            expectEquals(before, SetupTestKit.snapshot(files), "an unchanged record is not written again");
            expect(!new File(root, "install.json.tmp").exists(), "no temporary file is left");
        });

        check("guard: a whole copy and import touch nothing under saves or private-trace and create nothing outside game-install", () -> {
            File files = SetupTestKit.newFilesDir("guard-full");
            SetupTestKit.write(new File(files, "saves/slot0"), bytes(64, 94));
            SetupTestKit.write(new File(files, "saves/profile/options.bin"), bytes(640, 95));
            SetupTestKit.write(new File(files, "private-trace/diag-cexec-bools"), new byte[0]);
            SetupTestKit.write(new File(files, "private-trace/diag-v450-scale-fsr1"), new byte[0]);
            SetupTestKit.write(new File(files, "private-trace/run-0123/arguments.bin"), bytes(100, 96));
            SetupTestKit.write(new File(files, "settings/settings.json"), "{\"version\":1}\n".getBytes(StandardCharsets.UTF_8));
            TreeMap<String, String> before = SetupTestKit.snapshot(files);

            File root = InstallGuard.root(files);
            SetupTestKit.MemoryFolder disc = new SetupTestKit.MemoryFolder().put("default.xex", EXECUTABLE).put("common.rpf", ARCHIVE);
            VerifiedExtractor.Outcome copied = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(disc), root,
                    new SetupTestKit.Recorder(), SPEC, SetupTestKit.PLENTY);
            expect(copied.complete, "copy: " + copied.error);
            byte[] zip = new SetupTestKit.Bundle(SHA).add("lib/arm64-v8a/libgame.so", "game-library", LIBRARY)
                    .add("title.xex", "title-xex", EXECUTABLE).build();
            VerifiedExtractor.Outcome imported = CodeBundleImporter.importBundle(new ByteArrayInputStream(zip), root,
                    new SetupTestKit.Recorder(), SPEC, SetupTestKit.PLENTY);
            expect(imported.complete, "import: " + imported.error);
            expect(InstallGuard.readRecord(root).complete, "the install is recorded as complete");

            TreeMap<String, String> after = SetupTestKit.snapshot(files);
            TreeMap<String, String> outside = new TreeMap<String, String>();
            int inside = 0;
            for (Map.Entry<String, String> e : after.entrySet()) {
                if (e.getKey().equals("game-install") || e.getKey().startsWith("game-install/")) ++inside;
                else outside.put(e.getKey(), e.getValue());
            }
            expectEquals(before, outside, "everything outside game-install is exactly as before");
            expect(inside > 5, "and the install is inside it");

            // and from now on the install is closed: a second copy or import changes nothing at all
            VerifiedExtractor.Outcome again = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(disc), root,
                    new SetupTestKit.Recorder(), SPEC, SetupTestKit.PLENTY);
            VerifiedExtractor.Outcome againImport = CodeBundleImporter.importBundle(SetupTestKit.untouchable(), root,
                    new SetupTestKit.Recorder(), SPEC, SetupTestKit.PLENTY);
            expectContains(again.error, "An install already exists and was not changed", "second copy");
            expectContains(againImport.error, "An install already exists and was not changed", "second import");
            expect(again.problem == VerifiedExtractor.Problem.EXISTING_INSTALL && againImport.problem == VerifiedExtractor.Problem.EXISTING_INSTALL,
                    "both with the kind the page turns into 'nothing can be chosen here'");
            expectEquals(after, SetupTestKit.snapshot(files), "nothing changed");
        });

        SetupTestKit.finish("InstallGuardTest");
    }
}
