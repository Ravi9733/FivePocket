package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.LauncherTestSupport.ID_A;
import static dev.recompbootstrap.smoke.LauncherTestSupport.ID_B;
import static dev.recompbootstrap.smoke.LauncherTestSupport.ID_C;
import static dev.recompbootstrap.smoke.LauncherTestSupport.ID_D;
import static dev.recompbootstrap.smoke.LauncherTestSupport.addCode;
import static dev.recompbootstrap.smoke.LauncherTestSupport.addDecoys;
import static dev.recompbootstrap.smoke.LauncherTestSupport.addDirectory;
import static dev.recompbootstrap.smoke.LauncherTestSupport.addInput;
import static dev.recompbootstrap.smoke.LauncherTestSupport.addRun;
import static dev.recompbootstrap.smoke.LauncherTestSupport.bytes;
import static dev.recompbootstrap.smoke.LauncherTestSupport.check;
import static dev.recompbootstrap.smoke.LauncherTestSupport.difference;
import static dev.recompbootstrap.smoke.LauncherTestSupport.discard;
import static dev.recompbootstrap.smoke.LauncherTestSupport.equal;
import static dev.recompbootstrap.smoke.LauncherTestSupport.join;
import static dev.recompbootstrap.smoke.LauncherTestSupport.mode;
import static dev.recompbootstrap.smoke.LauncherTestSupport.newFilesDir;
import static dev.recompbootstrap.smoke.LauncherTestSupport.privateTrace;
import static dev.recompbootstrap.smoke.LauncherTestSupport.run;
import static dev.recompbootstrap.smoke.LauncherTestSupport.sha256;
import static dev.recompbootstrap.smoke.LauncherTestSupport.snapshot;
import static dev.recompbootstrap.smoke.LauncherTestSupport.validArguments;
import static dev.recompbootstrap.smoke.LauncherTestSupport.write;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.Random;
import java.util.TreeMap;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Host test of LaunchRequest: rewriting the start arguments, the hash, the game's argument rule, and making a new
 * start request in a temporary directory tree (what is created, what is cleaned up, what is never touched).
 * Run with java -ea.
 *
 *   java ... LaunchRequestTest [recorded-run-directory]
 *       recorded-run-directory: optional; one host-side record of a real start (arguments.bin and request.json).
 *       Only counts are printed from it.
 *   java ... LaunchRequestTest --emit-cases directory
 *       writes argument-rule cases and this class's verdicts for the comparison with the game's own C++ rule.
 */
public final class LaunchRequestTest {
    private static final long T0 = 1_700_000_000_000L;
    /** The device's private-trace directory, as in the game's own C++ test of the rule. */
    private static final String DEVICE_ROOT = "/data/user/0/dev.recompbootstrap.smoke/files/private-trace";

