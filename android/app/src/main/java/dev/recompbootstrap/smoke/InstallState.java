package dev.recompbootstrap.smoke;

import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.StandardOpenOption;
import java.nio.file.attribute.PosixFilePermission;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.EnumSet;
import java.util.List;
import java.util.Set;

/**
 * What is installed in the app's files directory, found by looking only (design section 4.C). Pure Java.
 *
 * detect() opens files for reading and lists directories. It never creates, changes, renames or removes anything.
 *
 * The answer is one Problem: NONE when everything a start of the game needs is in place, otherwise the first thing
 * that stands in the way. The words the user reads for each answer are LaunchAdvice's; this class holds facts only.
 *
 * What a start needs, all under files/private-trace (this is where the install kit on the computer puts them):
 *   game data      an earlier start request (run-(id)/arguments.bin and the game image) and every file and directory
 *                  that request names under inputs/ and modules/;
 *   game code      private-trace-live.so, in the form the game window's loader accepts (see CodeFault);
 *   shader file    generic-shaders-v41.bin, which the game code's renderer loads its shaders from.
 */
final class InstallState {
    /** What stands between this device and a start of the game. */
    enum Problem {
        /** Nothing: Play can start the game. */
        NONE,
        /** No start request, no game code, nothing copied from discs. */
        NOTHING_INSTALLED,
        /** Only files that the app's own disc setup copied (files/game-install); this version cannot start them. */
        DISC_COPY_ONLY,
        /** The game code is there but no start request is, so the game data cannot be found. */
        DATA_MISSING,
        /** A start request is there but files it names are missing (missingFiles), or it names none. */
        DATA_INCOMPLETE,
        /** The game data is there, the game code file is not. */
        CODE_MISSING,
        /** The game code file is there but the game window would refuse to load it (codeFault says why). */
        CODE_UNLOADABLE,
        /** Game data and game code are there, the shader file is not. */
        SHADERS_MISSING
    }

    /** Why the game window's loader would refuse the game code file. The loader's own rule: private_trace_jni.cpp, libraryFile and ownedDirectory. */
    enum CodeFault {
        NONE,
        /** The first 64 bytes are not the header of a 64-bit ARM shared library. */
        NOT_A_PROGRAM,
        /** The file's permissions are not exactly "owner may read" (0400). */
        PERMISSIONS,
        /** The file has more than one name (hard link). */
        LINKED,
        /** The file is larger than 512 MiB. */
        TOO_LARGE,
        /** The directory private-trace does not have exactly the permissions "owner only" (0700). */
        FOLDER_PERMISSIONS
    }

    static final String PRIVATE_TRACE = "private-trace";
    /** The game code file the live game modes load. */
    static final String LIVE_LIBRARY = "private-trace-live.so";
    /**
     * The shader file, next to the game code. The game code names it itself (the standing build, attempt v464:
     * renderer_boundary_trace.cpp, genericReady); LauncherContractTest compares this name with that build's library.
     */
    static final String SHADER_FILE = "generic-shaders-v41.bin";
    /** Optional, placed by the install kit (or the developer): 32 hex digits naming the run to copy start requests from. */
    static final String TEMPLATE_PIN = "settings/launch-template.txt";
    /** Written by the app's own disc setup when it has copied files from discs. */
    static final String DISC_INSTALL = "game-install/install.json";
    static final String DISC_BUNDLE = "game-install/bundle";
    static final long MINIMUM_CODE_BYTES = 64;

    private static final Set<PosixFilePermission> OWNER_READ_ONLY =
            Collections.unmodifiableSet(EnumSet.of(PosixFilePermission.OWNER_READ));
    private static final Set<PosixFilePermission> OWNER_ONLY = Collections.unmodifiableSet(EnumSet.of(
            PosixFilePermission.OWNER_READ, PosixFilePermission.OWNER_WRITE, PosixFilePermission.OWNER_EXECUTE));

    final Problem problem;
    /** NONE unless the game code file is there and the game window would refuse it. */
    final CodeFault codeFault;
    /** The run whose start request Play copies: 32 hex digits, or null when there is none. */
    final String templateRunId;
    /** Size and modification time of the game code file; both 0 when it is missing. */
    final long codeBytes, codeModifiedMs;
    /** True when a start request exists and everything it names under inputs/ and modules/ exists. */
    final boolean dataPresent;
    /** How many files and directories the start request names, and how many of them are missing. */
    final int referencedFiles, missingFiles;
    /** True when the shader file is there (a file that is not empty). */
    final boolean shaderFilePresent;
    /** True when the app's own disc setup has copied disc files and a code bundle under files/game-install. */
    final boolean discInstallComplete;
    /** True when settings/launch-template.txt is there and cannot be used (it holds no run id, or names a run that
     *  cannot be used), so the newest run was taken. */
    final boolean pinIgnored;
    /** True when settings/qualified-code.txt exists, so that Play compares the game code with the version it names. */
    final boolean versionOnRecord;

