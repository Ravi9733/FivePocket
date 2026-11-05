package dev.recompbootstrap.smoke;

import java.io.EOFException;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

/**
 * One entry point for both of the game's discs. Pure Java; never writes to the image.
 *
 * open(image, game) says which disc an image is (the play disc, the install disc, not one of the two as far as the
 * image shows, not a disc image, a damaged image, or one that could not be read), with one sentence for the player
 * and one for what to do, and lists the disc's candidate files: a display name, a size and a way to open the file's
 * bytes as a stream.
 *  - Play disc: every file of the disc outside the console's system-update folder. The stream is the file's byte
 *    range in the image; the disc carries no check values for it, so the SHA-256 of what was read is its only check.
 *  - Install disc: every file inside each of the game's content packages (PIRS, content type 2, the game's title).
 *    The stream is Disc2Stfs.openEntry: every 4096-byte block is compared with the SHA-1 the package's hash tree
 *    holds for it before any of its bytes are handed out, and the tree was checked from its root hash down.
 * Both go through Disc2ImageProbe (where the game partition is) and one Disc2Window at that position.
 *
 * plan(candidates, pinned, installed) is the copy plan: a candidate is read only when some pinned entry of exactly
 * its size is not installed yet; after a file was read, judge() says whether its SHA-256 is a pinned one.
 * sha256(candidate, out, watch) reads one candidate to its end, optionally writing it, and returns its SHA-256.
 *
 * A usable disc is one that was recognised and can be read. Whether it holds every file the pinned record expects of
 * it is a question for the plan, because only the pinned record knows what is complete: Plan.shortSizes answers it
 * before anything is copied, missing() after the copy, and wrongContent() re-reads what is in place.
 *
 * One stream at a time per disc: the streams of a disc share its reader and must not be read from two threads at
 * once. The image stays open as long as streams are used; whoever opened it closes it.
 */
final class Disc2Discs {
    /** The content type of the packages a game installs from its install disc. */
    static final long INSTALL_CONTENT_TYPE = 2;
    /** The console's own update files on every game disc; never game data, so never a candidate. */
    static final String SYSTEM_UPDATE_FOLDER = "$SystemUpdate/";
    /** An executable larger than this is not hashed: no supported release comes near it. */
    static final long MAX_EXECUTABLE_BYTES = 256L << 20;
    private static final int BUFFER_BYTES = 1 << 20;

    static final String NEXT_NEW_COPY = "Copy the image to this device again, or make a new copy of the disc.";
    static final String NEXT_RECONNECT = "Check that the storage it is on is still connected, then choose it again.";
    /**
     * After a file that is not a disc image. Said once: whether to unpack is the refusal's own sentence, and only for an
     * archive (setup-fixes, C4-2). The setup page says instead which disc's image it expects (SetupDiscModel.expectedImage).
     */
    static final String NEXT_CHOOSE_IMAGE = "Choose the image file of the disc, usually an .iso file.";

    enum Kind {
        /** The supported release's play disc (Disc 2). */
        PLAY,
        /** A disc that holds content packages of this game (Disc 1). */
        INSTALL,
        /**
         * A readable Xbox 360 disc image that is neither, as far as the image shows: another game, another release of
         * this game, or a copy of one of the two discs with damage in a place the image holds no check value for (its
         * game program, a folder list, the first fields of a content package). The image cannot tell which.
         */
        OTHER_GAME,
        /** No Xbox 360 game partition in the file. */
        NOT_A_DISC,
        /** The image is cut off, contradicts itself or fails its own hash checks: reading it again gives the same result. */
        DAMAGED,
        /** Reading failed for a reason outside the image (storage removed, permission gone). */
        UNREADABLE
    }

    /** What tells this game's discs from others. */
    static final class Game {
        final String label, executableName, executableSha256;
        final long titleId;

        /**
         * label: the release's name in sentences; executableName and executableSha256: the play disc's game program
         * (64 lower-case hex digits); titleId: the title in the headers of the install disc's packages.
         */
        Game(String label, String executableName, String executableSha256, long titleId) {
            if (label == null || executableName == null || executableName.isEmpty() || !isSha256(executableSha256)) {
                throw new IllegalArgumentException("a game needs a label, an executable name and its SHA-256");
            }
            this.label = label;
            this.executableName = executableName;
            this.executableSha256 = executableSha256;
            this.titleId = titleId;
        }
    }

