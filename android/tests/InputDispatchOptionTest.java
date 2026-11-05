package dev.recompbootstrap.smoke;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

/**
 * Host test of InputDispatchOption (the "low-delay stick input" option of the game activity). Needs only a JDK:
 *   javac -d out android/app/src/debug/java/dev/recompbootstrap/smoke/InputDispatchOption.java android/tests/InputDispatchOptionTest.java
 *   java -ea -cp out dev.recompbootstrap.smoke.InputDispatchOptionTest
 * Prints one line per case (PASS / FAIL / NOT RUN) and exits with 1 if any case failed. The checks throw
 * AssertionError themselves, so the result does not depend on -ea.
 *
 * Every file is made inside fresh temporary directories that the test removes again; the paths below the temporary
 * "files" directory are written out literally ("private-trace/diag-lt2-unbuffered") because that path is the contract
 * with the settings screen, which creates the file.
 */
public final class InputDispatchOptionTest {
    private interface Case { void run() throws Exception; }
    private static final class NotRun extends RuntimeException {
        private static final long serialVersionUID = 1L;
        NotRun(String why) { super(why); }
    }

    private static int passed, failed, notRun;
    private static final List<File> roots = new ArrayList<>();

    private static void test(String name, Case body) {
        try {
            body.run();
            ++passed;
            System.out.println("PASS     " + name);
        } catch (NotRun skipped) {
            ++notRun;
            System.out.println("NOT RUN  " + name + ": " + skipped.getMessage());
        } catch (Throwable error) {
            ++failed;
            System.out.println("FAIL     " + name + ": " + error);
        }
    }

    private static void check(boolean condition, String what) {
        if (!condition) throw new AssertionError(what);
    }

    /** A fresh, empty stand-in for the app's files directory. */
    private static File newFilesDirectory() throws IOException {
        File root = Files.createTempDirectory("input-option-test").toFile();
        roots.add(root);
        File files = new File(root, "files");
        check(files.mkdir(), "could not create " + files);
        return files;
    }

    private static File directory(File parent, String name) {
        File made = new File(parent, name);
        check(made.mkdirs(), "could not create directory " + made);
        return made;
    }

    private static void file(File parent, String name, String content) throws IOException {
        try (FileOutputStream out = new FileOutputStream(new File(parent, name))) {
            out.write(content.getBytes("UTF-8"));
        }
    }

    private static void removeTree(File node) {
        File[] children = node.listFiles();
        if (children != null) for (File child : children) removeTree(child);
        node.delete();
    }

    /** The focus changes of one run, as the activity reports them; returns how often the request was handed out. */
    private static int requestsOver(InputDispatchOption option, boolean... focusChanges) {
        int requests = 0;
        for (boolean focused : focusChanges) if (option.takeRequest(focused)) ++requests;
        return requests;
    }

