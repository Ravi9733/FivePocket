package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SettingsCoreTestKit.ALL_NAMES;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.PHONE_STANDING;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.V458_NAMES;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.V461_NAMES;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.V462_NAMES;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.ascii;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.caps;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.check;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.minus;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.names;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.plus;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.same;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.sameBytes;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.test;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

/**
 * Host test of RuntimeMapping against the game runtime's final contracts (local-analysis/agents/app-ui-2/CONTRACTS.md):
 * the fixed list of files, the table settings -> files, its inverse, and the comparison with what is on disk.
 * The expected files are written out here as literal paths and contents, independently of the constants in the code
 * under test. The capability sets are the names found in the real libraries (SettingsCoreTestKit; the scan itself is
 * CapabilityScanReportTest). Run with java -ea.
 */
public final class RuntimeMappingTest {
    private static final String LIMIT30 = "private-trace/diag-v280-limit30", LIMIT60 = "private-trace/diag-v280-limit60",
            PACE40 = "private-trace/diag-pace-40", PACE30 = "private-trace/diag-pace-30", PACE24 = "private-trace/diag-pace-24",
            PACEAUTO = "private-trace/diag-pace-auto", FSR1 = "private-trace/diag-v450-scale-fsr1",
            CUBIC = "private-trace/diag-v409-scale-cubic", LINEAR = "private-trace/diag-v409-scale-linear",
            STOPS = "private-trace/diag-v450-fsr1-stops", OVERLAY = "private-trace/diag-fps-overlay",
            UNBUFFERED = "private-trace/diag-lt2-unbuffered", SCALER_TXT = "settings/scaler.txt",
            SHARPNESS_TXT = "settings/fsr-sharpness.txt", RENDER_TXT = "settings/render-scale.txt", WORLD_TXT = "settings/world-load.txt",
            GSR_SWITCH = "private-trace/diag-v460-scale-gsr1",
            // The two switch files of frame generation, one per order (frame-gen/DESIGN-stage1.md section 7), and the
            // timing dry run's two, which v464 reads and this code must never name.
            FG_BETWEEN = "private-trace/diag-fg-between", FG_AHEAD = "private-trace/diag-fg-ahead",
            PAIR_BETWEEN = "private-trace/diag-fg-pair-between", PAIR_AHEAD = "private-trace/diag-fg-pair-ahead";

    private static final String[] SETTING_FILES = {"settings/settings.json", "settings/capabilities.txt", SCALER_TXT, SHARPNESS_TXT, RENDER_TXT,
            WORLD_TXT, LIMIT30, LIMIT60, PACE40, PACE30, PACE24, PACEAUTO, "private-trace/diag-pace-auto-low", FG_BETWEEN, FG_AHEAD, FSR1, CUBIC, LINEAR,
            STOPS, OVERLAY, UNBUFFERED};
    private static final String[] REMOVE_ONLY = {"private-trace/diag-wl-test", "private-trace/diag-wl-test-min",
            "private-trace/diag-wl-test-vehicles", "private-trace/diag-wl-test-peds", "private-trace/diag-wl-test-parked",
            "private-trace/diag-wl-test-scenario", "private-trace/diag-wl-test-lod", "private-trace/diag-wl-lod-unlock"};

    private static Settings defaults() { return Settings.defaults(); }

    private static Capabilities v669() { return SettingsCoreTestKit.v669(); }
    private static Capabilities v458() { return caps(V458_NAMES); }
    private static Capabilities v461() { return caps(V461_NAMES); }
    private static Capabilities v462() { return caps(V462_NAMES); }
    private static Capabilities all() { return caps(ALL_NAMES); }
    /** A build that announces frame generation (the marker and both order switches), and the phone's v464 as it is. */
    private static Capabilities framegen() { return SetTestKit.framegen(); }
    private static Capabilities v464() { return SetTestKit.v464WithDryRun(); }

    /** The entries of planned() about frame generation. */
    private static List<String> generationEntries(List<String> planned) {
        ArrayList<String> out = new ArrayList<String>();
        for (String entry : planned) if (entry.contains("diag-fg")) out.add(entry);
        return out;
    }

    private static SettingsCoreTestKit.MemoryFiles files() { return new SettingsCoreTestKit.MemoryFiles(); }

    /** The phone's setting-related files of 2026-10-05, plus two files that are not settings. */
    private static SettingsCoreTestKit.MemoryFiles phone() {
        return files().touch(PHONE_STANDING).touch("private-trace/diag-direct-present", "saves/slot0");
    }

    private static Map<String, byte[]> plan(Settings settings, Capabilities capabilities, FileView files) {
        return RuntimeMapping.plan(settings, capabilities, files);
    }

    /**
     * The plan without the entries every plan has (the input switch and the eight remove-only files), as text:
     * "path=content" for a value file, "path" for a switch file, "-path" for a file that must be absent.
     */
    private static List<String> planned(Settings settings, Capabilities capabilities, FileView files) {
        ArrayList<String> out = new ArrayList<String>();
        for (Map.Entry<String, byte[]> entry : plan(settings, capabilities, files).entrySet()) {
            if (entry.getKey().equals(UNBUFFERED) || Arrays.asList(REMOVE_ONLY).contains(entry.getKey())) continue;
            byte[] content = entry.getValue();
            out.add(content == null ? "-" + entry.getKey() : content.length == 0 ? entry.getKey()
                    : entry.getKey() + "=" + new String(content, java.nio.charset.StandardCharsets.US_ASCII).replace("\n", "|"));
        }
        return out;
    }

    private static List<String> list(String... items) { return Arrays.asList(items); }

    private static List<Integer> ints(Integer... items) { return Arrays.asList(items); }

    /** The entries of planned() about the frame cap. */
    private static List<String> capEntries(List<String> planned) {
        ArrayList<String> out = new ArrayList<String>();
        for (String entry : planned) if (entry.contains("limit") || entry.contains("diag-pace")) out.add(entry);
        return out;
    }

    /** text followed by blanks and one line feed, total bytes long. */
    private static String padded(String text, int total) {
        StringBuilder out = new StringBuilder(text);
        while (out.length() < total - 1) out.append(' ');
        return out.append('\n').toString();
    }

    private static Settings imported(Capabilities capabilities, SettingsCoreTestKit.MemoryFiles files) throws IOException {
        return RuntimeMapping.importFrom(files, capabilities);
    }

