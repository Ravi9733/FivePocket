package dev.recompbootstrap.smoke;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStream;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Puts the game code that travels inside the APK where the game window expects it. Pure Java.
 *
 * Runs on the first start after an install and after every app update (ShipStartActivity asks check() on every
 * start). From the bundled record (ShipConfig) it makes, all under the app's files directory:
 *
 *   private-trace/private-trace-live.so          the game library, mode 0400, one name (what the loader requires);
 *   private-trace/(shader pack name)             the shader pack, under the literal name the library opens;
 *   private-trace/run-(id)/arguments.bin         the start request Play copies, mode 0400;
 *   settings/launch-template.txt                 names that run;
 *   settings/qualified-code.txt                  the library's SHA-256: Play compares the game code with it;
 *   private-trace/inputs/(sha256).bin, private-trace/modules/boot/..., private-trace/run-(id)/decoded.image.bin
 *                                                the pinned game data files that travel inside the APK
 *                                                (ShipConfig.bundledData), mode 0400, the image last;
 *   private-trace/(directories)                  the cache directories the start request names (empty);
 *   private-trace/diag-...                       the switch files of the standing configuration (empty), except the
 *                                                frame-rate counter's (COUNTER_SWITCH: off until the player turns it on);
 *   settings/ship-installed.txt                  the record's SHA-256, written last.
 *
 * Safe to run twice: every step first looks at what is there, and a piece that already has the recorded size and
 * SHA-256 is not rewritten. Safe to interrupt: a piece is written under a ".ship-part" name and gets its real name
 * only after its size and SHA-256 were checked; the last step is the mark that everything is in place, so an
 * interrupted run is finished by the next start. The part files a killed run left behind are removed before the
 * free space is asked for, so that the app's own left-overs never make it refuse for lack of room. Nothing else is
 * touched: no file is removed except its own part files and, while the player has never saved settings, the
 * frame-rate counter's switch file (counterOffUnlessSaved); a switch file that exists is never rewritten (one may
 * hold values), and nothing under files/saves, files/profile or the player's own game data is opened at all.
 *
 * An update whose bundled start request has another run id: the new run gets its own start request and the image
 * (out of the APK when it is bundled; otherwise the image the player imported is moved over from the run that
 * holds it), and only then the pin is written, so that Play never copies an older version's start request. Just
 * before the pin changes, while it still names the earlier run, the two files an earlier version of this class put
 * into that run's folder (its start request and its copy of the image, 31 MB in the real build) are removed, and
 * the folder when it is then empty: nobody could reach them in a build that is not debuggable. They go before the
 * pin and not after the mark, because the pin is the only record of which run was the earlier one: a start that is
 * killed after the pin was rewritten could never find them again.
 *
 * Before the free space is asked for, the half files that a killed copy of game data left behind are removed as
 * well (the two-disc setup's disc-staging/ and the folder import's staging), unless such a copy runs right now.
 */
final class ShipFirstRun {
    /** The SHA-256 of the record whose pieces are installed and checked; written last, read by check(). */
    static final String STAMP = "settings/ship-installed.txt";
    /** Exists once the user has saved settings: from then on the settings screen owns its switch files. */
    static final String SETTINGS_JSON = "settings/settings.json";
    /**
     * The frame-rate counter's switch file, relative to the files directory (the settings screen owns it:
     * RuntimeMapping.FPS_OVERLAY). The record lists it among the standing switch files, and the record stands as it
     * is; but this class does not create it: the counter is off in a fresh install until the player turns it on
     * (Settings, Frame Rate, Frame-Rate Counter). The lead's decision of 2026-10-06. Every other switch file of the
     * record is created as before.
     */
    static final String COUNTER_SWITCH = RuntimeMapping.FPS_OVERLAY;
    /** Space that must stay free beyond the bytes still to write. */
    static final long RESERVE_BYTES = 64L << 20;
    static final String BUSY = "The game code is already being prepared; wait a moment.";
    /** Play's reason when the bundled game code is not in place (the first screen did not finish). */
    static final String NOT_INSTALLED = "this app version's game code is not in place yet; close the app and open it again";
    private static final String NOT_USABLE = "The game code inside this app is not usable: ";
    /** Play's reason when the game code file no longer is the one this app version carries. */
    static final String CODE_CHANGED = "the game code on this device is not the one this app version carries; close the app"
            + " and open it again to put it back";
    /** Play's reason when another file this app version carries (the shader pack, a bundled piece) is absent or changed. */
    static final String PIECE_CHANGED = "a file this app version carries is missing or was changed on this device; close the"
            + " app and open it again to put it back";