    interface Opener {
        InputStream open() throws IOException;
    }

    /** One file a disc offers. */
    static final class Candidate {
        /**
         * The path on the play disc, or "PACKAGE/path inside the package" on the install disc. For the log (the FILE
         * lines, a failed copy's line); a handed-out build's screens say "file k of n" and never show it.
         */
        final String name;
        final long bytes;
        /** True when every block is checked against the package's hash tree while it is read (install disc). */
        final boolean blockChecked;
        private final Opener opener;

        Candidate(String name, long bytes, boolean blockChecked, Opener opener) {
            this.name = name;
            this.bytes = bytes;
            this.blockChecked = blockChecked;
            this.opener = opener;
        }

        /**
         * The file's bytes from its first to its last. A block of a package that does not match its hash is a
         * Disc2Stfs.Damaged from read(): the disc copy is bad, reading again gives the same. The caller closes it.
         */
        InputStream open() throws IOException {
            return opener.open();
        }
    }

    /** What open() found. */
    static final class Disc {
        final Kind kind;
        /**
         * One sentence for the player: what was found. It names no file, folder or package of the disc and not the
         * game program's file (the lead's decision of 2026-10-06: no internal names on a handed-out build's screens).
         */
        final String message;
        /**
         * For the log only (SetupDiscLog's DISC line): what the sentence leaves out: the disc path of the file or package
         * it is about, the game program's file name, or the positions, sizes and values behind a refusal (no hex value
         * or byte count is shown to the player: setup-fixes, C4-1/C4-3). null when the sentence leaves nothing out.
         */
        final String named;
        /** What to do now; null when the disc is usable. */
        final String next;
        /** Byte position of the game partition in the image, or -1. */
        final long partitionOffset;
        /** Empty unless the kind is PLAY or INSTALL. Unmodifiable. */
        final List<Candidate> candidates;
        /** Paths of the content packages that were opened (install disc), in the order of the candidates. */
        final List<String> packages;
        /** Paths of packages on the disc that are not this game's install content, each with the reason. */
        final List<String> ignored;

        Disc(Kind kind, String message, String next, long partitionOffset, List<Candidate> candidates, List<String> packages,
                List<String> ignored) {
            this(kind, message, null, next, partitionOffset, candidates, packages, ignored);
        }

        Disc(Kind kind, String message, String named, String next, long partitionOffset, List<Candidate> candidates, List<String> packages,
                List<String> ignored) {
            this.kind = kind;
            this.message = message;
            this.named = named;
            this.next = next;
            this.partitionOffset = partitionOffset;
            this.candidates = Collections.unmodifiableList(candidates);
            this.packages = Collections.unmodifiableList(packages);
            this.ignored = Collections.unmodifiableList(ignored);
        }

        /**
         * True when the disc was recognised and its files can be read. It does not say that the disc holds every file
         * the pinned record expects of it (an install disc is recognised by its title alone, and one that lacks a
         * package is still usable): ask Plan.shortSizes before copying and missing() after it.
         */
        boolean usable() {
            return kind == Kind.PLAY || kind == Kind.INSTALL;
        }

        /** The sentence with the name it leaves out, for a log line; the sentence alone when it leaves none out. */
        String logged() {
            return named == null ? message : message + " [" + named + "]";
        }
    }

    private Disc2Discs() {}

    // ---- which disc, and what it offers --------------------------------------------------------------------------

    /** Never throws; never writes. Reads the folder tables, the package headers and hash tables, and (play disc) the executable. */
    static Disc open(DiscSource image, Game game) {
        try {
            return openOrThrow(image, game);
        } catch (EOFException e) {
            return refused(Kind.DAMAGED, "This disc image is incomplete: it ends before the disc's data does" + onlyBytes(image) + ".",
                    imageBytes(image), NEXT_NEW_COPY);
        } catch (Xdvdfs.Damaged e) {
            return refused(Kind.DAMAGED, "This disc image is damaged (" + e.what() + ").", NEXT_NEW_COPY);
        } catch (IOException e) {
            return refused(Kind.UNREADABLE, "The disc image could not be read (" + VerifiedExtractor.reason(e) + ").", NEXT_RECONNECT);
        } catch (RuntimeException e) {
            return refused(Kind.UNREADABLE, "The disc image could not be read (" + VerifiedExtractor.reason(e) + ").", NEXT_RECONNECT);
        }
    }

