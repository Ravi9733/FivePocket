package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.LauncherTestSupport.ID_A;
import static dev.recompbootstrap.smoke.LauncherTestSupport.addCode;
import static dev.recompbootstrap.smoke.LauncherTestSupport.addInput;
import static dev.recompbootstrap.smoke.LauncherTestSupport.addRun;
import static dev.recompbootstrap.smoke.LauncherTestSupport.check;
import static dev.recompbootstrap.smoke.LauncherTestSupport.discard;
import static dev.recompbootstrap.smoke.LauncherTestSupport.equal;
import static dev.recompbootstrap.smoke.LauncherTestSupport.newFilesDir;
import static dev.recompbootstrap.smoke.LauncherTestSupport.run;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Checks that what the launcher sends and looks for is what the other side really reads. Each check compares a value
 * taken from the launcher's code with the file that owns the other end of the agreement:
 *
 *   - the four extras of a launcher start, against the game activity's source (which reads them) and the
 *     measurement start script (which passes them with "am start --es");
 *   - the process name the launcher treats as "the game is running", against the debug manifest;
 *   - the licence notice shown in About, against the licence file in the tree;
 *   - the name of the shader file the launcher looks for, against the standing game library (which opens it);
 *   - the rule by which the launcher judges a game code file, against the game window's loader;
 *   - the marker and the refusal file the launcher looks for after a start, against the game window's sources;
 *   - the numbers the launcher takes for "crashed", "low memory" and "not responding", against Android's own.
 *
 * A check whose other end is not on this machine prints a NOTE line and does not count as run.
 *
 * Run with java -ea LauncherContractTest [repository root [directory that holds DebugGameLauncher.java]].
 * Without arguments the repository root is looked for upwards from the current directory. The second argument is
 * for the mutant runs, which test a changed copy of the sources; normally it is left out.
 */
