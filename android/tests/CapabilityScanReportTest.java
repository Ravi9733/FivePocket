package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SettingsCoreTestKit.check;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.same;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.skip;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.tempDir;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.test;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Set;
import java.util.TreeSet;

/**
 * The capability scan on the REAL game libraries of this machine: the library the phone ran until 2026-10-05
 * (qualified v669), v458, v461, v462, v463 and v464 (the phone's standing library since then). For each library that
 * exists it runs the app's own loader
 * (CapabilityScanner.load, through a files/ folder whose game library is a link to the real file), prints one line
 * per setting - "enabled" with the values on offer, or the reason shown under the disabled row - and fails when the
 * names found are not the ones the other tests assume (SettingsCoreTestKit). A library that is not on this machine is
 * printed as SKIP, never counted as a pass.
 *
 * Argument: the repository root. Run with java -ea.
 */
public final class CapabilityScanReportTest {
    private static final String PHONE_SHA256 = "78a1c820098ba786af18e44d73773d76b9b1ff1ba9800ac88a8af654d94978f9";
    /** The library installed as the phone's standing build on 2026-10-05 (DISCOVERIES 10:54, INSTALLED_LIBRARY_SHA). */
    private static final String V464_SHA256 = "0acd381cd536e77553cf0783ce5307e9e50f37d84737b1b09ccb606894d356ff";