    private InstallState(Problem problem, CodeFault codeFault, String templateRunId, long codeBytes,
            long codeModifiedMs, boolean dataPresent, int referencedFiles, int missingFiles, boolean shaderFilePresent,
            boolean discInstallComplete, boolean pinIgnored, boolean versionOnRecord) {
        this.problem = problem;
        this.codeFault = codeFault;
        this.templateRunId = templateRunId;
        this.codeBytes = codeBytes;
        this.codeModifiedMs = codeModifiedMs;
        this.dataPresent = dataPresent;
        this.referencedFiles = referencedFiles;
        this.missingFiles = missingFiles;
        this.shaderFilePresent = shaderFilePresent;
        this.discInstallComplete = discInstallComplete;
        this.pinIgnored = pinIgnored;
        this.versionOnRecord = versionOnRecord;
    }

    /** True when Play can start the game. */
    boolean ready() {
        return problem == Problem.NONE;
    }

    private static final class Template {
        final String id;
        final byte[] arguments;

        Template(String id, byte[] arguments) {
            this.id = id;
            this.arguments = arguments;
        }
    }

    private static final class Candidate {
        final String id;
        final long modifiedMs;

        Candidate(String id, long modifiedMs) {
            this.id = id;
            this.modifiedMs = modifiedMs;
        }
    }

    /** Looks at filesDir (the app's files directory) and reports what is there. Never throws, never writes. */
    static InstallState detect(File filesDir) {
        File root = new File(filesDir, PRIVATE_TRACE);

        File code = new File(root, LIVE_LIBRARY);
        long codeBytes = 0, codeModifiedMs = 0;
        if (regularFile(code) && code.length() >= MINIMUM_CODE_BYTES) {
            codeBytes = code.length();
            codeModifiedMs = code.lastModified();
        }
        boolean codePresent = codeBytes > 0;
        CodeFault codeFault = codePresent ? codeFault(root, code) : CodeFault.NONE;

        File pin = new File(filesDir, TEMPLATE_PIN);
        String pinned = readPin(pin);
        Template template = pinned == null ? null : qualify(root, pinned);
        // A pin that is there and cannot be used is said so, whatever is wrong with it: it names a run that is gone,
        // or it holds no run id at all. (Silence would start the game from another request than the install chose.)
        boolean pinIgnored = template == null && present(pin);
        if (template == null) {
            template = newestTemplate(root);
        }

        int referenced = 0, missing = 0;
        if (template != null) {
            for (String path : LaunchRequest.referencedInputs(template.arguments, filesDir.getAbsolutePath())) {
                ++referenced;
                File named = new File(path);
                // Most are files (inputs/<sha256>.bin); a few are directories the game fills itself
                // (inputs/native-cache, inputs/native-update, inputs/native-game-cache, modules/boot).
                if (!named.isFile() && !named.isDirectory()) {
                    ++missing;
                }
            }
        }
        boolean dataPresent = template != null && referenced > 0 && missing == 0;

        // The game code opens it with fopen, which follows a link; an empty file holds no shaders.
        File shaders = new File(root, SHADER_FILE);
        boolean shaderFilePresent = shaders.isFile() && shaders.length() > 0;

        boolean discInstall = regularFile(new File(filesDir, DISC_INSTALL));
        boolean discBundle = hasEntries(new File(filesDir, DISC_BUNDLE));

        Problem problem;
        if (template == null && !codePresent) {
            problem = discInstall ? Problem.DISC_COPY_ONLY : Problem.NOTHING_INSTALLED;
        } else if (template == null) {
            problem = Problem.DATA_MISSING;
        } else if (!dataPresent) {
            problem = Problem.DATA_INCOMPLETE;
        } else if (!codePresent) {
            problem = Problem.CODE_MISSING;
        } else if (codeFault != CodeFault.NONE) {
            problem = Problem.CODE_UNLOADABLE;
        } else if (!shaderFilePresent) {
            problem = Problem.SHADERS_MISSING;
        } else {
            problem = Problem.NONE;
        }
        return new InstallState(problem, codeFault, template == null ? null : template.id, codeBytes, codeModifiedMs,
                dataPresent, referenced, missing, shaderFilePresent, discInstall && discBundle, pinIgnored,
                LaunchRequest.qualifiedHash(filesDir) != null);
    }