    private static Disc openOrThrow(DiscSource image, Game game) throws IOException {
        Disc2ImageProbe.Result probe = Disc2ImageProbe.probe(image);
        if (!probe.usable()) {
            if (probe.kind == Disc2ImageProbe.Kind.CUT_OFF) return refused(Kind.DAMAGED, probe.message, probe.detail, NEXT_NEW_COPY);
            if (probe.kind == Disc2ImageProbe.Kind.UNREADABLE) return refused(Kind.UNREADABLE, probe.message, NEXT_RECONNECT);
            // What the file is not (a disc image of this game), what it seems to be, and the advice to unpack only for
            // an archive, once (setup-fixes, C4-2).
            return refused(Kind.NOT_A_DISC, "This file is not a disc image of " + game.label + (probe.what == null ? "." : ": " + probe.what + ".")
                    + (probe.packed ? " " + Disc2ImageProbe.TEXT_UNPACK : ""), probe.detail, NEXT_CHOOSE_IMAGE);
        }
        long imageSize = image.size();
        final DiscSource partition = new Disc2Window(image, probe.partitionOffset, imageSize - probe.partitionOffset);
        Xdvdfs fs = Xdvdfs.open(partition);
        if (fs == null || fs.partitionOffset() != 0) {
            return refused(Kind.NOT_A_DISC, "This file is not an Xbox 360 disc image that this app can read.", NEXT_CHOOSE_IMAGE);
        }
        List<Xdvdfs.Entry> files = fs.list();
        for (Xdvdfs.Entry e : files) {
            if (e.offset + e.size > partition.size()) {
                // Plain words for the player: not which file, no byte position (the log's DISC line has both).
                return refused(Kind.DAMAGED, "This disc image is incomplete: it ends before the disc's data does ("
                        + sizeClause(imageSize) + ").", e.path + " goes on to byte "
                        + (probe.partitionOffset + e.offset + e.size) + ", the image has " + imageSize + " bytes", NEXT_NEW_COPY);
            }
        }

        List<Candidate> candidates = new ArrayList<Candidate>();
        List<String> packages = new ArrayList<String>(), ignored = new ArrayList<String>();
        byte[] head = new byte[0x364];
        for (Xdvdfs.Entry e : files) {
            String why;           // why this file is not install content of this game; null when it is
            String plain = null;  // the same for the player, when why holds a number or a hex value
            String note = null;   // for the list of ignored packages
            if (e.size < Disc2Stfs.HEADER_BYTES) {
                why = "it is too small to be a content package";
            } else {
                partition.readFully(e.offset, head, 0, head.length);
                long type = Disc2Stfs.be32(head, 0x344), title = Disc2Stfs.be32(head, 0x360);
                if (head[0] != 'P' || head[1] != 'I' || head[2] != 'R' || head[3] != 'S') {
                    why = "it does not begin as a content package does";
                } else if (type != INSTALL_CONTENT_TYPE) {
                    why = "its header names content type " + type + ", not install content";
                    plain = "it is a package of another kind, not install content";
                    note = e.path + " (a package of content type " + type + ", not install content)";
                } else if (title != game.titleId) {
                    why = "its header names another title, " + String.format(Locale.ROOT, "%08X", title);
                    plain = "it belongs to another game";
                    note = e.path + " (install content of another title, " + String.format(Locale.ROOT, "%08X", title) + ")";
                } else {
                    why = null;
                }
            }
            if (why != null) {
                // The folder says "install content of this game" and the file says otherwise: leaving it out without a
                // word would offer a part of the disc as if it were the whole.
                if (inInstallFolder(e.path, game.titleId)) {
                    // The player's sentence says what is wrong; which file it is goes to the log (Disc.named).
                    return notSupported(game, "This disc image cannot be used: a file in the game's install-content folder is"
                            + " not an install package of the game (" + (plain == null ? why : plain) + ").",
                            plain == null ? e.path : e.path + " (" + why + ")", probe.partitionOffset, ignored);
                }
                if (note != null) ignored.add(note);
                continue;
            }
            final String name = e.path.substring(e.path.lastIndexOf('/') + 1);
            final Disc2Stfs opened;
            try {
                opened = Disc2Stfs.open(new Disc2Window(partition, e.offset, e.size), game.titleId);
            } catch (Disc2Stfs.Damaged d) {
                // The package's own reason (a level and a block, a size) is for the log (setup-fixes, C4-1).
                return refused(Kind.DAMAGED, "This disc image is damaged: one of its content packages does not pass its own checks.",
                        e.path + ": " + d.getMessage(), NEXT_NEW_COPY);
            } catch (Disc2Stfs.Refused r) {
                // Intact as far as it was looked at, but not a form that is read: another release, or damage in a header
                // field that no hash covers. A new copy helps only in the second case, so the advice names both.
                return notSupported(game, "This disc image cannot be used: one of its content packages is in a form this version does"
                        + " not read.", e.path + ": " + r.getMessage(), probe.partitionOffset, ignored);
            }
            packages.add(e.path);
            for (final Disc2Stfs.Entry entry : opened.entries()) {
                if (entry.directory) continue;
                candidates.add(new Candidate(name + "/" + entry.path, entry.size, true, new Opener() {
                    @Override
                    public InputStream open() throws IOException {
                        return opened.openEntry(entry);
                    }
                }));
            }
        }
        if (!packages.isEmpty()) {
            return new Disc(Kind.INSTALL, "Recognised: " + game.label + ", the install disc (" + packages.size()
                    + (packages.size() == 1 ? " content package, " : " content packages, ") + candidates.size()
                    + (candidates.size() == 1 ? " file)." : " files)."), null, probe.partitionOffset, candidates, packages, ignored);
        }

        Xdvdfs.Entry executable = fs.find(game.executableName);
        if (executable == null || executable.directory) {
            return notSupported(game, "This disc image holds no game program, so it is not a disc of " + game.label + ".",
                    game.executableName, probe.partitionOffset, ignored);
        }
        if (executable.size > MAX_EXECUTABLE_BYTES) {
            return notSupported(game, "This is not a disc of " + game.label + " (its game program is larger than the supported one).",
                    probe.partitionOffset, ignored);
        }
        String found = sha256(range(partition, executable.offset, executable.size), executable.size, null, null);
        if (!found.equals(game.executableSha256)) {
            // The disc holds no check value for its game program, so one changed bit in a copy of the right disc and
            // another game's program look the same from here.
            // The player's sentence names no check value (SetupDiscLog's DISC line carries the facts for the lead).
            return notSupported(game, "This is not the play disc of " + game.label + ", or its copy is damaged: its game program is not the"
                    + " supported one.", probe.partitionOffset, ignored);
        }
        for (final Xdvdfs.Entry e : files) {
            if (e.path.regionMatches(true, 0, SYSTEM_UPDATE_FOLDER, 0, SYSTEM_UPDATE_FOLDER.length())) continue;
            candidates.add(new Candidate(e.path, e.size, false, new Opener() {
                @Override
                public InputStream open() {
                    return range(partition, e.offset, e.size);
                }
            }));
        }
        return new Disc(Kind.PLAY, "Recognised: " + game.label + ", the play disc (" + candidates.size()
                + (candidates.size() == 1 ? " file)." : " files)."), null, probe.partitionOffset, candidates, packages, ignored);
    }

