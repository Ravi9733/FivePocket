package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SetupTestKit.check;
import static dev.recompbootstrap.smoke.SetupTestKit.expect;
import static dev.recompbootstrap.smoke.SetupTestKit.expectEquals;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/** Run with java -ea. SetupDiscStore: the remembered choice per disc in files/settings. */
public final class SetupDiscStoreTest {
    public static void main(String[] args) throws Exception {
        SetupTestKit.requireAssertions();

        check("save and load per disc, in files/settings, three lines; the two discs are kept apart; forget removes", () -> {
            File files = SetupTestKit.newFilesDir("store");
            SetupDiscStore store = new SetupDiscStore(files);
            expect(store.load(0) == null && store.load(1) == null, "nothing at first");
            store.save(0, new SetupDiscStore.Choice("content://a/document/play", "Disc2-Play.iso", 8738846720L));
            store.save(1, new SetupDiscStore.Choice("content://a/document/one", null, -1));
            expectEquals("content://a/document/play\nDisc2-Play.iso\n8738846720\n",
                    new String(SetupTestKit.read(new File(files, "settings/setup-disc-play.txt")), StandardCharsets.UTF_8), "the file");
            SetupDiscStore.Choice play = new SetupDiscStore(files).load(0), install = store.load(1);
            expectEquals("content://a/document/play", play.uri, "uri");
            expectEquals("Disc2-Play.iso", play.name, "name");
            expectEquals(8738846720L, play.bytes, "bytes");
            expectEquals("content://a/document/one", install.uri, "the other disc");
            expect(install.name == null && install.bytes == -1, "no name, size unknown");
            expect(store.forget(0) && !store.forget(0), "forget once");
            expect(store.load(0) == null && store.load(1) != null, "only that disc");
            expect(!new File(files, "settings/setup-disc-play.txt.tmp").exists(), "no temporary file left");
        });

        check("what is not a remembered choice counts as nothing: damaged, empty, another address, a folder, a link, too large", () -> {
            File files = SetupTestKit.newFilesDir("store-bad");
            SetupDiscStore store = new SetupDiscStore(files);
            File f = store.file(0);
            String[] bad = {"", "content://x\n", "file:///sdcard/x.iso\nx\n1\n", "content://x\nname\nmany\n"};
            for (String text : bad) {
                SetupTestKit.write(f, text.getBytes(StandardCharsets.UTF_8));
                expect(store.load(0) == null, "refused: " + text.replace("\n", "|"));
            }
            SetupTestKit.write(f, new byte[SetupDiscStore.MAX_BYTES + 1]);
            expect(store.load(0) == null, "too large");
            Files.delete(f.toPath());
            expect(f.mkdir(), "a folder in its place");
            expect(store.load(0) == null, "a folder");
            expect(!store.forget(0), "a folder is not removed");
            Files.delete(f.toPath());
            File elsewhere = new File(files, "elsewhere.txt");
            SetupTestKit.write(elsewhere, "content://x\nname\n1\n".getBytes(StandardCharsets.UTF_8));
            Files.createSymbolicLink(f.toPath(), elsewhere.toPath());
            expect(store.load(0) == null, "a link is not followed");
        });

        check("save refuses an address that is not a document's or has a line break; the name's line break becomes a space", () -> {
            File files = SetupTestKit.newFilesDir("store-save");
            SetupDiscStore store = new SetupDiscStore(files);
            for (String uri : new String[] {null, "file:///x", "content://x\nevil"}) {
                try {
                    store.save(0, new SetupDiscStore.Choice(uri, "n", 1));
                    throw new AssertionError("saved " + uri);
                } catch (IOException expected) {
                    // refused
                }
            }
            store.save(0, new SetupDiscStore.Choice("content://x", "two\nlines", 5));
            expectEquals("two lines", store.load(0).name, "one line");
        });

        SetupTestKit.finish("SetupDiscStoreTest");
    }
}