public final class LauncherContractTest {
    public static void main(String[] args) throws Exception {
        LauncherTestSupport.requireAssertions();
        if (args.length > 2) {
            throw new IllegalArgumentException("usage: LauncherContractTest [repository root [debug source directory]]");
        }
        final File repository = args.length >= 1 ? new File(args[0]) : findRepository();
        final File debug = new File(repository, "android/app/src/debug");
        final File helperDirectory = args.length == 2 ? new File(args[1])
                : new File(debug, "java/dev/recompbootstrap/smoke");
        // The game window and the controller check are read from the same directory as the helper.
        final File gameWindow = new File(helperDirectory, "PrivateTraceActivity.java");
        final File controllerCheck = new File(helperDirectory, "ControllerCheckActivity.java");

        run("LauncherContract.extrasAreTheOnesTheGameActivityReadsAndTheScriptPasses", () -> {
            File files = newFilesDir();
            addRun(files, ID_A, 1_700_000_000_000L, addInput(files, "one.bin"));
            addCode(files, 4096);
            LaunchRequest.Prepared prepared = LaunchRequest.prepare(files);
            equal(null, prepared.error, "a start request could be made");
            String[][] extras = prepared.extras();
            discard(files);

            String activity = text(gameWindow);
            // Every text extra the activity reads from its intent ...
            java.util.Set<String> read = new java.util.TreeSet<String>();
            Matcher reads = Pattern.compile("getIntent\\(\\)\\.getStringExtra\\(\"([^\"]+)\"\\)").matcher(activity);
            while (reads.find()) {
                read.add(reads.group(1));
            }
            // ... is exactly the set the launcher sends.
            java.util.Set<String> sent = new java.util.TreeSet<String>();
            String mode = null;
            for (String[] extra : extras) {
                sent.add(extra[0]);
                if (extra[0].equals("private_trace")) {
                    mode = extra[1];
                }
            }
            equal(read, sent, "extras read by the game activity and extras sent by the launcher");
            check(activity.contains("\"" + mode + "\".equals(mode)"), "the activity knows the mode " + mode);

            File script = new File(repository, "local-analysis/repro-tag-leaf/replay-staged-api-target.py");
            if (!script.isFile()) {
                System.out.println("     NOTE the start script is not in this checkout: compared with the activity only");
                return;
            }
            String start = text(script);
            int at = start.indexOf("'am', 'start'");
            check(at >= 0, "the script has an 'am start'");
            String call = start.substring(at, start.indexOf(")", start.indexOf("'manifest_sha256'", at)));
            java.util.Set<String> passed = new java.util.TreeSet<String>();
            Matcher passes = Pattern.compile("'--es',\\s*'([^']+)'").matcher(call);
            while (passes.find()) {
                passed.add(passes.group(1));
            }
            equal(passed, sent, "extras passed by the script and extras sent by the launcher");
            check(call.contains("'private_trace', '" + mode + "'"), "the script passes the mode " + mode);
            check(call.contains("'/.PrivateTraceActivity'"), "the script starts the game activity");
            String helper = text(new File(helperDirectory, "DebugGameLauncher.java"));
            check(helper.contains("PrivateTraceActivity.class"), "the launcher's helper starts the same activity");
            check(!Pattern.compile("\\.(addFlags|setFlags|setAction|setData|addCategory)\\(").matcher(helper).find(),
                    "the launcher's helper adds no flags, action, data or category to the intent");
        });

        run("LauncherContract.gameProcessNameIsTheManifests", () -> {
            String manifest = text(new File(debug, "AndroidManifest.xml"));
            Matcher activity = Pattern.compile(
                    "<activity\\s+android:name=\"\\.PrivateTraceActivity\"\\s+android:process=\"([^\"]+)\"")
                    .matcher(manifest);
            check(activity.find(), "the debug manifest declares the game activity with a process name");
            equal(activity.group(1), ScreenStack.GAME_PROCESS_SUFFIX, "process name suffix");
        });

        run("LauncherContract.aboutShowsTheLicenceFileWordForWord", () -> {
            String licence = text(new File(repository,
                    "android/app/src/main/assets/licenses/AMD-FidelityFX-FSR1-MIT.txt"));
            equal(licence, AboutScreen.FSR1_NOTICE, "the AMD FSR 1 notice");
        });

        run("LauncherContract.shaderFileIsTheOneTheStandingGameLibraryOpens", () -> {
            String path = System.getProperty("standing.library");
            File library = path == null ? null : new File(path);
            if (library == null || !library.isFile()) {
                System.out.println("     NOTE the standing game library (-Dstanding.library) is not on this machine: this check did not run");
                return;
            }
            // The game code holds the whole path it opens, as a zero-terminated text.
            String opened = "/files/" + InstallState.PRIVATE_TRACE + "/" + InstallState.SHADER_FILE;
            check(occurs(library, (opened + "\0").getBytes(StandardCharsets.US_ASCII)),
                    "the library holds the path " + opened + " (" + library.getName() + ")");
            // The check can fail: a name the library does not open is not found.
            check(!occurs(library, ("/files/" + InstallState.PRIVATE_TRACE + "/generic-shaders-v99.bin\0").getBytes(StandardCharsets.US_ASCII)),
                    "(control) a shader file name the library does not open is not found in it");
            System.out.println("     " + library.getName() + " (" + library.length() + " bytes) opens " + InstallState.SHADER_FILE);
        });

        run("LauncherContract.gameCodeRuleAndStartMarkerAreTheGameWindowsLoaders", () -> {
            String loader = text(new File(debug, "cpp/private_trace_jni.cpp")).replaceAll("\\s+", "");
            // libraryFile: a regular file of mode 0400 with one link, 64 bytes to 512 MiB, with this header.
            check(loader.contains("(info.st_mode&0777)!=0400||info.st_nlink!=1||info.st_size<64||info.st_size>512LL*1024*1024"),
                    "the loader's rule for the game code file: mode 0400, one link, 64 bytes to 512 MiB");
            equal(64L, InstallState.MINIMUM_CODE_BYTES, "the launcher's smallest game code file");
            equal(512L * 1024 * 1024, LaunchRequest.MAXIMUM_LIBRARY_BYTES, "the launcher's largest game code file");
            check(loader.contains("constexprunsignedcharelf[]{0x7f,'E','L','F',2,1,1};constexprunsignedcharmachine[]{3,0,0xb7,0};constexprunsignedcharentry[8]{};"),
                    "the loader's header rule: ELF, 64-bit, little-endian, version 1; type 3, machine 0xb7; entry zero");
            check(loader.contains("!std::memcmp(header,elf,sizeof(elf))&&!std::memcmp(header+16,machine,4)&&!std::memcmp(header+24,entry,8)"),
                    "the places the loader compares: bytes 0 to 6, 16 to 19, 24 to 31");
            // ownedDirectory: private-trace must be mode 0700.
            check(loader.contains("openat(parent,\"" + InstallState.PRIVATE_TRACE + "\",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW)")
                    && loader.contains("(info.st_mode&0777)!=0700"), "the loader's rule for the directory: mode 0700");
            check(loader.contains("constexprcharLiveLibraryName[]=\"" + InstallState.LIVE_LIBRARY + "\";"), "the file the loader opens for a game start");
            // claimRequest: the marker that says "this request was accepted", created before anything is loaded.
            Matcher marker = Pattern.compile("charmarker\\[38\\]=\"([^\"]+)\";").matcher(loader);
            check(marker.find(), "the loader builds a marker name");
            equal(marker.group(1), LaunchOutcome.CLAIM_PREFIX, "the marker's prefix");
            check(loader.contains("openat(parent,marker,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0400)"), "the marker is created by the claim");
            String activity = text(gameWindow);
            check(activity.indexOf("claimRequest(getFilesDir().getAbsolutePath(),requestId)") > 0
                    && activity.indexOf("claimRequest(getFilesDir().getAbsolutePath(),requestId)") < activity.indexOf("verifyAndCheck(expected,requestId,manifestHash)"),
                    "the game window claims the request before it checks and loads the game code");
        });

        run("LauncherContract.everyRefusalWordOfTheGameWindowBelongsToAFamilyTheLauncherHasASentenceFor", () -> {
            String activity = text(gameWindow);
            java.util.Set<String> words = new java.util.TreeSet<String>();
            Matcher word = Pattern.compile("\"(REJECT_[A-Z_]*)\"").matcher(activity);
            while (word.find()) {
                words.add(word.group(1));
            }
            check(words.size() >= 10, "the game window's refusal words were found: " + words);
            // The families LaunchAdvice.afterStart tells apart (LaunchAdviceTest pins the sentence of each), and the
            // words that are never written to the refusal file because the request was not accepted at all.
            String[] families = {"REJECT_LIBRARY_", "REJECT_SIZE", "REJECT_HASH", "REJECT_MANIFEST_", "REJECT_NATIVE_RUN", "REJECT_RUN_SETUP_",
                    "REJECT_EMPTY_PROTOCOL", "REJECT_PROCESS", "REJECT_SURFACE", "REJECT_IO_", "REJECT_ABI_",
                    "REJECT_REQUEST", "REJECT_USED_REQUEST", "REJECT_REQUEST_STORAGE"};
            for (String one : words) {
                if (one.equals("REJECT_")) continue;   // not a word: the beginning by which refusals are recognised
                boolean known = false;
                for (String family : families) {
                    known |= family.endsWith("_") ? one.startsWith(family) : one.equals(family);
                }
                check(known, "a refusal word nobody has written a sentence for: " + one);
            }
            if (activity.contains(LaunchOutcome.REFUSAL_FILE + "\"")) {
                // The earlier form of the change (lane menu-launcher's): the activity writes the file itself.
                check(activity.contains("startsWith(\"REJECT_\")"), "the game window writes the file for its refusals");
                return;
            }
            // A game window that knows GameWindowStatus at all is held to every part of the connection.
            if (!activity.contains("GameWindowStatus.")) {
                System.out.println("     NOTE the game window does not write " + LaunchOutcome.REFUSAL_FILE + " yet (cross-lane change to"
                        + " PrivateTraceActivity.java not made): a refusal after the request was accepted is shown only in the game window."
                        + " The launcher's reading of that file is NOT connected to a writer.");
                return;
            }
            // show(String) is where every status word passes. Its first statement after the log line hands the word,
            // with the request this window accepted, to GameWindowStatus.report (GameWindowStatusTest follows the
            // file from there to the Play page).
            String show = body(activity, "private void show(String result){");
            int report = show.indexOf("GameWindowStatus.report(getFilesDir(),acceptedLauncherRequest,result);");
            check(report >= 0, "show() hands every status word to GameWindowStatus.report");
            check(report < show.indexOf("return;"), "before anything in show() can return");
            // The request is "accepted" only once the claim succeeded, and before the game code is checked and loaded.
            int claim = activity.indexOf("claimRequest(getFilesDir().getAbsolutePath(),requestId)");
            int refused = activity.indexOf("if(claimed!=0){");
            int accepted = activity.indexOf("acceptedLauncherRequest=requestId;");
            check(claim > 0 && refused > claim && accepted > refused && accepted < activity.indexOf("startRunner=()->{"),
                    "the accepted request is noted after the claim succeeded and before the check of the game code starts");
            equal(1, count(activity, "acceptedLauncherRequest="), "and nowhere else");
        });

        run("LauncherContract.theHandedOutBuildsGameWindowShowsNoDeveloperTextOverTheGame", () -> {
            String activity = text(gameWindow);
            if (!activity.contains("GameWindowStatus.")) {
                System.out.println("     NOTE the game window of this tree draws its developer label over every start (cross-lane change to"
                        + " PrivateTraceActivity.java not made): GameWindowStatus.label is NOT connected to the window.");
                return;
            }
            // Which window is plain is GameWindowStatus's answer for "is this build debuggable, and where are its
            // files" (the debug build's switch file), asked once, before anything is shown (the first show() of
            // onCreate may be a refusal of the request itself).
            int decided = activity.indexOf("plainWindow=GameWindowStatus.plainWindow((getApplicationInfo().flags&ApplicationInfo.FLAG_DEBUGGABLE)!=0,getFilesDir());");
            check(decided > 0 && decided < activity.indexOf("show(\"REJECT_REQUEST\")"),
                    "plainWindow is GameWindowStatus.plainWindow(debuggable, files), decided before the first status word is shown");
            equal(1, count(activity, "plainWindow="), "and nowhere else: the switch file is read once, at the window's start");
            // The grey page with the status word (the "CHECKING PRIVATE LIBRARY" card) is the same view: a plain
            // window hides it before the first word is shown.
            String create = activity.substring(decided, activity.indexOf("ActivityManager manager="));
            check(create.contains("if(plainWindow){") && create.contains("status.setVisibility(View.GONE);"),
                    "a plain window hides the status view before anything is shown");
            String show = body(activity, "private void show(String result){");
            int plain = show.indexOf("if(plainWindow) {");
            check(plain >= 0, "show() has a branch for the plain window");
            check(show.indexOf("String text=GameWindowStatus.label(result);") >= 0
                    && show.indexOf("String text=GameWindowStatus.label(result);") < plain, "the plain window's text is GameWindowStatus.label's");
            String branch = show.substring(plain, show.indexOf("return;", plain));
            check(branch.contains("status.setVisibility(text==null?View.GONE:View.VISIBLE);"), "no text: the label is not drawn at all");
            check(!branch.contains("Development") && !branch.contains("unverified"), "the developer label is not in that branch");
            // The developer label stays where it was, after the plain window's branch has returned: debug builds only.
            check(show.indexOf("Development build") > show.indexOf("return;", plain), "the developer label is reached only when the window is not plain");
            // A refused or stopped start can be left with B in a plain window and in every start the launcher made.
            check(show.contains("leaveWithB=(plainWindow||launcherStart) && text!=null;"), "B leaves a refused or stopped start");
            // A launcher start is told from a script start by the launcher's own list of runs, before the request is claimed.
            int told = activity.indexOf("launcherStart=InputDispatchOption.closesUsedRequest(1,getFilesDir(),requestId);");
            check(told > 0 && told < activity.indexOf("claimRequest(getFilesDir().getAbsolutePath(),requestId)"),
                    "launcherStart is decided from settings/launcher-runs.txt before the request is claimed");
            // The window no longer refuses to start in a build that is not debuggable: that build's menu is its only caller.
            String request = activity.substring(activity.indexOf("if(!privateProcess ||"), activity.indexOf("show(\"REJECT_REQUEST\")"));
            check(!request.contains("FLAG_DEBUGGABLE"), "the request check does not ask whether the app is debuggable");
        });

        run("LauncherContract.stickOnlyPlayAndTheCaptureComboAreConnectedToTheGameWindow", () -> {
            // The decisions are pure classes with their own tests (PhysicalMotionLogTest, CaptureComboTest); this holds
            // the one line each that connects them to the window, which no host test can run.
            String input = text(new File(helperDirectory, "PrivateGamepadInput.java")).replaceAll("\\s+", "");
            check(input.contains("privatestaticfinalStringTAG=\"RecompNativeInput\";"), "the input path logs under the tag RecompNativeInput");
            String motion = input.substring(input.indexOf("booleanmotion(MotionEventevent){"));
            int mapped = motion.indexOf("mapper.motion("), asked = motion.indexOf("if(motionLog.moved(event.getDeviceId(),deviceId,");
            check(mapped > 0 && asked > mapped, "every motion event the game got is put to PhysicalMotionLog, after it was mapped");
            check(motion.substring(asked).contains("event.getEventTime()))Log.i(TAG,PhysicalMotionLog.LINE);"),
                    "with the event's own time, and its answer is the only thing that writes the line");
            // (How often the line is written, also by the timer of a stick that is held still, is run on the host since
            // the repair: step window-rig, GameWindowRigTest. This test no longer counts the places in the source.)
            String activity = text(gameWindow).replaceAll("\\s+", "");
            String keys = activity.substring(activity.indexOf("publicbooleandispatchKeyEvent(KeyEventevent){"));
            keys = keys.substring(0, keys.indexOf("@Override"));
            int combo = keys.indexOf("if(capture!=null&&capture.key(event))returntrue;");
            check(combo > 0 && combo < keys.indexOf("gamepadInput.key(event)"), "a key goes to the capture combo before it goes to the game");
            check(activity.contains("capture=CaptureController.attach(this,surface,content,e->gamepadInput!=null&&gamepadInput.key(e));"),
                    "the combo is attached where the game's picture is made, and gives Select and A back to the game's input");
            // A capture made with a real controller leaves a "PHYSICAL" line of its own (its two presses never reach
            // the game and so never leave theirs); injected input does not.
            String controllerSource = text(new File(helperDirectory, "CaptureController.java")).replaceAll("\\s+", "");
            check(controllerSource.contains("staticfinalStringPHYSICAL_CAPTURE=\"PHYSICAL")
                    && controllerSource.contains("if(combo.device()>=0)io.post(()->Log.i(TAG,PHYSICAL_CAPTURE));"),
                    "a capture from a real controller is logged as a PHYSICAL line under RecompNativeInput");
            // The renderer's one-frame record is armed in a debuggable build only.
            String controller = text(new File(helperDirectory, "CaptureController.java")).replaceAll("\\s+", "");
            check(controller.contains("booleancensusOn=(activity.getApplicationInfo().flags&ApplicationInfo.FLAG_DEBUGGABLE)!=0;")
                    && controller.contains("census=newCaptureCensus(activity.getFilesDir(),censusOn);"),
                    "the capture asks for the renderer's one-frame record only when the app is debuggable");
        });

        run("LauncherContract.theControllerCheckShowsControllerCheckTextsWordsOnly", () -> {
            String activity = text(controllerCheck);
            if (!activity.contains("ControllerCheckText.")) {
                System.out.println("     NOTE the controller check of this tree writes its own developer text (cross-lane change to"
                        + " ControllerCheckActivity.java not made): ControllerCheckText is NOT connected to the screen.");
                return;
            }
            // Every setText of the activity takes ControllerCheckText's words; the activity has no sentence of its own.
            java.util.regex.Matcher shown = Pattern.compile("status\\.setText\\(([A-Za-z\"]+)").matcher(activity);
            int texts = 0;
            while (shown.find()) {
                ++texts;
                equal("ControllerCheckText", shown.group(1), "what a setText of the controller check shows");
            }
            check(texts >= 2, "the two screens (readable, unreadable) were found: " + texts);
            // The native snapshot's fields reach the text in the order the text names them: buttons, triggers, sticks.
            check(activity.contains("ControllerCheckText.screen(input.description(),s[2],s[3],s[4],s[5],s[6],s[7],s[8],"),
                    "buttons s[2], triggers s[3] s[4], sticks s[5] to s[8]");
        });

        run("LauncherContract.exitReasonNumbersAreAndroids", () -> {
            Class<?> info;
            try {
                info = Class.forName("android.app.ApplicationExitInfo");
            } catch (ClassNotFoundException absent) {
                System.out.println("     NOTE android.jar is not on the class path: this check did not run");
                return;
            }
            equal(info.getField("REASON_LOW_MEMORY").getInt(null), LaunchOutcome.ANDROID_REASON_LOW_MEMORY, "REASON_LOW_MEMORY");
            equal(info.getField("REASON_CRASH").getInt(null), LaunchOutcome.ANDROID_REASON_CRASH, "REASON_CRASH");
            equal(info.getField("REASON_CRASH_NATIVE").getInt(null), LaunchOutcome.ANDROID_REASON_CRASH_NATIVE, "REASON_CRASH_NATIVE");
            equal(info.getField("REASON_ANR").getInt(null), LaunchOutcome.ANDROID_REASON_ANR, "REASON_ANR");
            // The reason of an ordinary end (the game window ends its own process with a signal) is not one of them.
            equal(LaunchOutcome.Exit.UNKNOWN_OR_ORDINARY, LaunchOutcome.exitFor(info.getField("REASON_SIGNALED").getInt(null)), "REASON_SIGNALED is ordinary");
            equal(LaunchOutcome.Exit.UNKNOWN_OR_ORDINARY, LaunchOutcome.exitFor(info.getField("REASON_USER_REQUESTED").getInt(null)), "REASON_USER_REQUESTED is ordinary");
        });

        LauncherTestSupport.finish("LauncherContractTest");
    }

