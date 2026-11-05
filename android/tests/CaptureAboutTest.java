package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.ShipTestKit.check;
import static dev.recompbootstrap.smoke.ShipTestKit.expect;
import static dev.recompbootstrap.smoke.ShipTestKit.expectContains;
import static dev.recompbootstrap.smoke.ShipTestKit.expectEquals;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;

/**
 * The About tab's "Captures" section: what it counts, what "Copy to Pictures" does and says, and when A is offered.
 * The counting and the copy are the app's own (CaptureStore, CaptureExport) on a real folder of made-up pictures;
 * only the shared Pictures folder is a stand-in. Run with java -ea.
 */
public final class CaptureAboutTest {
    /**
     * The shared folder: keeps what was published; can refuse from a given picture on. listing: what it answers when
     * asked which pictures it holds ("true": what it holds, as Android's media store does; "none": it cannot say;
     * "empty": it says it holds nothing, whatever it was given, like a media store that does not show an app its
     * own pictures).
     */
    private static final class Pictures implements CaptureExport.Sink {
        final List<String> published = new ArrayList<String>();
        int refuseFrom = Integer.MAX_VALUE, opened, asked;
        /** How Android refuses from refuseFrom on: an IOException while writing (storage), or one of its own refusals. */
        RuntimeException refusal;
        String listing = "true";

        @Override public java.util.Set<String> names() {
            ++asked;
            if (listing.equals("none")) return null;
            return listing.equals("empty") ? new java.util.HashSet<String>() : new java.util.HashSet<String>(published);
        }

        @Override public CaptureExport.Target open(final String displayName, String mime) throws IOException {
            ++opened;
            if (published.size() >= refuseFrom && refusal != null) throw refusal;
            if (published.size() >= refuseFrom) throw new IOException("No space left on device");
            final ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            return new CaptureExport.Target() {
                @Override public OutputStream stream() {
                    return bytes;
                }

                @Override public void publish() {
                    published.add(displayName);
                }

                @Override public void abandon() {}
            };
        }
    }

    private static final String RUN = "run-0123456789abcdef0123456789abcdef";

    private static File captures(String label, int pictures) throws IOException {
        File root = new File(ShipTestKit.newFilesDir(label), CaptureStore.FOLDER);
        for (int n = 1; n <= pictures; ++n) {
            ShipTestKit.write(new File(new File(root, RUN), CaptureStore.pictureName(n)), ShipTestKit.bytes(700000 + n, n));
        }
        return root;
    }

    /** The About tab with the highlight on the Captures section, as LauncherScreen and AboutScreen draw it. */
    private static MenuScenes.Page section(long[] summary, boolean copying, CaptureExport.Result last) {
        ShellNav nav = new ShellNav();
        nav.setColumnCount(ShellNav.ABOUT, ShellPages.ABOUT_SECTIONS.length);
        nav.place(ShellNav.ABOUT, ShellPages.ABOUT_CAPTURES, false);
        MenuScenes.Page page = new MenuScenes.Page();
        List<String> lines = new ArrayList<String>();
        for (String line : CaptureAbout.text(summary, copying, last).split("\n")) lines.add(line);
        ShellPages.about(page, nav, lines, 0, CaptureAbout.canCopy(summary, copying));
        return page;
    }

    private static List<String> prompts(MenuScenes.Page page) {
        List<String> out = new ArrayList<String>();
        for (MenuScenes.Prompt prompt : page.prompts) out.add(prompt.glyph + " " + prompt.label);
        return out;
    }

