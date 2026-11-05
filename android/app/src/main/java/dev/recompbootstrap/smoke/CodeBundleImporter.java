package dev.recompbootstrap.smoke;

import java.io.EOFException;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.FileVisitResult;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.StandardCopyOption;
import java.nio.file.attribute.BasicFileAttributes;
import java.security.MessageDigest;
import java.util.HashSet;
import java.util.Set;
import java.util.zip.CRC32;

/**
 * Imports a code bundle made by the desktop builder into files/game-install/bundle. Pure Java.
 *
 * The file is read once, front to back (it comes from the system file chooser as a stream). Every component is
 * written into staging/bundle.part while its size, CRC-32 and SHA-256 are checked against the manifest; the seal
 * over the whole file is checked at the end; only then is the directory renamed to bundle/ in one step. Any
 * failure removes staging/bundle.part and leaves bundle/ absent. An existing bundle/ is never replaced.
 *
 * Checks, in the order of tools/own_disc/BUNDLE-FORMAT.md: (3) the first entry is manifest.json, no name repeats,
 * every entry is stored, not encrypted and well named; (4) format version 1 and the runtime interface this app
 * implements; (6) the bundle was built from the game executable of the supported release, and the player's own
 * copy of that executable is already in inputs/; (5) the entries are exactly the listed components, each with the
 * listed size and SHA-256; (1, 2) the file ends with the seal record and the seal equals the SHA-256 of every byte
 * before it; (7) components are stored with mode 0400 and the manifest is kept beside them.
 */
final class CodeBundleImporter {
    static final String BUNDLE = "bundle";
    static final String PART = VerifiedExtractor.STAGING + "/bundle.part";
    static final int SEAL_BYTES = 85, END_RECORD_BYTES = 22, MAX_MANIFEST_BYTES = 1 << 20;
    private static final byte[] SEAL_PREFIX = "own-disc-seal sha256=".getBytes(StandardCharsets.US_ASCII);
    private static final int LOCAL_HEADER = 0x04034b50, CENTRAL_HEADER = 0x02014b50, END_RECORD = 0x06054b50;

    private CodeBundleImporter() {}

    static VerifiedExtractor.Outcome importBundle(InputStream zip, File root, VerifiedExtractor.Progress progress) {
        return importBundle(zip, root, progress, SetupCatalog.spec(), VerifiedExtractor.DISK);
    }

    /** As above with the expected release and the free-space source given (tests use synthetic ones). */
    static VerifiedExtractor.Outcome importBundle(InputStream zip, File root, VerifiedExtractor.Progress progress,
            SetupCatalog.Spec spec, VerifiedExtractor.Space space) {
        if (!VerifiedExtractor.begin()) {
            return VerifiedExtractor.Outcome.failed(VerifiedExtractor.Problem.BUSY, VerifiedExtractor.BUSY_MESSAGE);
        }
        try {
            return importLocked(zip, root, progress, spec, space, null);
        } finally {
            VerifiedExtractor.end();
        }
    }

    /** Public imports remain quarantined until all pinned disc files are copied and the launch gate passes. */
    static VerifiedExtractor.Outcome importPublic(InputStream zip, File root, VerifiedExtractor.Progress progress,
            SetupCatalog.Spec spec, VerifiedExtractor.Space space, ShipConfig contract) {
        if (contract == null) return VerifiedExtractor.Outcome.failed(PublicCodeWords.APP_RECORD);
        if (!VerifiedExtractor.begin()) return VerifiedExtractor.Outcome.failed(VerifiedExtractor.Problem.BUSY, PublicCodeWords.BUSY);
        try { return importLocked(zip, root, progress, spec, space, contract); }
        finally { VerifiedExtractor.end(); }
    }