    /** The steps, in order; told to Progress so that a screen can say what is happening. */
    enum Step { RECORD, SPACE, GAME_CODE, SHADERS, START_REQUEST, GAME_DATA, SWITCHES, FINISH }

    interface Progress {
        /** bytesTotal is 0 for a step that copies nothing large. Called on the worker thread. */
        void at(Step step, long bytesDone, long bytesTotal);
    }

    enum State {
        /** This APK carries no game code (a developer build): nothing to do, the app behaves as before. */
        NOT_BUNDLED,
        /** The bundled game code is in place. */
        CURRENT,
        /** A first start, an update, or an interrupted run: run() has work to do. */
        NEEDED
    }

    static final class Result {
        /** False when the APK carries no game code; nothing was done then. */
        final boolean bundled;
        /** null on success; otherwise one plain sentence. */
        final String error;
        /** The bundled record; null when there is none or it cannot be read. */
        final ShipConfig config;
        /** What this run created or replaced, relative to the files directory. Empty when everything was in place. */
        final List<String> changed;
        /**
         * One line per bundled piece this run looked at, for the log: "(place relative to the files directory)
         * bytes=(n) sha256=(first 12 digits) copied" or "... kept" (it was in place with that SHA-256 already).
         */
        final List<String> report;
        /** The bytes of half files of a killed game data copy that this run removed before it asked for free space. */
        final long leftOversFreed;

        private Result(boolean bundled, String error, ShipConfig config, List<String> changed, List<String> report) {
            this(bundled, error, config, changed, report, 0);
        }

        private Result(boolean bundled, String error, ShipConfig config, List<String> changed, List<String> report,
                long leftOversFreed) {
            this.bundled = bundled;
            this.error = error;
            this.config = config;
            this.changed = Collections.unmodifiableList(new ArrayList<String>(changed));
            this.report = Collections.unmodifiableList(new ArrayList<String>(report));
            this.leftOversFreed = leftOversFreed;
        }

        boolean ok() {
            return bundled && error == null;
        }
    }

    private static final AtomicBoolean RUNNING = new AtomicBoolean();

    private ShipFirstRun() {}

    /** The bundled record, or null when this APK carries none. IOException: it is there but cannot be used. */
    static ShipConfig config(ShipBundle bundle) throws IOException {
        InputStream in;
        try {
            in = bundle.open(ShipConfig.ASSET);
        } catch (FileNotFoundException none) {
            return null;
        }
        return ShipConfig.parse(ShipFiles.readAll(in, ShipConfig.MAX_BYTES));
    }

    /**
     * Whether run() has work to do. Cheap: it reads the record and a few small files and compares sizes and modes; it
     * does not hash the large pieces. changedCode() is that other half: ShipReady and ShipStart ask it wherever the
     * answer is shown or acted on, and Play hashes both files itself. Never writes, never throws.
     */
    static State check(ShipBundle bundle, File filesDir) {
        ShipConfig config;
        try {
            config = config(bundle);
        } catch (IOException | RuntimeException unusable) {
            return State.NEEDED;   // run() says why
        }
        if (config == null) return State.NOT_BUNDLED;
        return installed(config, filesDir) ? State.CURRENT : State.NEEDED;
    }

    /** True when the mark of this record is there and every piece has its place, size and mode. Read-only. */
    static boolean installed(ShipConfig config, File filesDir) {
        try {
            if (!(config.sha256 + "\n").equals(smallText(new File(filesDir, STAMP)))) return false;
        } catch (IOException | RuntimeException unreadable) {
            return false;
        }
        return placed(config, filesDir);
    }

