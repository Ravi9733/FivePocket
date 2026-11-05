package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SetupTestKit.Node.dir;
import static dev.recompbootstrap.smoke.SetupTestKit.Node.file;
import static dev.recompbootstrap.smoke.SetupTestKit.bytes;
import static dev.recompbootstrap.smoke.SetupTestKit.check;
import static dev.recompbootstrap.smoke.SetupTestKit.expect;
import static dev.recompbootstrap.smoke.SetupTestKit.expectContains;
import static dev.recompbootstrap.smoke.SetupTestKit.expectEquals;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;

/** Run with java -ea. Synthetic discs only, except one read-only check of the local executable when it is there. */
public final class DiscIdentifierTest {
    private static final byte[] EXECUTABLE = bytes(70000, 11), OTHER = bytes(70000, 12);

    public static void main(String[] args) {
        SetupTestKit.requireAssertions();
        final SetupCatalog.Spec spec = SetupTestKit.spec(EXECUTABLE, new ArrayList<SetupCatalog.Item>(), true);
        final SetupTestKit.Image good = new SetupTestKit.Image(dir("", file("Default.XEX", EXECUTABLE), file("common.rpf", bytes(10, 13))));
        final SetupTestKit.Image other = new SetupTestKit.Image(dir("", file("default.xex", OTHER)));

        check("identify image: the expected executable hash means the supported play disc (at any partition offset)", () -> {
            for (long offset : Xdvdfs.PARTITION_OFFSETS) {
                DiscIdentifier.Result r = DiscIdentifier.identifyImage(good.at(offset), spec);
                expectEquals(DiscIdentifier.Kind.SUPPORTED_PLAY_DISC, r.kind, "kind at " + offset);
                expectEquals(SetupTestKit.sha256(EXECUTABLE), r.executableSha256, "hash");
                expectEquals("Recognised: Synthetic test release, the play disc.", r.message, "what was found: the release's name and which disc it is");
                expect(r.next == null, "nothing to do about a supported disc");
            }
        });

        check("identify image: another game program is an unsupported disc; a short check code is shown and what to choose instead", () -> {
            DiscIdentifier.Result r = DiscIdentifier.identifyImage(other.at(0), spec);
            expectEquals(DiscIdentifier.Kind.UNSUPPORTED_VERSION, r.kind, "kind");
            expectEquals(SetupTestKit.sha256(OTHER), r.executableSha256, "the hash that was found");
            expectEquals("This is not the play disc this version supports (its game program has check code "
                    + SetupTestKit.sha256(OTHER).substring(0, 12) + ").", r.message, "what was found, with 12 digits to quote in a report");
            expectContains(r.next, "the play disc (Disc 2) of Synthetic test release only", "what is supported");
            expectContains(r.next, "If you chose the install disc (Disc 1), choose Disc 2 instead.", "the likeliest mistake and its remedy");
        });

        check("identify image: one changed byte of the executable is enough to refuse", () -> {
            byte[] data = good.data.clone();
            int record = good.records.get("Default.XEX");
            int sector = (data[record + 4] & 0xFF) | (data[record + 5] & 0xFF) << 8;
            data[sector * 2048 + 69999] ^= 1;      // the last byte of the executable
            DiscIdentifier.Result r = DiscIdentifier.identifyImage(new SetupTestKit.MemoryDisc(0, data), spec);
            expectEquals(DiscIdentifier.Kind.UNSUPPORTED_VERSION, r.kind, "kind");
        });

        check("identify image: no disc file system, or no executable on it, is not a game disc", () -> {
            DiscIdentifier.Result none = DiscIdentifier.identifyImage(new SetupTestKit.MemoryDisc(0, bytes(200000, 14)), spec);
            expectEquals(DiscIdentifier.Kind.NOT_A_GAME_DISC, none.kind, "random bytes");
            expect(none.executableSha256 == null, "no hash");
            expectEquals("This file is not an Xbox 360 disc image that this app can read.", none.message, "what was found");
            expectContains(none.next, "Choose the .iso image of the play disc (Disc 2), or the folder that holds the disc's files.", "what to choose");
            expectContains(none.next, "An image packed inside a .zip, .7z or .rar file has to be unpacked first.", "the likeliest cause");
            SetupTestKit.Image noExe = new SetupTestKit.Image(dir("", file("readme.txt", bytes(10, 15)), dir("default.xex")));
            DiscIdentifier.Result missing = DiscIdentifier.identifyImage(noExe.at(0), spec);
            expectEquals(DiscIdentifier.Kind.NOT_A_GAME_DISC, missing.kind, "a folder named default.xex is not the executable");
            expectEquals("This disc image holds no game program (default.xex).", missing.message, "message");
            expectContains(missing.next, "the play disc (Disc 2) of Synthetic test release only", "what is supported");
        });

        check("identify image: an image that is cut off or whose folder tables are wrong is a damaged copy, not 'unsupported': make a new one", () -> {
            int record = good.records.get("Default.XEX");
            int sector = (good.data[record + 4] & 0xFF) | (good.data[record + 5] & 0xFF) << 8;
            DiscIdentifier.Result r = DiscIdentifier.identifyImage(good.truncated(sector * 2048 + 30000), spec);
            expectEquals(DiscIdentifier.Kind.DAMAGED, r.kind, "cut inside the game program: kind");
            expect(r.executableSha256 == null, "no hash of half a file");
            expectEquals("This disc image is incomplete: it ends before the disc's data does.", r.message, "cut: what was found");
            expectEquals("Copy it to this device again, or make a new copy of the disc.", r.next, "cut: what to do");
            // a folder table whose entries loop: the entry that is not the first of the root table points at itself
            byte[] looping = good.data.clone();
            int table = 33 * 2048, second = good.records.get("common.rpf");
            expect(second > table, "a record with a non-zero position in the root table");
            SetupTestKit.put16(looping, second, (second - table) / 4);
            DiscIdentifier.Result l = DiscIdentifier.identifyImage(new SetupTestKit.MemoryDisc(0, looping), spec);
            expectEquals(DiscIdentifier.Kind.DAMAGED, l.kind, "looping table: kind");
            expectEquals("This disc image is damaged (a folder table loops).", l.message, "looping table: what was found");
            expectEquals(r.next, l.next, "looping table: the same remedy");
        });

        check("identify image: a source that fails is reported, never thrown", () -> {
            DiscSource broken = new DiscSource() {
                @Override public long size() { return 1 << 20; }
                @Override public void readFully(long position, byte[] buffer, int offset, int length) throws IOException {
                    throw new IOException("card removed");
                }
                @Override public void close() { }
            };
            DiscIdentifier.Result r = DiscIdentifier.identifyImage(broken, spec);
            expectEquals(DiscIdentifier.Kind.UNREADABLE, r.kind, "kind: reading failed, the copy itself may be fine");
            expectEquals("The disc copy you chose could not be read (card removed).", r.message, "message");
            expectEquals("Check that the storage it is on is still connected, then choose it again.", r.next, "what to do");
            DiscSource wild = new DiscSource() {
                @Override public long size() { throw new IllegalStateException("provider died"); }
                @Override public void readFully(long position, byte[] buffer, int offset, int length) { }
                @Override public void close() { }
            };
            expectEquals(DiscIdentifier.Kind.UNREADABLE, DiscIdentifier.identifyImage(wild, spec).kind, "runtime failure");
            // a folder whose provider fails, and a folder whose game program ends while it is read
            DiscIdentifier.Result f = DiscIdentifier.identifyFolder(new DiscIdentifier.FolderSource() {
                @Override public DiscSource open(String relativePath) throws IOException { throw new IOException("the folder cannot be listed"); }
            }, spec);
            expectEquals(DiscIdentifier.Kind.UNREADABLE, f.kind, "folder that cannot be listed: kind");
            expectContains(f.message, "the folder cannot be listed", "folder that cannot be listed: the reason is shown");
            DiscIdentifier.Result shrinking = DiscIdentifier.identifyFolder(new DiscIdentifier.FolderSource() {
                @Override public DiscSource open(String relativePath) {
                    return new DiscSource() {
                        @Override public long size() { return EXECUTABLE.length; }
                        @Override public void readFully(long position, byte[] buffer, int offset, int length) throws IOException {
                            if (position > 0) throw new java.io.EOFException("gone");
                            System.arraycopy(EXECUTABLE, 0, buffer, offset, length);
                        }
                        @Override public void close() { }
                    };
                }
            }, spec);
            expectEquals(DiscIdentifier.Kind.DAMAGED, shrinking.kind, "a game program that ends early in a folder: a damaged copy");
            expectContains(shrinking.message, "The game program in this folder is incomplete (default.xex).", "what was found");
        });

        check("identify folder: supported, wrong version, no executable; the opened file is closed again", () -> {
            SetupTestKit.MemoryFolder folder = new SetupTestKit.MemoryFolder().put("DEFAULT.XEX", EXECUTABLE);
            expectEquals(DiscIdentifier.Kind.SUPPORTED_PLAY_DISC, DiscIdentifier.identifyFolder(folder, spec).kind, "supported");
            expect(folder.sources.size() == 1 && folder.sources.get(0).closed, "closed");
            SetupTestKit.MemoryFolder wrong = new SetupTestKit.MemoryFolder().put("default.xex", OTHER);
            DiscIdentifier.Result r = DiscIdentifier.identifyFolder(wrong, spec);
            expectEquals(DiscIdentifier.Kind.UNSUPPORTED_VERSION, r.kind, "wrong version");
            expectContains(r.message, SetupTestKit.sha256(OTHER).substring(0, 12), "hash prefix");
            DiscIdentifier.Result none = DiscIdentifier.identifyFolder(new SetupTestKit.MemoryFolder().put("common.rpf", OTHER), spec);
            expectEquals(DiscIdentifier.Kind.NOT_A_GAME_DISC, none.kind, "no executable");
            expectEquals("This folder does not hold the play disc's files: default.xex is not in it.", none.message, "no executable: what was found");
            expectEquals("Choose the folder that holds default.xex and the .rpf files directly, not a folder above it.", none.next,
                    "no executable: the likeliest mistake (the folder above) and its remedy");
        });

        check("host check: the local play-disc executable is recognised by the app's real catalog", () -> {
            final File folder = new File(System.getProperty("repo.root", "."), "local-analysis/Disc2-Play");
            if (!new File(folder, "default.xex").isFile()) {
                SetupTestKit.note("skipped: local-analysis/Disc2-Play/default.xex is not on this machine");
                return;
            }
            DiscIdentifier.Result r = DiscIdentifier.identifyFolder(new DiscIdentifier.FolderSource() {
                @Override
                public DiscSource open(String relativePath) throws IOException {
                    File f = new File(folder, relativePath);
                    return f.isFile() ? new FileDiscSource(f) : null;
                }
            });
            expectEquals(DiscIdentifier.Kind.SUPPORTED_PLAY_DISC, r.kind, "kind");
            expectContains(r.message, "Xbox 360", "the recognised release is named with its console (owners of other editions read it too)");
            SetupTestKit.note("local default.xex: " + r.kind + ", executable hash " + r.executableSha256.substring(0, 12) + "\u2026");
        });

        SetupTestKit.finish("DiscIdentifierTest");
    }
}