    private static VerifiedExtractor.Outcome importLocked(InputStream zip, File root, VerifiedExtractor.Progress progress,
            SetupCatalog.Spec spec, VerifiedExtractor.Space space, ShipConfig publicContract) {
        File part = null;
        boolean committed = false;
        long total = 0;
        try {
            String problem = InstallGuard.rootProblem(root);
            if (problem != null) return VerifiedExtractor.Outcome.failed(problem);
            String existing = InstallGuard.existingInstall(root.getAbsoluteFile().getParentFile());
            if (publicContract == null && existing != null) {
                return VerifiedExtractor.Outcome.failed(VerifiedExtractor.Problem.EXISTING_INSTALL, VerifiedExtractor.EXISTING_MESSAGE + existing);
            }
            File bundleDirectory = InstallGuard.under(root, BUNDLE);
            if (bundleDirectory.exists()) {
                // An earlier import renamed the directory into place but could not write the record any more.
                String damage = publicContract == null ? verifyInstalled(root, spec) : PublicGameCode.installedProblem(root, publicContract);
                if (damage != null) {
                    if (publicContract != null) return VerifiedExtractor.Outcome.failed(VerifiedExtractor.Problem.EXISTING_INSTALL, damage);
                    return VerifiedExtractor.Outcome.failed(VerifiedExtractor.Problem.EXISTING_INSTALL, "Game code is already on this device"
                            + " but does not pass its check (" + damage + "); it was not changed.");
                }
                if (publicContract == null) InstallGuard.refreshRecord(root, spec);
                return VerifiedExtractor.Outcome.done();
            }
            if (publicContract == null && !new File(new File(root, VerifiedExtractor.INPUTS), spec.executableSha256 + ".bin").isFile()) {
                return VerifiedExtractor.Outcome.failed("Copy the game files from your play disc first: the game code is checked"
                        + " against the game program from your own disc.");
            }

            Sealed in = new Sealed(zip);
            byte[] header = new byte[30];

            // the manifest: always the first entry
            String name = readLocalHeader(in, header, true);
            if (!CodeBundle.MANIFEST_NAME.equals(name)) return notABundle("its first entry is not " + CodeBundle.MANIFEST_NAME);
            long manifestBytes = u32(header, 22);
            if (manifestBytes > MAX_MANIFEST_BYTES) return notABundle("its manifest is larger than 1 MiB");
            byte[] manifestData = new byte[(int) manifestBytes];
            readFully(in, manifestData, manifestData.length);
            CRC32 crc = new CRC32();
            crc.update(manifestData, 0, manifestData.length);
            if (crc.getValue() != u32(header, 14)) return notABundle("its manifest is damaged");
            CodeBundle bundle;
            try {
                bundle = CodeBundle.parseManifest(new String(manifestData, StandardCharsets.UTF_8));
            } catch (JsonLite.ParseException e) {
                return notABundle("its manifest cannot be read");
            } catch (IllegalArgumentException e) {
                return notABundle(e.getMessage());
            }
            if (bundle.version != (publicContract == null ? 1 : 2)) {
                if (publicContract != null) return VerifiedExtractor.Outcome.failed(VerifiedExtractor.Problem.BUNDLE_OTHER, PublicCodeWords.VERSION);
                return VerifiedExtractor.Outcome.failed(VerifiedExtractor.Problem.BUNDLE_OTHER, "This code bundle has format "
                        + bundle.version + "; this app version reads format 1 only.");
            }
            if (!(publicContract == null ? CodeBundle.RUNTIME_ABI : CodeBundle.PUBLIC_ABI).equals(bundle.runtimeAbi)) {
                return VerifiedExtractor.Outcome.failed(VerifiedExtractor.Problem.BUNDLE_OTHER, "This code bundle was built for"
                        + " another app version (runtime interface " + bundle.runtimeAbi + ").");
            }
            if (!spec.executableSha256.equals(bundle.executableSha256)) {
                if (publicContract != null) return VerifiedExtractor.Outcome.failed(VerifiedExtractor.Problem.BUNDLE_OTHER, PublicCodeWords.GAME);
                return VerifiedExtractor.Outcome.failed(VerifiedExtractor.Problem.BUNDLE_OTHER, "This code bundle was built from"
                        + " another game program (check code " + bundle.executableSha256.substring(0, 12) + ") than the one on the"
                        + " play disc of " + spec.label + ".");
            }

            if (publicContract != null) {
                String refusal = PublicGameCode.contractProblem(bundle, publicContract);
                if (refusal != null) return VerifiedExtractor.Outcome.failed(VerifiedExtractor.Problem.BUNDLE_OTHER, refusal);
            }

            // What an interrupted run left in staging is removed below, so its bytes are free for this import.
            total = bundle.totalBytes();
            long need = total + VerifiedExtractor.RESERVE_BYTES;
            long free = VerifiedExtractor.freeWithLeftovers(root, space);
            if (need > free) return VerifiedExtractor.Outcome.noSpace("Not enough space", need, free);

            // from here on files are written, all of them under staging/bundle.part
            part = InstallGuard.under(root, PART);   // refuses a link standing there before anything is removed
            VerifiedExtractor.clearLeftovers(root);  // left by an interrupted run: start again
            VerifiedExtractor.makeDirectory(part);
            writeFile(InstallGuard.under(root, PART + "/" + CodeBundle.MANIFEST_NAME), manifestData);

            Set<String> seen = new HashSet<String>();
            byte[] buffer = new byte[1 << 20];
            long done = 0;
            while (true) {
                name = readLocalHeader(in, header, false);
                if (name == null) break;   // the central directory starts: no more entries
                CodeBundle.Entry entry = bundle.entry(name);
                if (entry == null) return notABundle("entry " + name + " is not listed in its manifest");
                if (!seen.add(name)) return notABundle("entry " + name + " appears twice");
                if (progress.cancelled()) return VerifiedExtractor.Outcome.stopped();
                progress.update(done, total, seen.size() - 1, bundle.entries.size(), name);
                if (u32(header, 22) != entry.bytes) {
                    return damaged("The code bundle is damaged: " + name + " has another size than its list of contents says.");
                }
                File target = InstallGuard.under(root, PART + "/" + name);
                VerifiedExtractor.makeDirectory(target.getParentFile());
                MessageDigest sha = VerifiedExtractor.sha256();
                crc.reset();
                FileChannel out = VerifiedExtractor.createNew(target);
                try {
                    long left = entry.bytes;
                    while (left > 0) {
                        if (progress.cancelled()) return VerifiedExtractor.Outcome.stopped();
                        int n = in.read(buffer, 0, (int) Math.min(buffer.length, left));
                        if (n < 0) return damaged("The bundle file is incomplete: it ends inside " + name + ".");
                        VerifiedExtractor.writeAll(out, buffer, n);
                        sha.update(buffer, 0, n);
                        crc.update(buffer, 0, n);
                        left -= n;
                        done += n;
                        progress.update(done, total, seen.size() - 1, bundle.entries.size(), name);
                    }
                    out.force(true);
                } finally {
                    out.close();
                }
                if (crc.getValue() != u32(header, 14) || !VerifiedExtractor.hex(sha.digest()).equals(entry.sha256)) {
                    return damaged("The code bundle is damaged: " + name + " does not match its list of contents.");
                }
                VerifiedExtractor.ownerReadOnly(target);
            }
            for (CodeBundle.Entry entry : bundle.entries) {
                if (!seen.contains(entry.name)) return notABundle("component " + entry.name + " is missing");
            }

            // the rest of the file (the ZIP directory and the seal record), then the seal over everything
            while (in.read(buffer, 0, buffer.length) >= 0) {
                if (progress.cancelled()) return VerifiedExtractor.Outcome.stopped();
            }
            String sealProblem = in.sealProblem();
            if (sealProblem != null) return damaged(sealProblem);

            if (bundleDirectory.exists()) {
                return VerifiedExtractor.Outcome.failed(VerifiedExtractor.Problem.EXISTING_INSTALL, "Game code appeared in place"
                        + " meanwhile; it was not changed.");
            }
            if (publicContract != null) writeFile(InstallGuard.under(root, PART + "/seal.txt"),
                    (in.seal() + "\n").getBytes(StandardCharsets.US_ASCII));
            VerifiedExtractor.syncDirectory(part);
            Files.move(part.toPath(), bundleDirectory.toPath(), StandardCopyOption.ATOMIC_MOVE);
            committed = true;
            VerifiedExtractor.syncDirectory(root);
            if (publicContract == null) InstallGuard.refreshRecord(root, spec);
            progress.update(total, total, bundle.entries.size(), bundle.entries.size(), "");
            return VerifiedExtractor.Outcome.done();
        } catch (EOFException e) {
            return damaged("The bundle file is incomplete (" + VerifiedExtractor.reason(e) + ").");
        } catch (NotABundle e) {
            return notABundle(VerifiedExtractor.reason(e));
        } catch (SourceFailed e) {
            return VerifiedExtractor.Outcome.failed(VerifiedExtractor.Problem.SOURCE_UNREADABLE, "The bundle file could not be read ("
                    + VerifiedExtractor.reason(e) + ").");
        } catch (IOException e) {
            // Writing the staging copy failed. Asked after that copy is gone: with too little room for the whole
            // bundle now, space is the reason to give.
            if (part != null) {
                discard(part);
                long free = space.usable(VerifiedExtractor.existingAncestor(root));
                long need = total + VerifiedExtractor.RESERVE_BYTES;
                if (need > free) return VerifiedExtractor.Outcome.noSpace("This device ran out of space during the import", need, free);
            }
            return VerifiedExtractor.Outcome.failed(VerifiedExtractor.Problem.STORAGE, "Could not import the game code: "
                    + VerifiedExtractor.reason(e) + ".");
        } catch (RuntimeException e) {
            return VerifiedExtractor.Outcome.failed("Import stopped unexpectedly: " + VerifiedExtractor.reason(e) + ".");
        } finally {
            if (part != null && !committed) discard(part);
        }
    }