    /**
     * Everything installed() asks except the mark: each bundled piece has its place, size and mode, the start
     * request beside the pin is the bundled one (by SHA-256: it is a few kilobytes), and the two records name this
     * version. The game code and the shader pack are compared by size here; Play hashes both.
     */
    private static boolean placed(ShipConfig config, File filesDir) {
        try {
            if (!config.filesRoot.equals(filesDir.getAbsolutePath())) return false;
            File root = new File(filesDir, InstallState.PRIVATE_TRACE);
            File library = new File(root, InstallState.LIVE_LIBRARY);
            if (!ShipFiles.regularFileIn(filesDir, library) || library.length() != config.library.bytes
                    || !"r--------".equals(ShipFiles.mode(library))) {
                return false;
            }
            File pack = new File(root, config.shaderPackName);
            if (!ShipFiles.regularFileIn(filesDir, pack) || pack.length() != config.shaderPack.bytes) return false;
            File arguments = new File(new File(root, "run-" + config.runId), LaunchRequest.ARGUMENTS);
            if (!ShipFiles.regularFileIn(filesDir, arguments) || arguments.length() != config.startRequest.bytes) return false;
            if (!config.startRequest.sha256.equals(ShipFiles.sha256Of(arguments))) return false;
            if (!(config.runId + "\n").equals(smallText(new File(filesDir, InstallState.TEMPLATE_PIN)))) return false;
            if (!(config.library.sha256 + "\n").equals(smallText(new File(filesDir, LaunchRequest.QUALIFIED_CODE)))) return false;
            for (ShipConfig.BundledData piece : config.bundledData) {
                File file = ShipData.place(config, filesDir, piece.path);
                if (!ShipFiles.regularFileIn(filesDir, file) || file.length() != piece.bytes) return false;
            }
            // The folders the start request names are the app's own too: one that is reached through a link, or is a
            // link or a file itself, is not in place. (One that is simply absent is the launcher's own look to report.)
            for (String directory : config.directories) {
                File made = new File(root, directory);
                if (ShipFiles.present(made) && !ShipFiles.directoryIn(filesDir, made)) return false;
            }
            // The same for the folders the player's own files go to (the files themselves are ShipData's to count):
            // a link under such a folder's name, also one that points nowhere, is for the first start to replace.
            for (ShipConfig.DataFile pinned : config.pinnedData) {
                File folder = ShipData.place(config, filesDir, pinned.path).getParentFile();
                if (ShipFiles.present(folder) && !ShipFiles.directoryIn(filesDir, folder)) return false;
            }
            return true;
        } catch (IOException | RuntimeException unreadable) {
            return false;
        }
    }

    /**
     * The game code file and the shader file by their content: the names (relative to the files directory) of those
     * whose SHA-256 is not the recorded one, the game code first; empty when both are the files this app version
     * carries. placed() compares these two by size only, because they are large (339 MB and 73 MB in the real
     * build, well under a second to read on a phone); this is the other half, asked wherever "ready" is said, so
     * that the menu never reads Ready over a file Play would then refuse. A file that cannot be read counts as changed.
     */
    static List<String> changedCode(ShipConfig config, File filesDir) {
        List<String> changed = new ArrayList<String>();
        File root = new File(filesDir, InstallState.PRIVATE_TRACE);
        String[] names = {InstallState.LIVE_LIBRARY, config.shaderPackName};
        ShipConfig.Piece[] pieces = {config.library, config.shaderPack};
        for (int at = 0; at < names.length; ++at) {
            boolean same;
            try {
                same = pieces[at].sha256.equals(ShipFiles.sha256Of(new File(root, names[at])));
            } catch (IOException | RuntimeException unreadable) {
                same = false;
            }
            if (!same) changed.add(InstallState.PRIVATE_TRACE + "/" + names[at]);
        }
        return changed;
    }

    /**
     * Makes the counter's default hold in an install whose player never saved settings: removes the counter's switch
     * file when it is there (earlier versions of this class created it on every fresh install, so an update finds
     * it). Once settings were saved (settings/settings.json exists) the file is the settings screen's and is left
     * exactly as it is, there or not: what the player saved is kept. Removes only a regular file under that one
     * name, reached through real folders; a link or a folder under the name is left alone. Returns true when it
     * removed the file. Never throws. The game reads the switch when it starts, so this is called on the app's
     * start (ShipStart) and in run(), never while the game runs.
     */
    static boolean counterOffUnlessSaved(File filesDir) {
        try {
            if (ShipFiles.present(new File(filesDir, SETTINGS_JSON))) return false;
            File counter = new File(filesDir, COUNTER_SWITCH);
            if (!ShipFiles.regularFileIn(filesDir, counter)) return false;
            return java.nio.file.Files.deleteIfExists(counter.toPath());
        } catch (IOException | RuntimeException kept) {
            return false;
        }
    }

    /** Removes the mark, so that the next start checks every piece again (and puts back one that changed). */
    static void forget(File filesDir) {
        ShipFiles.deleteQuietly(new File(filesDir, STAMP));
    }

    /**
     * Does the work. Call it off the UI thread: it hashes and copies several hundred megabytes on a first start.
     * Never throws; a failure is one sentence in Result.error, and whatever was completed stays for the next run.
     */
    static Result run(ShipBundle bundle, File filesDir, Progress progress, ShipFiles.Space space) {
        if (!RUNNING.compareAndSet(false, true)) {
            return new Result(true, BUSY, null, Collections.<String>emptyList(), Collections.<String>emptyList());
        }
        try {
            return runLocked(bundle, filesDir, progress, space);
        } finally {
            RUNNING.set(false);
        }
    }