    /**
     * What the game will do for these settings on this build: every value the build cannot honour replaced by its
     * default, and no frame generation under any cap but 30 fps (value 3 of frameCap; the generator's own gate).
     */
    private static Settings effective(Settings settings, Capabilities capabilities) {
        Settings out = settings;
        for (SettingSpec spec : SettingsSchema.all()) out = out.with(spec.key, capabilities.effective(spec.key, settings.get(spec.key)));
        return out.get("frameCap") == 3 ? out : out.with("frameGeneration", 0);
    }

    private static TreeMap<String, String> state(SettingsCoreTestKit.MemoryFiles files) {
        TreeMap<String, String> out = new TreeMap<String, String>();
        for (Map.Entry<String, byte[]> entry : files.files.entrySet()) out.put(entry.getKey(), SettingsCoreTestKit.show(entry.getValue()));
        return out;
    }

    private static SettingsCoreTestKit.MemoryFiles copy(SettingsCoreTestKit.MemoryFiles files) {
        SettingsCoreTestKit.MemoryFiles out = files();
        out.files.putAll(files.files);
        return out;
    }

    /** A deterministic spread of settings: every combination of the choices and toggles, slider values cycling through samples. */
    private static List<Settings> manySettings() {
        int[] sharpness = {2, 0, 1, 3, 10, 25, 39, 40};
        int[][] world = {{100, 100, 100, 100, 100}, {25, 30, 50, 95, 100}, {100, 100, 100, 100, 95}, {35, 100, 100, 100, 100}, {25, 25, 25, 25, 70}};
        ArrayList<Settings> out = new ArrayList<Settings>();
        int n = 0;
        for (int cap = 0; cap < 6; cap++) for (int scaler = 0; scaler < 4; scaler++) for (int scale = 0; scale < 7; scale++)
            for (int generation = 0; generation < 3; generation++) for (int overlay = 0; overlay < 2; overlay++) for (int input = 0; input < 2; input++) {
                int[] w = world[n % world.length];
                out.add(defaults().with("frameCap", cap).with("scaler", scaler).with("renderScalePercent", scale)
                        .with("frameGeneration", generation).with("fpsOverlay", overlay).with("inputUnbuffered", input)
                        .with("fsrSharpnessTenths", sharpness[n % sharpness.length])
                        .with("worldTraffic", w[0]).with("worldPedestrians", w[1]).with("worldParkedCars", w[2])
                        .with("worldScenarioPedestrians", w[3]).with("worldDrawDistance", w[4]));
                n++;
            }
        out.add(defaults());
        return out;
    }

    private static Capabilities[] capabilitySets() {
        return new Capabilities[] {v669(), v458(), v461(), v462(), all(), Capabilities.of(names(ALL_NAMES), false, true),
                caps(plus(V458_NAMES, "scaler.txt")), caps(plus(V461_NAMES, "fsr-sharpness.txt")), caps(minus(V462_NAMES, "recomp-cap:gsr1")),
                caps(minus(V461_NAMES, "diag-pace-30")), Capabilities.of(names(ALL_NAMES), true, false), v464(), framegen(),
                caps(minus(SetTestKit.FRAMEGEN_NAMES, "diag-fg-between"))};
    }

    /** Starting states of the game's files: nothing, today's phone, and leftovers of every kind. */
    private static SettingsCoreTestKit.MemoryFiles[] startStates() {
        return new SettingsCoreTestKit.MemoryFiles[] {files(), phone(),
                phone().touch(GSR_SWITCH), files().touch(LINEAR, LIMIT30, PACE24, PACEAUTO, "private-trace/diag-wl-test"),
                phone().put(STOPS, "1.5\n").put(SCALER_TXT, "cubic\n").put(SHARPNESS_TXT, "3.0\n").put(RENDER_TXT, "75\n")
                        .put(WORLD_TXT, "traffic=50\n").touch(UNBUFFERED, PACE40, FG_AHEAD),
                files().touch(CUBIC).put(STOPS, "0.25\n").put(SCALER_TXT, "what\n").put(WORLD_TXT, "lod=10 peds=0\n")};
    }

