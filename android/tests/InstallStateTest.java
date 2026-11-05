package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.LauncherTestSupport.ID_A;
import static dev.recompbootstrap.smoke.LauncherTestSupport.ID_B;
import static dev.recompbootstrap.smoke.LauncherTestSupport.ID_C;
import static dev.recompbootstrap.smoke.LauncherTestSupport.addCode;
import static dev.recompbootstrap.smoke.LauncherTestSupport.addDecoys;
import static dev.recompbootstrap.smoke.LauncherTestSupport.addDirectory;
import static dev.recompbootstrap.smoke.LauncherTestSupport.addInput;
import static dev.recompbootstrap.smoke.LauncherTestSupport.addRun;
import static dev.recompbootstrap.smoke.LauncherTestSupport.addRunWithArguments;
import static dev.recompbootstrap.smoke.LauncherTestSupport.bytes;
import static dev.recompbootstrap.smoke.LauncherTestSupport.check;
import static dev.recompbootstrap.smoke.LauncherTestSupport.difference;
import static dev.recompbootstrap.smoke.LauncherTestSupport.discard;
import static dev.recompbootstrap.smoke.LauncherTestSupport.equal;
import static dev.recompbootstrap.smoke.LauncherTestSupport.join;
import static dev.recompbootstrap.smoke.LauncherTestSupport.newFilesDir;
import static dev.recompbootstrap.smoke.LauncherTestSupport.privateTrace;
import static dev.recompbootstrap.smoke.LauncherTestSupport.run;
import static dev.recompbootstrap.smoke.LauncherTestSupport.snapshot;
import static dev.recompbootstrap.smoke.LauncherTestSupport.validArguments;
import static dev.recompbootstrap.smoke.LauncherTestSupport.write;

import java.io.File;
import java.nio.file.Files;
import java.util.Map;

/**
 * Host test of InstallState.detect: what stands in the way of a start in each state of the files (nothing, game
 * data, game code, a game code file the game window would refuse, the shader file), which earlier run is copied
 * from, and that looking changes nothing. Run with java -ea. Every detect call in this file goes through detectUnchanged, which compares a full
 * listing of the directory tree (names, kinds, permissions, sizes, times, content hashes) before and after.
 */
public final class InstallStateTest {
    private static final long T0 = 1_700_000_000_000L;