    /**
     * Removes what a run of this class that was killed inside a copy left behind: its part files beside the game
     * code, the shader pack, the start request, the bundled pieces and its records. Returns the bytes freed. Only
     * names ending in ".ship-part" are removed, and only in the places this class writes to.
     */
    static long removeLeftOvers(ShipConfig config, File filesDir) {
        File root = new File(filesDir, InstallState.PRIVATE_TRACE);
        long freed = ShipFiles.removeParts(root, 0);
        freed += ShipFiles.removeParts(new File(root, "inputs"), 0);
        freed += ShipFiles.removeParts(new File(root, "modules"), ShipDataManifest.MAX_MODULE_DEPTH + 1);
        freed += ShipFiles.removeParts(new File(root, "run-" + config.runId), 0);
        for (String record : new String[] {STAMP, InstallState.TEMPLATE_PIN, LaunchRequest.QUALIFIED_CODE}) {
            ShipFiles.deleteQuietly(ShipFiles.partOf(new File(filesDir, record)));
        }
        return freed;
    }

    /**
     * Removes the half files a killed copy of game data left in the app's storage: the two-disc setup's
     * (private-trace/disc-staging/) and the folder import's. Each engine removes its own, and only while none of its
     * copies runs. They are never read again; only their room is needed. Returns the bytes freed.
     */
    static long removeCopyLeftOvers(File filesDir) {
        return ShipDataImport.removeLeftOvers(filesDir)
                + SetupDiscCopy.removeLeftOvers(new File(filesDir, InstallState.PRIVATE_TRACE));
    }

    /** The run id in settings/launch-template.txt, or null when the file is absent or holds no run id. */
    private static String pinnedRun(File filesDir) {
        try {
            String text = smallText(new File(filesDir, InstallState.TEMPLATE_PIN));
            String id = text == null ? null : text.trim();
            return LaunchRequest.isId(id) ? id : null;
        } catch (IOException | RuntimeException unreadable) {
            return null;
        }
    }

    /**
     * After an update whose start request has another run id: removes, from the run an earlier version of this
     * class had pinned, the two files it had put there (the start request and the image), and the folder when it is
     * then empty. Nothing else in it is touched. Returns what was removed, relative to the files directory.
     */
    private static List<String> removeEarlierRun(File filesDir, String earlierRun) {
        List<String> removed = new ArrayList<String>();
        File run = new File(new File(filesDir, InstallState.PRIVATE_TRACE), "run-" + earlierRun);
        if (!ShipFiles.directory(run)) return removed;
        for (String name : new String[] {LaunchRequest.ARGUMENTS, ShipDataManifest.IMAGE}) {
            File file = new File(run, name);
            if (ShipFiles.regularFile(file) && file.delete()) {
                removed.add("-" + InstallState.PRIVATE_TRACE + "/" + run.getName() + "/" + name);
            }
        }
        String[] left = run.list();
        if (left != null && left.length == 0 && run.delete()) {
            removed.add("-" + InstallState.PRIVATE_TRACE + "/" + run.getName() + "/");
        }
        return removed;
    }

