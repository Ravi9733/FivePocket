package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.Rig.*;

import android.graphics.Bitmap;
import android.view.PixelCopy;
import checksim.Sim;
import java.io.File;
import java.io.FileOutputStream;
import java.util.ArrayList;
import java.util.List;

/**
 * The game window's input and capture as a whole, on the host (Rig): what the game receives, what the log says, what
 * the screen says and which files exist, for the cases the checker of the ship integration found wrong. Each case
 * names one behaviour a player or the lead's tools depend on. "S+@250" = the game is given Select pressed at 250 ms.
 * Run by run_tests.sh, step window-rig (the stand-ins of android.* stand first on the class path).
 *
 * What is tested here and not elsewhere: what only the window's own glue can get wrong (the two clocks it hands to
 * the combo, what it does when a controller goes away, what it shows, what it does with an exception, the numbers and
 * the left-overs, the timer of a held stick, Android's device number -1). The combo's rule itself (the window, two
 * controllers, late handling, the tap that keeps its length) is CaptureComboTest's, the motion rule
 * PhysicalMotionLogTest's, the store's rules CaptureStoreTest's; they are not replayed here.
 */
public final class GameWindowRigTest {
    private static final int PAD = 5, OTHER = 7, INJ = -1;
    private static final String RUN = "run-" + REQUEST;
    private static final String MOTION = "RecompNativeInput: PHYSICAL MOTION";

    private static Win window() {
        pad(PAD, "Pad One");
        return new Win(false, true);
    }

    /** One Select + A at t (A at t + 100), both released 100 ms later. */
    private static void combo(Win w, long t) {
        w.down(t, SELECT, PAD); w.down(t + 100, A, PAD); w.up(t + 200, A, PAD); w.up(t + 210, SELECT, PAD);
    }

    private static String page(PrivateGamepadInput input) {
        // as ControllerCheckActivity.render gives it to the page
        return ControllerCheckText.screen(input.description(), 0, 0, 0, 0, 0, 0, 0, 0, false, false, 0);
    }

    private static List<Long> motionLines() {
        List<Long> out = new ArrayList<>();
        for (String line : log(MOTION)) out.add(Long.parseLong(line.substring(0, line.indexOf(' '))));
        return out;
    }

    private static void write(File f, int bytes, long modified) throws Exception {
        f.getParentFile().mkdirs();
        try (FileOutputStream out = new FileOutputStream(f)) { out.write(new byte[bytes]); }
        f.setLastModified(modified);
    }

    private static boolean shownContains(String text) {
        for (String s : shown()) if (s.contains(text)) return true;
        return false;
    }