    private static Disc refused(Kind kind, String message, String next) {
        return refused(kind, message, null, next);
    }

    private static Disc refused(Kind kind, String message, String named, String next) {
        return new Disc(kind, message, named, next, -1, new ArrayList<Candidate>(), new ArrayList<String>(), new ArrayList<String>());
    }

    /** Kind OTHER_GAME: nothing is offered; the advice names both ways out, because the image cannot tell which applies. */
    private static Disc notSupported(Game game, String message, long partitionOffset, List<String> ignored) {
        return notSupported(game, message, null, partitionOffset, ignored);
    }

    private static Disc notSupported(Game game, String message, String named, long partitionOffset, List<String> ignored) {
        return new Disc(Kind.OTHER_GAME, message, named, nextSupported(game), partitionOffset, new ArrayList<Candidate>(),
                new ArrayList<String>(), ignored);
    }

    /** What to do with an image that is not one of the two supported discs as far as it shows. */
    static String nextSupported(Game game) {
        return "This version works with the two discs of " + game.label + " only: choose the image of its play disc (Disc 2) or of its"
                + " install disc (Disc 1). If this image was made from one of them, make a new copy of that disc: this copy is damaged.";
    }

    /** " (it is only 2.1 GB)", or nothing when the size cannot be read. GB with one decimal, never a byte count (C4-3). */
    private static String onlyBytes(DiscSource image) {
        try {
            return " (" + sizeClause(image.size()) + ")";
        } catch (IOException e) {
            return "";
        } catch (RuntimeException e) {
            return "";
        }
    }