    private static String sha256(File file) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        InputStream in = new FileInputStream(file);
        try {
            byte[] buffer = new byte[1 << 20];
            for (int read; (read = in.read(buffer)) > 0; ) digest.update(buffer, 0, read);
        } finally {
            in.close();
        }
        StringBuilder hex = new StringBuilder();
        for (byte b : digest.digest()) hex.append(String.format("%02x", b));
        return hex.toString();
    }

    /** The app's loader on a files/ folder whose private-trace-live.so is a link to the real library. */
    private static Capabilities load(File library) throws IOException {
        File files = new File(tempDir(), "files");
        check(new File(files, "private-trace").mkdirs(), "test setup");
        Files.createSymbolicLink(new File(files, CapabilityScanner.LIBRARY).toPath(), library.toPath());
        Capabilities first = CapabilityScanner.load(files, true);
        check(new File(files, "settings/capabilities.txt").isFile(), "the scan wrote its cache");
        same(first.names(), CapabilityScanner.load(files, true).names(), "the cached answer equals the scan");
        return first;
    }

    /** Names in the stream followed by a zero byte, for names the app deliberately does not probe. */
    private static Set<String> raw(File library, String... names) throws IOException {
        InputStream in = new FileInputStream(library);
        try {
            return CapabilityScanner.scan(in, Arrays.asList(names));
        } finally {
            in.close();
        }
    }

    /** The text the settings screen shows for a value of a setting under these capabilities (MenuModel's row value). */
    private static String shown(Capabilities capabilities, SettingSpec spec, int row, int value) {
        MenuModel model = new MenuModel();
        model.loaded(Settings.defaults().with(spec.key, value), true, new ArrayList<String>(), new ArrayList<String>(), capabilities);
        return model.rowValue(row);
    }

    /**
     * One line per setting row, as the settings screen decides it (MenuModel) for a first visit at the defaults: the
     * values the row steps through, then those it shows when saved but does not step onto, then those the build lacks.
     */
    private static List<String> report(String label, Capabilities capabilities) {
        MenuModel model = new MenuModel();
        model.loaded(Settings.defaults(), false, new ArrayList<String>(), new ArrayList<String>(), capabilities);
        ArrayList<String> lines = new ArrayList<String>();
        for (int row = 0; row < model.rowCount(); row++) {
            if (model.rowKind(row) == MenuModel.KIND_ACTION || model.rowKind(row) == MenuModel.KIND_BANNER) continue;
            SettingSpec spec = SettingsSchema.all().get(lines.size());
            same(spec.label, model.rowLabel(row), "row order");
            StringBuilder line = new StringBuilder(String.format("SCAN  %-22s %-22s ", label, spec.label));
            if (!model.rowEnabled(row)) {
                line.append("DISABLED: ").append(model.rowSecondLine(row));
            } else {
                StringBuilder offered = new StringBuilder(), kept = new StringBuilder(), missing = new StringBuilder();
                int count = 0;
                for (int value = spec.min; value <= spec.max; value += spec.step) {
                    if (capabilities.offered(spec.key, value)) {
                        count++;
                        if (spec.kind != SettingSpec.Kind.SLIDER) offered.append(offered.length() == 0 ? "" : " | ").append(shown(capabilities, spec, row, value));
                    } else if (capabilities.valueSupported(spec.key, value)) {
                        kept.append(kept.length() == 0 ? "" : " | ").append(shown(capabilities, spec, row, value));
                    } else {
                        missing.append(missing.length() == 0 ? "" : " | ").append(spec.format(value)).append(" (").append(capabilities.reason(spec.key, value)).append(')');
                    }
                }
                line.append("enabled: ");
                if (spec.kind == SettingSpec.Kind.SLIDER) line.append(count).append(" values, ").append(spec.format(spec.min)).append(" to ").append(spec.format(spec.max));
                else line.append(offered);
                if (kept.length() > 0) line.append("; shown when saved, not a step: ").append(kept);
                if (missing.length() > 0) line.append("; not offered: ").append(missing);
            }
            lines.add(line.toString());
        }
        return lines;
    }

    private static List<String> sorted(String... names) {
        return new ArrayList<String>(new TreeSet<String>(Arrays.asList(names)));
    }

    private static List<String> disabledRows(Capabilities capabilities) {
        ArrayList<String> out = new ArrayList<String>();
        for (SettingSpec spec : SettingsSchema.all()) if (!capabilities.anyChoice(spec.key)) out.add(spec.label);
        return out;
    }

    public static void main(String[] args) {
        SettingsCoreTestKit.begin("CapabilityScanReportTest");
        final File root = new File(args.length > 0 ? args[0] : System.getProperty("repo.root", "."));
        final File attempts = new File(root, "local-analysis/attempts");

        test("the probe list: setting files (the two frame-generation switches among them), the GSR 1 switch, five markers; no remove-only file, no frame-pair dry-run name", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                same(Arrays.asList("scaler.txt", "fsr-sharpness.txt", "render-scale.txt", "world-load.txt", "diag-v280-limit30", "diag-v280-limit60",
                        "diag-pace-40", "diag-pace-30", "diag-pace-24", "diag-pace-auto", "diag-pace-auto-low", "diag-fg-between", "diag-fg-ahead",
                        "diag-v450-scale-fsr1", "diag-v409-scale-cubic",
                        "diag-v409-scale-linear", "diag-v450-fsr1-stops", "diag-fps-overlay", "diag-lt2-unbuffered", "diag-v460-scale-gsr1",
                        "recomp-cap:gsr1", "recomp-cap:wl-lod", "recomp-cap:framegen1", "recomp-cap:framegen-auto1", "recomp-cap:render-stage-b"),
                        CapabilityScanner.probeNames(), "probeNames()");
            }
        });

        final File phone = new File(root, "local-analysis/abi24-25-repair-20260921/exposure-copy-local-native-v668/libhost_guest_trace-blur320-local-v669.so");
        if (!phone.isFile()) skip("the library the phone ran until 2026-10-05 (qualified v669)", "not on this machine: " + phone);
        else test("the library the phone ran until 2026-10-05 (qualified v669): reads none of the setting files; only the app's own input row is enabled", new SettingsCoreTestKit.Case() {
            @Override public void run() throws Exception {
                same(PHONE_SHA256, sha256(phone), "this file is the library the phone ran (the phone's code-hash.txt of 2026-10-04)");
                Capabilities capabilities = load(phone);
                for (String line : report("v669", capabilities)) System.out.println(line);
                same(sorted(SettingsCoreTestKit.V669_NAMES), capabilities.names(), "names found");
                same(Arrays.asList("Frame rate cap", "Scaler", "FSR sharpness", "Render resolution", "Frame generation (experimental)", "Frame-rate counter", "Traffic",
                        "Pedestrians", "Parked cars", "Scenario pedestrians", "Draw distance"), disabledRows(capabilities), "disabled rows");
            }
        });

        final File v458 = new File(attempts, "v458/build/libhost_guest_trace-attempt-v458.so");
        if (!v458.isFile()) skip("v458", "not on this machine: " + v458);
        else test("v458: the older limiter and the scaler switch files; none of the new files", new SettingsCoreTestKit.Case() {
            @Override public void run() throws Exception {
                Capabilities capabilities = load(v458);
                for (String line : report("v458", capabilities)) System.out.println(line);
                same(sorted(SettingsCoreTestKit.V458_NAMES), capabilities.names(), "names found");
                same(Arrays.asList("Render resolution", "Frame generation (experimental)", "Traffic", "Pedestrians", "Parked cars", "Scenario pedestrians", "Draw distance"),
                        disabledRows(capabilities), "disabled rows");
            }
        });

        final File v461 = new File(attempts, "v461/build/libhost_guest_trace-attempt-v461.so");
        if (!v461.isFile()) skip("v461", "not on this machine: " + v461);
        else test("v461: the four caps and the world sliders; GSR 1 only by its developer switch, so not selectable; the dry run does not enable frame generation",
                new SettingsCoreTestKit.Case() {
            @Override public void run() throws Exception {
                Capabilities capabilities = load(v461);
                for (String line : report("v461", capabilities)) System.out.println(line);
                same(sorted(SettingsCoreTestKit.V461_NAMES), capabilities.names(), "names found");
                same(Arrays.asList("Render resolution", "Frame generation (experimental)", "Draw distance"), disabledRows(capabilities), "disabled rows");
                check(!capabilities.valueSupported("scaler", SettingsSchema.SCALER_GSR1), "GSR 1 is not selectable on v461");
                // names the library really contains and that must NOT make anything available
                same(sorted("diag-fg-pair-ahead", "diag-fg-pair-between", "diag-wl-lod-unlock", "diag-wl-test"),
                        new ArrayList<String>(raw(v461, "diag-fg-pair-ahead", "diag-fg-pair-between", "diag-wl-test", "diag-wl-lod-unlock", "diag-fg-ahead", "diag-fg-between")),
                        "v461 contains the dry-run and world test names (and not the app's old provisional frame-generation names)");
                same("Not in this version", capabilities.reason("frameGeneration", 1), "frame generation with the dry run in the library");
                check(!capabilities.has("diag-wl-test") && !capabilities.has("diag-fg-pair-ahead"), "those names are not capabilities");
            }
        });

        final File v462 = new File(attempts, "v462/build/libhost_guest_trace-attempt-v462.so");
        if (!v462.isFile()) skip("v462 library", "not built yet: " + v462);
        else test("v462: the two value files and the GSR 1 marker; all four scalers selectable", new SettingsCoreTestKit.Case() {
            @Override public void run() throws Exception {
                Capabilities capabilities = load(v462);
                for (String line : report("v462", capabilities)) System.out.println(line);
                same(sorted(SettingsCoreTestKit.V462_NAMES), capabilities.names(), "names found");
                same(Arrays.asList("Render resolution", "Frame generation (experimental)", "Draw distance"), disabledRows(capabilities), "disabled rows");
                check(capabilities.valueSupported("scaler", SettingsSchema.SCALER_GSR1), "GSR 1 is selectable");
            }
        });

        final File v463 = new File(attempts, "v463/build/libhost_guest_trace-attempt-v463.so");
        if (!v463.isFile()) skip("v463 library", "not on this machine: " + v463);
        else test("v463: render-scale.txt and no stage B marker; the Render Resolution row keeps its seven steps", new SettingsCoreTestKit.Case() {
            @Override public void run() throws Exception {
                Capabilities capabilities = load(v463);
                for (String line : report("v463", capabilities)) System.out.println(line);
                same(sorted(SettingsCoreTestKit.V463_NAMES), capabilities.names(), "names found");
                same(Arrays.asList("Frame generation (experimental)", "Draw distance"), disabledRows(capabilities), "disabled rows");
                check(!capabilities.renderStageB(), "no stage B");
            }
        });

        final File v464 = new File(attempts, "v464/build/libhost_guest_trace-attempt-v464.so");
        if (!v464.isFile()) skip("v464 library", "not on this machine: " + v464);
        else test("v464 (the phone's standing library since 2026-10-05): the stage B marker is found; the Render Resolution row has the five modes", new SettingsCoreTestKit.Case() {
            @Override public void run() throws Exception {
                same(V464_SHA256, sha256(v464), "this file is the library installed on the phone (DISCOVERIES 2026-10-05 10:54)");
                Capabilities capabilities = load(v464);
                for (String line : report("v464 (phone today)", capabilities)) System.out.println(line);
                same(sorted(SettingsCoreTestKit.V464_NAMES), capabilities.names(), "names found");
                same(Arrays.asList("Frame generation (experimental)", "Draw distance"), disabledRows(capabilities), "disabled rows");
                check(capabilities.renderStageB(), "stage B");
                same("Not in this version", capabilities.reason("frameGeneration", 1), "frame generation: v464 holds the dry run only, and no marker");
                same(sorted(SetTestKit.DRY_RUN), new ArrayList<String>(raw(v464, SetTestKit.DRY_RUN)), "(the dry run's four names are in this library, so the line above is not trivially true)");
            }
        });

        if (!v464.isFile()) skip("v464: the library's own path strings", "not on this machine: " + v464);
        else test("v464: each setting file is read from the folder the app writes it to (the library's own path strings)", new SettingsCoreTestKit.Case() {
            @Override public void run() throws Exception {
                // The capability scan asks for base names only. Here the whole path the library opens is looked for,
                // followed by a zero byte, for the files v464 names in one string (generic_renderer.cpp, world_load.h).
                String files = "/data/user/0/dev.recompbootstrap.smoke/files/";
                List<String> whole = Arrays.asList("settings/render-scale.txt", "settings/world-load.txt", "private-trace/diag-v280-limit30",
                        "private-trace/diag-v280-limit60", "private-trace/diag-pace-40", "private-trace/diag-pace-30", "private-trace/diag-pace-24",
                        "private-trace/diag-pace-auto", "private-trace/diag-fps-overlay");
                // The others are opened as folder + name (scaler_settings.h SettingsDir, direct_present.h Switches).
                List<String> inSettings = Arrays.asList("settings/scaler.txt", "settings/fsr-sharpness.txt");
                List<String> inSwitches = Arrays.asList("private-trace/diag-v450-scale-fsr1", "private-trace/diag-v409-scale-cubic",
                        "private-trace/diag-v409-scale-linear", "private-trace/diag-v450-fsr1-stops");
                Capabilities capabilities = load(v464);
                ArrayList<String> asked = new ArrayList<String>(), paths = new ArrayList<String>();
                for (String path : RuntimeMapping.runtimeFiles()) {
                    if (!capabilities.reads(path) || path.endsWith("diag-lt2-unbuffered")) continue;   // (the input switch is the app's own)
                    asked.add(path);
                    paths.add(files + path);
                }
                paths.add(files + "settings/");
                paths.add(files + "private-trace/");
                Set<String> found = raw(v464, paths.toArray(new String[0]));
                ArrayList<String> wholeFound = new ArrayList<String>(), split = new ArrayList<String>();
                for (String path : asked) (found.contains(files + path) ? wholeFound : split).add(path);
                same(sorted(whole.toArray(new String[0])), sorted(wholeFound.toArray(new String[0])), "paths the library holds whole");
                ArrayList<String> expectedSplit = new ArrayList<String>(inSettings);
                expectedSplit.addAll(inSwitches);
                same(sorted(expectedSplit.toArray(new String[0])), sorted(split.toArray(new String[0])), "paths the library builds from a folder and a name");
                check(found.contains(files + "settings/") && found.contains(files + "private-trace/"), "and both folders are strings of the library: " + found);
            }
        });

        // Until the v462 library exists: its compiled check objects (the integration lane's), scanned one by one.
        final File objects = new File(root, "local-analysis/agents/integrate-v462/obj/s3-census-crash");
        final File[] list = objects.listFiles();
        int objectFiles = 0;
        if (list != null) for (File object : list) if (object.getName().endsWith(".o")) objectFiles++;
        if (objectFiles == 0) skip("v462 check objects", "none on this machine any more (the v462 library itself is scanned above when it exists): " + objects);
        else test("v462 check objects (NOT the library: object files of the integration lane): the names v462's sources compile to", new SettingsCoreTestKit.Case() {
            @Override public void run() throws Exception {
                TreeSet<String> found = new TreeSet<String>();
                int scanned = 0;
                for (File object : list) {
                    if (!object.getName().endsWith(".o")) continue;
                    InputStream in = new FileInputStream(object);
                    try {
                        found.addAll(CapabilityScanner.scan(in, CapabilityScanner.probeNames()));
                    } finally {
                        in.close();
                    }
                    scanned++;
                }
                check(scanned > 0, "object files were scanned");
                Capabilities capabilities = Capabilities.of(found, true, true);
                for (String line : report("v462 (objects only)", capabilities)) System.out.println(line);
                same(sorted(SettingsCoreTestKit.V462_NAMES), capabilities.names(), "names found in " + scanned + " object files");
            }
        });

        SettingsCoreTestKit.end();
    }
}