    private static Result runLocked(ShipBundle bundle, File filesDir, Progress progress, ShipFiles.Space space) {
        List<String> changed = new ArrayList<String>();
        List<String> report = new ArrayList<String>();
        ShipConfig config = null;
        long leftOversFreed = 0;
        try {
            progress.at(Step.RECORD, 0, 0);
            config = config(bundle);
            if (config == null) return new Result(false, null, null, changed, report);

            // The game library holds its file names as literal absolute paths: it only works in that one place.
            String place = filesDir.getAbsolutePath();
            if (!config.filesRoot.equals(place)) {
                return failed(config, changed, report, "This app only works for the device's main user: its game code was built for "
                        + config.filesRoot + ", but Android keeps this app's files in " + place + ".");
            }
            File root = new File(filesDir, InstallState.PRIVATE_TRACE);

            // The start request is small: it is read and checked whole before anything is written.
            byte[] request = readPiece(bundle, config.startRequest, "start request");
            String refusal = LaunchRequest.checkArguments(request, root.getAbsolutePath(), config.runId);
            if (refusal != null) {
                return failed(config, changed, report, "The start request inside this app does not fit this device: " + refusal + ".");
            }
            String prefix = root.getAbsolutePath() + "/";
            for (String named : LaunchRequest.referencedInputs(request, place)) {
                String relative = named.substring(prefix.length());
                if (!config.requiredInputs.contains(relative) && !config.directories.contains(relative)) {
                    return failed(config, changed, report, "The start request inside this app names " + relative
                            + ", which its record does not list. Install the app again.");
                }
            }

            // What is already there (an update that did not change the game code, a second run, the owner's phone).
            File library = new File(root, InstallState.LIVE_LIBRARY);
            File pack = new File(root, config.shaderPackName);
            boolean libraryInPlace = inPlace(filesDir, library, config.library);
            boolean packInPlace = inPlace(filesDir, pack, config.shaderPack);
            // The bundled game data, the image last: until the image is there the new run is no start request the
            // launcher would copy, so a run that is killed among the pieces never leaves a half-made one behind.
            List<ShipConfig.BundledData> pieces = new ArrayList<ShipConfig.BundledData>();
            for (ShipConfig.BundledData piece : config.bundledData) {
                if (!ShipDataManifest.IMAGE.equals(piece.path)) pieces.add(piece);
            }
            if (config.bundled(ShipDataManifest.IMAGE) != null) pieces.add(config.bundled(ShipDataManifest.IMAGE));
            List<ShipConfig.BundledData> piecesToCopy = new ArrayList<ShipConfig.BundledData>();
            long piecesBytes = 0, piecesBytesToCopy = 0;
            for (ShipConfig.BundledData piece : pieces) {
                piecesBytes += piece.bytes;
                if (!dataInPlace(filesDir, ShipData.place(config, filesDir, piece.path), piece)) {
                    piecesToCopy.add(piece);
                    piecesBytesToCopy += piece.bytes;
                }
            }

            progress.at(Step.SPACE, 0, 0);
            long need = (libraryInPlace ? 0 : config.library.bytes) + (packInPlace ? 0 : config.shaderPack.bytes)
                    + piecesBytesToCopy;
            // What a killed run left half-written is this class's own and is never read again: it goes before the
            // free space is asked for, or it would be counted against the copy that replaces it.
            removeLeftOvers(config, filesDir);
            // The same for the half file a killed copy of the player's game data left (up to 2.8 GB): without this an
            // update opened right after such a copy could be refused for lack of room for good.
            leftOversFreed = removeCopyLeftOvers(filesDir);
            // The run an earlier version of this app had installed and pinned (only a version that finished: its mark is there).
            String earlierRun = ShipFiles.present(new File(filesDir, STAMP)) ? pinnedRun(filesDir) : null;
            if (need > 0) {
                long free = space.usable(ShipFiles.existingAncestor(root));
                if (need + RESERVE_BYTES > free) {
                    return failed(config, changed, report, "Not enough free space for the game code: it needs "
                            + ShipFiles.amount(need + RESERVE_BYTES, true) + " and " + ShipFiles.amount(free, false)
                            + " is free. Free some space, then try again.");
                }
            }

            // The loader accepts files/private-trace only with mode 0700 (private_trace_jni.cpp, ownedDirectory).
            ShipFiles.makeDirectoryIn(filesDir, root, ShipFiles.OWNER_ONLY, true);
            File settings = new File(filesDir, "settings");
            ShipFiles.makeDirectoryIn(filesDir, settings, ShipFiles.OWNER_ONLY, false);

            progress.at(Step.GAME_CODE, libraryInPlace ? config.library.bytes : 0, config.library.bytes);
            if (!libraryInPlace) {
                String problem = install(bundle, config.library, library, Step.GAME_CODE, progress, 0, config.library.bytes,
                        "game code", true);
                if (problem != null) return failed(config, changed, report, problem);
                changed.add(InstallState.PRIVATE_TRACE + "/" + InstallState.LIVE_LIBRARY);
            } else if (readOnly(library)) {
                changed.add(InstallState.PRIVATE_TRACE + "/" + InstallState.LIVE_LIBRARY + " (mode)");
            }
            String refused = loaderProblem(library);
            if (refused != null) return failed(config, changed, report, NOT_USABLE + refused + ".");
            report.add(line(InstallState.PRIVATE_TRACE + "/" + InstallState.LIVE_LIBRARY, config.library.bytes,
                    config.library.sha256, !libraryInPlace));

            progress.at(Step.SHADERS, packInPlace ? config.shaderPack.bytes : 0, config.shaderPack.bytes);
            if (!packInPlace) {
                String problem = install(bundle, config.shaderPack, pack, Step.SHADERS, progress, 0, config.shaderPack.bytes,
                        "shader pack", false);
                if (problem != null) return failed(config, changed, report, problem);
                changed.add(InstallState.PRIVATE_TRACE + "/" + config.shaderPackName);
            }
            report.add(line(InstallState.PRIVATE_TRACE + "/" + config.shaderPackName, config.shaderPack.bytes,
                    config.shaderPack.sha256, !packInPlace));

            progress.at(Step.START_REQUEST, 0, 0);
            File run = new File(root, "run-" + config.runId);
            ShipFiles.makeDirectoryIn(filesDir, run, ShipFiles.OWNER_ONLY, true);
            File arguments = new File(run, LaunchRequest.ARGUMENTS);
            boolean requestWritten = !Arrays.equals(request, existing(arguments));
            if (requestWritten) {
                ShipFiles.writeSmall(arguments, request, ShipFiles.OWNER_READ);
                changed.add(InstallState.PRIVATE_TRACE + "/" + run.getName() + "/" + LaunchRequest.ARGUMENTS);
            } else {
                readOnly(arguments);
            }
            report.add(line(InstallState.PRIVATE_TRACE + "/" + run.getName() + "/" + LaunchRequest.ARGUMENTS,
                    config.startRequest.bytes, config.startRequest.sha256, requestWritten));

            progress.at(Step.GAME_DATA, piecesBytes - piecesBytesToCopy, piecesBytes);
            long piecesDone = piecesBytes - piecesBytesToCopy;
            for (ShipConfig.BundledData piece : pieces) {
                File target = ShipData.place(config, filesDir, piece.path);
                String relative = InstallState.PRIVATE_TRACE + "/" + ShipDataManifest.target(piece.path, config.runId);
                boolean copy = piecesToCopy.contains(piece);
                if (copy) {
                    ShipFiles.makeDirectoryIn(filesDir, target.getParentFile(), ShipFiles.OWNER_ONLY, false);
                    String problem = install(bundle, new ShipConfig.Piece(piece.asset, piece.bytes, piece.sha256), target,
                            Step.GAME_DATA, progress, piecesDone, piecesBytes, "game data file " + piece.path, false);
                    if (problem != null) return failed(config, changed, report, problem);
                    piecesDone += piece.bytes;
                    changed.add(relative);
                }
                report.add(line(relative, piece.bytes, piece.sha256, copy));
            }
            // The image the player imported under an earlier version's run, when this version does not carry it.
            String adopted = adoptImage(config, root);
            if (adopted != null) changed.add(adopted);

            // The earlier version's run goes while the pin still names it: once the pin is rewritten nothing says
            // which run that was, and a start killed between the pin and the mark would leave its files for good.
            // The new run is whole at this point; until the mark is written Play starts nothing anyway.
            if (earlierRun != null && !earlierRun.equals(config.runId)) changed.addAll(removeEarlierRun(filesDir, earlierRun));
            // The pin comes after the new run is whole: from here on Play copies this version's start request.
            record(filesDir, InstallState.TEMPLATE_PIN, config.runId + "\n", changed);
            record(filesDir, LaunchRequest.QUALIFIED_CODE, config.library.sha256 + "\n", changed);

            progress.at(Step.SWITCHES, 0, 0);
            // A link under the name of a folder that holds pinned files, also one that only holds the player's own
            // files (nothing of this run goes there), is replaced by a real folder here, so that the copy from the
            // discs finds a folder it can write to. A folder that is simply not there yet is left to whoever fills it.
            for (ShipConfig.DataFile pinned : config.pinnedData) {
                File folder = ShipData.place(config, filesDir, pinned.path).getParentFile();
                if (ShipFiles.present(folder) && !ShipFiles.directoryIn(filesDir, folder)) {
                    ShipFiles.makeDirectoryIn(filesDir, folder, ShipFiles.OWNER_ONLY, false);
                    changed.add(InstallState.PRIVATE_TRACE + "/" + ShipDataManifest.target(pinned.path, config.runId).replaceFirst("/[^/]*$", "") + "/");
                }
            }
            for (String directory : config.directories) {
                File made = new File(root, directory);
                if (!ShipFiles.directoryIn(filesDir, made)) {
                    ShipFiles.makeDirectoryIn(filesDir, made, ShipFiles.OWNER_ONLY, false);
                    changed.add(InstallState.PRIVATE_TRACE + "/" + directory + "/");
                }
            }
            // The switch files the settings screen owns are a default, not a rule: once the user has saved
            // settings, a switch that is absent is the user's choice and stays absent.
            boolean userSaved = ShipFiles.present(new File(filesDir, SETTINGS_JSON));
            Set<String> settingsOwned = new HashSet<String>(RuntimeMapping.ownedFiles());
            for (String name : config.switches) {
                File file = new File(root, name);
                String relative = InstallState.PRIVATE_TRACE + "/" + name;
                if (relative.equals(COUNTER_SWITCH)) continue;   // the frame-rate counter is never switched on by an install
                if (ShipFiles.present(file) || (userSaved && settingsOwned.contains(relative))) continue;
                FileChannel.open(file.toPath(), StandardOpenOption.WRITE, StandardOpenOption.CREATE_NEW).close();
                changed.add(relative);
            }
            // An update of an install whose settings were never saved: the counter an earlier version switched on goes.
            if (counterOffUnlessSaved(filesDir)) changed.add("-" + COUNTER_SWITCH);
            ShipFiles.syncDirectory(root);

            progress.at(Step.FINISH, 0, 0);
            // The mark says "everything is in place": it is written only after that was looked at once more.
            if (!placed(config, filesDir)) {
                return failed(config, changed, report, "The game code was copied but is not in place as this app version expects"
                        + " it. Open the app again; if this stays, install the app again.");
            }
            record(filesDir, STAMP, config.sha256 + "\n", changed);
            return new Result(true, null, config, changed, report, leftOversFreed);
        } catch (IOException e) {
            String reason = ShipFiles.reason(e);
            // Sentences of ShipConfig and readPiece are complete already; everything else is a file system failure.
            boolean sentence = reason.endsWith(".");
            return failed(config, changed, report, sentence ? reason : "Could not put the game code in place: " + reason + ".");
        } catch (RuntimeException e) {
            return failed(config, changed, report, "Preparing the game code stopped unexpectedly: " + ShipFiles.reason(e) + ".");
        }
    }