    public static void main(String[] args) {
        // ---- requested(): is the switch file there? ----
        test("requested: no switch file in an existing private-trace directory -> off", () -> {
            File trace = directory(newFilesDirectory(), "private-trace");
            file(trace, "diag-fps-overlay", "");
            check(!InputDispatchOption.requested(trace), "reported on without the file");
        });
        test("requested: empty switch file -> on", () -> {
            File trace = directory(newFilesDirectory(), "private-trace");
            file(trace, "diag-lt2-unbuffered", "");
            check(InputDispatchOption.requested(trace), "empty file not seen");
        });
        test("requested: switch file with any content -> on (the content is never read)", () -> {
            File trace = directory(newFilesDirectory(), "private-trace");
            file(trace, "diag-lt2-unbuffered", "0\noff\n");
            check(InputDispatchOption.requested(trace), "file with content not seen");
        });
        test("requested: a directory of that name -> on (anything of that name counts, like the runtime's access(F_OK))", () -> {
            File trace = directory(newFilesDirectory(), "private-trace");
            directory(trace, "diag-lt2-unbuffered");
            check(InputDispatchOption.requested(trace), "directory of that name not seen");
        });
        test("requested: similar names do not count (.tmp of an atomic write, the older diag-lat-unbuffered-input, a shorter and a longer name)", () -> {
            File trace = directory(newFilesDirectory(), "private-trace");
            file(trace, "diag-lt2-unbuffered.tmp", "");
            file(trace, "diag-lat-unbuffered-input", "");
            file(trace, "diag-lt2-unbuffere", "");
            file(trace, "diag-lt2-unbuffered-input", "");
            check(!InputDispatchOption.requested(trace), "a similar name turned the option on");
        });
        test("requested: private-trace directory missing -> off", () -> {
            File files = newFilesDirectory();
            check(!InputDispatchOption.requested(new File(files, "private-trace")), "reported on in a missing directory");
        });
        test("requested: directory that may not be searched -> off, no exception", () -> {
            File trace = directory(newFilesDirectory(), "private-trace");
            file(trace, "diag-lt2-unbuffered", "");
            check(InputDispatchOption.requested(trace), "setup: file not seen before the permission change");
            check(trace.setReadable(false, false) & trace.setExecutable(false, false), "setup: could not change permissions");
            try {
                if (trace.list() != null) throw new NotRun("this user can read a directory without permission (root?)");
                check(!InputDispatchOption.requested(trace), "reported on although the directory may not be searched");
            } finally {
                trace.setReadable(true, false);
                trace.setExecutable(true, false);
            }
            check(InputDispatchOption.requested(trace), "file not seen after the permissions were restored");
        });

        // ---- shouldRequest(): the decision ----
        test("shouldRequest: true only for a game run, with the switch, on API 30 or newer", () -> {
            Object[][] table = {
                // game run, switch present, API level, expected
                {true,  true,  30, true},
                {true,  true,  31, true},
                {true,  true,  33, true},
                {true,  true,  35, true},
                {true,  true,  29, false},
                {true,  true,  26, false},
                {true,  false, 30, false},
                {true,  false, 35, false},
                {true,  false, 29, false},
                {false, true,  30, false},
                {false, true,  35, false},
                {false, true,  29, false},
                {false, false, 30, false},
                {false, false, 35, false},
                {false, false, 29, false},
            };
            for (Object[] row : table) {
                boolean actual = InputDispatchOption.shouldRequest((Boolean) row[0], (Boolean) row[1], (Integer) row[2]);
                check(actual == (Boolean) row[3], "game=" + row[0] + " switch=" + row[1] + " api=" + row[2] + " gave " + actual);
            }
        });

        // ---- forRun() + takeRequest(): from the file the settings screen writes to the one request ----
        test("run: files/private-trace/diag-lt2-unbuffered present, game run, API 33 -> one request, at the first focus gain", () -> {
            File files = newFilesDirectory();
            file(directory(files, "private-trace"), "diag-lt2-unbuffered", "");
            InputDispatchOption option = InputDispatchOption.forRun(true, files, 33);
            check(!option.takeRequest(false), "request handed out on a focus loss");
            check(option.takeRequest(true), "no request at the first focus gain");
            check(requestsOver(option, true, false, true, true, false) == 0, "request handed out a second time");
        });
        test("run: switch absent -> never a request (other switch files, and the name in files/ or files/settings, do not count)", () -> {
            File files = newFilesDirectory();
            File trace = directory(files, "private-trace");
            file(trace, "diag-fps-overlay", "");
            file(trace, "diag-v450-scale-fsr1", "");
            file(files, "diag-lt2-unbuffered", "");
            file(directory(files, "settings"), "diag-lt2-unbuffered", "");
            InputDispatchOption option = InputDispatchOption.forRun(true, files, 33);
            check(requestsOver(option, true, false, true) == 0, "request without the switch file");
        });
        test("run: switch present, API 29 -> never a request; API 30 -> one", () -> {
            File files = newFilesDirectory();
            file(directory(files, "private-trace"), "diag-lt2-unbuffered", "");
            check(requestsOver(InputDispatchOption.forRun(true, files, 29), true, false, true) == 0, "request on API 29");
            check(requestsOver(InputDispatchOption.forRun(true, files, 30), true, false, true) == 1, "not exactly one request on API 30");
        });
        test("run: switch present, not a game run (ABI check) -> never a request", () -> {
            File files = newFilesDirectory();
            file(directory(files, "private-trace"), "diag-lt2-unbuffered", "");
            check(requestsOver(InputDispatchOption.forRun(false, files, 33), true, false, true) == 0, "request for a non-game run");
        });
        test("run: the switch is read once at the start (created later -> still off; removed later -> still on)", () -> {
            File files = newFilesDirectory();
            File trace = directory(files, "private-trace");
            InputDispatchOption startedWithout = InputDispatchOption.forRun(true, files, 33);
            file(trace, "diag-lt2-unbuffered", "");
            InputDispatchOption startedWith = InputDispatchOption.forRun(true, files, 33);
            check(new File(trace, "diag-lt2-unbuffered").delete(), "setup: could not remove the switch file");
            check(requestsOver(startedWithout, true, true) == 0, "a run started without the file made a request");
            check(requestsOver(startedWith, true, true) == 1, "a run started with the file did not make exactly one request");
        });

        test("used request: closes only for claim result 1 and an id listed in files/settings/launcher-runs.txt", () -> {
            File files = newFilesDirectory();
            String mine = "0123456789abcdef0123456789abcdef", script = "ffffffffffffffffffffffffffffffff";
            check(!InputDispatchOption.closesUsedRequest(1, files, mine), "no list file: a used request is shown as refused (script behaviour)");
            file(directory(files, "settings"), "launcher-runs.txt", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n" + mine + "\n");
            check(InputDispatchOption.closesUsedRequest(1, files, mine), "used + made by the launcher: close");
            check(!InputDispatchOption.closesUsedRequest(1, files, script), "used + not on the list (a script's request): refusal screen as before");
            check(!InputDispatchOption.closesUsedRequest(0, files, mine), "a fresh request (claim result 0) is never closed");
            check(!InputDispatchOption.closesUsedRequest(2, files, mine), "a storage failure (claim result 2) is shown, not closed");
            check(!InputDispatchOption.closesUsedRequest(1, files, null), "no request id");
            check(!InputDispatchOption.closesUsedRequest(1, files, mine.substring(0, 31)), "a prefix of a listed id does not count");
            check(!InputDispatchOption.closesUsedRequest(1, files, ""), "an empty id does not match an empty line");
            check(!InputDispatchOption.closesUsedRequest(1, new File(files, "missing"), mine), "no files directory: no exception, refusal screen");
        });

        for (File root : roots) removeTree(root);
        System.out.println("InputDispatchOptionTest: " + passed + " passed, " + failed + " failed, " + notRun + " not run");
        if (failed != 0) System.exit(1);
        System.out.println("INPUT_DISPATCH_OPTION_PASS");
    }
}