    /** Removes the staging copy of an import that did not finish. A failure is left to the next run, which clears staging first. */
    private static void discard(File part) {
        try {
            if (part.exists()) deleteTree(part);
        } catch (IOException ignored) {
            // the next copy or import removes it before it starts
        }
    }

    /**
     * Re-checks the imported components against the manifest kept beside them (size and SHA-256 of every one).
     * null when everything matches, else the first problem. Read-only.
     */
    static String verifyInstalled(File root, SetupCatalog.Spec spec) {
        try {
            File directory = InstallGuard.under(root, BUNDLE);
            byte[] manifestData = InstallGuard.readSmall(new File(directory, CodeBundle.MANIFEST_NAME), MAX_MANIFEST_BYTES);
            if (manifestData == null) return "its manifest is missing";
            CodeBundle bundle = CodeBundle.parseManifest(new String(manifestData, StandardCharsets.UTF_8));
            if (bundle.version != 1 || !CodeBundle.RUNTIME_ABI.equals(bundle.runtimeAbi)) return "it is for another app version";
            if (!spec.executableSha256.equals(bundle.executableSha256)) return "it was built from another game executable";
            byte[] buffer = new byte[1 << 20];
            for (CodeBundle.Entry entry : bundle.entries) {
                File file = InstallGuard.under(root, BUNDLE + "/" + entry.name);
                if (!file.isFile() || file.length() != entry.bytes) return entry.name + " is missing or has another size";
                MessageDigest sha = VerifiedExtractor.sha256();
                FileInputStream in = new FileInputStream(file);
                try {
                    int n;
                    while ((n = in.read(buffer, 0, buffer.length)) >= 0) sha.update(buffer, 0, n);
                } finally {
                    in.close();
                }
                if (!VerifiedExtractor.hex(sha.digest()).equals(entry.sha256)) return entry.name + " has changed";
            }
            return null;
        } catch (JsonLite.ParseException e) {
            return "its manifest cannot be read";
        } catch (IOException e) {
            return VerifiedExtractor.reason(e);
        } catch (RuntimeException e) {
            return VerifiedExtractor.reason(e);
        }
    }