    public static void main(String[] args) throws Exception {
        LauncherTestSupport.requireAssertions();
        if (args.length == 2 && args[0].equals("--emit-cases")) {
            emitCases(new File(args[1]));
            return;
        }
        final File recorded = args.length == 1 ? new File(args[0]) : null;

        run("LaunchRequest.rewriteReplacesEveryOccurrenceAndNothingElse", () -> {
            // The id appears in the first two paths, inside a longer value, and twice back to back.
            String[] template = {"/r/run-" + ID_A + "/host_guest_trace", "/r/run-" + ID_A + "/decoded.image.bin",
                    "keep", "x" + ID_A + "y", ID_A + ID_A, "aaaa", "tail"};
            String[] expected = {"/r/run-" + ID_B + "/host_guest_trace", "/r/run-" + ID_B + "/decoded.image.bin",
                    "keep", "x" + ID_B + "y", ID_B + ID_B, "aaaa", "tail"};
            byte[] input = join(template);
            byte[] kept = input.clone();
            byte[] output = LaunchRequest.rewriteArguments(input, ID_A, ID_B);
            check(Arrays.equals(join(expected), output), "every occurrence replaced, the rest byte for byte");
            equal(input.length, output.length, "length");
            check(Arrays.equals(kept, input), "the template array itself is not changed");
        });

        run("LaunchRequest.rewriteRefusesWhatItCannotDo", () -> {
            byte[] template = join("/r/run-" + ID_A + "/host_guest_trace", "value");
            refusedRewrite(template, ID_C, ID_B, "does not name its own run id");
            refusedRewrite(template, ID_A, ID_B.substring(1), "32 lower-case hex digits");
            refusedRewrite(template, ID_A.substring(1), ID_B, "32 lower-case hex digits");
            refusedRewrite(template, ID_A, ID_B.toUpperCase(), "32 lower-case hex digits");
            refusedRewrite(template, ID_A, ID_A, "equals the earlier one");
            refusedRewrite(new byte[0], ID_A, ID_B, "empty");
            // Replacing can bring the old id back: old = 0101...01, new = 2010...10 followed by a 1 in the template.
            String old = "01010101010101010101010101010101", fresh = "20101010101010101010101010101010";
            refusedRewrite((old + "1").getBytes(StandardCharsets.US_ASCII), old, fresh, "still present");
        });

        run("LaunchRequest.sha256MatchesKnownDigests", () -> {
            // FIPS 180 examples.
            equal("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", sha("abc".getBytes("US-ASCII")),
                    "abc");
            equal("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", sha(new byte[0]), "empty");
            byte[] million = new byte[1_000_000];
            Arrays.fill(million, (byte) 'a');
            equal("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", sha(million), "one million a");
            // A stream that hands out a few bytes at a time must give the same digest.
            InputStream trickle = new ByteArrayInputStream(million) {
                @Override public synchronized int read(byte[] buffer, int offset, int length) {
                    return super.read(buffer, offset, Math.min(length, 7));
                }
            };
            equal("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", LaunchRequest.sha256Hex(trickle),
                    "one million a, 7 bytes per read");
        });

        run("LaunchRequest.newIdHasTheFormTheGameAccepts", () -> {
            String first = LaunchRequest.newId(), second = LaunchRequest.newId();
            check(first.matches("[0-9a-f]{32}"), "32 lower-case hex digits: " + first);
            check(!first.equals(second), "two ids differ");
            check(!LaunchRequest.isId(first.toUpperCase().replace('0', 'A')) && !LaunchRequest.isId(first + "0")
                    && !LaunchRequest.isId(first.substring(1)) && !LaunchRequest.isId(null), "isId refuses other forms");
        });

        run("LaunchRequest.referencedInputsAreExactlyTheInputsAndModulesPaths", () -> {
            String files = "/data/user/0/app/files";
            String a = files + "/private-trace/inputs/a.bin", cache = files + "/private-trace/inputs/native-cache";
            String boot = files + "/private-trace/modules/boot";
            byte[] arguments = join(files + "/private-trace/run-" + ID_A + "/host_guest_trace",
                    files + "/private-trace/run-" + ID_A + "/decoded.image.bin", "--read-only-file", a,
                    "--native-cache-directory", cache, "--color-program", boot,
                    "--read-only-file", a,                                  // named twice: listed once
                    "note=" + a,                                            // the path is not at the start
                    files + "/private-trace/inputs-other/x.bin",            // a sibling directory
                    files + "/private-trace/private-trace-live.so",         // not under inputs or modules
                    "/data/user/0/other/files/private-trace/inputs/b.bin",  // another files directory
                    files + "/saves/slot0");
            equal(Arrays.asList(a, cache, boot), LaunchRequest.referencedInputs(arguments, files), "named paths");
        });

        run("LaunchRequest.checkArgumentsAcceptsWhatTheGameAccepts", () -> {
            String[] valid = validArguments(DEVICE_ROOT, ID_A, "--read-only-file", "151", DEVICE_ROOT + "/inputs/abc.bin",
                    "--color-program", DEVICE_ROOT + "/modules/boot");
            equal(null, LaunchRequest.checkArguments(join(valid), DEVICE_ROOT, ID_A), "a valid request");
            equal(null, LaunchRequest.checkArguments(join(validArguments(DEVICE_ROOT, ID_A)), DEVICE_ROOT, ID_A),
                    "exactly eleven arguments");
        });

        run("LaunchRequest.checkArgumentsNamesEachRefusal", () -> {
            String[] valid = validArguments(DEVICE_ROOT, ID_A, "--read-only-file", DEVICE_ROOT + "/inputs/abc.bin");
            byte[] blob = join(valid);
            refusedArguments(new byte[0], DEVICE_ROOT, ID_A, "is empty");
            refusedArguments(Arrays.copyOf(blob, blob.length - 1), DEVICE_ROOT, ID_A, "does not end with a zero byte");
            refusedArguments(Arrays.copyOf(blob, blob.length + 1), DEVICE_ROOT, ID_A, "an argument is empty");
            refusedArguments(new byte[65537], DEVICE_ROOT, ID_A, "longer than 65536 bytes");
            refusedArguments(blob, "relative/root", ID_A, "not a plain absolute path");
            refusedArguments(blob, DEVICE_ROOT + "/", ID_A, "not a plain absolute path");
            refusedArguments(blob, DEVICE_ROOT, ID_A.substring(1), "run id is not");
            // Under another run id every path of this request lies outside "this run".
            refusedArguments(blob, DEVICE_ROOT, ID_B, "outside this run");
            refusedArguments(join(with(valid, 0, DEVICE_ROOT + "/run-" + ID_A + "/other_program")), DEVICE_ROOT, ID_A,
                    "first argument is not this run's program path");
            refusedArguments(join(with(valid, 1, DEVICE_ROOT + "/run-" + ID_A + "/other.bin")), DEVICE_ROOT, ID_A,
                    "second argument is not this run's game image");
            refusedArguments(join(Arrays.copyOf(valid, 10)), DEVICE_ROOT, ID_A, "fewer than 11 arguments");
            refusedArguments(join(with(valid, 5, "tab\there")), DEVICE_ROOT, ID_A, "not printable ASCII");
            refusedArguments(join(with(valid, 5, "café")), DEVICE_ROOT, ID_A, "not printable ASCII");
            refusedArguments(join(with(valid, 5, "inputs/abc.bin")), DEVICE_ROOT, ID_A, "relative path");
            refusedArguments(join(with(valid, 5, "inputs\\abc.bin")), DEVICE_ROOT, ID_A, "relative path");
            for (String outside : new String[] {"/etc/passwd", "/data/user/0/dev.recompbootstrap.smoke/files/saves/slot0",
                    DEVICE_ROOT + "/private-trace-live.so", DEVICE_ROOT + "/inputs2/abc.bin",
                    DEVICE_ROOT + "/inputs/../../saves/slot0", DEVICE_ROOT + "/inputs//abc.bin", DEVICE_ROOT + "/inputs/",
                    DEVICE_ROOT + "/inputs/a b", DEVICE_ROOT + "/run-" + ID_B + "/decoded.image.bin"}) {
                refusedArguments(join(with(valid, 12, outside)), DEVICE_ROOT, ID_A, "outside this run");
            }
            // The limits: 4096 bytes in one argument and 1024 arguments are accepted, one more is not.
            equal(null, LaunchRequest.checkArguments(join(with(valid, 5, repeat('x', 4096))), DEVICE_ROOT, ID_A),
                    "an argument of 4096 bytes");
            refusedArguments(join(with(valid, 5, repeat('x', 4097))), DEVICE_ROOT, ID_A, "longer than 4096 bytes");
            equal(null, LaunchRequest.checkArguments(join(padded(valid, 1024)), DEVICE_ROOT, ID_A), "1024 arguments");
            refusedArguments(join(padded(valid, 1025)), DEVICE_ROOT, ID_A, "more than 1024 arguments");
        });

        run("LaunchRequest.recordedRealRequestIsAccepted", () -> {
            if (recorded == null || !new File(recorded, "arguments.bin").isFile()
                    || !new File(recorded, "request.json").isFile()) {
                System.out.println("     NOTE no recorded run directory given or found: this check did not run");
                return;
            }
            byte[] arguments = Files.readAllBytes(new File(recorded, "arguments.bin").toPath());
            String request = new String(Files.readAllBytes(new File(recorded, "request.json").toPath()), "UTF-8");
            Matcher id = Pattern.compile("\"request_id\"\\s*:\\s*\"([0-9a-f]{32})\"").matcher(request);
            Matcher manifest = Pattern.compile("\"manifest_sha256\"\\s*:\\s*\"([0-9a-f]{64})\"").matcher(request);
            check(id.find() && manifest.find(), "request.json holds an id and an arguments hash");
            equal(manifest.group(1), sha(arguments), "the arguments file is the one the record describes");
            equal(null, LaunchRequest.checkArguments(arguments, DEVICE_ROOT, id.group(1)), "the real request is accepted");

            String filesDir = DEVICE_ROOT.substring(0, DEVICE_ROOT.length() - "/private-trace".length());
            List<String> named = LaunchRequest.referencedInputs(arguments, filesDir);
            int absolute = 0, underRun = 0;
            for (String argument : new String(arguments, StandardCharsets.ISO_8859_1).split("\0")) {
                if (argument.startsWith("/")) {
                    ++absolute;
                    if (argument.startsWith(DEVICE_ROOT + "/run-" + id.group(1) + "/")) {
                        ++underRun;
                    }
                }
            }
            // The game allows absolute paths only under the run, inputs and modules directories, so every absolute
            // path that is not under the run directory must be in the list (the list holds each path once).
            check(!named.isEmpty(), "the real request names game files");
            check(named.size() <= absolute - underRun, "no path is listed that the request does not hold");
            for (String path : named) {
                check(path.startsWith(DEVICE_ROOT + "/inputs/") || path.startsWith(DEVICE_ROOT + "/modules/"),
                        "a listed path is under inputs or modules");
            }
            byte[] rewritten = LaunchRequest.rewriteArguments(arguments, id.group(1), ID_D);
            equal(null, LaunchRequest.checkArguments(rewritten, DEVICE_ROOT, ID_D), "accepted again under a new id");
            equal(named, LaunchRequest.referencedInputs(rewritten, filesDir), "the rewrite leaves the named files alone");
            System.out.println("     recorded request: " + arguments.length + " bytes, " + absolute
                    + " absolute paths, " + underRun + " under the run directory, " + named.size()
                    + " distinct paths under inputs/ and modules/");
        });

        run("LaunchRequest.prepareMakesWhatTheGameActivityRequires", () -> {
            File files = readyTree();
            File root = privateTrace(files);
            // No settings directory yet, as on a device where the settings screen was never used.
            check(new File(files, "settings/settings.json").delete() && new File(files, "settings").delete(), "clear");
            Map<String, String> before = snapshot(files, false);
            byte[] templateArguments = Files.readAllBytes(new File(root, "run-" + ID_A + "/arguments.bin").toPath());
            byte[] templateImage = Files.readAllBytes(new File(root, "run-" + ID_A + "/decoded.image.bin").toPath());
            byte[] code = Files.readAllBytes(new File(root, "private-trace-live.so").toPath());

            equal(null, LaunchRequest.newestLauncherRun(files), "the launcher has made no run yet");
            LaunchRequest.Prepared prepared = LaunchRequest.prepare(files);
            equal(null, prepared.error, "no error");
            check(prepared.requestId.matches("[0-9a-f]{32}") && !prepared.requestId.equals(ID_A), "a new id");
            equal(prepared.requestId, LaunchRequest.newestLauncherRun(files), "the run the launcher made last");
            File run = new File(root, "run-" + prepared.requestId);
            File arguments = new File(run, "arguments.bin"), image = new File(run, "decoded.image.bin");

            // What the game activity checks before it reads the arguments: directory 0700, file 0400, one link.
            equal("rwx------", mode(run), "run directory mode");
            equal("r--------", mode(arguments), "arguments mode");
            equal(1, Files.getAttribute(arguments.toPath(), "unix:nlink"), "arguments link count");
            equal("r--------", mode(image), "image mode");
            byte[] written = Files.readAllBytes(arguments.toPath());
            byte[] expected = new String(templateArguments, StandardCharsets.ISO_8859_1)
                    .replace(ID_A, prepared.requestId).getBytes(StandardCharsets.ISO_8859_1);
            check(Arrays.equals(expected, written), "arguments are the template's with the new id");
            equal(null, LaunchRequest.checkArguments(written, root.getAbsolutePath(), prepared.requestId),
                    "the game's rule accepts them");
            check(Arrays.equals(templateImage, Files.readAllBytes(image.toPath())), "the image is a full copy");
            equal(sha256(written), prepared.manifestSha256, "manifest_sha256 is the hash of the written arguments");
            equal(sha256(code), prepared.expectedSha256, "expected_sha256 is the hash of the game code file");
            equal(prepared.requestId + "\n", text(new File(files, "settings/launcher-runs.txt")), "the launcher's list");
            check(prepared.imagesRemoved.isEmpty(), "nothing to clean up on the first start");

            // Nothing that was there before changed; exactly five entries are new.
            Map<String, String> after = snapshot(files, false);
            Map<String, String> added = new TreeMap<String, String>(after);
            for (Map.Entry<String, String> entry : before.entrySet()) {
                equal(entry.getValue(), after.get(entry.getKey()), "unchanged: " + entry.getKey());
                added.remove(entry.getKey());
            }
            String name = "private-trace/run-" + prepared.requestId;
            equal(Arrays.asList(name, name + "/arguments.bin", name + "/decoded.image.bin", "settings",
                    "settings/launcher-runs.txt"), new ArrayList<String>(added.keySet()), "new entries");
            // And the launcher itself sees the new request as a usable one.
            check(new File(root, "run-" + ID_A + "/arguments.bin").setLastModified(T0), "keep the template older");
            equal(prepared.requestId, InstallState.detect(files).templateRunId, "the new request is the newest template");
            discard(files);
        });

        run("LaunchRequest.extrasAreTheFourTheScriptsPass", () -> {
            File files = readyTree();
            LaunchRequest.Prepared prepared = LaunchRequest.prepare(files);
            equal(null, prepared.error, "no error");
            String[][] extras = prepared.extras();
            equal(4, extras.length, "four extras");
            Map<String, String> byName = new TreeMap<String, String>();
            for (String[] extra : extras) {
                equal(2, extra.length, "name and value");
                byName.put(extra[0], extra[1]);
            }
            equal("run-native-display", byName.get("private_trace"), "private_trace");
            equal(prepared.requestId, byName.get("request_id"), "request_id");
            equal(prepared.expectedSha256, byName.get("expected_sha256"), "expected_sha256");
            equal(prepared.manifestSha256, byName.get("manifest_sha256"), "manifest_sha256");
            equal(4, byName.size(), "four different names");
            discard(files);
        });

        run("LaunchRequest.laterStartsRemoveOnlyEarlierImageCopies", () -> {
            File files = readyTree();
            File root = privateTrace(files);
            // A second script-made request, older than the template; it is never in the launcher's list.
            addRun(files, ID_B, T0 - 50_000, addInput(files, "one.bin"));
            Map<String, String> others = snapshotWithout(files, "private-trace/run-", "settings/launcher-runs.txt");
            Map<String, String> scriptRuns = snapshot(root, false);

            String first = start(files, T0 + 10_000);    // copied from ID_A
            String second = start(files, T0 + 20_000);   // copied from first (the newest); first stays whole
            check(new File(root, "run-" + first + "/decoded.image.bin").isFile(), "the template in use keeps its image");
            equal(first + "\n" + second + "\n", text(new File(files, "settings/launcher-runs.txt")), "list after two");

            LaunchRequest.Prepared third = LaunchRequest.prepare(files);   // copied from second; first is cleaned
            equal(null, third.error, "no error");
            equal(Arrays.asList(first), third.imagesRemoved, "which image copy was removed");
            equal(third.requestId, LaunchRequest.newestLauncherRun(files), "the run made last, after a clean-up");
            equal(Arrays.asList("arguments.bin"), Arrays.asList(new File(root, "run-" + first).list()),
                    "the earlier run lost its image and nothing else");
            check(new File(root, "run-" + second + "/decoded.image.bin").isFile(), "the template in use keeps its image");
            equal(second + "\n" + third.requestId + "\n", text(new File(files, "settings/launcher-runs.txt")),
                    "list after three");

            // With a pinned script-made template, both remaining launcher copies are earlier ones.
            write(new File(files, "settings/launch-template.txt"), ID_A + "\n");
            LaunchRequest.Prepared fourth = LaunchRequest.prepare(files);
            equal(null, fourth.error, "no error");
            equal(Arrays.asList(second, third.requestId), fourth.imagesRemoved, "removed with a pinned template");
            equal(fourth.requestId + "\n", text(new File(files, "settings/launcher-runs.txt")), "list after four");
            equal(fourth.requestId, LaunchRequest.newestLauncherRun(files), "the run made last, with a pinned template");
            check(new File(files, "settings/launch-template.txt").delete(), "remove the pin");

            // Script-made requests (never listed) are whole, and everything outside the run directories is as before.
            Map<String, String> runsNow = snapshot(root, false);
            int compared = 0;
            for (Map.Entry<String, String> entry : scriptRuns.entrySet()) {
                if (entry.getKey().startsWith("run-" + ID_A) || entry.getKey().startsWith("run-" + ID_B)) {
                    equal(entry.getValue(), runsNow.get(entry.getKey()), "script-made: " + entry.getKey());
                    ++compared;
                }
            }
            equal(6, compared, "two script-made directories with two files each were compared");
            check(!new File(files, "settings/launcher-runs.txt.tmp").exists(), "no temporary list file is left");
            equal("", difference(others, snapshotWithout(files, "private-trace/run-", "settings/launcher-runs.txt")),
                    "saves, switches, inputs, code and other settings");
            discard(files);
        });

        run("LaunchRequest.listLinesThatAreNotLauncherRunsDeleteNothing", () -> {
            File files = readyTree();
            File root = privateTrace(files);
            addRun(files, ID_B, T0 - 50_000, addInput(files, "one.bin"));
            // A listed id whose directory is a link to a script-made request, and one whose image is a link to a save.
            Files.createSymbolicLink(new File(root, "run-" + ID_C).toPath(), new File(root, "run-" + ID_B).toPath());
            File odd = new File(root, "run-" + ID_D);
            check(odd.mkdir(), "make a directory");
            Files.createSymbolicLink(new File(odd, "decoded.image.bin").toPath(), new File(files, "saves/slot0").toPath());
            // Lines that are not ids, among them one that would lead to the script-made request through "..".
            write(new File(files, "settings/launcher-runs.txt"), "../saves/slot0\nsaves\n/etc/hosts\nrun-" + ID_B + "\n"
                    + ID_B + "/../run-" + ID_B + "\n" + ID_B.toUpperCase() + "\n" + ID_C + "\n" + ID_D + "\n\n");
            Map<String, String> before = snapshotWithout(files, "settings/launcher-runs.txt");

            LaunchRequest.Prepared prepared = LaunchRequest.prepare(files);
            equal(null, prepared.error, "no error");
            check(prepared.imagesRemoved.isEmpty(), "nothing was removed");
            Map<String, String> after = snapshotWithout(files, "settings/launcher-runs.txt",
                    "private-trace/run-" + prepared.requestId);
            equal("", difference(before, after), "everything that was there is still there, unchanged");
            // The two listed ids had nothing this launcher may delete; they leave the list. Other lines were never ids.
            equal(prepared.requestId + "\n", text(new File(files, "settings/launcher-runs.txt")), "the list afterwards");
            discard(files);
        });

        run("LaunchRequest.failedCopyLeavesNoRunDirectory", () -> {
            File files = readyTree();
            File root = privateTrace(files);
            File templateImage = new File(root, "run-" + ID_A + "/decoded.image.bin");
            mode(templateImage, "---------");   // the copy of the image will fail after the directory was made
            Map<String, String> before = snapshotWithout(files, "settings/launcher-runs.txt");

            LaunchRequest.Prepared prepared = LaunchRequest.prepare(files);
            check(prepared.error != null && prepared.error.contains("decoded.image.bin"),
                    "the error names the file: " + prepared.error);
            check(prepared.error.startsWith(LaunchRequest.ERROR_IO_PREFIX), "and is the file error: " + prepared.error);
            equal(null, prepared.requestId, "no id is handed out");
            equal(0, prepared.extras().length, "no extras");
            equal("", difference(before, snapshotWithout(files, "settings/launcher-runs.txt")),
                    "no run directory is left behind");
            discard(files);
        });

        run("LaunchRequest.notReadyCreatesNothing", () -> {
            File files = readyTree();
            check(new File(privateTrace(files), "private-trace-live.so").delete(), "remove the game code");
            Map<String, String> before = snapshot(files.getParentFile(), true);
            LaunchRequest.Prepared noCode = LaunchRequest.prepare(files);
            equal("the game code is missing", noCode.error, "error without code");
            equal("", difference(before, snapshot(files.getParentFile(), true)), "nothing created without code");

            addCode(files, 4096);
            check(new File(privateTrace(files), "inputs/two.bin").delete(), "remove a game file");
            before = snapshot(files.getParentFile(), true);
            equal("the game files are not installed", LaunchRequest.prepare(files).error, "error without data");
            equal("", difference(before, snapshot(files.getParentFile(), true)), "nothing created without data");
            discard(files);
        });

        run("LaunchRequest.aStartTheGameWindowWouldRefuseOrThatLacksItsShaderFileIsNotPrepared", () -> {
            // The shader file is missing.
            File files = readyTree();
            File root = privateTrace(files);
            File shaders = new File(root, InstallState.SHADER_FILE);
            byte[] kept = Files.readAllBytes(shaders.toPath());
            check(shaders.delete(), "remove the shader file");
            Map<String, String> before = snapshot(files.getParentFile(), true);
            LaunchRequest.Prepared noShaders = LaunchRequest.prepare(files);
            equal(LaunchRequest.ERROR_SHADERS_MISSING, noShaders.error, "error without the shader file");
            equal(0, noShaders.extras().length, "no extras to start with");
            equal("", difference(before, snapshot(files.getParentFile(), true)), "nothing created without the shader file");
            write(shaders, kept);

            // The game code file is one the game window's loader refuses: each fault is named in the reason.
            File code = new File(root, "private-trace-live.so");
            mode(code, "rw-------");
            before = snapshot(files.getParentFile(), true);
            equal(LaunchRequest.ERROR_CODE_UNLOADABLE_PREFIX + "permissions", LaunchRequest.prepare(files).error, "writable game code");
            equal("", difference(before, snapshot(files.getParentFile(), true)), "nothing created for a refused file");
            mode(code, "r--------");
            mode(root, "rwxr-xr-x");
            equal(LaunchRequest.ERROR_CODE_UNLOADABLE_PREFIX + "folder_permissions", LaunchRequest.prepare(files).error, "open directory");
            mode(root, "rwx------");
            check(code.delete(), "remove the game code");
            write(code, bytes(4096, 7));
            mode(code, "r--------");
            before = snapshot(files.getParentFile(), true);
            equal(LaunchRequest.ERROR_CODE_UNLOADABLE_PREFIX + "not_a_program", LaunchRequest.prepare(files).error, "a file that is no library");
            equal("", difference(before, snapshot(files.getParentFile(), true)), "nothing created for a file that is no library");
            File second = new File(root, "second-name.so");
            check(code.delete(), "remove it");
            addCode(files, 4096);
            Files.createLink(second.toPath(), code.toPath());
            equal(LaunchRequest.ERROR_CODE_UNLOADABLE_PREFIX + "linked", LaunchRequest.prepare(files).error, "a file with two names");
            check(second.delete(), "remove the second name");

            // With the faults gone the same tree starts.
            equal(null, LaunchRequest.prepare(files).error, "the repaired tree is prepared");
            discard(files);
        });

        run("LaunchRequest.qualifiedCodeFileStopsPlayOnOtherCodeBeforeAnythingIsCreated", () -> {
            File files = readyTree();
            File library = new File(privateTrace(files), "private-trace-live.so");
            String actual;
            try (java.io.InputStream in = new java.io.FileInputStream(library)) {
                actual = LaunchRequest.sha256Hex(in);
            }
            String other = actual.charAt(0) == '0' ? "1" + actual.substring(1) : "0" + actual.substring(1);
            equal(null, LaunchRequest.qualifiedHash(files), "no file: nothing to compare");
            check(LaunchRequest.codeQualified(null, actual), "no file: any code is accepted, as before this check existed");

            // The developer's file names another build.
            File qualified = write(new File(files, "settings/qualified-code.txt"), other + "\n");
            equal(other, LaunchRequest.qualifiedHash(files), "the hash in the file");
            Map<String, String> before = snapshot(files.getParentFile(), true);
            LaunchRequest.Prepared refused = LaunchRequest.prepare(files);
            equal(LaunchRequest.UNQUALIFIED, refused.error, "Play on other code is stopped");
            equal(0, refused.extras().length, "no extras to start with");
            equal("", difference(before, snapshot(files.getParentFile(), true)), "nothing was created, listed or removed");

            // The user was asked and said yes.
            LaunchRequest.Prepared accepted = LaunchRequest.prepare(files, true);
            equal(null, accepted.error, "an explicit yes starts the code that is there");
            equal(actual, accepted.expectedSha256, "with that code's own hash");
            equal(other + "\n", new String(java.nio.file.Files.readAllBytes(qualified.toPath()), java.nio.charset.StandardCharsets.US_ASCII), "the app does not rewrite the developer's file");

            // The file names the code that is there, in the form "sha256sum" prints.
            write(qualified, actual + "  files/private-trace/private-trace-live.so\n");
            equal(actual, LaunchRequest.qualifiedHash(files), "first word of a sha256sum line");
            equal(null, LaunchRequest.prepare(files).error, "the qualified build starts without a question");

            // A file that holds no hash cannot confirm anything: the launcher asks.
            for (String text : new String[] {"", "not a hash\n", actual.substring(1) + "\n", actual.toUpperCase(java.util.Locale.ROOT) + "\n", actual + "0\n"}) {
                write(qualified, text);
                equal("", LaunchRequest.qualifiedHash(files), "unusable content <" + text.trim() + ">");
                equal(LaunchRequest.UNQUALIFIED, LaunchRequest.prepare(files).error, "unusable content asks: <" + text.trim() + ">");
            }
            discard(files);
        });

        LauncherTestSupport.finish("LaunchRequestTest");
    }

