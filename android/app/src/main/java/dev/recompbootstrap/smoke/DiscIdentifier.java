package dev.recompbootstrap.smoke;

import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.security.MessageDigest;

/**
 * Says which disc the player chose by the SHA-256 of its game executable (rips of the same disc differ as images,
 * their files do not). Pure Java. Never throws; never writes.
 *
 * Every answer is two sentences for a person who has never seen this project: what was found (message) and, unless
 * the disc is the supported one, what to do about it (next).
 */
final class DiscIdentifier {
    /**
     * DAMAGED: the disc copy itself is incomplete or its tables are wrong (reading it again gives the same result).
     * UNREADABLE: reading failed for a reason outside the copy (storage removed, permission gone).
     */
    enum Kind { SUPPORTED_PLAY_DISC, SUPPORTED_INSTALL_DISC, UNSUPPORTED_VERSION, NOT_A_GAME_DISC, DAMAGED, UNREADABLE }

    /** An executable larger than this is not hashed: no supported release comes near it. */
    static final long MAX_EXECUTABLE_BYTES = 256L << 20;

    static final String NEXT_UNPACK = "Choose the .iso image of the play disc (Disc 2), or the folder that holds the disc's files."
            + " An image packed inside a .zip, .7z or .rar file has to be unpacked first.";
    static final String NEXT_FOLDER = "Choose the folder that holds default.xex and the .rpf files directly, not a folder above it.";
    static final String NEXT_NEW_COPY = "Copy it to this device again, or make a new copy of the disc.";
    static final String NEXT_RECONNECT = "Check that the storage it is on is still connected, then choose it again.";

    static final class Result {
        final Kind kind;
        /** 64 hex digits when the executable was read, else null. */
        final String executableSha256;
        /** One sentence for the screen: what was found. */
        final String message;
        /** One or two sentences: what to do now. null when the disc is the supported one. */
        final String next;

        Result(Kind kind, String executableSha256, String message, String next) {
            this.kind = kind;
            this.executableSha256 = executableSha256;
            this.message = message;
            this.next = next;
        }
    }

    /** An extracted disc: a folder. Names are matched without regard to case. */
    interface FolderSource {
        /** null if absent. The caller closes the result. */
        DiscSource open(String relativePath) throws IOException;
    }

    private DiscIdentifier() {}

    static Result identifyImage(DiscSource image) {
        return identifyImage(image, SetupCatalog.spec());
    }

    static Result identifyFolder(FolderSource folder) {
        return identifyFolder(folder, SetupCatalog.spec());
    }

    /** As identifyImage with the expected release given (tests use a synthetic one). */
    static Result identifyImage(DiscSource image, SetupCatalog.Spec spec) {
        try {
            Xdvdfs fs = Xdvdfs.open(image);
            if (fs == null) {
                return new Result(Kind.NOT_A_GAME_DISC, null, "This file is not an Xbox 360 disc image that this app can read.", NEXT_UNPACK);
            }
            Xdvdfs.Entry executable = fs.find(spec.executableName);
            if (executable == null || executable.directory) {
                return new Result(Kind.NOT_A_GAME_DISC, null, "This disc image holds no game program (" + spec.executableName + ").",
                        nextSupported(spec));
            }
            return judge(VerifiedExtractor.rangeStream(image, executable.offset, executable.size, false), executable.size, spec);
        } catch (EOFException e) {
            return new Result(Kind.DAMAGED, null, "This disc image is incomplete: it ends before the disc's data does.", NEXT_NEW_COPY);
        } catch (Xdvdfs.Damaged e) {
            return new Result(Kind.DAMAGED, null, "This disc image is damaged (" + e.what() + ").", NEXT_NEW_COPY);
        } catch (IOException e) {
            return unreadable(e);
        } catch (RuntimeException e) {
            return unreadable(e);
        }
    }

    /** As identifyFolder with the expected release given. */
    static Result identifyFolder(FolderSource folder, SetupCatalog.Spec spec) {
        try {
            DiscSource executable = folder.open(spec.executableName);
            if (executable == null) {
                return new Result(Kind.NOT_A_GAME_DISC, null, "This folder does not hold the play disc's files: " + spec.executableName
                        + " is not in it.", NEXT_FOLDER);
            }
            try {
                long size = executable.size();
                return judge(VerifiedExtractor.rangeStream(executable, 0, size, false), size, spec);
            } finally {
                executable.close();
            }
        } catch (EOFException e) {
            return new Result(Kind.DAMAGED, null, "The game program in this folder is incomplete (" + spec.executableName + ").", NEXT_NEW_COPY);
        } catch (IOException e) {
            return unreadable(e);
        } catch (RuntimeException e) {
            return unreadable(e);
        }
    }

    /** The answer when the chosen file or folder could not be read at all (also used by the screen for its own failures). */
    static Result unreadable(Exception e) {
        return new Result(Kind.UNREADABLE, null, "The disc copy you chose could not be read (" + VerifiedExtractor.reason(e) + ").",
                NEXT_RECONNECT);
    }

    private static String nextSupported(SetupCatalog.Spec spec) {
        return "This version works with the play disc (Disc 2) of " + spec.label + " only. If you chose the install disc (Disc 1),"
                + " choose Disc 2 instead.";
    }

    private static Result judge(InputStream executable, long size, SetupCatalog.Spec spec) throws IOException {
        if (size > MAX_EXECUTABLE_BYTES) {
            return new Result(Kind.UNSUPPORTED_VERSION, null, "This is not the play disc this version supports (its game program is"
                    + " larger than the supported one).", nextSupported(spec));
        }
        MessageDigest sha = VerifiedExtractor.sha256();
        byte[] buffer = new byte[1 << 16];
        long left = size;
        while (left > 0) {
            int n = executable.read(buffer, 0, (int) Math.min(buffer.length, left));
            if (n < 0) throw new EOFException("the executable ends early");
            sha.update(buffer, 0, n);
            left -= n;
        }
        String found = VerifiedExtractor.hex(sha.digest());
        if (found.equals(spec.executableSha256)) {
            return new Result(Kind.SUPPORTED_PLAY_DISC, found, "Recognised: " + spec.label + ", the play disc.", null);
        }
        return new Result(Kind.UNSUPPORTED_VERSION, found, "This is not the play disc this version supports (its game program has"
                + " check code " + found.substring(0, 12) + ").", nextSupported(spec));
    }
}