    private static VerifiedExtractor.Outcome notABundle(String why) {
        return VerifiedExtractor.Outcome.failed(VerifiedExtractor.Problem.NOT_A_BUNDLE, "This file is not a usable code bundle: " + why + ".");
    }

    private static VerifiedExtractor.Outcome damaged(String sentence) {
        return VerifiedExtractor.Outcome.failed(VerifiedExtractor.Problem.BUNDLE_DAMAGED, sentence);
    }

    /** The file is not a code bundle of format 1 at all (as opposed to a bundle that is cut off or cannot be read). */
    private static final class NotABundle extends IOException {
        NotABundle(String message) {
            super(message);
        }
    }

    /** Reading the chosen file failed (as opposed to writing the copy). */
    private static final class SourceFailed extends IOException {
        SourceFailed(String message) {
            super(message);
        }
    }

    /**
     * Reads the next ZIP local file header into header (30 bytes) and returns the entry's name; returns null when
     * the central directory starts instead (only when first is false). Refuses everything format 1 does not allow.
     */
    private static String readLocalHeader(Sealed in, byte[] header, boolean first) throws IOException {
        readFully(in, header, 4);
        long signature = u32(header, 0);
        if (!first && (signature == CENTRAL_HEADER || signature == END_RECORD)) return null;
        if (signature != LOCAL_HEADER) throw new NotABundle("it does not begin like one");
        readFully(in, header, 4, 26);
        int flags = u16(header, 6), method = u16(header, 8);
        long packed = u32(header, 18), size = u32(header, 22);
        int nameLength = u16(header, 26), extraLength = u16(header, 28);
        if ((flags & ~0x0800) != 0) throw new NotABundle("it has an encrypted or streamed entry");
        if (method != 0 || packed != size) throw new NotABundle("it has a compressed entry");
        if (size > CodeBundle.MAX_BYTES || nameLength == 0 || nameLength > 512) throw new NotABundle("it has a damaged entry");
        byte[] nameBytes = new byte[nameLength];
        readFully(in, nameBytes, nameLength);
        String name = new String(nameBytes, StandardCharsets.UTF_8);
        if (!CodeBundle.validName(name)) throw new NotABundle("it has a badly named entry");
        byte[] extra = new byte[extraLength];
        readFully(in, extra, extraLength);
        return name;
    }