    private static Result failed(ShipConfig config, List<String> changed, List<String> report, String sentence) {
        return new Result(true, sentence, config, changed, report);
    }

    private static String line(String relative, long bytes, String sha256, boolean copied) {
        return relative + " bytes=" + bytes + " sha256=" + sha256.substring(0, 12) + (copied ? " copied" : " kept");
    }

    /** True when a bundled game data file is in place: a regular file with the pinned size and SHA-256. */
    private static boolean dataInPlace(File filesDir, File file, ShipConfig.BundledData piece) throws IOException {
        return ShipFiles.regularFileIn(filesDir, file) && file.length() == piece.bytes && piece.sha256.equals(ShipFiles.sha256Of(file));
    }

    /**
     * After an update whose start request has another run id, when the image is pinned but not inside the APK: the
     * image the player imported lies beside an earlier start request. It is moved (a rename inside private-trace,
     * no copy) into this version's run, after its size and SHA-256 were compared with the pin. Returns what was
     * created, relative to the files directory, or null when nothing was moved (the image is there, is bundled, or
     * no earlier run holds the pinned image; the first screen then asks for the game data).
     */
    private static String adoptImage(ShipConfig config, File root) throws IOException {
        ShipConfig.DataFile image = config.pinned(ShipDataManifest.IMAGE);
        if (image == null || config.bundled(ShipDataManifest.IMAGE) != null) return null;
        File run = new File(root, "run-" + config.runId);
        File target = new File(run, ShipDataManifest.IMAGE);
        if (ShipFiles.present(target)) return null;
        String[] names = root.list();
        if (names == null) return null;
        Arrays.sort(names);
        for (String name : names) {
            if (!name.startsWith("run-") || !LaunchRequest.isId(name.substring(4)) || name.equals(run.getName())) continue;
            File directory = new File(root, name);
            File candidate = new File(directory, ShipDataManifest.IMAGE);
            if (!ShipFiles.directory(directory) || !ShipFiles.regularFile(candidate) || candidate.length() != image.bytes) continue;
            if (!image.sha256.equals(ShipFiles.sha256Of(candidate))) continue;
            java.nio.file.Files.move(candidate.toPath(), target.toPath(), java.nio.file.StandardCopyOption.ATOMIC_MOVE);
            readOnly(target);
            ShipFiles.syncDirectory(run);
            return InstallState.PRIVATE_TRACE + "/" + run.getName() + "/" + ShipDataManifest.IMAGE;
        }
        return null;
    }

