package dev.recompbootstrap.smoke;

import java.io.File;
import java.io.IOException;

/**
 * A packaging proof, not a unit test: runs the app's own first start (ShipStart, as ShipStartActivity runs it) on
 * this computer against a BUILT APK file. What it shows: the APK carries the record and the pieces under the names
 * the code reads, the record written by ship_stage.py is one ShipConfig accepts, the pieces inflate to the recorded
 * size and SHA-256, the log lines of a first and of a later start (which expected_log.py must predict), and that
 * Play refuses while the files from the discs are missing.
 *
 *   java -cp ... dev.recompbootstrap.smoke.ShipApkHostCheck APK FILES_DIR
 *
 * FILES_DIR must be the files directory the APK's game code was staged for (ship_stage.py --files-root): for a
 * real APK that is a path on the device, so this check is for stand-in APKs staged for a folder on this computer.
 * Exit 0 only when every step succeeded. What it cannot show is in VM-TEST.md: the loader itself runs on a device.
 */
public final class ShipApkHostCheck {
    public static void main(String[] args) throws Exception {
        if (args.length != 2) {
            System.out.println("usage: ShipApkHostCheck APK FILES_DIR");
            System.exit(2);
        }
        String apk = args[0];
        File files = new File(args[1]);
        if (!files.isDirectory() && !files.mkdirs()) throw new IOException("cannot create " + files);
        ShipBundle.Apk bundle = new ShipBundle.Apk(apk);
        boolean ok = true;
        try {
            System.out.println("check before: " + ShipFirstRun.check(bundle, files));
            ShipStart.Watch watch = new ShipStart.Watch() {
                private ShipFirstRun.Step last;

                @Override public void working() {
                    System.out.println("  the screen is told that the work has started");
                }

                @Override public void at(ShipFirstRun.Step step, long done, long total) {
                    if (step != last) System.out.println("  step " + step + (total > 0 ? " (" + total + " bytes)" : ""));
                    last = step;
                }
            };
            // The app's own first start, as ShipStartActivity runs it: what it writes to the log is Outcome.log.
            ShipStart.Outcome first = ShipStart.run(bundle, files, false, false, watch, ShipFiles.DISK);
            System.out.println("first start: " + (first.ok() ? "ok" : "FAILED: " + first.error));
            for (String line : first.log) System.out.println("LOG " + line);   // what the device logs under RecompShip
            ok &= first.ok() && first.bundled;
            if (first.ok() && first.config != null) {
                ShipConfig config = first.config;
                File code = new File(files, "private-trace/private-trace-live.so");
                System.out.println("record  " + config.sha256 + "  build " + config.build);
                System.out.println("code    " + (code.isFile() ? ShipFiles.sha256Of(code) : "absent") + "  recorded " + config.library.sha256
                        + "  mode " + ShipFiles.mode(code) + "  names " + ShipFiles.linkCount(code));
                File pack = new File(files, "private-trace/" + config.shaderPackName);
                System.out.println("shaders " + (pack.isFile() ? ShipFiles.sha256Of(pack) : "absent") + "  recorded " + config.shaderPack.sha256);
                System.out.println("loader's file rule (Java copy): " + (code.isFile() ? String.valueOf(ShipFirstRun.loaderProblem(code)) : "no file")
                        + "  (null = accepted)");
                System.out.println("check after: " + ShipFirstRun.check(bundle, files));
                ok &= ShipFirstRun.check(bundle, files) == ShipFirstRun.State.CURRENT;
                // A later start of the app, nothing copied from discs in between.
                ShipStart.Outcome later = ShipStart.run(bundle, files, false, false, watch, ShipFiles.DISK);
                for (String line : later.log) System.out.println("LOG2 " + line);
                ok &= later.ok();
                ShipReady before = ShipReady.of(apk, files);
                System.out.println("ready rule: " + before.stands + "; pinned " + config.pinnedData.size() + ", bundled "
                        + config.bundledData.size() + ", not bundled " + config.notBundled.size() + ", " + config.notBundledBytes() + " bytes");
                for (ShipConfig.BundledData piece : config.bundledData) {
                    File placed = ShipData.place(config, files, piece.path);
                    boolean same = placed.isFile() && piece.sha256.equals(ShipFiles.sha256Of(placed)) && "r--------".equals(ShipFiles.mode(placed));
                    if (!same) System.out.println("  NOT IN PLACE: the bundled piece " + piece.path);
                    ok &= same;
                }
                // with nothing from the player's discs yet, exactly the files that are not bundled are missing
                ok &= config.notBundled.isEmpty() ? before.stands == ShipReady.Stands.NOTHING
                        : before.stands == ShipReady.Stands.DATA && before.data.absent.size() == config.notBundled.size();
                ShipPlay.Result early = ShipPlay.prepare(apk, files, false);
                System.out.println("Play before the disc files are there: " + (early.error == null ? "PREPARED (it must not be)" : "refused: " + early.error));
                ok &= config.notBundled.isEmpty() || early.error != null;
            }
        } finally {
            bundle.close();
        }
        System.out.println(ok ? "SHIP APK HOST CHECK PASSED" : "SHIP APK HOST CHECK FAILED");
        System.exit(ok ? 0 : 1);
    }
}