    /** Two game files, one directory, one script-made start request (ID_A), the game code, and decoys. */
    private static File readyTree() throws Exception {
        File files = newFilesDir();
        String one = addInput(files, "one.bin");
        String two = addInput(files, "two.bin");
        String cache = addDirectory(files, "inputs/native-cache");
        addRun(files, ID_A, T0, one, two, cache);
        addCode(files, 4096);
        addDecoys(files);
        return files;
    }

    /** One successful prepare whose arguments file is then dated, so that "newest" is the same on every machine. */
    private static String start(File files, long modifiedMs) throws Exception {
        LaunchRequest.Prepared prepared = LaunchRequest.prepare(files);
        equal(null, prepared.error, "no error");
        File arguments = new File(privateTrace(files), "run-" + prepared.requestId + "/arguments.bin");
        check(arguments.setLastModified(modifiedMs), "set time");
        return prepared.requestId;
    }

    private static Map<String, String> snapshotWithout(File files, String... prefixes) throws Exception {
        Map<String, String> entries = snapshot(files, false);
        for (java.util.Iterator<String> names = entries.keySet().iterator(); names.hasNext(); ) {
            String name = names.next();
            for (String prefix : prefixes) {
                if (name.startsWith(prefix)) {
                    names.remove();
                    break;
                }
            }
        }
        return entries;
    }