    /** True when the file is a regular file with one name that has the recorded size and SHA-256. */
    private static boolean inPlace(File filesDir, File file, ShipConfig.Piece piece) throws IOException {
        if (!ShipFiles.regularFileIn(filesDir, file) || file.length() != piece.bytes) return false;
        if (ShipFiles.linkCount(file) > 1) return false;   // a fresh copy has one name again
        return piece.sha256.equals(ShipFiles.sha256Of(file));
    }

    /**
     * Copies one bundled piece to its place, checked. null on success, else the sentence. program: the piece is the
     * game code, whose program header must be one the loader accepts before the file gets its name. doneBefore and
     * total are for the progress reports of a step that copies several pieces.
     */
    private static String install(ShipBundle bundle, final ShipConfig.Piece piece, File target, final Step step,
            final Progress progress, final long doneBefore, final long total, String what, boolean program) throws IOException {
        InputStream in;
        try {
            in = bundle.open(piece.asset);
        } catch (FileNotFoundException missing) {
            return damaged(what, "is missing");
        }
        File part = ShipFiles.partOf(target);
        ShipFiles.Copy copy = ShipFiles.copyVerified(in, part, piece.bytes, piece.sha256, new ShipFiles.CopyWatch() {
            @Override public boolean copied(long bytes) {
                progress.at(step, doneBefore + bytes, total);
                return true;
            }
        });
        switch (copy) {
            case VERIFIED:
                String header = program ? headerProblem(part) : null;
                if (header != null) {
                    ShipFiles.deleteQuietly(part);
                    return NOT_USABLE + header + ".";
                }
                ShipFiles.commit(part, target, ShipFiles.OWNER_READ);
                return null;
            case WRONG_HASH:
                return damaged(what, "does not match its recorded SHA-256");
            case TOO_SHORT:
                return damaged(what, "is shorter than recorded");
            default:
                return damaged(what, "is longer than recorded");
        }
    }

