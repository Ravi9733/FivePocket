package dev.recompbootstrap.smoke;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.Locale;

/**
 * Finds the game partition in a disc image the player picked and says in one true sentence what the file is.
 * Pure Java; reads at most a few sectors; never writes.
 *
 * A full image (as a disc drive reads the whole disc) carries the game partition at a fixed position after the
 * video partition; a trimmed image ("XISO") starts with it. The positions are Xdvdfs.PARTITION_OFFSETS (0, XGD2
 * 0x0FD90000, XGD3 0x02080000, XGD1 0x18300000). Xdvdfs.open takes the first position whose sector 32 carries the
 * volume text; this probe additionally requires that the root folder named there lies inside the file and that its
 * first record is well formed, so a stray text or a cut-off image is reported as such, and a damaged descriptor at
 * an earlier position does not hide a good partition at a later one.
 *
 * Two texts for a file that is refused (setup-fixes, defects C4-1/C4-2/C4-3 of the vm-robust run): message, for the
 * player, without positions, hex values or byte counts; detail, for the log only, with them. For a file that is not
 * a disc image at all, what says what it seems to be instead, and packed whether it is an archive a disc image may
 * be packed in; Disc2Discs builds the player's sentence from them with the game's name.
 */
final class Disc2ImageProbe {
    enum Kind { TRIMMED, FULL_XGD2, FULL_XGD3, FULL_XGD1, CUT_OFF, NOT_A_DISC_IMAGE, UNREADABLE }

    static final class Result {
        final Kind kind;
        /** Byte position of the game partition, or -1. */
        final long partitionOffset;
        /** For the player: no position, hex value or byte count. */
        final String message;
        /** For the log: the facts the message leaves out (positions, sizes); null when it leaves nothing out. */
        final String detail;
        /** NOT_A_DISC_IMAGE: what the file seems to be instead ("it is a ZIP archive"), or null when that is not known. */
        final String what;
        /** NOT_A_DISC_IMAGE: an archive a disc image may be packed in (ZIP, RAR, 7-Zip; not an app). */
        final boolean packed;

        Result(Kind kind, long partitionOffset, String message) {
            this(kind, partitionOffset, message, null, null, false);
        }

        Result(Kind kind, long partitionOffset, String message, String detail, String what, boolean packed) {
            this.kind = kind;
            this.partitionOffset = partitionOffset;
            this.message = message;
            this.detail = detail;
            this.what = what;
            this.packed = packed;
        }

        boolean usable() {
            return partitionOffset >= 0;
        }
    }

    private static final int SECTOR = Xdvdfs.SECTOR;
    private static final byte[] MAGIC = "MICROSOFT*XBOX*MEDIA".getBytes(StandardCharsets.US_ASCII);
    static final String TEXT_CUT_OFF = "This disc image is damaged: the disc's file system in it is incomplete or cut off.";
    static final String TEXT_UNPACK = "If a disc image is packed inside it, unpack it first.";
    /** The largest ZIP central directory read to look for an app's manifest; an APK's is far smaller. */
    private static final int MAX_ZIP_DIRECTORY = 8 << 20;

    private Disc2ImageProbe() {}

    static Kind kindOf(long offset) {
        if (offset == 0L) return Kind.TRIMMED;
        if (offset == 0x0FD90000L) return Kind.FULL_XGD2;
        if (offset == 0x02080000L) return Kind.FULL_XGD3;
        if (offset == 0x18300000L) return Kind.FULL_XGD1;
        throw new IllegalArgumentException("not a known partition offset");
    }

    static String describe(Kind kind) {
        switch (kind) {
            case TRIMMED: return "a trimmed disc image";
            case FULL_XGD2: return "a full disc image (XGD2 layout)";
            case FULL_XGD3: return "a full disc image (XGD3 layout)";
            case FULL_XGD1: return "a full disc image (XGD1 layout)";
            default: return kind.name();
        }
    }

    static Result probe(DiscSource image) {
        try {
            return probeOrThrow(image);
        } catch (IOException e) {
            return new Result(Kind.UNREADABLE, -1, "Unreadable: " + VerifiedExtractor.reason(e) + ".");
        } catch (RuntimeException e) {
            return new Result(Kind.UNREADABLE, -1, "Unreadable: " + VerifiedExtractor.reason(e) + ".");
        }
    }

    private static Result probeOrThrow(DiscSource image) throws IOException {
        long size = image.size();
        String cutOff = null;
        byte[] head = new byte[SECTOR];
        for (long offset : Xdvdfs.PARTITION_OFFSETS) {
            if (offset + 33L * SECTOR > size) continue;
            image.readFully(offset + 32L * SECTOR, head, 0, SECTOR);
            if (!startsWith(head, 0, MAGIC) || !startsWith(head, SECTOR - MAGIC.length, MAGIC)) continue;
            long rootSector = le32(head, 20), rootSize = le32(head, 24);
            long rootStart = offset + rootSector * SECTOR;
            if (rootSize == 0 || rootSize > Xdvdfs.MAX_DIRECTORY_BYTES) {
                if (cutOff == null) cutOff = "the disc file system at " + hex(offset) + " names an impossible root folder";
                continue;
            }
            if (rootStart + rootSize > size) {
                if (cutOff == null) {
                    cutOff = "the disc file system at " + hex(offset) + " needs at least " + (rootStart + rootSize)
                            + " bytes but the file has " + size + ": the image is cut off";
                }
                continue;
            }
            byte[] first = new byte[14];
            image.readFully(rootStart, first, 0, 14);
            int nameLength = first[13] & 0xFF;
            boolean padding = (first[0] & 0xFF) == 0xFF && (first[1] & 0xFF) == 0xFF;
            if (padding || nameLength == 0 || 14 + nameLength > rootSize) {
                if (cutOff == null) cutOff = "the root folder of the disc file system at " + hex(offset) + " is damaged";
                continue;
            }
            Kind kind = kindOf(offset);
            return new Result(kind, offset, "Found " + describe(kind) + ", game partition at byte " + offset + ".");
        }
        if (cutOff != null) return new Result(Kind.CUT_OFF, -1, TEXT_CUT_OFF, "Damaged image: " + cutOff + ".", null, false);
        String[] sniffed = sniff(image, size);
        boolean packed = sniffed[2] != null;
        String what = sniffed[1];
        return new Result(Kind.NOT_A_DISC_IMAGE, -1, "This file is not a disc image" + (what == null ? "." : ": " + what + ".")
                + (packed ? " " + TEXT_UNPACK : ""), "Not a disc image this app reads: " + sniffed[0], what, packed);
    }

