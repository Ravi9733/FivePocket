package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SetupTestKit.check;
import static dev.recompbootstrap.smoke.SetupTestKit.expect;
import static dev.recompbootstrap.smoke.SetupTestKit.expectEquals;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/**
 * setup-fixes: the small pure pieces the fixes added, one check each (they do not exist before the fix, so this class
 * does not compile against those sources; the behaviour they give is tested by the Setup*Test classes, which do).
 * Run with java -ea.
 */
public final class SetupFixesUnitTest {
    static byte[] zip(String... names) throws Exception {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        try (ZipOutputStream z = new ZipOutputStream(out)) {
            for (String name : names) {
                z.putNextEntry(new ZipEntry(name));
                z.write(SetupTestKit.bytes(100, 1));
                z.closeEntry();
            }
        }
        return out.toByteArray();
    }

    public static void main(String[] args) throws Exception {
        SetupTestKit.requireAssertions();

        check("C1-1/M1: the copy marker: a running copy of another process is interrupted, of this process not; B leaves"
                + " \"stopped\", leaving the app \"left\", an end by itself nothing; one per disc; a folder is no marker", () -> {
            File files = SetupTestKit.newFilesDir("marker");
            SetupDiscStore here = new SetupDiscStore(files, "p1"), later = new SetupDiscStore(files, "p2");
            int play = SetupDiscModel.PLAY, install = SetupDiscModel.INSTALL;
            expectEquals(SetupDiscStore.LAST_NONE, here.lastCopy(play), "none at first");
            SetupDiscStore.Marker m = here.marker(play);
            expect(m.begin(), "written");
            expectEquals(SetupDiscStore.LAST_NONE, here.lastCopy(play), "this process's running copy is not called interrupted");
            expectEquals(SetupDiscStore.LAST_INTERRUPTED, later.lastCopy(play), "another process finds it cut off");
            expectEquals(SetupDiscStore.LAST_NONE, later.lastCopy(install), "the install disc's is apart");
            m.end(true);
            expectEquals(SetupDiscStore.LAST_STOPPED, later.lastCopy(play), "a stop with B: stopped, in every process");
            expectEquals(SetupDiscStore.LAST_STOPPED, here.lastCopy(play), "also in this one");
            SetupDiscStore.Marker left = here.marker(play);
            left.begin();
            left.appLeft();
            left.end(true);
            expectEquals(SetupDiscStore.LAST_LEFT, later.lastCopy(play), "a stop by leaving the app: left");
            SetupDiscStore.Marker done = here.marker(play);
            done.begin();
            done.appLeft();
            done.end(false);
            expectEquals(SetupDiscStore.LAST_NONE, later.lastCopy(play), "a copy that ended by itself leaves nothing");
            expectEquals(null, SetupDiscStore.Marker.after(false, true), "rule: ended by itself");
            expectEquals(SetupDiscStore.STOPPED, SetupDiscStore.Marker.after(true, false), "rule: B");
            expectEquals(SetupDiscStore.LEFT, SetupDiscStore.Marker.after(true, true), "rule: left");
            expect(here.copyingFile(play).mkdirs(), "a folder under the marker's name");
            expectEquals(SetupDiscStore.LAST_NONE, later.lastCopy(play), "is not a marker");
            expect(!here.marker(play).begin(), "and cannot be written over: begin() says so instead of throwing");
        });

        check("C3-1/M2: the window's focus: the first focus is not passed on, every later gain is, a loss never", () -> {
            FocusRelay relay = new FocusRelay();
            expect(!relay.changed(true), "the first focus: the page has just looked");
            expect(!relay.changed(false), "a loss");
            expect(relay.changed(true), "the focus back: the page looks again");
            expect(!relay.changed(false), "a loss");
            expect(relay.changed(true), "and again");
        });

        check("C3-1/M2, check-2 minor 3: two overlapping looks: the second is refused; only the look that took the gate releases it;"
                + " a look asked for meanwhile follows; none while an image is read", () -> {
            SetupDiscModel m = new SetupDiscModel(), other = new SetupDiscModel();
            SetupDiscModel.Look first = m.lookStarts();
            expect(first != null, "the first look starts");
            expect(m.lookStarts() == null, "a second one while it runs does not");
            SetupDiscModel.Look foreign = other.lookStarts();
            expect(!m.lookDone(foreign) && m.lookStarts() == null, "another look's end does not open the gate");
            expect(!m.lookDone(null) && m.lookStarts() == null, "nor does no look");
            m.lookLater();   // forgetLater while the first look runs
            expect(m.lookDone(first), "the first look ends: the one asked for meanwhile is due");
            SetupDiscModel.Look second = m.lookStarts();
            expect(second != null, "and starts");
            expect(!m.lookDone(second), "nothing more is due after it");
            expect(!m.lookDone(first), "an old token ends nothing");
            m.readingStarted(SetupDiscModel.PLAY, false);
            expect(m.lookStarts() == null, "not while an image is read");
        });

        check("check-2: a stop whose marker cannot be written (full storage) is not told later as an interruption", () -> {
            File files = SetupTestKit.newFilesDir("marker-full");
            SetupDiscStore here = new SetupDiscStore(files, "p1"), later = new SetupDiscStore(files, "p2");
            SetupDiscStore.Marker m = here.marker(SetupDiscModel.PLAY);
            expect(m.begin(), "running written");
            // the next write fails as on a full storage (its temporary file cannot be made); removing still works
            expect(AtomicFiles.tempFor(here.copyingFile(SetupDiscModel.PLAY)).mkdirs(), "the write is made to fail");
            m.end(true);
            expectEquals(SetupDiscStore.LAST_NONE, later.lastCopy(SetupDiscModel.PLAY), "no marker left: no false \"interrupted\"");
            expect(!here.copyingFile(SetupDiscModel.PLAY).exists(), "the marker was removed");
        });

        check("check-2 minors 1/2: Home while a copy runs tells the marker (\"left\"); Home after B (already stopping) does not", () -> {
            File files = SetupTestKit.newFilesDir("leave-rule");
            SetupDiscStore store = new SetupDiscStore(files, "p1"), later = new SetupDiscStore(files, "p2");
            SetupDiscModel m = new SetupDiscModel();
            SetupDiscStore.Marker running = store.marker(SetupDiscModel.PLAY);
            running.begin();
            m.copyStarted(SetupDiscModel.PLAY, 0);
            expect(m.appLeft(running), "Home: the copy is to stop");
            running.end(true);
            expectEquals(SetupDiscStore.LAST_LEFT, later.lastCopy(SetupDiscModel.PLAY), "left");
            SetupDiscModel b = new SetupDiscModel();
            SetupDiscStore.Marker second = store.marker(SetupDiscModel.PLAY);
            second.begin();
            b.copyStarted(SetupDiscModel.PLAY, 0);
            b.nav(NavInput.BACK, false);
            b.nav(NavInput.CONFIRM, false);   // B, Stop
            expectEquals(SetupDiscModel.ACT_STOP, b.takeAction(), "B asked the stop");
            expect(!b.appLeft(second), "Home after B: nothing more to stop");
            second.end(true);
            expectEquals(SetupDiscStore.LAST_STOPPED, later.lastCopy(SetupDiscModel.PLAY), "still \"stopped\"");
            expect(!new SetupDiscModel().appLeft(second), "no copy: false");
        });

        check("C4-2: an app is told apart from another ZIP archive by its manifest at the top level", () -> {
            expect(Disc2ImageProbe.androidApp(disc(zip("classes.dex", "AndroidManifest.xml")), zip("classes.dex", "AndroidManifest.xml").length),
                    "an app");
            byte[] other = zip("res/AndroidManifest.xml", "Disc2-Play.iso");
            expect(!Disc2ImageProbe.androidApp(disc(other), other.length), "a manifest in a folder is not an app's");
            byte[] broken = zip("AndroidManifest.xml");
            broken = Arrays.copyOf(broken, broken.length - 10);   // the end record cut off
            expect(!Disc2ImageProbe.androidApp(disc(broken), broken.length), "no end record: not called an app");
        });

        check("C4-4: the read permission of a chosen file is given back exactly when it was taken and the file is refused", () -> {
            expect(SetupDiscModel.releaseGrant(true, -1), "taken, refused: given back");
            expect(!SetupDiscModel.releaseGrant(true, SetupDiscModel.PLAY), "taken, the play disc: kept (it is remembered)");
            expect(!SetupDiscModel.releaseGrant(true, SetupDiscModel.INSTALL), "taken, the install disc: kept");
            expect(!SetupDiscModel.releaseGrant(false, -1), "not taken: nothing to give back");
        });

        check("C5-1: two damaged files are named together; a file whose size no missing entry has is not called damaged", () -> {
            List<SetupDiscCopy.FileResult> two = new ArrayList<SetupDiscCopy.FileResult>();
            String sha = SetupTestKit.sha256(new byte[0]);
            two.add(new SetupDiscCopy.FileResult(2, 10, "a", 5, sha, Disc2Discs.Verdict.NOT_PINNED, false));
            two.add(new SetupDiscCopy.FileResult(4, 10, "c", 7, sha, Disc2Discs.Verdict.WANTED, true));   // kept: read before b, same size
            two.add(new SetupDiscCopy.FileResult(9, 10, "b", 7, sha, Disc2Discs.Verdict.NOT_PINNED, false));
            List<Disc2Discs.Pin> missing = new ArrayList<Disc2Discs.Pin>();
            missing.add(new Disc2Discs.Pin(SetupTestKit.sha256(new byte[] {1}), 7));
            missing.add(new Disc2Discs.Pin(SetupTestKit.sha256(new byte[] {2}), 5));
            List<SetupDiscCopy.FileResult> damaged = SetupDiscCopy.failedCheck(two, missing);
            expectEquals(2, damaged.size(), "both are explained by a missing entry of their size; a kept file is not damaged");
            expectEquals("Files 3 and 10 of 10 are damaged in your disc image.", SetupDiscCopy.shownDamaged(damaged, 10), "the sentence");
            expectEquals("File 3 of 10 is damaged in your disc image.", SetupDiscCopy.shownDamaged(damaged.subList(0, 1), 10), "one");
            missing.remove(1);
            expectEquals(1, SetupDiscCopy.failedCheck(two, missing).size(), "only the file of a missing entry's size");
        });

        check("minor: a size for the player: GB with one decimal, or smaller than 0.1 GB", () -> {
            expectEquals("it is only 2.1 GB", Disc2Discs.sizeClause(2147483648L), "the 2 GB file of the vm-robust run");
            expectEquals("it is only 0.1 GB", Disc2Discs.sizeClause(100000000L), "0.1 GB");
            expectEquals("it is smaller than 0.1 GB", Disc2Discs.sizeClause(99999999L), "below");
        });

        check("minor: an image name is kept whole up to 60 characters and shortened in the middle above", () -> {
            StringBuilder b = new StringBuilder();
            while (b.length() < 56) b.append((char) ('a' + b.length() % 26));
            String sixty = b.append(".iso").toString();
            expectEquals(60, sixty.length(), "a name of 60");
            expectEquals(sixty, SetupDiscModel.shortName(sixty), "stays whole");
            String longer = "Z" + sixty;
            String s = SetupDiscModel.shortName(longer);
            expect(s.length() <= SetupDiscModel.NAME_MAX, "at most 60 characters: " + s.length());
            expect(s.startsWith(longer.substring(0, 29)) && s.endsWith(longer.substring(longer.length() - 29)) && s.contains("\u2026"),
                    "its start, its end and the mark between: " + s);
        });

        check("C7-1: the padding of a text view is its own plus what the bars and the cutout cover on the sides it touches", () -> {
            int[] base = {24, 24, 24, 12};
            int[] bars = {0, 63, 0, 126};
            expectEquals("[24, 87, 24, 12]", Arrays.toString(WindowFit.padded(base, bars, WindowFit.LEFT | WindowFit.TOP | WindowFit.RIGHT)),
                    "the title: below the status bar");
            expectEquals("[24, 24, 24, 138]", Arrays.toString(WindowFit.padded(base, bars, WindowFit.LEFT | WindowFit.BOTTOM | WindowFit.RIGHT)),
                    "the status lines: above the navigation bar");
            expectEquals("[104, 87, 24, 138]", Arrays.toString(WindowFit.padded(base, new int[] {80, 63, 0, 126}, WindowFit.ALL)),
                    "a cutout on the left, all sides");
            expectEquals("[24, 24, 24, 12]", Arrays.toString(WindowFit.padded(base, new int[] {-5, -5, -5, -5}, WindowFit.ALL)),
                    "a negative inset counts as none");
            expectEquals("[24, 24, 24, 12]", Arrays.toString(base), "the base is not changed");
            expect(WindowFit.CHECK_TEXT_MIN_SP < WindowFit.CHECK_TEXT_SP / 2, "Controller Check's text may become small enough for the largest"
                    + " font (200 %) to show what 100 % shows");
        });

        SetupTestKit.finish("SetupFixesUnitTest");
    }

    static DiscSource disc(byte[] data) {
        return new SetupTestKit.MemoryDisc(0, data);
    }
}