    /**
     * detect(), narrowed by the rule of a build that carries its game code (ShipReady; in a build without game code
     * inside, and for null, the answer is detect()'s own). Such a build installs its game code itself and pins every
     * game data file, and the start request does not name all of them (the boot modules lie in a directory it names),
     * so "ready" there needs more than detect() can see: this app version's game code in place, every pinned file
     * there with its size, the bundled pieces and the small files with their SHA-256. The narrowing can only take
     * "ready" away, never give it:
     *   pinned game data absent or not the pinned file   DATA_INCOMPLETE, counted in pinned files;
     *   this app version's game code, or a file it carries, not in place   CODE_MISSING when no game code file is
     *       there, else CODE_UNLOADABLE (the file that is there is not this version's; the first screen replaces it).
     * Call it off the UI thread, like detect(): making the ShipReady answer reads about 490 MB in the real build.
     */
    static InstallState detect(File filesDir, ShipReady ship) {
        InstallState found = detect(filesDir);
        if (ship == null || !ship.bundled() || ship.ready()) {
            return found;
        }
        if (ship.stands == ShipReady.Stands.DATA) {
            if (found.problem != Problem.NONE && found.problem != Problem.DATA_INCOMPLETE) {
                return found;   // something else stands in the way first (no game code, no start request)
            }
            return new InstallState(Problem.DATA_INCOMPLETE, found.codeFault, found.templateRunId, found.codeBytes,
                    found.codeModifiedMs, false, ship.data.pinned, ship.data.missing(), found.shaderFilePresent,
                    found.discInstallComplete, found.pinIgnored, found.versionOnRecord);
        }
        if (found.problem != Problem.NONE) {
            return found;
        }
        return new InstallState(found.codeBytes > 0 ? Problem.CODE_UNLOADABLE : Problem.CODE_MISSING, found.codeFault,
                found.templateRunId, found.codeBytes, found.codeModifiedMs, found.dataPresent, found.referencedFiles,
                found.missingFiles, found.shaderFilePresent, found.discInstallComplete, found.pinIgnored,
                found.versionOnRecord);
    }

    /**
     * Why the game window's loader would refuse this game code file, or NONE. It repeats the loader's own rule
     * (android/app/src/debug/cpp/private_trace_jni.cpp: ownedDirectory and libraryFile), so that a file the game
     * window would only answer with "REJECT LIBRARY" is named here, in words, before a start is tried. A fault is
     * reported only where the loader's rule is certain to refuse: a check that cannot be made counts as passed.
     */
    private static CodeFault codeFault(File root, File code) {
        try {
            if (!OWNER_ONLY.equals(Files.getPosixFilePermissions(root.toPath(), LinkOption.NOFOLLOW_LINKS))) {
                return CodeFault.FOLDER_PERMISSIONS;
            }
            if (!OWNER_READ_ONLY.equals(Files.getPosixFilePermissions(code.toPath(), LinkOption.NOFOLLOW_LINKS))) {
                return CodeFault.PERMISSIONS;
            }
        } catch (IOException | RuntimeException unknown) {
            // permissions cannot be read on this file system: not judged
        }
        try {
            Object links = Files.getAttribute(code.toPath(), "unix:nlink", LinkOption.NOFOLLOW_LINKS);
            if (links instanceof Number && ((Number) links).longValue() != 1) {
                return CodeFault.LINKED;
            }
        } catch (IOException | RuntimeException unknown) {
            // no link count on this file system: not judged
        }
        if (code.length() > LaunchRequest.MAXIMUM_LIBRARY_BYTES) {
            return CodeFault.TOO_LARGE;
        }
        byte[] header = new byte[64];
        try (FileChannel in = FileChannel.open(code.toPath(), StandardOpenOption.READ, LinkOption.NOFOLLOW_LINKS)) {
            ByteBuffer buffer = ByteBuffer.wrap(header);
            while (buffer.hasRemaining() && in.read(buffer) > 0) {
                // keep reading
            }
            if (buffer.hasRemaining()) {
                return CodeFault.NOT_A_PROGRAM;
            }
        } catch (IOException | RuntimeException unreadable) {
            return CodeFault.NONE;   // not judged; the start itself reports what cannot be read
        }
        return libraryHeader(header) ? CodeFault.NONE : CodeFault.NOT_A_PROGRAM;
    }