    public static void main(String[] args) {
        LauncherTestSupport.requireAssertions();

        run("InstallState.emptyDirectoryIsNothingInstalled", () -> {
            File files = newFilesDir();
            InstallState state = detectUnchanged(files);
            equal(InstallState.Problem.NOTHING_INSTALLED, state.problem, "problem");
            check(!state.ready(), "not ready");
            equal(null, state.templateRunId, "template");
            equal(0L, state.codeBytes, "code size");
            equal(0L, state.codeModifiedMs, "code time");
            check(!state.dataPresent && !state.shaderFilePresent && !state.versionOnRecord, "no data, shader file or record");
            // A files directory that does not exist at all (the app was never opened) is the same answer.
            equal(InstallState.Problem.NOTHING_INSTALLED, detectUnchanged(new File(files, "absent")).problem, "no directory");
            discard(files);
        });

        run("InstallState.dataAndCodeIsReady", () -> {
            File files = readyTree();
            File code = new File(privateTrace(files), "private-trace-live.so");
            check(code.setLastModified(T0 + 5000), "set code time");
            InstallState state = detectUnchanged(files);
            equal(InstallState.Problem.NONE, state.problem, "problem");
            check(state.ready(), "ready");
            equal(InstallState.CodeFault.NONE, state.codeFault, "the game code file is one the game window loads");
            check(state.shaderFilePresent, "shader file present");
            equal(ID_A, state.templateRunId, "template");
            equal(4096L, state.codeBytes, "code size");
            equal(T0 + 5000, state.codeModifiedMs, "code time");
            check(state.dataPresent, "data present");
            equal(3, state.referencedFiles, "named files");
            equal(0, state.missingFiles, "missing files");
            discard(files);
        });

        run("InstallState.codeMissingOrTooSmallIsCodeMissing", () -> {
            File files = readyTree();
            File code = new File(privateTrace(files), "private-trace-live.so");
            check(code.delete(), "remove code");
            InstallState absent = detectUnchanged(files);
            equal(InstallState.Problem.CODE_MISSING, absent.problem, "problem without code");
            equal(0L, absent.codeBytes, "code size without code");
            check(absent.dataPresent, "data still present");

            addCode(files, 63);
            equal(InstallState.Problem.CODE_MISSING, detectUnchanged(files).problem, "problem with 63 bytes of code");
            check(code.delete(), "remove code");
            addCode(files, 64);
            equal(InstallState.Problem.NONE, detectUnchanged(files).problem, "problem with 64 bytes of code");
            check(code.delete(), "remove code");
            check(code.mkdir(), "a directory with the code file's name");
            equal(InstallState.Problem.CODE_MISSING, detectUnchanged(files).problem, "problem with a directory as code");
            discard(files);
        });

        run("InstallState.missingGameFileIsDataIncomplete", () -> {
            File files = readyTree();
            check(new File(privateTrace(files), "inputs/two.bin").delete(), "remove one input");
            InstallState state = detectUnchanged(files);
            equal(InstallState.Problem.DATA_INCOMPLETE, state.problem, "problem");
            check(!state.dataPresent, "data not present");
            equal(3, state.referencedFiles, "named files");
            equal(1, state.missingFiles, "missing files");
            equal(4096L, state.codeBytes, "the code is still reported");
            discard(files);
        });

        run("InstallState.directoriesNamedByTheRequestCountAsPresent", () -> {
            // The real start request names three directories under inputs/ and one under modules/.
            File files = newFilesDir();
            String cache = addDirectory(files, "inputs/native-cache");
            String boot = addDirectory(files, "modules/boot");
            String file = addInput(files, "one.bin");
            addRun(files, ID_A, T0, file, cache, boot);
            addCode(files, 4096);
            InstallState state = detectUnchanged(files);
            equal(InstallState.Problem.NONE, state.problem, "problem");
            equal(3, state.referencedFiles, "named files");
            equal(0, state.missingFiles, "missing files");
            check(new File(boot).delete(), "remove modules/boot");
            InstallState without = detectUnchanged(files);
            equal(InstallState.Problem.DATA_INCOMPLETE, without.problem, "problem without modules/boot");
            equal(1, without.missingFiles, "missing files without modules/boot");
            discard(files);
        });

        run("InstallState.requestThatNamesNoGameFilesIsNotData", () -> {
            File files = newFilesDir();
            addRun(files, ID_A, T0);
            addCode(files, 4096);
            InstallState state = detectUnchanged(files);
            equal(ID_A, state.templateRunId, "template");
            check(!state.dataPresent, "data not present");
            equal(InstallState.Problem.DATA_INCOMPLETE, state.problem, "problem");
            equal(0, state.missingFiles, "nothing is counted as missing: nothing is named");
            discard(files);
        });

        run("InstallState.newestRequestIsTheTemplate", () -> {
            File files = newFilesDir();
            String input = addInput(files, "one.bin");
            addCode(files, 4096);
            addRun(files, ID_B, T0 + 1000, input);
            addRun(files, ID_C, T0 + 3000, input);
            addRun(files, ID_A, T0 + 2000, input);
            equal(ID_C, detectUnchanged(files).templateRunId, "newest of three");
            // Equal times: the smaller id, so that the choice does not depend on directory order.
            File arguments = new File(privateTrace(files), "run-" + ID_A + "/arguments.bin");
            check(arguments.setLastModified(T0 + 3000), "set time");
            equal(ID_A, detectUnchanged(files).templateRunId, "equal times");
            discard(files);
        });

        run("InstallState.pinnedRunWinsOverNewest", () -> {
            File files = newFilesDir();
            String input = addInput(files, "one.bin");
            addCode(files, 4096);
            addRun(files, ID_A, T0 + 1000, input);
            addRun(files, ID_B, T0 + 9000, input);
            write(new File(files, "settings/launch-template.txt"), ID_A + "\n");
            InstallState pinned = detectUnchanged(files);
            equal(ID_A, pinned.templateRunId, "pinned");
            check(!pinned.pinIgnored, "the pin is used");

            write(new File(files, "settings/launch-template.txt"), ID_C + "\n");   // names a run that is not there
            InstallState fallback = detectUnchanged(files);
            equal(ID_B, fallback.templateRunId, "falls back to the newest");
            check(fallback.pinIgnored, "says that the pin was not usable");
            equal(InstallState.Problem.NONE, fallback.problem, "still ready");

            write(new File(files, "settings/launch-template.txt"), "not an id\n");
            InstallState garbage = detectUnchanged(files);
            equal(ID_B, garbage.templateRunId, "a pin that holds no run id: the newest run is used");
            check(garbage.pinIgnored, "and that pin too is reported as not usable (the Play page then says so)");
            write(new File(files, "settings/launch-template.txt"), "");
            check(detectUnchanged(files).pinIgnored, "an empty pin file is a pin that cannot be used");
            check(new File(files, "settings/launch-template.txt").delete(), "(the pin file is removed)");
            InstallState none = detectUnchanged(files);
            equal(ID_B, none.templateRunId, "no pin file: the newest run");
            check(!none.pinIgnored, "no pin file, nothing ignored");
            discard(files);
        });

        run("InstallState.directoriesThatAreNotRequestsAreIgnored", () -> {
            // Each of these is newer than the one good request (ID_A) and must not be chosen instead of it.
            File files = newFilesDir();
            String input = addInput(files, "one.bin");
            addCode(files, 4096);
            addRun(files, ID_A, T0, input);
            File root = privateTrace(files);
            String rootPath = root.getAbsolutePath();
            long newer = T0 + 60_000;

            // names that are not "run-" + 32 lower-case hex digits
            String upper = "ABCDEFABCDEFABCDEFABCDEFABCDEFAB";
            copyRequest(root, "run-" + upper, join(validArguments(rootPath, upper, "--read-only-file", input)), newer);
            String shortId = ID_B.substring(1);
            copyRequest(root, "run-" + shortId, join(validArguments(rootPath, shortId, "--read-only-file", input)), newer);
            copyRequest(root, "runs-" + ID_B, join(validArguments(rootPath, ID_B, "--read-only-file", input)), newer);
            // a plain file with a run directory's name
            write(new File(root, "run-" + ID_B), bytes(10, 3));
            equal(ID_A, detectUnchanged(files).templateRunId, "wrong names");
            check(new File(root, "run-" + ID_B).delete(), "remove the plain file");

            // no game image
            File noImage = addRun(files, ID_B, newer, input);
            check(new File(noImage, "decoded.image.bin").delete(), "remove image");
            equal(ID_A, detectUnchanged(files).templateRunId, "no image");
            LauncherTestSupport.mode(noImage, "rwx------");

            // arguments of 0 bytes and of 65537 bytes; 65536 bytes of valid arguments are accepted
            File arguments = new File(noImage, "arguments.bin");
            write(new File(noImage, "decoded.image.bin"), bytes(100, 4));
            LauncherTestSupport.mode(arguments, "rw-------");
            write(arguments, new byte[0]);
            check(arguments.setLastModified(newer), "set time");
            equal(ID_A, detectUnchanged(files).templateRunId, "empty arguments");
            write(arguments, sized(rootPath, ID_B, input, 65537));
            check(arguments.setLastModified(newer), "set time");
            equal(ID_A, detectUnchanged(files).templateRunId, "arguments one byte too long");
            write(arguments, sized(rootPath, ID_B, input, 65536));
            check(arguments.setLastModified(newer), "set time");
            equal(ID_B, detectUnchanged(files).templateRunId, "arguments of exactly 65536 bytes");

            // arguments the game would refuse for this run: they name another run's image
            write(arguments, join(validArguments(rootPath, ID_C, "--read-only-file", input)));
            check(arguments.setLastModified(newer), "set time");
            equal(ID_A, detectUnchanged(files).templateRunId, "arguments of another run");

            // a link named like a run directory, pointing at a complete request for that very id kept elsewhere
            // (the game opens run directories without following links, so it would refuse this one)
            check(arguments.delete() && new File(noImage, "decoded.image.bin").delete() && noImage.delete(), "clear");
            File elsewhere = new File(files, "elsewhere");
            copyRequest(elsewhere, "target", join(validArguments(rootPath, ID_C, "--read-only-file", input)), newer);
            Files.createSymbolicLink(new File(root, "run-" + ID_C).toPath(), new File(elsewhere, "target").toPath());
            equal(ID_A, detectUnchanged(files).templateRunId, "a link to a run directory");
            discard(files);
        });

        run("InstallState.filesCopiedFromDiscsByTheAppAreNotAnInstallThisVersionCanStart", () -> {
            File files = newFilesDir();
            write(new File(files, "game-install/install.json"), "{\"version\":1,\"complete\":false}\n");
            InstallState partial = detectUnchanged(files);
            equal(InstallState.Problem.DISC_COPY_ONLY, partial.problem, "problem without a bundle");
            check(!partial.discInstallComplete, "not complete");

            write(new File(files, "game-install/bundle/code.so"), bytes(100, 5));
            InstallState complete = detectUnchanged(files);
            // Such an install cannot be started by this app version; it must not be shown as Ready.
            equal(InstallState.Problem.DISC_COPY_ONLY, complete.problem, "problem with a bundle");
            check(!complete.ready(), "not ready");
            check(complete.discInstallComplete, "reported as copied from discs in full");

            // What the install kit puts under private-trace is judged on its own, whatever stands in game-install.
            addRun(files, ID_A, T0, addInput(files, "one.bin"));
            equal(InstallState.Problem.CODE_MISSING, detectUnchanged(files).problem, "game data from the kit, no game code");
            addCode(files, 4096);
            equal(InstallState.Problem.NONE, detectUnchanged(files).problem, "a kit install beside disc copies is ready");
            discard(files);
        });

        run("InstallState.gameCodeWithoutAStartRequestIsDataMissing", () -> {
            File files = newFilesDir();
            addCode(files, 4096);
            addInput(files, "one.bin");   // game files without a start request that names them cannot be found
            InstallState state = detectUnchanged(files);
            equal(InstallState.Problem.DATA_MISSING, state.problem, "problem");
            equal(null, state.templateRunId, "no template");
            equal(4096L, state.codeBytes, "the code is reported");
            discard(files);
        });

        run("InstallState.shaderFileMissingOrEmptyStandsInTheWayOnlyWhenTheRestIsThere", () -> {
            File files = readyTree();
            File shaders = new File(privateTrace(files), InstallState.SHADER_FILE);
            check(shaders.delete(), "remove the shader file");
            InstallState without = detectUnchanged(files);
            equal(InstallState.Problem.SHADERS_MISSING, without.problem, "problem without the shader file");
            check(!without.shaderFilePresent && without.dataPresent && without.codeBytes > 0, "only the shader file is missing");
            write(shaders, new byte[0]);
            equal(InstallState.Problem.SHADERS_MISSING, detectUnchanged(files).problem, "an empty shader file holds no shaders");
            check(shaders.delete() && shaders.mkdir(), "a directory with the shader file's name");
            equal(InstallState.Problem.SHADERS_MISSING, detectUnchanged(files).problem, "a directory is not the shader file");
            check(shaders.delete(), "remove the directory");
            write(shaders, bytes(1, 3));
            equal(InstallState.Problem.NONE, detectUnchanged(files).problem, "a shader file of one byte is there");
            // An older shader file under another name is not the one this game code was run with.
            check(shaders.delete(), "remove the shader file");
            write(new File(privateTrace(files), "generic-shaders.bin"), bytes(50, 4));
            equal(InstallState.Problem.SHADERS_MISSING, detectUnchanged(files).problem, "another shader file does not count");

            // What is reported first: game data, then game code, then the shader file.
            File code = new File(privateTrace(files), "private-trace-live.so");
            check(code.delete(), "remove the game code");
            equal(InstallState.Problem.CODE_MISSING, detectUnchanged(files).problem, "code and shader file missing: the code is named");
            check(new File(privateTrace(files), "inputs/two.bin").delete(), "remove a game file");
            equal(InstallState.Problem.DATA_INCOMPLETE, detectUnchanged(files).problem, "data, code and shader file missing: the data is named");
            discard(files);
        });

        run("InstallState.gameCodeTheGameWindowWouldRefuseIsNamedWithItsFault", () -> {
            File files = readyTree();
            File root = privateTrace(files);
            File code = new File(root, "private-trace-live.so");
            byte[] good = LauncherTestSupport.libraryBytes(4096);

            // The header: every byte the loader compares, changed one at a time, makes the file "not a program".
            for (int at : new int[] {0, 1, 2, 3, 4, 5, 6, 16, 17, 18, 19, 24, 25, 26, 27, 28, 29, 30, 31}) {
                byte[] damaged = good.clone();
                damaged[at] ^= 0x40;
                replaceCode(code, damaged);
                InstallState state = detectUnchanged(files);
                equal(InstallState.Problem.CODE_UNLOADABLE, state.problem, "problem with header byte " + at + " changed");
                equal(InstallState.CodeFault.NOT_A_PROGRAM, state.codeFault, "fault with header byte " + at + " changed");
                equal(4096L, state.codeBytes, "the file is still reported as there");
            }
            // Bytes the loader does not compare may hold anything.
            for (int at : new int[] {7, 15, 20, 23, 32, 63, 64, 4095}) {
                byte[] other = good.clone();
                other[at] ^= 0x40;
                replaceCode(code, other);
                equal(InstallState.Problem.NONE, detectUnchanged(files).problem, "problem with byte " + at + " changed");
            }
            replaceCode(code, good);

            // Permissions: exactly "owner may read". One more bit, or the owner's read bit missing, is refused.
            for (String mode : new String[] {"rw-------", "r--r-----", "r-x------", "r-------x", "---------"}) {
                LauncherTestSupport.mode(code, mode);
                InstallState state = detectUnchanged(files);
                equal(InstallState.Problem.CODE_UNLOADABLE, state.problem, "problem with mode " + mode);
                equal(InstallState.CodeFault.PERMISSIONS, state.codeFault, "fault with mode " + mode);
            }
            LauncherTestSupport.mode(code, "r--------");
            equal(InstallState.CodeFault.NONE, detectUnchanged(files).codeFault, "mode 0400 again");

            // The directory: exactly "owner only".
            for (String mode : new String[] {"rwxr-x---", "rwx--x--x", "rwxrwxrwx"}) {
                LauncherTestSupport.mode(root, mode);
                InstallState state = detectUnchanged(files);
                equal(InstallState.Problem.CODE_UNLOADABLE, state.problem, "problem with directory mode " + mode);
                equal(InstallState.CodeFault.FOLDER_PERMISSIONS, state.codeFault, "fault with directory mode " + mode);
            }
            LauncherTestSupport.mode(root, "rwx------");

            // A second name for the file.
            File second = new File(root, "private-trace-live-backup.so");
            Files.createLink(second.toPath(), code.toPath());
            InstallState linked = detectUnchanged(files);
            equal(InstallState.Problem.CODE_UNLOADABLE, linked.problem, "problem with two names");
            equal(InstallState.CodeFault.LINKED, linked.codeFault, "fault with two names");
            check(second.delete(), "remove the second name");
            equal(InstallState.Problem.NONE, detectUnchanged(files).problem, "one name again");

            // A fault of the game code is named only when the game data is there: data comes first.
            replaceCode(code, bytes(4096, 7));
            check(new File(root, "inputs/two.bin").delete(), "remove a game file");
            InstallState both = detectUnchanged(files);
            equal(InstallState.Problem.DATA_INCOMPLETE, both.problem, "data missing and a bad code file: the data is named");
            equal(InstallState.CodeFault.NOT_A_PROGRAM, both.codeFault, "the fault is still known");
            discard(files);
        });

        run("InstallState.gameCodeLargerThanTheLoadersLimitIsTooLarge", () -> {
            // A file with a hole: 512 MiB and one byte long, a few kilobytes on disk. (Not hashed: detect is called directly.)
            File files = readyTree();
            File code = new File(privateTrace(files), "private-trace-live.so");
            LauncherTestSupport.mode(code, "rw-------");
            try (java.io.RandomAccessFile file = new java.io.RandomAccessFile(code, "rw")) {
                file.setLength(512L * 1024 * 1024);
            }
            LauncherTestSupport.mode(code, "r--------");
            equal(InstallState.Problem.NONE, InstallState.detect(files).problem, "exactly 512 MiB is accepted");
            LauncherTestSupport.mode(code, "rw-------");
            try (java.io.RandomAccessFile file = new java.io.RandomAccessFile(code, "rw")) {
                file.setLength(512L * 1024 * 1024 + 1);
            }
            LauncherTestSupport.mode(code, "r--------");
            InstallState state = InstallState.detect(files);
            equal(InstallState.Problem.CODE_UNLOADABLE, state.problem, "problem with 512 MiB and one byte");
            equal(InstallState.CodeFault.TOO_LARGE, state.codeFault, "fault");
            check(code.delete(), "remove the large file at once");
            discard(files);
        });

        run("InstallState.versionOnRecordSaysWhetherPlayComparesTheGameCode", () -> {
            File files = readyTree();
            check(!detectUnchanged(files).versionOnRecord, "no record file: nothing is compared");
            write(new File(files, "settings/qualified-code.txt"), "0000000000000000000000000000000000000000000000000000000000000000\n");
            check(detectUnchanged(files).versionOnRecord, "a record file: Play compares");
            write(new File(files, "settings/qualified-code.txt"), "not a hash\n");
            check(detectUnchanged(files).versionOnRecord, "a record file that holds no hash still makes Play ask");
            discard(files);
        });

        LauncherTestSupport.finish("InstallStateTest");
    }