    public static void main(String[] args) {
        SettingsCoreTestKit.begin("RuntimeMappingTest");

        test("the fixed list: the setting files and the remove-only files, none near the saves, none of the frame-pair dry run", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                ArrayList<String> owned = new ArrayList<String>(Arrays.asList(SETTING_FILES));
                owned.addAll(Arrays.asList(REMOVE_ONLY));
                same(owned, RuntimeMapping.ownedFiles(), "ownedFiles()");
                same(Arrays.asList(SETTING_FILES).subList(2, SETTING_FILES.length), RuntimeMapping.runtimeFiles(), "runtimeFiles()");
                same(Arrays.asList(REMOVE_ONLY), RuntimeMapping.removeOnlyFiles(), "removeOnlyFiles()");
                for (String path : RuntimeMapping.ownedFiles()) {
                    check(path.startsWith("settings/") || path.startsWith("private-trace/diag-"), path + " is outside settings/ and the switch files");
                    check(!path.contains("save") && !path.contains("profile") && !path.contains("sandbox") && !path.contains(".."), path + " is near the saves");
                    check(!path.contains("diag-fg-pair") && !path.contains("diag-fg-hold") && !path.contains("diag-fg-nocap"), path + " is a file of the frame-pair dry run");
                    check(path.indexOf('/') == path.lastIndexOf('/'), path + " is deeper than one folder");
                }
                check(!RuntimeMapping.ownedFiles().contains(GSR_SWITCH), "the GSR 1 developer switch is only looked at");
                SettingsCoreTestKit.throwsA(UnsupportedOperationException.class, "the list cannot be changed", new SettingsCoreTestKit.Case() {
                    @Override public void run() { RuntimeMapping.ownedFiles().add("saves/slot0"); }
                });
                same("diag-pace-40", RuntimeMapping.name(PACE40), "name()");
            }
        });

        test("today's phone: a Save of what the screen shows changes no file, with each of the four game libraries", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                Capabilities[] builds = {v669(), v458(), v461(), v462()};
                String[] label = {"v669 (on the phone today)", "v458", "v461", "v462"};
                for (int b = 0; b < builds.length; b++) {
                    SettingsCoreTestKit.MemoryFiles phone = phone();
                    Settings shown = imported(builds[b], phone);
                    same(SettingsSchema.CAP_60, shown.get("frameCap"), label[b] + ": frame cap shown");
                    same(SettingsSchema.SCALER_FSR1, shown.get("scaler"), label[b] + ": scaler shown");
                    same(b == 0 ? 0 : 1, shown.get("fpsOverlay"), label[b] + ": counter shown (the old library does not read its file)");
                    Map<String, byte[]> plan = plan(shown, builds[b], phone);
                    check(!plan.containsKey(LIMIT60), label[b] + ": diag-v280-limit60 is not in the plan");
                    same(state(phone), state(copy(phone).apply(plan)), label[b] + ": files after applying the plan");
                    same(new ArrayList<String>(), RuntimeMapping.drift(shown, builds[b], phone), label[b] + ": nothing differs");
                    // the same for the plain defaults, except the counter, which stands on the phone
                    Settings originals = defaults().with("fpsOverlay", shown.get("fpsOverlay"));
                    same(state(phone), state(copy(phone).apply(plan(originals, builds[b], phone))), label[b] + ": files after saving the defaults");
                }
                same(new ArrayList<String>(), planned(defaults(), v669(), phone()), "v669: the plan names no file the game reads");
                same(9, plan(defaults(), v669(), phone()).size(), "v669: only the app's own switch and the eight remove-only files");
            }
        });

        test("a file the installed build does not read is left alone", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                SettingsCoreTestKit.MemoryFiles any = phone().touch(PACE40, LINEAR, LIMIT30).put(STOPS, "1.0\n").put(SCALER_TXT, "cubic\n")
                        .put(SHARPNESS_TXT, "1.0\n").put(RENDER_TXT, "75\n").put(WORLD_TXT, "traffic=50\n");
                for (Settings settings : manySettings()) {
                    same(new ArrayList<String>(), planned(settings, v669(), any), "v669, " + settings);
                    Map<String, byte[]> onV458 = plan(settings, v458(), any);
                    for (String unread : list(SCALER_TXT, SHARPNESS_TXT, RENDER_TXT, WORLD_TXT)) {
                        check(!onV458.containsKey(unread), "v458 does not read " + unread + ", but the plan names it for " + settings);
                    }
                    // The one exception: 30 fps made by diag-v280-limit30 never stands beside a cap file, read or not.
                    boolean byLimiter = settings.get("frameCap") == SettingsSchema.CAP_30;
                    for (String cap : list(PACE40, PACE30, PACE24, PACEAUTO)) {
                        if (byLimiter) check(onV458.containsKey(cap) && onV458.get(cap) == null, "v458, 30 fps by the limiter: " + cap + " must be planned absent");
                        else check(!onV458.containsKey(cap), "v458 does not read " + cap + ", but the plan names it for " + settings);
                    }
                    Map<String, byte[]> onV462 = plan(settings, v462(), any);
                    for (String unasked : list(FSR1, CUBIC, LINEAR, STOPS, RENDER_TXT)) {
                        check(!onV462.containsKey(unasked), "v462: " + unasked + " is not consulted or not read, but the plan names it for " + settings);
                    }
                }
            }
        });

        test("frame cap: the pacing switch files; never a cap file beside diag-v280-limit30; diag-v280-limit60 only for the old 30", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                String[] none = {"-" + LIMIT30, "-" + PACE40, "-" + PACE30, "-" + PACE24, "-" + PACEAUTO};
                String[] untouched = {"-" + PACE40, "-" + PACE30, "-" + PACE24, "-" + PACEAUTO};
                // (the phone's diag-v280-limit60 stands: at 60 a limit30 beside it has no effect and is left as it is)
                String[][] expected = {untouched, untouched,
                        {"-" + LIMIT30, PACE40, "-" + PACE30, "-" + PACE24, "-" + PACEAUTO},
                        {"-" + LIMIT30, "-" + PACE40, PACE30, "-" + PACE24, "-" + PACEAUTO},
                        {"-" + LIMIT30, "-" + PACE40, "-" + PACE30, PACE24, "-" + PACEAUTO},
                        {"-" + LIMIT30, "-" + PACE40, "-" + PACE30, "-" + PACE24, PACEAUTO}};
                for (int cap = 0; cap < 6; cap++) {
                    List<String> got = planned(defaults().with("frameCap", cap), v461(), phone().touch(LIMIT30));
                    same(list(expected[cap]), capEntries(got), "v461, cap " + cap + " (a standing limit30 goes whenever a cap file is planned)");
                    check(!got.contains(LIMIT60) && !got.contains("-" + LIMIT60), "v461, cap " + cap + ": diag-v280-limit60 is left alone");
                }
                same(list("-" + LIMIT30, "-" + PACE40, "-" + PACE30, "-" + PACE24, "-" + PACEAUTO), capEntries(planned(defaults(), v461(), files().touch(LIMIT30))),
                        "v461, 60 fps, limit30 standing alone (it would give 30): it goes");
                // v458 has only the game's own limiter. 30 fps: limit30, and no cap file beside it although v458 reads none
                // (review finding: a leftover diag-pace-40 stood beside the new limit30)
                same(list(LIMIT30, "-" + LIMIT60, "-" + PACE40, "-" + PACE30, "-" + PACE24, "-" + PACEAUTO),
                        capEntries(planned(defaults().with("frameCap", SettingsSchema.CAP_30), v458(), phone().touch(PACE40))), "v458, 30 fps");
                same(list(), capEntries(planned(defaults(), v458(), phone())), "v458, 60 fps on the phone: no cap file is named, limit60 neither");
                same(list(), capEntries(planned(defaults(), v458(), phone().touch(LIMIT30, PACE40))), "v458, 60 fps: a limit30 beside limit60 changes nothing and stays");
                same(list("-" + LIMIT30), capEntries(planned(defaults(), v458(), files().touch(LIMIT30))), "v458, 60 fps, limit30 alone: it goes");
                same(list(), capEntries(planned(defaults().with("frameCap", SettingsSchema.CAP_40), v458(), phone())), "v458, 40 fps is not supported: as 60");
                // a build that reads the pacing files but not limit30 still never leaves limit30 beside a cap file
                Capabilities paceOnly = caps("diag-pace-40", "diag-pace-30");
                same(list("-" + LIMIT30, PACE40, "-" + PACE30), planned(defaults().with("frameCap", SettingsSchema.CAP_40), paceOnly, files()), "pace only, 40");
                same(list("-" + PACE40, "-" + PACE30), planned(defaults(), paceOnly, files()), "pace only, 60: limit30 is not read, not named");
            }
        });

        test("scaler on a build that reads scaler.txt: only the word file, and only when the switch files say otherwise", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                String[] word = {"fsr1", "gsr1", "cubic", "nearest"};
                // {files standing, the scaler those files give without a word}
                Object[][] cases = {{phone(), 0}, {phone().touch(GSR_SWITCH), 1}, {files().touch(CUBIC), 2}, {files(), 3},
                        {files().touch(LINEAR), -1}, {files().touch(GSR_SWITCH, LINEAR), 1}, {files().touch(FSR1), 0}};
                for (Object[] c : cases) {
                    for (int scaler = 0; scaler < 4; scaler++) {
                        List<String> got = planned(defaults().with("scaler", scaler), v462(), (FileView) c[0]);
                        String expected = scaler == (Integer) c[1] ? "-" + SCALER_TXT : SCALER_TXT + "=" + word[scaler] + "|";
                        check(got.contains(expected), "switches give " + c[1] + ", chosen " + scaler + ": expected " + expected + " in " + got);
                        for (String entry : got) check(!entry.contains("scale-"), "a scaler switch file is in the plan: " + entry);
                    }
                }
                // a word already there does not change the answer: the plan is made from the switch files
                same(true, planned(defaults(), v462(), phone().put(SCALER_TXT, "cubic\n")).contains("-" + SCALER_TXT), "FSR 1 chosen, word cubic standing: removed");
                check(!plan(defaults(), v462(), phone().put(SCALER_TXT, " FSR1\n")).containsKey(SCALER_TXT), "FSR 1 chosen, the word fsr1 standing: left as it is");
                check(planned(defaults().with("scaler", 2), v462(), phone().put(SCALER_TXT, "CUBIC")).contains(SCALER_TXT + "=cubic|"),
                        "a word is needed and the file holds it in another form: written in this screen's form");
            }
        });

        test("scaler on a build without scaler.txt: the switch files", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                String[][] expected = {{FSR1, CUBIC}, {FSR1, CUBIC}, {"-" + FSR1, CUBIC}, {"-" + FSR1, "-" + CUBIC, "-" + LINEAR}};
                for (Capabilities build : new Capabilities[] {v458(), v461()}) {
                    for (int scaler = 0; scaler < 4; scaler++) {
                        List<String> got = planned(defaults().with("scaler", scaler), build, phone());
                        for (String entry : expected[scaler]) check(got.contains(entry), "scaler " + scaler + ": expected " + entry + " in " + got);
                        check(!got.toString().contains("scaler.txt"), "scaler.txt is not read by this build: " + got);
                        same(scaler == 3, got.contains("-" + LINEAR), "the bilinear switch is named only for Nearest (it loses against the others): " + got);
                    }
                }
                same(list("-" + LIMIT30, CUBIC, "-" + OVERLAY), planned(defaults(), caps("diag-v409-scale-cubic", "diag-v280-limit30", "diag-fps-overlay"), files()),
                        "a build older than FSR 1: only the files it reads");
            }
        });

        test("FSR sharpness: fsr-sharpness.txt only when diag-v450-fsr1-stops does not already give the value", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                Settings half = defaults().with("fsrSharpnessTenths", 5), three = defaults().with("fsrSharpnessTenths", 3);
                check(planned(defaults(), v462(), phone()).contains("-" + SHARPNESS_TXT), "default, no older file: absent");
                check(planned(half, v462(), phone()).contains(SHARPNESS_TXT + "=0.5|"), "0.5, no older file: written");
                check(planned(defaults().with("fsrSharpnessTenths", 40), v462(), phone()).contains(SHARPNESS_TXT + "=4.0|"), "4.0");
                check(planned(defaults().with("fsrSharpnessTenths", 0), v462(), phone()).contains(SHARPNESS_TXT + "=0.0|"), "0.0");
                check(planned(half, v462(), phone().put(STOPS, "0.5\n")).contains("-" + SHARPNESS_TXT), "0.5, older file says 0.5: absent");
                check(planned(defaults(), v462(), phone().put(STOPS, "0.5\n")).contains(SHARPNESS_TXT + "=0.2|"), "default, older file says 0.5: 0.2 is written");
                check(planned(three, v462(), phone().put(STOPS, "0.25\n")).contains(SHARPNESS_TXT + "=0.3|"), "0.3, older file says 0.25 (not 0.3): written");
                check(planned(defaults(), v462(), phone().put(STOPS, "2e-1\n")).contains(SHARPNESS_TXT + "=0.2|"), "older file in a form only scanf reads: written");
                check(!plan(half, v462(), phone().put(STOPS, "0.5\n").put(SHARPNESS_TXT, "0.50")).containsKey(SHARPNESS_TXT), "0.5, both files say 0.5: left as it is");
                for (String entry : planned(half, v462(), phone().put(STOPS, "1.0\n"))) check(!entry.contains("fsr1-stops"), "v462: the older file is left alone: " + entry);
                // builds without the value file
                check(planned(half, v461(), phone()).contains(STOPS + "=0.5|"), "v461, 0.5: the older file");
                check(planned(defaults(), v461(), phone().put(STOPS, "0.5\n")).contains("-" + STOPS), "v461, default: the older file goes");
                // least change: a file that already gives the chosen value is not named (review finding)
                check(!plan(defaults(), v461(), phone().put(STOPS, "0.2\n")).containsKey(STOPS), "v461, default, the file says 0.2: left as it is");
                check(!plan(half, v461(), phone().put(STOPS, "0.5")).containsKey(STOPS), "v461, 0.5, the file says 0.5 without a line feed: left as it is");
                check(!plan(defaults(), v461(), phone()).containsKey(STOPS), "v461, default, no file: not named");
                check(planned(half, v461(), phone().put(STOPS, "0.50001\n")).contains(STOPS + "=0.5|"), "v461, 0.5, the file is not an exact tenth: rewritten");
                check(!planned(half, v461(), phone()).toString().contains("fsr-sharpness"), "v461 does not read fsr-sharpness.txt");
            }
        });

        test("render resolution: the bare percentage, no file at 100, nothing for a build that does not read it", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                String[] text = {"50", "75", "88", null, "115", "125", "150"};
                for (int i = 0; i < text.length; i++) {
                    List<String> got = planned(defaults().with("renderScalePercent", i), all(), phone());
                    check(got.contains(text[i] == null ? "-" + RENDER_TXT : RENDER_TXT + "=" + text[i] + "|"), "index " + i + ": " + got);
                    check(!planned(defaults().with("renderScalePercent", i), v462(), phone().put(RENDER_TXT, "75\n")).toString().contains("render-scale"),
                            "v462 does not read render-scale.txt");
                }
            }
        });

        test("world sliders: no file at all when every slider is at the original; draw distance is not written", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                check(planned(defaults(), v461(), phone()).contains("-" + WORLD_TXT), "all original: absent");
                check(planned(defaults(), v461(), phone().put(WORLD_TXT, "traffic=100\n")).contains("-" + WORLD_TXT), "all original: a standing file goes");
                Settings some = defaults().with("worldTraffic", 35).with("worldScenarioPedestrians", 25);
                check(planned(some, v461(), phone()).contains(WORLD_TXT + "=traffic=35|pedestrians=100|parked_cars=100|scenario_pedestrians=25|"),
                        "four lines, the app's spellings: " + planned(some, v461(), phone()));
                Settings far = defaults().with("worldDrawDistance", 80);
                check(planned(far, v461(), phone()).contains("-" + WORLD_TXT), "v461: draw distance is locked, so 80 writes nothing");
                check(planned(far.with("worldTraffic", 50), v461(), phone()).contains(WORLD_TXT + "=traffic=50|pedestrians=100|parked_cars=100|scenario_pedestrians=100|"),
                        "v461: no draw_distance line");
                check(planned(far, all(), phone()).contains(WORLD_TXT + "=traffic=100|pedestrians=100|parked_cars=100|scenario_pedestrians=100|draw_distance=80|"),
                        "a build that announces draw distance: five lines");
                check(planned(defaults(), all(), phone()).contains("-" + WORLD_TXT), "that build, all original: absent");
            }
        });

        test("frame generation: its switch file only in a build that announces it, only for the chosen order, only under the 30 fps cap", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                // Builds without the marker - v464 with its dry run among them, and a build that holds the two switch
                // names unannounced: nothing is named, also when such a file stands (it is somebody's test).
                Capabilities unannounced = caps(minus(SetTestKit.FRAMEGEN_NAMES, "recomp-cap:framegen1"));
                SettingsCoreTestKit.MemoryFiles standing = phone().touch(PACE30, FG_BETWEEN, PAIR_AHEAD);
                for (Capabilities build : new Capabilities[] {v669(), v461(), all(), v464(), unannounced}) {
                    for (int generation = 0; generation < 3; generation++) {
                        Settings chosen = defaults().with("frameCap", SettingsSchema.CAP_30).with("frameGeneration", generation);
                        same(list(), generationEntries(planned(chosen, build, standing)), "no marker, frame generation " + generation + ", build " + build.names());
                    }
                }
                // The announced build. Off (the default) plans both switches absent: on today's phone nothing is written.
                same(list("-" + FG_BETWEEN, "-" + FG_AHEAD), generationEntries(planned(defaults(), framegen(), phone())), "defaults");
                Settings shown = RuntimeMapping.importFrom(phone(), framegen());
                same(SettingsSchema.FG_OFF, shown.get("frameGeneration"), "today's phone under the announced build: the row shows Off");
                same(state(phone()), state(phone().apply(plan(shown, framegen(), phone()))), "a Save of what the screen shows changes no file on today's phone");
                Settings between = defaults().with("frameCap", SettingsSchema.CAP_30).with("frameGeneration", 1);
                Settings ahead = defaults().with("frameCap", SettingsSchema.CAP_30).with("frameGeneration", 2);
                same(list(FG_BETWEEN, "-" + FG_AHEAD), generationEntries(planned(between, framegen(), phone())), "in-between frames under 30 fps");
                same(list("-" + FG_BETWEEN, FG_AHEAD), generationEntries(planned(ahead, framegen(), phone())), "predicted frames under 30 fps");
                sameBytes(new byte[0], plan(between, framegen(), phone()).get(FG_BETWEEN), "a switch file: created empty");
                check(planned(between, framegen(), phone()).contains(PACE30), "the 30 fps cap file is planned with it");
                // Under every other cap the generator makes no picture, and a standing switch would still change the
                // timing: both files are planned absent, whichever order is chosen.
                for (int cap : new int[] {SettingsSchema.CAP_OFF, SettingsSchema.CAP_60, SettingsSchema.CAP_40, SettingsSchema.CAP_24, SettingsSchema.CAP_AUTO}) {
                    for (int generation = 1; generation < 3; generation++) {
                        same(list("-" + FG_BETWEEN, "-" + FG_AHEAD), generationEntries(planned(between.with("frameCap", cap).with("frameGeneration", generation),
                                framegen(), phone().touch(FG_BETWEEN))), "cap " + cap + ", frame generation " + generation);
                    }
                }
                // A build with one order only: its file alone is named, and the order it lacks counts as Off.
                Capabilities aheadOnly = caps(minus(SetTestKit.FRAMEGEN_NAMES, "diag-fg-between"));
                same(list(FG_AHEAD), generationEntries(planned(ahead, aheadOnly, phone())), "ahead only: predicted frames");
                same(list("-" + FG_AHEAD), generationEntries(planned(between, aheadOnly, phone())), "ahead only: in-between frames is not in the build");
                // The dry run's files are never in a plan, whatever stands and whatever is chosen.
                for (Settings settings : new Settings[] {defaults(), between, ahead}) {
                    Map<String, byte[]> plan = plan(settings, framegen(), phone().touch(PAIR_BETWEEN, PAIR_AHEAD));
                    check(!plan.containsKey(PAIR_BETWEEN) && !plan.containsKey(PAIR_AHEAD), "a dry-run file is in the plan for " + settings);
                    for (String leftover : REMOVE_ONLY) check(plan.containsKey(leftover) && plan.get(leftover) == null, leftover + " planned absent");
                }
            }
        });

        test("import: frame generation as the game decides it - between wins, and only the pacer's 30 fps cap opens it", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                same(1, imported(framegen(), files().touch(PACE30, FG_BETWEEN)).get("frameGeneration"), "the between file under the 30 fps cap");
                same(2, imported(framegen(), files().touch(PACE30, FG_AHEAD)).get("frameGeneration"), "the ahead file under the 30 fps cap");
                same(1, imported(framegen(), files().touch(PACE30, FG_AHEAD, FG_BETWEEN)).get("frameGeneration"), "both files: between, as the game decides");
                same(0, imported(framegen(), files().touch(PACE30)).get("frameGeneration"), "no file: off");
                same(0, imported(framegen(), files().touch(PACE40, FG_BETWEEN)).get("frameGeneration"), "under 40 fps the game makes no second picture: off");
                same(0, imported(framegen(), files().touch(FG_AHEAD)).get("frameGeneration"), "under 60 fps: off");
                same(0, imported(framegen(), files().touch(LIMIT30, FG_BETWEEN)).get("frameGeneration"), "30 fps by the game's own limiter is not the pacer's cap: off");
                same(0, imported(framegen(), files().touch(PACE30, PACEAUTO, FG_BETWEEN)).get("frameGeneration"), "the automatic cap wins over 30: off");
                same(0, imported(framegen(), files().touch(PACE30, PAIR_BETWEEN, PAIR_AHEAD)).get("frameGeneration"), "the dry-run switches are not frame generation");
                same(0, imported(v464(), files().touch(PACE30, FG_BETWEEN, PAIR_AHEAD)).get("frameGeneration"), "v464 has no generator: off, whatever stands");
                // A switch standing under another cap is not in the form this screen writes: a Save would remove it, and drift says so.
                SettingsCoreTestKit.MemoryFiles odd = phone().touch(PACE40, FG_BETWEEN);
                same(list(FG_BETWEEN), RuntimeMapping.drift(imported(framegen(), odd), framegen(), odd), "the between file under 40 fps");
                SettingsCoreTestKit.MemoryFiles both = phone().touch(PACE30, FG_BETWEEN, FG_AHEAD);
                same(list(FG_AHEAD), RuntimeMapping.drift(imported(framegen(), both), framegen(), both), "both files: the one the game ignores");
                SettingsCoreTestKit.MemoryFiles exact = phone().touch(PACE30, FG_AHEAD);
                same(list(), RuntimeMapping.drift(imported(framegen(), exact), framegen(), exact), "one file under the 30 fps cap: a Save changes nothing");
            }
        });

        test("frame-rate counter and input switch", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                check(planned(defaults().with("fpsOverlay", 1), v461(), files()).contains(OVERLAY), "counter on");
                check(planned(defaults(), v461(), files()).contains("-" + OVERLAY), "counter off");
                sameBytes(new byte[0], plan(defaults().with("inputUnbuffered", 1), v669(), files()).get(UNBUFFERED), "input on: the app's own switch, with any library");
                sameBytes(null, plan(defaults(), v669(), files()).get(UNBUFFERED), "input off");
                sameBytes(null, plan(defaults().with("inputUnbuffered", 1), Capabilities.of(names(ALL_NAMES), true, false), files()).get(UNBUFFERED), "no input option in the app: off");
            }
        });

        test("every plan: only files of the fixed list, in its order, and value files end with one line feed", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                List<String> owned = RuntimeMapping.ownedFiles();
                for (Capabilities capabilities : capabilitySets()) {
                    for (Settings settings : manySettings()) {
                        int last = -1;
                        for (Map.Entry<String, byte[]> entry : plan(settings, capabilities, phone()).entrySet()) {
                            int at = owned.indexOf(entry.getKey());
                            check(at > last, entry.getKey() + " is out of order or not on the fixed list");
                            last = at;
                            byte[] content = entry.getValue();
                            if (content != null && content.length > 0) {
                                check(entry.getKey().endsWith(".txt") || entry.getKey().equals(STOPS), entry.getKey() + " is a switch file but has content");
                                check(content[content.length - 1] == '\n' && content[content.length - 2] != '\n', entry.getKey() + ": one line feed at the end");
                                check(content.length < (entry.getKey().equals(WORLD_TXT) ? 513 : 64), entry.getKey() + ": short enough for the game's reader");
                            }
                        }
                    }
                }
            }
        });

        test("round trip: after applying a plan nothing differs, a second plan changes nothing, and the files stand for what the game will do", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                int checked = 0;
                for (Capabilities capabilities : capabilitySets()) {
                    SettingsCoreTestKit.MemoryFiles[] starts = startStates();
                    for (int s = 0; s < starts.length; s++) {
                        int n = 0;
                        for (Settings settings : manySettings()) {
                            if (n++ % 7 != s % 7 && !settings.isDefault()) continue; // a seventh of the spread per starting state
                            SettingsCoreTestKit.MemoryFiles disk = copy(starts[s]);
                            disk.apply(plan(settings, capabilities, disk));
                            String what = "state " + s + ", " + settings + ", build " + capabilities.names();
                            same(new ArrayList<String>(), RuntimeMapping.drift(settings, capabilities, disk), "drift right after applying: " + what);
                            same(state(disk), state(copy(disk).apply(plan(settings, capabilities, disk))), "a second save changes nothing: " + what);
                            check(!(disk.exists(LIMIT30) && (disk.exists(PACE40) || disk.exists(PACE30) || disk.exists(PACE24) || disk.exists(PACEAUTO)))
                                    || !capabilities.reads(PACE40), "a cap file beside diag-v280-limit30: " + what);
                            // Files the build reads but this app never creates can overrule a row from outside; leave those states out.
                            // The one file a build reads that this app never touches and no word file overrules: the GSR 1
                            // developer switch on a build without scaler.txt (the screen warns about it instead).
                            boolean foreign = disk.exists(GSR_SWITCH) && capabilities.reads(GSR_SWITCH) && !capabilities.reads(SCALER_TXT);
                            if (!foreign) {
                                Settings want = effective(settings, capabilities);
                                Settings got = imported(capabilities, disk);
                                for (SettingSpec spec : SettingsSchema.all()) {
                                    if (spec.key.equals("fsrSharpnessTenths") && want.get("scaler") != SettingsSchema.SCALER_FSR1) continue;
                                    same(want.get(spec.key), got.get(spec.key), spec.key + " read back: " + what);
                                }
                            }
                            checked++;
                        }
                    }
                }
                check(checked > 1500, "the round trip covered " + checked + " cases");
            }
        });

        test("import: the frame cap with the game's precedence", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                same(SettingsSchema.CAP_60, imported(v461(), files()).get("frameCap"), "no file");
                same(SettingsSchema.CAP_60, imported(v461(), files().touch(LIMIT60)).get("frameCap"), "limit60");
                same(SettingsSchema.CAP_30, imported(v461(), files().touch(LIMIT30)).get("frameCap"), "limit30");
                same(SettingsSchema.CAP_60, imported(v461(), files().touch(LIMIT30, LIMIT60)).get("frameCap"), "limit60 forces 60 over limit30");
                same(SettingsSchema.CAP_40, imported(v461(), files().touch(PACE40)).get("frameCap"), "pace-40");
                same(SettingsSchema.CAP_30, imported(v461(), files().touch(PACE40, PACE30)).get("frameCap"), "several fixed caps: the lowest rate");
                same(SettingsSchema.CAP_24, imported(v461(), files().touch(PACE40, PACE30, PACE24)).get("frameCap"), "the lowest rate of three");
                same(SettingsSchema.CAP_AUTO, imported(v461(), files().touch(PACE24, PACEAUTO)).get("frameCap"), "automatic wins over the fixed ones");
                same(SettingsSchema.CAP_60, imported(v458(), files().touch(PACE40, PACEAUTO)).get("frameCap"), "v458 does not read the pacing files");
                same(SettingsSchema.CAP_60, imported(v669(), files().touch(LIMIT30, PACE24)).get("frameCap"), "v669 reads none of them");
            }
        });

        test("import: the scaler word as the game reads it, then the switch files with the game's precedence", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                Object[][] words = {{"fsr1\n", 0}, {"gsr1", 1}, {"cubic\n", 2}, {"nearest\n", 3}, {"  CuBiC \r\n", 2}, {"\tNEAREST", 3},
                        // not valid: the switch files decide (here: FSR 1 by the phone's two switch files)
                        {"", 0}, {"bicubic\n", 0}, {"cubic nearest\n", 0}, {"cubic\0", 0}, {"ïcubic", 0},
                        {padded("cubic", 64), 0}};
                for (Object[] w : words) same(w[1], imported(v462(), phone().put(SCALER_TXT, (String) w[0])).get("scaler"), "word <" + w[0] + ">");
                same(2, imported(v462(), phone().put(SCALER_TXT, padded("cubic", 63))).get("scaler"), "63 bytes is still read");
                same(SettingsSchema.SCALER_FSR1, imported(caps(minus(V462_NAMES, "recomp-cap:gsr1")), files().put(SCALER_TXT, "gsr1\n")).get("scaler"),
                        "the word gsr1 in a build without the pass is FSR 1");
                same(SettingsSchema.SCALER_GSR1, imported(v462(), phone().touch(GSR_SWITCH)).get("scaler"), "v462, no word, GSR switch standing: GSR 1 is what runs");
                same(SettingsSchema.SCALER_BICUBIC, imported(v462(), phone().touch(GSR_SWITCH).put(SCALER_TXT, "cubic\n")).get("scaler"), "a valid word wins over the GSR switch");
                same(SettingsSchema.SCALER_FSR1, imported(v461(), phone().touch(GSR_SWITCH)).get("scaler"), "v461: GSR 1 is not a row value there; the row shows what the other switches say (FSR 1)");
                // review finding: with only the cubic switch beside it the row showed FSR 1 and a Save created the FSR 1 switch
                SettingsCoreTestKit.MemoryFiles gsrAndCubic = files().touch(GSR_SWITCH, CUBIC);
                same(SettingsSchema.SCALER_BICUBIC, imported(v461(), gsrAndCubic).get("scaler"), "v461, GSR switch + cubic only: Bicubic");
                same(new ArrayList<String>(), RuntimeMapping.drift(imported(v461(), gsrAndCubic), v461(), gsrAndCubic), "v461, GSR switch + cubic only: a Save at the shown values changes no file");
                same(SettingsSchema.SCALER_NEAREST, imported(v461(), files().touch(GSR_SWITCH)).get("scaler"), "v461, GSR switch alone: Nearest");
                same(SettingsSchema.SCALER_FSR1, imported(v461(), files().touch(FSR1)).get("scaler"), "FSR switch alone");
                same(SettingsSchema.SCALER_BICUBIC, imported(v461(), files().touch(CUBIC, LINEAR)).get("scaler"), "cubic wins over linear");
                same(SettingsSchema.SCALER_NEAREST, imported(v461(), files().touch(LINEAR)).get("scaler"), "linear has no row value: shown as nearest");
                same(SettingsSchema.SCALER_NEAREST, imported(v461(), files()).get("scaler"), "no switch: nearest");
                same(SettingsSchema.SCALER_FSR1, imported(v669(), files()).get("scaler"), "a build that reads no scaler input: the default, whatever the files are");
                same(SettingsSchema.SCALER_FSR1, imported(v458(), files().touch(FSR1).put(SCALER_TXT, "nearest\n")).get("scaler"), "v458 does not read scaler.txt");
            }
        });

        test("import: sharpness as the game reads it", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                Object[][] texts = {{"0", 0}, {"0.2\n", 2}, {" 1.25 \n", 13}, {"4", 40}, {"4.0", 40}, {"3.95", 40}, {"0.04", 0}, {"0.05", 1}, {"1.", 10}, {".5", 5},
                        // not valid for the game: the older file, then the default
                        {"4.1", 2}, {"-1", 2}, {"1e0", 2}, {"1,5", 2}, {"", 2}, {".", 2}, {"1.2.3", 2}, {"0000000000000001.5", 2}};
                for (Object[] t : texts) same(t[1], imported(v462(), phone().put(SHARPNESS_TXT, (String) t[0])).get("fsrSharpnessTenths"), "text <" + t[0] + ">");
                same(15, imported(v462(), phone().put(SHARPNESS_TXT, "9").put(STOPS, "1.5\n")).get("fsrSharpnessTenths"), "invalid value file: the older file");
                same(15, imported(v462(), phone().put(SHARPNESS_TXT, padded("1.5", 63))).get("fsrSharpnessTenths"), "a 63-byte file is read");
                same(2, imported(v462(), phone().put(SHARPNESS_TXT, padded("1.5", 64))).get("fsrSharpnessTenths"), "a 64-byte file is too long for the game");
                same(30, imported(v462(), phone().put(SHARPNESS_TXT, "3.0").put(STOPS, "1.5\n")).get("fsrSharpnessTenths"), "the value file wins");
                same(15, imported(v461(), phone().put(SHARPNESS_TXT, "3.0").put(STOPS, "1.5\n")).get("fsrSharpnessTenths"), "v461 reads only the older file");
                same(2, imported(v669(), phone().put(STOPS, "1.5\n")).get("fsrSharpnessTenths"), "v669 reads neither");
            }
        });

        test("import: render resolution as the game reads it, by the size it gives", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                Object[][] texts = {{"50", 0}, {"75\n", 1}, {"88", 2}, {"100", 3}, {"115", 4}, {"125", 5}, {"150", 6}, {" 150 % \n", 6}, {"0150", 6},
                        {"87", 2} /* 14 sixteenths like 88 */, {"97", 3}, {"103", 3}, {"76", 1},
                        // sizes the screen does not offer, and texts the game does not accept: 100
                        {"96", 3}, {"200", 3}, {"49", 3}, {"201", 3}, {"1000", 3}, {"00150", 3}, {"75.0", 3}, {"+75", 3}, {"75 75", 3}, {"", 3}, {"%", 3}};
                for (Object[] t : texts) same(t[1], imported(all(), files().put(RENDER_TXT, (String) t[0])).get("renderScalePercent"), "text <" + t[0] + ">");
                same(3, imported(v462(), files().put(RENDER_TXT, "75")).get("renderScalePercent"), "v462 does not read the file");
                same(1, imported(all(), files().put(RENDER_TXT, padded("75", 63))).get("renderScalePercent"), "a 63-byte file is read");
                same(3, imported(all(), files().put(RENDER_TXT, padded("75", 64))).get("renderScalePercent"), "a 64-byte file is too long for the game");
            }
        });

        test("import: world sliders as the game reads them, raised to the game's floors", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                Settings got = imported(all(), files().put(WORLD_TXT, "traffic=35\npedestrians=0 parked=24\tscenario=101 lod=10\n"));
                same(ints(35, 25, 25, 100, 70), ints(got.get("worldTraffic"), got.get("worldPedestrians"), got.get("worldParkedCars"),
                        got.get("worldScenarioPedestrians"), got.get("worldDrawDistance")), "floors 25 and 70; an invalid number is the original");
                got = imported(v461(), files().put(WORLD_TXT, "vehicles=50 traffic=40 peds=x draw_distance=80 unknown=5 scenario_pedestrians=62\n"));
                same(ints(40, 100, 100, 60, 100), ints(got.get("worldTraffic"), got.get("worldPedestrians"), got.get("worldParkedCars"),
                        got.get("worldScenarioPedestrians"), got.get("worldDrawDistance")), "last one counts; nearest step; draw distance stays locked on v461");
                StringBuilder exact = new StringBuilder("traffic=50");
                while (exact.length() < 512) exact.append(' ');
                same(50, imported(v461(), files().put(WORLD_TXT, exact.toString())).get("worldTraffic"), "512 bytes are read");
                same(100, imported(v461(), files().put(WORLD_TXT, exact + " ")).get("worldTraffic"), "513 bytes: as no file");
                same(100, imported(v458(), files().put(WORLD_TXT, "traffic=50")).get("worldTraffic"), "v458 does not read the file");
            }
        });

        test("import: counter and input switch; files in a form this screen does not write are named by drift", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                same(1, imported(v461(), files().touch(OVERLAY)).get("fpsOverlay"), "counter");
                same(0, imported(v669(), files().touch(OVERLAY)).get("fpsOverlay"), "v669 does not read the counter file");
                same(1, imported(v669(), files().touch(UNBUFFERED)).get("inputUnbuffered"), "the input switch is the app's own");
                // what a Save would rewrite, per case
                Object[][] cases = {
                        {v461(), files().touch(LINEAR), list(LINEAR)},
                        {v462(), files().touch(LINEAR), list(SCALER_TXT)},
                        {v462(), phone().put(SCALER_TXT, "what\n"), list(SCALER_TXT)},
                        {v462(), phone().put(SCALER_TXT, "fsr1\n"), list()},
                        {v461(), phone().touch(PACE40, PACE24), list(PACE40)},
                        {v461(), phone().touch(LIMIT30, PACE40), list(LIMIT30)},
                        {v461(), phone().put(WORLD_TXT, "traffic=0\n"), list(WORLD_TXT)},
                        {v461(), phone().put(WORLD_TXT, "traffic=50\n"), list(WORLD_TXT)},
                        {all(), phone().put(RENDER_TXT, "100\n"), list(RENDER_TXT)},
                        {v461(), phone().touch("private-trace/diag-wl-test-min"), list("private-trace/diag-wl-test-min")},
                        {v669(), phone().touch("private-trace/diag-wl-lod-unlock"), list("private-trace/diag-wl-lod-unlock")},
                        {v461(), phone().put(STOPS, "0.25"), list(STOPS)},
                        {v461(), phone(), list()}, {v462(), phone().touch(GSR_SWITCH), list()}, {v462(), phone().put(STOPS, "1.5\n"), list()}};
                for (Object[] c : cases) {
                    SettingsCoreTestKit.MemoryFiles disk = (SettingsCoreTestKit.MemoryFiles) c[1];
                    Settings shown = imported((Capabilities) c[0], disk);
                    same(c[2], RuntimeMapping.drift(shown, (Capabilities) c[0], disk), "files a Save would change, state " + state(disk).keySet());
                }
            }
        });

        test("drift names exactly the files that differ; a switch file's content does not matter", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                Capabilities c = all();
                Settings settings = defaults().with("frameCap", SettingsSchema.CAP_40).with("fsrSharpnessTenths", 5).with("worldTraffic", 50).with("fpsOverlay", 1);
                SettingsCoreTestKit.MemoryFiles disk = phone();
                disk.apply(plan(settings, c, disk));
                same(new ArrayList<String>(), RuntimeMapping.drift(settings, c, disk), "nothing differs after applying the plan");
                disk.put(SHARPNESS_TXT, "0.6\n").put(PACE40, "x");
                disk.files.remove(WORLD_TXT);
                disk.files.remove(OVERLAY);
                disk.touch("private-trace/diag-wl-test");
                same(list(SHARPNESS_TXT, WORLD_TXT, OVERLAY, "private-trace/diag-wl-test"), RuntimeMapping.drift(settings, c, disk), "four differences");
                Settings thirty = defaults().with("frameCap", SettingsSchema.CAP_30);
                SettingsCoreTestKit.MemoryFiles old = files();
                old.apply(plan(thirty, v458(), old)).touch(LIMIT60);
                same(list(LIMIT60), RuntimeMapping.drift(thirty, v458(), old), "30 fps by the limiter, and a script put the 60 fps switch back");
            }
        });

        SettingsCoreTestKit.end();
    }
}