    private static void readFully(InputStream in, byte[] buffer, int length) throws IOException {
        readFully(in, buffer, 0, length);
    }

    private static void readFully(InputStream in, byte[] buffer, int offset, int length) throws IOException {
        int done = 0;
        while (done < length) {
            int n = in.read(buffer, offset + done, length - done);
            if (n < 0) throw new EOFException("it ends too early");
            done += n;
        }
    }

    private static void writeFile(File target, byte[] content) throws IOException {
        FileChannel out = VerifiedExtractor.createNew(target);
        try {
            VerifiedExtractor.writeAll(out, content, content.length);
            out.force(true);
        } finally {
            out.close();
        }
        VerifiedExtractor.ownerReadOnly(target);
    }

    /**
     * Removes staging/bundle.part with everything in it. The path comes from InstallGuard.under, so it lies inside
     * files/game-install. Symbolic links found inside are removed as links; they are never followed.
     */
    private static void deleteTree(File tree) throws IOException {
        Files.walkFileTree(tree.toPath(), new SimpleFileVisitor<Path>() {
            @Override
            public FileVisitResult visitFile(Path file, BasicFileAttributes attributes) throws IOException {
                Files.delete(file);
                return FileVisitResult.CONTINUE;
            }

            @Override
            public FileVisitResult postVisitDirectory(Path directory, IOException error) throws IOException {
                if (error != null) throw error;
                Files.delete(directory);
                return FileVisitResult.CONTINUE;
            }
        });
    }