    /** "it is only 2.1 GB" (GB with one decimal), or "it is smaller than 0.1 GB". */
    static String sizeClause(long bytes) {
        return bytes < 100000000L ? "it is smaller than 0.1 GB" : "it is only " + VerifiedExtractor.gigabytes(bytes, false);
    }

    /** For the log: "image bytes=N", or null when the size cannot be read. */
    private static String imageBytes(DiscSource image) {
        try {
            return "image bytes=" + image.size();
        } catch (IOException e) {
            return null;
        } catch (RuntimeException e) {
            return null;
        }
    }

    /**
     * True for a file directly inside the folder a game disc keeps this title's install content in:
     * content/SIXTEEN HEX DIGITS/TITLE/00000002/NAME (the real install disc: content/0000000000000000/545408A7/00000002/).
     */
    static boolean inInstallFolder(String path, long titleId) {
        String[] parts = path.split("/");
        if (parts.length != 5 || !parts[0].equalsIgnoreCase("content") || parts[1].length() != 16) return false;
        for (int i = 0; i < 16; ++i) {
            if (Character.digit(parts[1].charAt(i), 16) < 0) return false;
        }
        return parts[2].equalsIgnoreCase(String.format(Locale.ROOT, "%08X", titleId))
                && parts[3].equals(String.format(Locale.ROOT, "%08X", INSTALL_CONTENT_TYPE));
    }

    /** length bytes of source from offset on; a range that passes the end of the source fails with EOFException when read. */
    private static InputStream range(final DiscSource source, final long offset, final long length) {
        return new InputStream() {
            private long at = offset;
            private final long end = offset + length;

            @Override
            public int read() throws IOException {
                byte[] one = new byte[1];
                return read(one, 0, 1) < 0 ? -1 : one[0] & 0xFF;
            }

            @Override
            public int read(byte[] buffer, int off, int len) throws IOException {
                if (len == 0) return 0;
                if (at >= end) return -1;
                int n = (int) Math.min(len, end - at);
                if (at + n > source.size()) throw new EOFException("the image is cut off inside this file");
                source.readFully(at, buffer, off, n);
                at += n;
                return n;
            }
        };
    }

    // ---- reading one candidate ---------------------------------------------------------------------------------

    /** Told after every part read how many bytes of this file are done; false stops the read. */
    interface Watch {
        boolean more(long bytesDone);
    }

    /** The watch asked to stop. */
    static final class Stopped extends IOException {
        private static final long serialVersionUID = 1L;

        Stopped() {
            super("stopped");
        }
    }

    /**
     * Reads the candidate from its first byte to its last, writes every byte to out when out is not null, and returns
     * the SHA-256 of what was read (64 lower-case hex digits). The stream must hold exactly candidate.bytes bytes:
     * fewer is an EOFException, more an IOException. A package block that does not match its hash is a
     * Disc2Stfs.Damaged. Nothing here closes or flushes out.
     */
    static String sha256(Candidate candidate, OutputStream out, Watch watch) throws IOException {
        InputStream in = candidate.open();
        try {
            return sha256(in, candidate.bytes, out, watch);
        } finally {
            in.close();
        }
    }

    private static String sha256(InputStream in, long bytes, OutputStream out, Watch watch) throws IOException {
        MessageDigest sha = VerifiedExtractor.sha256();
        byte[] buffer = new byte[(int) Math.max(1, Math.min(BUFFER_BYTES, bytes))];
        long done = 0;
        while (done < bytes) {
            int n = in.read(buffer, 0, (int) Math.min(buffer.length, bytes - done));
            if (n < 0) throw new EOFException("the file ends after " + done + " of " + bytes + " bytes");
            sha.update(buffer, 0, n);
            if (out != null) out.write(buffer, 0, n);
            done += n;
            if (watch != null && !watch.more(done)) throw new Stopped();
        }
        if (in.read() >= 0) throw new IOException("the file is longer than " + bytes + " bytes");
        return VerifiedExtractor.hex(sha.digest());
    }

    // ---- the copy plan -------------------------------------------------------------------------------------------