    public static void main(String[] args) {
        Rig.scratch = new File(args[0]);

        // ------------------------------------------------------------------------------------ no controller
        check("no controller (only Android's own Virtual device, as on every device): the game is told none, and the check page says none is found and names Android's Back", () -> {
            Win w = new Win(false, true);
            same(0L, last()[1], "the game is told no controller is connected");
            same("No controller connected", w.gamepadInput.description(), "the input's own description");
            String page = page(w.gamepadInput);
            expect(page.contains("Controller: none found\n"), "the page: " + page);
            expect(!page.contains("Virtual"), "Android's virtual keyboard is not named as the controller: " + page);
            expect(page.contains("use this device's Back button or Back gesture") && !page.contains("hold Select + Start"),
                    "the way out is one a player without a controller has: " + page);
        });

        check("the controller is taken away while the check page is shown: the page says none is found; a controller that comes is named", () -> {
            Win w = window();
            expect(page(w.gamepadInput).contains("Controller: Pad One\n") && page(w.gamepadInput).contains("hold Select + Start"), "with the controller: " + page(w.gamepadInput));
            w.removeDevice(100, PAD);
            Sim.runUntil(500);
            expect(page(w.gamepadInput).contains("Controller: none found\n"), "after it went: " + page(w.gamepadInput));
            same(0L, last()[1], "the game is told no controller is connected");
            w.addDevice(600, 6, "Pad Two");
            Sim.runUntil(1000);
            expect(page(w.gamepadInput).contains("Controller: Pad Two\n"), "after another came: " + page(w.gamepadInput));
        });

        // ------------------------------------------------------------------------------------ the combo's input
        check("an injected held Select (input gamepad keyevent --longpress 109: its release carries the press's time) reaches the game as long as without the capture hook", () -> {
            String[] edges = new String[2];
            for (int i = 0; i < 2; ++i) {
                fresh();
                pad(PAD, "Pad One");
                Win w = new Win(false, i == 0);
                w.keyAt(1000, 1000, SELECT, DOWN, 0, INJ); w.keyAt(1400, 1400, SELECT, DOWN, 1, INJ); w.keyAt(1401, 1000, SELECT, UP, 0, INJ);
                Sim.runUntil(4000);
                edges[i] = edges();
            }
            same("S+@1000 S-@1401", edges[1], "without the hook");
            same("S+@1250 S-@1651", edges[0], "with the hook: late by the window, held 401 ms");
        });

        check("the player holds the combo and an injected A tap arrives: the tap's release reaches the game, nothing stays held", () -> {
            Win w = window();
            w.down(0, SELECT, PAD); w.down(100, A, PAD);
            w.down(150, A, INJ); w.up(151, A, INJ);
            w.up(400, A, PAD); w.up(450, SELECT, PAD);
            Sim.runUntil(4000);
            same("A+@150 A-@151", edges(), "what the game was given");
            same(0, buttonsNow(), "nothing held at the end");
            same(1, pictures().size(), "the held combo was one capture");
        });

        check("a controller that goes away settles its own Select only: the other controller's held-back press still arrives", () -> {
            pad(PAD, "Pad One"); pad(OTHER, "Pad Two");
            Win w = new Win(false, true);
            w.down(0, SELECT, PAD);                 // the player's press, held back until 250
            w.removeDevice(100, OTHER);             // the other controller disconnects meanwhile
            w.up(600, SELECT, PAD);
            Sim.runUntil(4000);
            same("S+@250 S-@850", edges(), "what the game was given");
        });

        check("the main thread is late by 300 ms: Select at 0 and A at 100 (their own times) are still the combo", () -> {
            Win w = window();
            w.at(0, () -> Sim.busyUntil(android.os.Looper.getMainLooper(), 300));
            w.keyAt(1, 0, SELECT, DOWN, 0, PAD);
            w.keyAt(301, 100, A, DOWN, 0, PAD);    // Android hands the next key over only after the first was handled
            w.keyAt(302, 200, A, UP, 0, PAD); w.keyAt(303, 280, SELECT, UP, 0, PAD);
            Sim.runUntil(4000);
            same("", edges(), "the game gets neither press");
            same(1, pictures().size(), "a picture");
        });

        // ------------------------------------------------------------------------------------ what the screen says
        check("a combo inside the second after a capture is answered on the screen: the first reads Captured 1, the second says to wait", () -> {
            Win w = window();
            combo(w, 0);
            combo(w, 400);
            Sim.runUntil(3000);
            same("[100 Captured 1, 500 " + CaptureBadge.TOO_SOON + "]", shown(), "what was shown");
            same(1, pictures().size(), "one picture");
        });

        check("a combo while the last capture is still being written is answered on the screen, and so is every later one while that lasts", () -> {
            Win w = window();
            PixelCopy.neverAnswers = true;
            combo(w, 0);
            combo(w, 5000);
            combo(w, 10000);
            Sim.runUntil(15000);
            same("[5100 " + CaptureBadge.BUSY + ", 10100 " + CaptureBadge.BUSY + "]", shown(), "what was shown");
            same(2, count("CAPTURE_SKIPPED reason=busy"), "the log has both");
        });

        check("no picture of the game yet (no Surface), no memory for it, no folder for it: each says so on the screen, and the next capture works", () -> {
            Win w = window();
            w.surface.surface.valid = false;
            combo(w, 0);
            Sim.runUntil(1000);
            same("[100 " + CaptureBadge.NO_PICTURE_YET + "]", shown(), "no Surface");
            fresh();
            Win m = window();
            Bitmap.fail = "oom-create";
            combo(m, 0);
            m.at(1500, () -> Bitmap.fail = "");
            combo(m, 2000);
            Sim.runUntil(4000);
            same("[100 " + CaptureBadge.NO_MEMORY + ", 2100 Captured 2]", shown(), "no memory, then a capture");
            fresh();
            Win f = window();
            write(new File(files, "captures"), 1, 0);          // a file stands where the captures folder belongs
            combo(f, 0);
            Sim.runUntil(1000);
            same("[100 " + CaptureBadge.NO_STORAGE + "]", shown(), "no folder");
            same(1, count("CAPTURE_ABORTED at=100 error=no-folder"), "the log says why");
            same(0, Sim.crashes.size(), "no exception left a thread: " + Sim.crashes);
        });

        check("an exception while the picture is encoded does not leave the capture thread: aborted in the log, a line on the screen, no half file, and the next capture works", () -> {
            Win w = window();
            Bitmap.fail = "throw";
            combo(w, 0);
            w.at(1500, () -> Bitmap.fail = "");
            combo(w, 2000);
            Sim.runUntil(5000);
            same("[]", Sim.crashes, "exceptions that left a thread (on Android each ends the game's process)");
            same(1, count("CAPTURE_ABORTED at=100 error=RuntimeException"), "the log");
            same("[100 " + CaptureBadge.FAILED + ", 2100 Captured 2]", shown(), "the screen");
            same("{captures/=-1, captures/" + RUN + "/=-1, captures/" + RUN + "/cap-002.png=1000, private-trace/=-1}", tree(files), "the files");
            same(0, Bitmap.live, "the bitmaps were given back");
        });

        check("storage full: the screen never reads Captured for a picture that is not there, and a capture that left no file does not hand on its number", () -> {
            Win w = window();
            Bitmap.fail = "false";                 // the encoder cannot write
            combo(w, 0);
            w.at(1500, () -> Bitmap.fail = "");
            combo(w, 2000);
            Sim.runUntil(6000);
            same("[100 " + CaptureBadge.notSaved(1) + ", 2100 Captured 2]", shown(), "the screen");
            same(1, log(": CAPTURE n=1 ").size(), "one CAPTURE line with the number 1");
            expect(log(": CAPTURE n=1 ").get(0).contains("file=none") && log(": CAPTURE n=1 ").get(0).contains("state=encode-refused"), "the first: " + log(": CAPTURE n=1 "));
            expect(log(": CAPTURE n=2 ").size() == 1 && log(": CAPTURE n=2 ").get(0).contains("file=cap-002.png"), "the second: " + log(": CAPTURE n=2 "));
            same("[" + RUN + "/cap-002.png]", pictures(), "the picture has the number the log and the screen gave it");
        });

        check("a half-written picture an earlier game process left is removed when a window starts; pictures and other files stay", () -> {
            File root = new File(files, "captures");
            String old = "run-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
            write(new File(root, old + "/cap-001.png"), 10, 1000);
            write(new File(root, old + "/cap-002.png.part"), 7, 2000);
            write(new File(root, old + "/notes.txt"), 3, 3000);
            write(new File(root, "exported.txt"), 5, 4000);
            write(new File(root, "elsewhere/cap-003.png.part"), 4, 5000);    // not a run folder: not this app's to remove
            Win w = window();
            Sim.runUntil(1000);
            same("{captures/=-1, captures/elsewhere/=-1, captures/elsewhere/cap-003.png.part=4, captures/exported.txt=5, captures/" + old + "/=-1, captures/"
                    + old + "/cap-001.png=10, captures/" + old + "/notes.txt=3, private-trace/=-1}", tree(files), "the files after the window's start");
            same(1, count("CAPTURE_LEFT_OVERS removed=1"), "the log says it");
        });

        check("the 200-picture rule: the new picture is confirmed first, then the screen says that the oldest was removed", () -> {
            File root = new File(files, "captures");
            String old = "run-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
            long before = System.currentTimeMillis() - 86400000L;
            for (int i = 1; i <= 200; ++i) write(new File(root, old + "/" + CaptureStore.pictureName(i)), 10, before + i * 1000L);
            Win w = window();
            combo(w, 0);
            Sim.runUntil(5000);
            same("[100 Captured 1, 600 " + CaptureBadge.removedOldest(1) + "]", shown(), "what was shown, in this order");
            expect(!new File(root, old + "/cap-001.png").exists() && new File(root, old + "/cap-002.png").exists(), "exactly the oldest went");
        });

        // ------------------------------------------------------------------------------------ stick-only play
        check("a stick pushed to its end and held still for 31 s (Android sends no event while nothing changes): a line every 5 s", () -> {
            Win w = window();
            w.moveAt(100, PAD, AX, 1f);
            Sim.runUntil(31000);
            same("[100, 5100, 10100, 15100, 20100, 25100, 30100]", motionLines(), "the times of the lines");
        });

        check("the lines of a held stick end when it goes back to rest, when the window loses focus and when the controller goes away", () -> {
            Win w = window();
            w.moveAt(100, PAD, AX, 1f);
            w.moveAt(6000, PAD, AX, 0f);            // back to rest
            Sim.runUntil(20000);
            same("[100, 5100]", motionLines(), "back to rest at 6 s");
            fresh();
            Win f = window();
            f.moveAt(100, PAD, AX, 1f);
            f.at(6000, () -> f.focus(false));
            Sim.runUntil(20000);
            same("[100, 5100]", motionLines(), "focus lost at 6 s (the game was given a controller at rest)");
            fresh();
            Win r = window();
            r.moveAt(100, PAD, AX, 1f);
            r.removeDevice(6000, PAD);
            Sim.runUntil(20000);
            same("[100, 5100]", motionLines(), "the controller went away at 6 s");
        });

        check("a held stick whose events keep arriving (every 8 ms) still leaves one line every 5 s, not more", () -> {
            Win w = window();
            for (long t = 100; t <= 21100; t += 8) w.moveAt(t, PAD, AX, 1f, AY, (t % 16) / 1000f);
            w.moveAt(21108, PAD, AX, 0f);
            Sim.runUntil(30000);
            List<Long> at = motionLines();
            same(5, at.size(), "lines: " + at);
            for (int i = 1; i < at.size(); ++i) expect(at.get(i) - at.get(i - 1) >= 5000 && at.get(i) - at.get(i - 1) < 5008, "gap " + (at.get(i) - at.get(i - 1)));
        });

        System.out.println("GameWindowRigTest: " + passed + " passed, " + failed + " failed");
        if (failed > 0) System.exit(1);
    }
}