    /** The nearest directory at or above the current one that holds the app's debug manifest. */
    private static File findRepository() {
        File at = new File(System.getProperty("user.dir")).getAbsoluteFile();
        for (int level = 0; at != null && level < 8; ++level, at = at.getParentFile()) {
            if (new File(at, "android/app/src/debug/AndroidManifest.xml").isFile()) {
                return at;
            }
        }
        throw new IllegalArgumentException("run from inside the repository, or give the repository root as argument");
    }

    /** True when the bytes occur in the file. Reads it once through a 1 MiB buffer; a match may lie across two reads. */
    private static boolean occurs(File file, byte[] wanted) throws Exception {
        byte[] buffer = new byte[(1 << 20) + wanted.length];
        int kept = 0;
        try (java.io.InputStream in = new java.io.FileInputStream(file)) {
            for (int count; (count = in.read(buffer, kept, buffer.length - kept)) != -1; ) {
                int filled = kept + count;
                outer:
                for (int at = 0; at + wanted.length <= filled; ++at) {
                    if (buffer[at] != wanted[0]) continue;
                    for (int index = 1; index < wanted.length; ++index) {
                        if (buffer[at + index] != wanted[index]) continue outer;
                    }
                    return true;
                }
                kept = Math.min(filled, wanted.length - 1);
                System.arraycopy(buffer, filled - kept, buffer, 0, kept);
            }
        }
        return false;
    }

    /** The text of a method from its first line to the closing brace at the method's own indent. */
    private static String body(String source, String firstLine) {
        int start = source.indexOf(firstLine);
        check(start >= 0, "the method is there: " + firstLine);
        int end = source.indexOf("\n    }\n", start);
        check(end > start, "the method ends: " + firstLine);
        return source.substring(start, end);
    }

    private static int count(String text, String part) {
        int count = 0;
        for (int at = text.indexOf(part); at >= 0; at = text.indexOf(part, at + 1)) ++count;
        return count;
    }

    private static String text(File file) throws Exception {
        return new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
    }
}