    private static String text(File file) throws Exception {
        return new String(Files.readAllBytes(file.toPath()), StandardCharsets.US_ASCII);
    }

    private static String sha(byte[] content) throws Exception {
        return LaunchRequest.sha256Hex(new ByteArrayInputStream(content));
    }

    private static void refusedRewrite(byte[] template, String oldId, String newId, String reasonPart) {
        try {
            LaunchRequest.rewriteArguments(template, oldId, newId);
        } catch (IllegalArgumentException refused) {
            check(refused.getMessage().contains(reasonPart),
                    "refused for another reason than <" + reasonPart + ">: " + refused.getMessage());
            return;
        }
        throw new AssertionError("not refused; expected <" + reasonPart + ">");
    }

    private static void refusedArguments(byte[] arguments, String root, String id, String reasonPart) {
        String reason = LaunchRequest.checkArguments(arguments, root, id);
        check(reason != null, "accepted; expected a refusal with <" + reasonPart + ">");
        check(reason.contains(reasonPart), "refused for another reason than <" + reasonPart + ">: " + reason);
    }

    private static String[] with(String[] arguments, int index, String value) {
        String[] changed = arguments.clone();
        changed[index] = value;
        return changed;
    }

    private static String repeat(char c, int count) {
        char[] text = new char[count];
        Arrays.fill(text, c);
        return new String(text);
    }