    /** One entry of the pinned record: a file the game needs, by content. */
    static final class Pin {
        /** 64 lower-case hex digits. */
        final String sha256;
        final long bytes;

        Pin(String sha256, long bytes) {
            if (!isSha256(sha256) || bytes < 0) throw new IllegalArgumentException("a pinned entry needs a SHA-256 value and a size");
            this.sha256 = sha256;
            this.bytes = bytes;
        }
    }

    enum Verdict {
        /** A pinned file that is not installed yet: keep it as inputs/<sha256>.bin. */
        WANTED,
        /** A pinned file that was installed when the plan was made: nothing to keep. */
        ALREADY_INSTALLED,
        /** Not a pinned file (its size is one, its content is not): nothing to keep. */
        NOT_PINNED
    }

    static final class Plan {
        /** The candidates to read, in the order they were given. Unmodifiable. */
        final List<Candidate> read;
        /** The bytes of all of them together. */
        final long bytes;
        /** The pinned entries that were not installed when the plan was made, in the order they were given. */
        final List<Pin> wanted;
        /**
         * Those of wanted whose size no candidate has: this disc cannot provide them. By size only: an entry is not
         * listed here as long as any candidate has its size, so for "does the disc hold all of them" use shortSizes.
         */
        final List<Pin> noCandidate;
        /**
         * One size for every wanted entry that is left over when each candidate is matched to at most one wanted entry
         * of its size: at least that many wanted entries cannot come from this disc, whatever its files contain.
         * Empty when the disc has a candidate for each. With four entries of 14 bytes wanted and three candidates of
         * 14 bytes it holds one 14. The question is exact only when pinned held just the entries this disc is to
         * provide: entries of the other disc, given along, are counted as short too.
         */
        final List<Long> shortSizes;
        private final List<Pin> pinned;
        private final Set<String> installed;

        Plan(List<Candidate> read, long bytes, List<Pin> wanted, List<Pin> noCandidate, List<Long> shortSizes, List<Pin> pinned,
                Set<String> installed) {
            this.read = Collections.unmodifiableList(read);
            this.bytes = bytes;
            this.wanted = Collections.unmodifiableList(wanted);
            this.noCandidate = Collections.unmodifiableList(noCandidate);
            this.shortSizes = Collections.unmodifiableList(shortSizes);
            this.pinned = pinned;
            this.installed = installed;
        }

        /** The pinned entry with this size and SHA-256, or null. */
        Pin pin(long bytes, String sha256) {
            for (Pin p : pinned) {
                if (p.bytes == bytes && p.sha256.equals(sha256)) return p;
            }
            return null;
        }

        /** After a file was read: what its SHA-256 says about it. The plan itself does not change. */
        Verdict judge(long bytes, String sha256) {
            Pin p = pin(bytes, sha256);
            if (p == null) return Verdict.NOT_PINNED;
            return installed.contains(p.sha256) ? Verdict.ALREADY_INSTALLED : Verdict.WANTED;
        }
    }

    /**
     * Which candidates to read. pinned: the record's entries; installedSha256: the SHA-256 values of the pinned
     * entries that are in place (see installed()). A candidate is read only when some pinned entry of exactly its
     * size is not installed: its content is known only after reading, so every candidate of such a size is read.
     */
    static Plan plan(List<Candidate> candidates, List<Pin> pinned, Collection<String> installedSha256) {
        Set<String> installed = new HashSet<String>(installedSha256);
        List<Pin> all = new ArrayList<Pin>(), wanted = new ArrayList<Pin>(), noCandidate = new ArrayList<Pin>();
        Set<String> seen = new HashSet<String>();
        Set<Long> wantedSizes = new HashSet<Long>(), candidateSizes = new HashSet<Long>();
        for (Pin p : pinned) {
            if (!seen.add(p.sha256 + "/" + p.bytes)) continue;   // the same entry listed twice counts once
            all.add(p);
            if (!installed.contains(p.sha256)) {
                wanted.add(p);
                wantedSizes.add(Long.valueOf(p.bytes));
            }
        }
        List<Candidate> read = new ArrayList<Candidate>();
        Map<Long, Integer> unmatched = new HashMap<Long, Integer>();   // candidates of each size not yet matched to a wanted entry
        long bytes = 0;
        for (Candidate c : candidates) {
            Long size = Long.valueOf(c.bytes);
            candidateSizes.add(size);
            Integer count = unmatched.get(size);
            unmatched.put(size, Integer.valueOf(count == null ? 1 : count.intValue() + 1));
            if (!wantedSizes.contains(size)) continue;
            read.add(c);
            bytes += c.bytes;
        }
        List<Long> shortSizes = new ArrayList<Long>();
        for (Pin p : wanted) {
            Long size = Long.valueOf(p.bytes);
            if (!candidateSizes.contains(size)) noCandidate.add(p);
            Integer left = unmatched.get(size);
            if (left == null || left.intValue() == 0) shortSizes.add(size);
            else unmatched.put(size, Integer.valueOf(left.intValue() - 1));
        }
        return new Plan(read, bytes, wanted, noCandidate, shortSizes, all, installed);
    }