    /**
     * What the file seems to be instead: {the log's sentence end (with sizes and positions), the player's clause or
     * null, non-null when it is an archive a disc image may be packed in}.
     */
    private static String[] sniff(DiscSource image, long size) throws IOException {
        if (size < 33L * SECTOR) return new String[] {"the file is only " + size + " bytes, smaller than any disc image.", "it is far too small", null};
        byte[] start = new byte[16];
        image.readFully(0, start, 0, 16);
        String four = new String(start, 0, 4, StandardCharsets.ISO_8859_1);
        if (four.equals("PIRS") || four.equals("LIVE") || four.equals("CON ")) {
            return new String[] {"it is an Xbox 360 content package, not a disc image.", "it is an Xbox 360 content package (a download, an add-on or a save)", null};
        }
        if (four.equals("XEX2")) return new String[] {"it is the game executable alone, not a disc image.", "it is a single game program file", null};
        if (four.equals("PK\u0003\u0004")) {
            // An app (an .apk is a ZIP archive with an Android manifest) holds no disc image: no advice to unpack it.
            if (androidApp(image, size)) return new String[] {"it is an Android app (a ZIP archive with AndroidManifest.xml).", "it is an Android app", null};
            return new String[] {"it is a ZIP archive.", "it is a ZIP archive", "packed"};
        }
        if (four.equals("Rar!")) return new String[] {"it is a RAR archive.", "it is a RAR archive", "packed"};
        if ((start[0] & 0xFF) == 0x37 && (start[1] & 0xFF) == 0x7A && (start[2] & 0xFF) == 0xBC && (start[3] & 0xFF) == 0xAF) {
            return new String[] {"it is a 7-Zip archive.", "it is a 7-Zip archive", "packed"};
        }
        if (size >= 0x8006) {
            byte[] iso = new byte[5];
            image.readFully(0x8001, iso, 0, 5);
            if (new String(iso, StandardCharsets.ISO_8859_1).equals("CD001")) {
                return new String[] {"it is an ordinary CD/DVD image without an Xbox 360 game partition at any known position"
                        + " (it may hold only the disc's video part).", "it is an ordinary CD or DVD image (it may hold only the video part of a disc)", null};
            }
        }
        return new String[] {"no Xbox 360 game partition at any of the known positions (0, 0x0FD90000, 0x02080000, 0x18300000).", null, null};
    }

    /**
     * True for a ZIP archive whose central directory names AndroidManifest.xml at its top level: an Android app.
     * Reads the end record (at most 64 KiB + 22 bytes from the end) and the central directory (at most 8 MiB). A
     * ZIP64 archive or one whose records do not add up is not called an app.
     */
    static boolean androidApp(DiscSource image, long size) throws IOException {
        int tail = (int) Math.min(size, 65535L + 22L);
        if (tail < 22) return false;
        byte[] end = new byte[tail];
        image.readFully(size - tail, end, 0, tail);
        for (int i = tail - 22; i >= 0; --i) {
            if (le32(end, i) != 0x06054B50L) continue;
            long directoryBytes = le32(end, i + 12), directoryAt = le32(end, i + 16);
            if (directoryBytes <= 0 || directoryBytes > MAX_ZIP_DIRECTORY || directoryAt + directoryBytes > size) return false;
            byte[] directory = new byte[(int) directoryBytes];
            image.readFully(directoryAt, directory, 0, directory.length);
            int at = 0;
            while (at + 46 <= directory.length && le32(directory, at) == 0x02014B50L) {
                int name = le16(directory, at + 28), extra = le16(directory, at + 30), comment = le16(directory, at + 32);
                if (at + 46 + name > directory.length) return false;
                if (new String(directory, at + 46, name, StandardCharsets.ISO_8859_1).equals("AndroidManifest.xml")) return true;
                at += 46 + name + extra + comment;
            }
            return false;
        }
        return false;
    }

    private static int le16(byte[] d, int at) {
        return (d[at] & 0xFF) | (d[at + 1] & 0xFF) << 8;
    }

    private static boolean startsWith(byte[] data, int at, byte[] text) {
        for (int i = 0; i < text.length; ++i) {
            if (data[at + i] != text[i]) return false;
        }
        return true;
    }

    private static long le32(byte[] d, int at) {
        return (d[at] & 0xFFL) | (d[at + 1] & 0xFFL) << 8 | (d[at + 2] & 0xFFL) << 16 | (d[at + 3] & 0xFFL) << 24;
    }

    private static String hex(long value) {
        return String.format(Locale.ROOT, "0x%08X", value);
    }
}