    public static void main(String[] args) {
        check("the section counts the pictures, says how to take one, and offers Copy To Pictures while some are not copied", () -> {
            File root = captures("about-count", 3);
            long[] summary = CaptureExport.summary(root);
            String text = CaptureAbout.text(summary, false, null);
            expectContains(text, "Captures: 3 pictures, 2 MB. None of them is in Pictures yet.", "count, size and what is not copied");
            expectContains(text, "In the game, hold Select and press A to take a picture of the game.", "how a capture is taken");
            // the store's limits are said here, not only in a line that flashes in the game when old pictures go
            expectContains(text, "The app keeps the newest 200 pictures, up to 800 MB in all. Older ones are removed, also ones that were never copied to Pictures",
                    "how many are kept");
            expectContains(CaptureAbout.text(new long[] {0, 0, 0}, false, null), "The app keeps the newest 200 pictures", "also before the first picture");
            expectContains(text, "Copy to Pictures: press A here. The pictures go to Pictures/FivePocket Captures", "what A does");
            MenuScenes.Page page = section(summary, false, null);
            expect(prompts(page).contains("A Copy To Pictures"), "A is offered as Copy To Pictures: " + prompts(page));
            expectEquals("Captures", page.column[page.columnSelected], "on the section named Captures");
        });

        check("Copy to Pictures copies each picture once, to the shared folder's name, and says what it did", () -> {
            File root = captures("about-copy", 3);
            Pictures pictures = new Pictures();
            CaptureExport.Result first = CaptureExport.copyAll(root, pictures);
            expectEquals("3 copied to Pictures/FivePocket Captures.", CaptureAbout.result(first), "the sentence");
            expectEquals("[gta5-" + RUN + "-cap-001.png, gta5-" + RUN + "-cap-002.png, gta5-" + RUN + "-cap-003.png]", pictures.published.toString(),
                    "what reached the shared folder");
            long[] after = CaptureExport.summary(root);
            expectContains(CaptureAbout.text(after, false, first), "Captures: 3 pictures, 2 MB. All of them have been copied to Pictures.", "the count afterwards");
            // nothing is left to copy: A is not offered, and pressing it would do nothing
            expect(!CaptureAbout.canCopy(after, false), "nothing to copy");
            expect(prompts(section(after, false, first)).toString().equals("[B Back]"), "only B is offered: " + prompts(section(after, false, first)));
            // one more capture: only that one is copied
            ShipTestKit.write(new File(new File(root, RUN), CaptureStore.pictureName(4)), ShipTestKit.bytes(500000, 44));
            CaptureExport.Result second = CaptureExport.copyAll(root, pictures);
            expectEquals("1 copied to Pictures/FivePocket Captures; 3 were already there.", CaptureAbout.result(second), "the second copy");
            expectEquals(4, pictures.published.size(), "four pictures in the shared folder, none twice");
        });

        check("a copy that the shared storage stops says so in plain words and keeps what is left for the next press", () -> {
            File root = captures("about-full", 3);
            Pictures pictures = new Pictures();
            pictures.refuseFrom = 1;
            CaptureExport.Result stopped = CaptureExport.copyAll(root, pictures);
            String said = CaptureAbout.result(stopped);
            expectEquals("1 copied to Pictures/FivePocket Captures. Then the copy stopped: a picture could not be written. Check that this device has free"
                    + " storage, then press A again.", said, "the sentence");
            expect(ShipWordsTest.forbiddenIn(said) == null && !said.contains("cap-00"), "it names no file of the app's own storage: " + said);
            long[] after = CaptureExport.summary(root);
            expectContains(CaptureAbout.count(after), "2 of them are not in Pictures yet.", "what is left");
            expect(CaptureAbout.canCopy(after, false), "A is offered again");
            expectEquals(3, CaptureStore.list(root).size(), "the pictures in the app's own storage are all still there");
        });

        check("a copy that Android refuses (no leave to add pictures, no shared storage) is not blamed on free storage", () -> {
            for (RuntimeException refusal : new RuntimeException[] {new SecurityException("Permission Denial: writing MediaProvider"),
                    new IllegalArgumentException("Unknown URL content://media/external_primary/images/media"), new IllegalStateException("Failed to build unique file")}) {
                File root = captures("about-refused", 2);
                Pictures pictures = new Pictures();
                pictures.refuseFrom = 0;
                pictures.refusal = refusal;
                CaptureExport.Result stopped = CaptureExport.copyAll(root, pictures);
                String said = CaptureAbout.result(stopped);
                expectEquals("0 copied to Pictures/FivePocket Captures. Then the copy stopped: this device did not accept a picture in its Pictures folder."
                        + " Press A to try again; if it stops again, restart the device.", said, "the sentence for " + refusal.getClass().getSimpleName());
                expect(ShipWordsTest.forbiddenIn(said) == null && !said.contains("cap-00"), "it names no file of the app's own storage: " + said);
                expect(CaptureAbout.canCopy(CaptureExport.summary(root), false), "A is offered again");
            }
            // a shared folder that answers with no row at all is a refusal too
            File root = captures("about-no-row", 1);
            CaptureExport.Result none = CaptureExport.copyAll(root, new CaptureExport.Sink() {
                @Override public CaptureExport.Target open(String displayName, String mime) {
                    return null;
                }
            });
            expectContains(CaptureAbout.result(none), "this device did not accept a picture", "no row");
        });

        check("a picture the player removed from the Gallery is not counted as being in Pictures, and the next A copies it again", () -> {
            File root = captures("about-removed", 3);
            Pictures pictures = new Pictures();
            CaptureExport.copyAll(root, pictures);
            expectContains(CaptureAbout.count(CaptureExport.summary(root, CaptureExport.shared(root, pictures))), "All of them have been copied to Pictures.", "after the copy");
            pictures.published.remove("gta5-" + RUN + "-cap-002.png");   // the player deletes one copy in the Gallery
            long[] summary = CaptureExport.summary(root, CaptureExport.shared(root, pictures));
            expectContains(CaptureAbout.count(summary), "Captures: 3 pictures, 2 MB. 1 of them is not in Pictures yet.", "the count asks the shared folder");
            expect(CaptureAbout.canCopy(summary, false), "A is offered");
            CaptureExport.Result again = CaptureExport.copyAll(root, pictures);
            expectEquals("1 copied to Pictures/FivePocket Captures; 2 were already there.", CaptureAbout.result(again), "only the removed one is copied again");
            expectEquals("[gta5-" + RUN + "-cap-001.png, gta5-" + RUN + "-cap-003.png, gta5-" + RUN + "-cap-002.png]", pictures.published.toString(), "the shared folder");
            expectEquals(3, CaptureExport.ledger(root).size(), "the app's own list names each picture once");
            expect(!CaptureAbout.canCopy(CaptureExport.summary(root, CaptureExport.shared(root, pictures)), false), "nothing left to copy");
        });

        check("a shared folder that cannot say what it holds, or does not show what it was just given, never makes the app copy everything again", () -> {
            // it cannot say (null): the app's own list is all that is known
            File root = captures("about-no-listing", 2);
            Pictures silent = new Pictures();
            silent.listing = "none";
            CaptureExport.copyAll(root, silent);
            expect(!CaptureAbout.canCopy(CaptureExport.summary(root, CaptureExport.shared(root, silent)), false), "nothing is offered again");
            expectEquals("0 copied to Pictures/FivePocket Captures; 2 were already there.", CaptureAbout.result(CaptureExport.copyAll(root, silent)), "a second copy copies nothing");
            // it answers "nothing" although it took the pictures: its answer is not used again on this device
            File other = captures("about-blind-listing", 2);
            Pictures blind = new Pictures();
            blind.listing = "empty";
            CaptureExport.Result first = CaptureExport.copyAll(other, blind);
            expectEquals("2 copied to Pictures/FivePocket Captures.", CaptureAbout.result(first), "the first copy");
            long[] after = CaptureExport.summary(other, CaptureExport.shared(other, blind));
            expectContains(CaptureAbout.count(after), "All of them have been copied to Pictures.", "the count afterwards goes by the app's own list");
            int opened = blind.opened;
            CaptureExport.Result second = CaptureExport.copyAll(other, blind);
            expectEquals("0/2", second.copied + "/" + second.already, "a second A copies nothing");
            expectEquals(opened, blind.opened, "and the shared folder is given no picture twice");
            // the pictures themselves and the list are untouched by that note
            expectEquals(2, CaptureStore.list(other).size(), "the pictures");
        });

        check("no pictures, counting, copying, and an Android version without the shared folder", () -> {
            File root = captures("about-none", 0);
            long[] none = CaptureExport.summary(root);
            expectContains(CaptureAbout.text(none, false, null), "Captures: none yet.", "no pictures");
            expect(!CaptureAbout.canCopy(none, false) && !prompts(section(none, false, null)).contains("A Copy To Pictures"), "nothing to copy: A is not offered");
            expectEquals("There is nothing to copy yet.", CaptureAbout.result(CaptureExport.copyAll(root, new Pictures())), "a copy of nothing");
            expectContains(CaptureAbout.text(null, false, null), "Captures: counting", "while the pictures are counted");
            expect(!CaptureAbout.canCopy(null, false), "not before they are counted");
            long[] some = {5, 9000000, 2};
            expectContains(CaptureAbout.text(some, true, null), "Copying to Pictures/FivePocket Captures", "while a copy runs");
            expect(!CaptureAbout.canCopy(some, true) && !prompts(section(some, true, null)).contains("A Copy To Pictures"), "one copy at a time");
            expectEquals("0 copied to Pictures/FivePocket Captures. The copy needs Android 10 or later.",
                    CaptureAbout.result(new CaptureExport.Result(0, 0, 5, "needs Android 10 or later")), "below Android 10");
            expectContains(CaptureAbout.count(new long[] {1, 2400000, 1}), "Captures: 1 picture, 2 MB. None of them is in Pictures yet.", "one picture");
        });

        ShipTestKit.finish("CaptureAboutTest");
    }
}