    /**
     * The loader's header rule: "\177ELF", 64-bit, little-endian, ELF version 1; a shared object (type 3) for
     * 64-bit ARM (machine 0xB7); entry address zero.
     */
    static boolean libraryHeader(byte[] header) {
        if (header == null || header.length < 64) {
            return false;
        }
        int[] start = {0x7f, 'E', 'L', 'F', 2, 1, 1};
        for (int at = 0; at < start.length; ++at) {
            if ((header[at] & 255) != start[at]) {
                return false;
            }
        }
        int[] kind = {3, 0, 0xb7, 0};
        for (int at = 0; at < kind.length; ++at) {
            if ((header[16 + at] & 255) != kind[at]) {
                return false;
            }
        }
        for (int at = 24; at < 32; ++at) {
            if (header[at] != 0) {
                return false;
            }
        }
        return true;
    }

    /** The qualifying run directory whose arguments file is newest; on equal times the smaller id. null if none. */
    private static Template newestTemplate(File root) {
        File[] children;
        try {
            children = root.listFiles();
        } catch (RuntimeException unreadable) {
            children = null;
        }
        if (children == null) {
            return null;
        }
        List<Candidate> candidates = new ArrayList<Candidate>();
        for (File child : children) {
            String name = child.getName();
            if (!name.startsWith("run-") || !LaunchRequest.isId(name.substring(4)) || !hasRequestFiles(child)) {
                continue;
            }
            candidates.add(new Candidate(name.substring(4), new File(child, LaunchRequest.ARGUMENTS).lastModified()));
        }
        Collections.sort(candidates, new Comparator<Candidate>() {
            @Override public int compare(Candidate a, Candidate b) {
                if (a.modifiedMs != b.modifiedMs) {
                    return a.modifiedMs > b.modifiedMs ? -1 : 1;
                }
                return a.id.compareTo(b.id);
            }
        });
        for (Candidate candidate : candidates) {
            Template template = qualify(root, candidate.id);
            if (template != null) {
                return template;
            }
        }
        return null;
    }

    /** The two files every start request has: arguments (1 to 65536 bytes) and the game image. No links. */
    private static boolean hasRequestFiles(File runDirectory) {
        try {
            if (!Files.isDirectory(runDirectory.toPath(), LinkOption.NOFOLLOW_LINKS)) {
                return false;
            }
        } catch (RuntimeException invalid) {
            return false;
        }
        File arguments = new File(runDirectory, LaunchRequest.ARGUMENTS);
        return regularFile(arguments) && arguments.length() >= 1
                && arguments.length() <= LaunchRequest.MAXIMUM_ARGUMENT_BYTES
                && regularFile(new File(runDirectory, LaunchRequest.IMAGE));
    }

    /**
     * A run directory can be copied from when it has the two request files and its arguments are ones the game
     * would accept for that run (LaunchRequest.checkArguments). Returns null when it cannot.
     */
    private static Template qualify(File root, String id) {
        File directory = new File(root, "run-" + id);
        if (!hasRequestFiles(directory)) {
            return null;
        }
        try {
            byte[] arguments = LaunchRequest.readBounded(new File(directory, LaunchRequest.ARGUMENTS),
                    LaunchRequest.MAXIMUM_ARGUMENT_BYTES);
            if (LaunchRequest.checkArguments(arguments, root.getAbsolutePath(), id) != null) {
                return null;
            }
            return new Template(id, arguments);
        } catch (IOException | RuntimeException unreadable) {
            return null;
        }
    }

    /** The 32 hex digits in the pin file (surrounding white space allowed), or null. */
    private static String readPin(File pin) {
        if (!regularFile(pin)) {
            return null;
        }
        try {
            String text = new String(LaunchRequest.readBounded(pin, 4096), StandardCharsets.US_ASCII).trim();
            return LaunchRequest.isId(text) ? text : null;
        } catch (IOException | RuntimeException unreadable) {
            return null;
        }
    }

    private static boolean regularFile(File file) {
        try {
            return Files.isRegularFile(file.toPath(), LinkOption.NOFOLLOW_LINKS);
        } catch (RuntimeException invalid) {
            return false;
        }
    }

    /** True when something of that name exists: a file, a directory or a link, whatever it holds. */
    private static boolean present(File file) {
        try {
            return Files.exists(file.toPath(), LinkOption.NOFOLLOW_LINKS);
        } catch (RuntimeException invalid) {
            return false;
        }
    }

    private static boolean hasEntries(File directory) {
        try {
            String[] names = directory.isDirectory() ? directory.list() : null;
            return names != null && names.length > 0;
        } catch (RuntimeException unreadable) {
            return false;
        }
    }
}