    private static String[] padded(String[] arguments, int count) {
        String[] result = Arrays.copyOf(arguments, count);
        for (int index = arguments.length; index < count; ++index) {
            result[index] = "p";
        }
        return result;
    }

    // ---- cases for the comparison with the game's own C++ rule (run_tests.sh, step "argument rule") ----

    private static final class RuleCase {
        final String root, id;
        final byte[] arguments;

        RuleCase(String root, String id, byte[] arguments) {
            this.root = root;
            this.id = id;
            this.arguments = arguments;
        }
    }

    /**
     * Writes directory/NNN.root, NNN.id, NNN.bin for every case and java-verdicts.txt with one line "NNN accept" or
     * "NNN reject" from LaunchRequest.checkArguments. A small C++ program then gives the game's own verdicts for the
     * same files (tools/host_guest_trace/private_trace_arguments.h) and the two lists must be equal.
     */
    private static void emitCases(File directory) throws Exception {
        List<RuleCase> cases = new ArrayList<RuleCase>();
        String[] valid = validArguments(DEVICE_ROOT, ID_A, "--read-only-file", "151", DEVICE_ROOT + "/inputs/abc.bin",
                "--color-program", DEVICE_ROOT + "/modules/boot");
        byte[] blob = join(valid);
        cases.add(new RuleCase(DEVICE_ROOT, ID_A, blob));
        cases.add(new RuleCase(DEVICE_ROOT, ID_A, join(validArguments(DEVICE_ROOT, ID_A))));
        cases.add(new RuleCase(DEVICE_ROOT, ID_A, join(Arrays.copyOf(valid, 10))));
        cases.add(new RuleCase(DEVICE_ROOT, ID_A, Arrays.copyOf(blob, blob.length - 1)));
        cases.add(new RuleCase(DEVICE_ROOT, ID_A, Arrays.copyOf(blob, blob.length + 1)));
        cases.add(new RuleCase(DEVICE_ROOT, ID_A, new byte[] {0}));
        for (String root : new String[] {"relative", "/bad/../root", "/bad//root", "/bad/./root", "/bad/", "/bad\\root",
                DEVICE_ROOT + "x", "/"}) {
            cases.add(new RuleCase(root, ID_A, blob));
        }
        for (String id : new String[] {ID_B, ID_A.substring(1), ID_A + "a", ID_A.toUpperCase(),
                "g" + ID_A.substring(1), ID_A.substring(0, 31) + "/"}) {
            cases.add(new RuleCase(DEVICE_ROOT, id, blob));
        }
        for (String value : new String[] {"/etc/passwd", "../input", "foo/bar", "foo\\bar", "/data/user/0/foreign/file",
                DEVICE_ROOT + "/inputs/../bad", DEVICE_ROOT + "/inputs/./bad", DEVICE_ROOT + "/inputs//bad",
                DEVICE_ROOT + "/inputs/", DEVICE_ROOT + "/inputs", DEVICE_ROOT + "/inputs2/bad",
                DEVICE_ROOT + "/modules/../inputs/bad", DEVICE_ROOT + "/run-" + ID_B + "/bad",
                DEVICE_ROOT + "/run-" + ID_A + "/extra.bin", DEVICE_ROOT + "/private-trace-library.so",
                DEVICE_ROOT + "/inputs/a b", DEVICE_ROOT + "/inputs/a_b-c.d", DEVICE_ROOT + "/modules/x/y/z",
                "tab\there", "del\u007f", "high\u0080", "café", " ", "~", "plain", "-", "--flag=value",
                repeat('x', 4095), repeat('x', 4096), repeat('x', 4097), "/" + repeat('y', 4095)}) {
            cases.add(new RuleCase(DEVICE_ROOT, ID_A, join(with(valid, valid.length - 1, value))));
            cases.add(new RuleCase(DEVICE_ROOT, ID_A, join(with(valid, 0, value))));
            cases.add(new RuleCase(DEVICE_ROOT, ID_A, join(with(valid, 1, value))));
        }
        for (int count : new int[] {11, 12, 1023, 1024, 1025, 1100}) {
            cases.add(new RuleCase(DEVICE_ROOT, ID_A, join(padded(valid, count))));
        }
        for (int total : new int[] {65535, 65536, 65537}) {   // around the size limit, with valid content
            cases.add(new RuleCase(DEVICE_ROOT, ID_A, sizedBlob(valid, total)));
        }
        // Random damage to the valid request: change, remove or insert one byte. A fixed seed keeps the set stable.
        Random random = new Random(20261004L);
        for (int round = 0; round < 600; ++round) {
            byte[] damaged;
            int at = random.nextInt(blob.length);
            switch (random.nextInt(3)) {
                case 0:
                    damaged = blob.clone();
                    damaged[at] = (byte) random.nextInt(256);
                    break;
                case 1:
                    damaged = new byte[blob.length - 1];
                    System.arraycopy(blob, 0, damaged, 0, at);
                    System.arraycopy(blob, at + 1, damaged, at, blob.length - at - 1);
                    break;
                default:
                    damaged = new byte[blob.length + 1];
                    System.arraycopy(blob, 0, damaged, 0, at);
                    damaged[at] = (byte) "/.\0a\\ -_".charAt(random.nextInt(8));
                    System.arraycopy(blob, at, damaged, at + 1, blob.length - at);
                    break;
            }
            cases.add(new RuleCase(DEVICE_ROOT, ID_A, damaged));
        }

        Files.createDirectories(directory.toPath());
        StringBuilder verdicts = new StringBuilder();
        int accepted = 0;
        for (int index = 0; index < cases.size(); ++index) {
            RuleCase one = cases.get(index);
            String name = String.format("%04d", index);
            Files.write(new File(directory, name + ".root").toPath(), one.root.getBytes(StandardCharsets.ISO_8859_1));
            Files.write(new File(directory, name + ".id").toPath(), one.id.getBytes(StandardCharsets.ISO_8859_1));
            Files.write(new File(directory, name + ".bin").toPath(), one.arguments);
            boolean accept = LaunchRequest.checkArguments(one.arguments, one.root, one.id) == null;
            accepted += accept ? 1 : 0;
            verdicts.append(name).append(accept ? " accept\n" : " reject\n");
        }
        Files.write(new File(directory, "java-verdicts.txt").toPath(), verdicts.toString().getBytes("US-ASCII"));
        Files.write(new File(directory, "count.txt").toPath(), String.valueOf(cases.size()).getBytes("US-ASCII"));
        System.out.println("     wrote " + cases.size() + " cases; this class accepts " + accepted + " and refuses "
                + (cases.size() - accepted));
    }

    private static byte[] sizedBlob(String[] valid, int total) {
        List<String> all = new ArrayList<String>(Arrays.asList(valid));
        int remaining = total - join(valid).length;
        while (remaining > 0) {
            int length = Math.min(remaining - 1, 4000);
            if (remaining - (length + 1) == 1) {
                --length;
            }
            all.add(repeat('x', length));
            remaining -= length + 1;
        }
        byte[] result = join(all.toArray(new String[0]));
        check(result.length == total, "padding gave " + result.length + " bytes, wanted " + total);
        return result;
    }
}