    private static String damaged(String what, String how) {
        return "This app's file is damaged: the " + what + " inside it " + how + ". Install the app again.";
    }

    /** A small bundled piece, whole, checked against its recorded size and SHA-256. */
    private static byte[] readPiece(ShipBundle bundle, ShipConfig.Piece piece, String what) throws IOException {
        InputStream in;
        try {
            in = bundle.open(piece.asset);
        } catch (FileNotFoundException missing) {
            throw new IOException(damaged(what, "is missing"));
        }
        byte[] data;
        try {
            data = ShipFiles.readAll(in, (int) ShipConfig.MAX_START_REQUEST_BYTES);
        } catch (IOException tooLong) {
            throw new IOException(damaged(what, "is longer than recorded"));
        }
        if (data.length != piece.bytes) {
            throw new IOException(damaged(what, data.length < piece.bytes ? "is shorter than recorded" : "is longer than recorded"));
        }
        if (!piece.sha256.equals(ShipFiles.sha256Hex(data))) {
            throw new IOException(damaged(what, "does not match its recorded SHA-256"));
        }
        return data;
    }

    /**
     * The loader's own rule for the game code file (private_trace_jni.cpp, libraryFile), asked here so that a file
     * it would refuse is reported when it is installed and not as a bare code in the game window: a 64-bit
     * little-endian ARM shared library without an entry point, mode 0400, one name. null when it would be accepted.
     */
    static String loaderProblem(File library) throws IOException {
        if (!"r--------".equals(ShipFiles.mode(library))) return "its file is not read-only for the app alone";
        if (ShipFiles.linkCount(library) > 1) return "its file has more than one name";
        return headerProblem(library);
    }

    /** The part of the loader's rule that is about the file's first 64 bytes. null when they are a library's. */
    private static String headerProblem(File library) throws IOException {
        byte[] header = new byte[64];
        try (FileChannel in = FileChannel.open(library.toPath(), StandardOpenOption.READ)) {
            java.nio.ByteBuffer buffer = java.nio.ByteBuffer.wrap(header);
            while (buffer.hasRemaining() && in.read(buffer) > 0) {
                // keep reading
            }
            if (buffer.hasRemaining()) return "it is shorter than a program header";
        }
        byte[] elf = {0x7f, 'E', 'L', 'F', 2, 1, 1};
        for (int at = 0; at < elf.length; ++at) {
            if (header[at] != elf[at]) return "it is not a 64-bit program file";
        }
        // e_type 3 (shared object), e_machine 0xb7 (AArch64), both little-endian
        if (header[16] != 3 || header[17] != 0 || (header[18] & 255) != 0xb7 || header[19] != 0) {
            return "it is not a 64-bit ARM library";
        }
        for (int at = 24; at < 32; ++at) {
            if (header[at] != 0) return "it is a program with an entry point, not a library";
        }
        return null;
    }

    /** Makes the file mode 0400 when it is not; true when the mode was changed. */
    private static boolean readOnly(File file) throws IOException {
        if ("r--------".equals(ShipFiles.mode(file))) return false;
        ShipFiles.setMode(file, ShipFiles.OWNER_READ);
        return true;
    }

    /** The file's bytes; null when it is absent, not a regular file or longer than a start request may be. */
    private static byte[] existing(File file) {
        try {
            return ShipFiles.readSmall(file, (int) ShipConfig.MAX_START_REQUEST_BYTES);
        } catch (IOException unreadable) {
            return null;
        }
    }

    private static String smallText(File file) throws IOException {
        byte[] data = ShipFiles.readSmall(file, 4096);
        return data == null ? null : new String(data, StandardCharsets.US_ASCII);
    }

    /** Writes a small text record, whole or not at all, and only when its content would change. */
    private static void record(File filesDir, String relative, String text, List<String> changed) throws IOException {
        File file = new File(filesDir, relative);
        String now;
        try {
            now = smallText(file);
        } catch (IOException tooLong) {
            now = null;
        }
        if (text.equals(now)) return;
        ShipFiles.writeSmall(file, text.getBytes(StandardCharsets.US_ASCII), ShipFiles.OWNER_WRITE);
        changed.add(relative);
    }
}