    /**
     * The SHA-256 values of the pinned entries that are in place in inputsDirectory: a regular file named
     * <sha256>.bin of exactly the pinned size. A symbolic link under that name is not the file, wherever it points:
     * the ship build's Ready rule (ShipData) does not follow one either, and if this look did, the setup page would
     * show the disc as not done while the copy found nothing to do. The copy then replaces the link itself by the
     * verified file (a rename; what the link pointed to is not touched). The content is taken on trust: this is right
     * as long as a file gets that name only after its content was verified, and it does not notice a file that was
     * changed afterwards. wrongContent() is the slow check that reads the files.
     */
    static Set<String> installed(File inputsDirectory, List<Pin> pinned) {
        Set<String> out = new HashSet<String>();
        for (Pin p : pinned) {
            File file = new File(inputsDirectory, p.sha256 + ".bin");
            if (regularFile(file) && file.length() == p.bytes) out.add(p.sha256);
        }
        return out;
    }

    /**
     * The pinned entries that are not in place in inputsDirectory by the rule of installed(), each once, in the order
     * given. Empty when every one of them is there. Asked after a disc was copied with the entries that disc is to
     * provide, a non-empty answer means the disc did not provide them all; asked with every pinned entry, it says
     * whether the game's files are complete.
     */
    static List<Pin> missing(File inputsDirectory, List<Pin> pinned) {
        Set<String> have = installed(inputsDirectory, pinned), seen = new HashSet<String>();
        List<Pin> out = new ArrayList<Pin>();
        for (Pin p : pinned) {
            if (seen.add(p.sha256 + "/" + p.bytes) && !have.contains(p.sha256)) out.add(p);
        }
        return out;
    }

    /**
     * The slow check of what installed() takes on trust: reads every pinned entry's file that is in place by name and
     * size, and returns the entries whose content does not have the SHA-256 the file is named after (each once, in
     * the order given). Entries without a file are not listed; missing() names those. Reads all the installed bytes
     * (16.4 GB for both discs); never writes and never removes a file. The watch is told the bytes done of the file
     * being read; when it answers false the check ends with Stopped.
     */
    static List<Pin> wrongContent(File inputsDirectory, List<Pin> pinned, Watch watch) throws IOException {
        List<Pin> out = new ArrayList<Pin>();
        Set<String> seen = new HashSet<String>();
        byte[] buffer = new byte[BUFFER_BYTES];
        for (Pin p : pinned) {
            File file = new File(inputsDirectory, p.sha256 + ".bin");
            if (!seen.add(p.sha256 + "/" + p.bytes) || !regularFile(file) || file.length() != p.bytes) continue;
            MessageDigest sha = VerifiedExtractor.sha256();
            long done = 0;
            FileInputStream in = new FileInputStream(file);
            try {
                int n;
                while ((n = in.read(buffer, 0, buffer.length)) >= 0) {
                    sha.update(buffer, 0, n);
                    done += n;
                    if (watch != null && !watch.more(done)) throw new Stopped();
                }
            } finally {
                in.close();
            }
            if (done != p.bytes || !VerifiedExtractor.hex(sha.digest()).equals(p.sha256)) out.add(p);
        }
        return out;
    }

    /** A regular file under exactly this name: a symbolic link is not followed. */
    private static boolean regularFile(File file) {
        try {
            return java.nio.file.Files.isRegularFile(file.toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS);
        } catch (RuntimeException invalid) {
            return false;
        }
    }

    private static boolean isSha256(String text) {
        if (text == null || text.length() != 64) return false;
        for (int i = 0; i < 64; ++i) {
            char c = text.charAt(i);
            if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f')) return false;
        }
        return true;
    }
}