    private static int u16(byte[] data, int at) {
        return (data[at] & 0xFF) | (data[at + 1] & 0xFF) << 8;
    }

    private static long u32(byte[] data, int at) {
        return (data[at] & 0xFFL) | (data[at + 1] & 0xFFL) << 8 | (data[at + 2] & 0xFFL) << 16 | (data[at + 3] & 0xFFL) << 24;
    }

    /**
     * The bundle stream with its seal computed on the way: the SHA-256 of every byte except the last 85, and the
     * last 107 bytes (the 22-byte ZIP end record and the 85-byte seal comment) kept for the final check.
     */
    private static final class Sealed extends InputStream {
        private static final int KEEP = END_RECORD_BYTES + SEAL_BYTES;
        private final InputStream in;
        private final MessageDigest sha = VerifiedExtractor.sha256();
        private final byte[] tail = new byte[KEEP];
        private int tailLength;
        private long total;

        Sealed(InputStream in) {
            this.in = in;
        }

        @Override
        public int read() throws IOException {
            byte[] one = new byte[1];
            return read(one, 0, 1) < 0 ? -1 : one[0] & 0xFF;
        }

        @Override
        public int read(byte[] buffer, int offset, int length) throws IOException {
            int n;
            try {
                n = in.read(buffer, offset, length);
            } catch (IOException e) {
                throw new SourceFailed(VerifiedExtractor.reason(e));
            }
            if (n <= 0) return n;
            total += n;
            if (total > CodeBundle.MAX_BYTES) throw new NotABundle("it is larger than 4 GiB");
            if (tailLength + n <= KEEP) {
                System.arraycopy(buffer, offset, tail, tailLength, n);
                tailLength += n;
            } else {
                // of the kept bytes followed by the new ones, everything but the last KEEP bytes is final: hash it
                int fromTail = Math.min(tailLength, tailLength + n - KEEP);
                sha.update(tail, 0, fromTail);
                int kept = tailLength - fromTail;
                if (kept > 0) System.arraycopy(tail, fromTail, tail, 0, kept);
                int fromBuffer = n - (KEEP - kept);
                if (fromBuffer > 0) sha.update(buffer, offset, fromBuffer);
                System.arraycopy(buffer, offset + fromBuffer, tail, kept, n - fromBuffer);
                tailLength = KEEP;
            }
            return n;
        }

        /** Call at the end of the stream. null when the file ends with a matching seal record. */
        String seal() {
            return new String(tail, END_RECORD_BYTES + SEAL_PREFIX.length, 64, StandardCharsets.US_ASCII);
        }

        String sealProblem() {
            if (tailLength < KEEP || u32(tail, 0) != END_RECORD || u16(tail, 20) != SEAL_BYTES) {
                return "This file is not a complete code bundle: its seal record is missing or damaged.";
            }
            for (int i = 0; i < SEAL_PREFIX.length; ++i) {
                if (tail[END_RECORD_BYTES + i] != SEAL_PREFIX[i]) {
                    return "This file is not a complete code bundle: its seal record is missing or damaged.";
                }
            }
            sha.update(tail, 0, END_RECORD_BYTES);
            String expected = new String(tail, END_RECORD_BYTES + SEAL_PREFIX.length, 64, StandardCharsets.US_ASCII);
            if (!VerifiedExtractor.hex(sha.digest()).equals(expected)) {
                return "The code bundle's seal does not match its contents.";
            }
            return null;
        }
    }
}