    /** Game data (two files and one directory), one start request and the game code, plus other people's files. */
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

    /** detect, with the proof that it created, changed and removed nothing. */
    private static InstallState detectUnchanged(File files) throws Exception {
        Map<String, String> before = snapshot(files.getParentFile(), true);
        InstallState state = InstallState.detect(files);
        Map<String, String> after = snapshot(files.getParentFile(), true);
        equal("", difference(before, after), "detect changed the directory tree");
        check(state != null && state.problem != null && state.codeFault != null, "an answer is always given");
        check(state.ready() == (state.problem == InstallState.Problem.NONE), "ready means that nothing stands in the way");
        return state;
    }

    /** Puts other bytes into the game code file and leaves it mode 0400 with one name, as the installer does. */
    private static void replaceCode(File code, byte[] content) throws Exception {
        check(!code.exists() || code.delete(), "remove the earlier game code file");
        write(code, content);
        LauncherTestSupport.mode(code, "r--------");
    }

    private static void copyRequest(File root, String directoryName, byte[] arguments, long modifiedMs) throws Exception {
        File directory = new File(root, directoryName);
        File argumentsFile = write(new File(directory, "arguments.bin"), arguments);
        write(new File(directory, "decoded.image.bin"), bytes(100, 6));
        check(argumentsFile.setLastModified(modifiedMs), "set time");
    }

    /** Valid arguments for id, padded with long plain values to exactly total bytes. */
    private static byte[] sized(String rootPath, String id, String input, int total) {
        java.util.List<String> extra = new java.util.ArrayList<String>();
        extra.add("--read-only-file");
        extra.add(input);
        int remaining = total - join(validArguments(rootPath, id, extra.toArray(new String[0]))).length;
        while (remaining > 0) {
            int length = Math.min(remaining - 1, 4000);   // each value costs its length plus one zero byte
            if (remaining - (length + 1) == 1) {
                --length;                                  // never leave a single byte: it could not hold a value
            }
            StringBuilder value = new StringBuilder();
            for (int index = 0; index < length; ++index) {
                value.append('x');
            }
            extra.add(value.toString());
            remaining -= length + 1;
        }
        byte[] result = join(validArguments(rootPath, id, extra.toArray(new String[0])));
        check(result.length == total, "padding gave " + result.length + " bytes, wanted " + total);
        return result;
    }
}
