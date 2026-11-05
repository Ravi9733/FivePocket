package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SettingsCoreTestKit.ALL_NAMES;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.V458_NAMES;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.age;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.allLanes;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.ascii;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.bytes;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.caps;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.check;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.differences;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.phoneFiles;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.put;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.same;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.sameBytes;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.snapshot;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.tempDir;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.test;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.throwsA;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.v458;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.v461;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.v462;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

/**
 * Host test of SettingsStore on real directories that mimic the phone's files/ folder: what a load and a save may and
 * may not touch. A snapshot records every file's size, SHA-256 and modification time, and every file of the fixture is
 * given an old modification time first, so "unchanged" below means byte for byte and not rewritten. Run with java -ea.
 */
public final class SettingsStoreTest {
    private static final String JSON = "settings/settings.json";
    private static final String DEFAULTS_TEXT = "{\"version\":1,\"frameCap\":\"60\",\"scaler\":\"fsr1\",\"fsrSharpnessTenths\":2,"
            + "\"renderScalePercent\":100,\"frameGeneration\":\"off\",\"fpsOverlay\":false,\"worldTraffic\":100,"
            + "\"worldPedestrians\":100,\"worldParkedCars\":100,\"worldScenarioPedestrians\":100,\"worldDrawDistance\":100,"
            + "\"inputUnbuffered\":false}\n";
    /** What a Save of the defaults records under v458 and under the all-lanes build: only the rows that build can change. */
    private static final String V458_DEFAULTS_TEXT = "{\"version\":1,\"frameCap\":\"60\",\"scaler\":\"fsr1\",\"fsrSharpnessTenths\":2,"
            + "\"fpsOverlay\":false,\"inputUnbuffered\":false}\n";
    private static final String ALL_DEFAULTS_TEXT = DEFAULTS_TEXT.replace("\"frameGeneration\":\"off\",", "");
    /** Design section 3.2: the only files a save may create, replace or remove. */
    private static final List<String> OWNED = Arrays.asList("settings/settings.json", "settings/capabilities.txt", "settings/scaler.txt",
            "settings/fsr-sharpness.txt", "settings/render-scale.txt", "settings/world-load.txt", "private-trace/diag-v280-limit30",
            "private-trace/diag-v280-limit60", "private-trace/diag-pace-40", "private-trace/diag-pace-30", "private-trace/diag-pace-24",
            "private-trace/diag-pace-auto", "private-trace/diag-fg-between", "private-trace/diag-fg-ahead",
            "private-trace/diag-v450-scale-fsr1", "private-trace/diag-v409-scale-cubic",
            "private-trace/diag-v409-scale-linear", "private-trace/diag-v450-fsr1-stops",
            "private-trace/diag-fps-overlay", "private-trace/diag-lt2-unbuffered",
            // only ever removed: leftover test files of the world sliders
            "private-trace/diag-wl-test", "private-trace/diag-wl-test-min", "private-trace/diag-wl-test-vehicles",
            "private-trace/diag-wl-test-peds", "private-trace/diag-wl-test-parked", "private-trace/diag-wl-test-scenario",
            "private-trace/diag-wl-test-lod", "private-trace/diag-wl-lod-unlock");
    /** The phone-like fixture has this developer switch standing; every load names it. */
    private static final String STANDING = SettingsStore.WARNING_STANDING + "diag-v450-fsr1-no-rcas (FSR sharpness has no effect).";
    private static final List<String> ONLY_SETTINGS_JSON_ADDED = Arrays.asList("added settings", "added " + JSON);

    private static final SettingsStore.GameRunning NOT_RUNNING = new SettingsStore.GameRunning() {
        @Override public boolean check() { return false; }
    };
    private static final SettingsStore.GameRunning RUNNING = new SettingsStore.GameRunning() {
        @Override public boolean check() { return true; }
    };

    private static Settings defaults() {
        return Settings.defaults();
    }

    private static File phone(String... extraSwitchFiles) throws IOException {
        return phoneFiles(tempDir(), V458_NAMES, extraSwitchFiles);
    }

    private static SettingsStore store(File files) {
        return new SettingsStore(files, NOT_RUNNING);
    }

    private static SettingsStore.SaveResult saved(File files, Settings settings, Capabilities capabilities) {
        SettingsStore.SaveResult result = store(files).save(settings, capabilities);
        check(result.ok, "save failed: " + result.error);
        same(null, result.error, "error of a successful save");
        check(!result.refusedGameRunning, "not refused");
        return result;
    }

    private static String text(File files, String path) throws IOException {
        byte[] content = bytes(files, path);
        return content == null ? null : new String(content, StandardCharsets.UTF_8);
    }

    /** Every file that is not a setting file must be exactly as before; nothing but setting files may have appeared. */
    private static void onlySettingFilesDiffer(Map<String, String> before, Map<String, String> after, String when) {
        for (String line : differences(before, after)) {
            String path = line.substring(line.indexOf(' ') + 1);
            check(path.equals("settings") || OWNED.contains(path), when + ": a file that is not a setting file was touched: " + line);
        }
        for (String path : after.keySet()) check(!path.endsWith(".tmp"), when + ": a temporary file was left: " + path);
    }

    public static void main(String[] args) {
        SettingsCoreTestKit.begin("SettingsStoreTest");

        test("load without settings.json: shows what the game's files stand for and writes nothing", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                File files = phone();
                Map<String, String> before = snapshot(files);
                SettingsStore.Loaded loaded = store(files).load(v458());
                check(!loaded.fromFile, "no settings.json: not from a file");
                same(defaults(), loaded.settings, "the phone's standing files are the defaults");
                same(Arrays.asList(STANDING), loaded.warnings, "warnings: only the developer switch of the fixture");
                same(new ArrayList<String>(), loaded.drift, "drift");
                same(new ArrayList<String>(), differences(before, snapshot(files)), "what load changed");
            }
        });

        test("saving the defaults on the phone-like directory changes no game file (bytes and modification times)", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                for (Capabilities capabilities : new Capabilities[] {v458(), allLanes()}) {
                    File files = phone();
                    Map<String, String> before = snapshot(files);
                    SettingsStore.SaveResult result = saved(files, defaults(), capabilities);
                    same(Arrays.asList(JSON), result.written, "written");
                    same(new ArrayList<String>(), result.removed, "removed");
                    same(ONLY_SETTINGS_JSON_ADDED, differences(before, snapshot(files)), "what the save changed");
                    same(capabilities.reads("settings/world-load.txt") ? ALL_DEFAULTS_TEXT : V458_DEFAULTS_TEXT, text(files, JSON), "settings.json");
                }
            }
        });

        test("standing switch files that happen to hold bytes are left exactly as they are", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                File files = phone();
                // as if they had been made with "echo 1 >" instead of "touch"
                put(files, "private-trace/diag-v450-scale-fsr1", "1\n");
                put(files, "private-trace/diag-v409-scale-cubic", "on");
                put(files, "private-trace/diag-v280-limit60", "1\n");
                age(files);
                Map<String, String> before = snapshot(files);
                same(defaults(), store(files).load(v458()).settings, "they still stand for the defaults");
                saved(files, defaults(), v458());
                same(ONLY_SETTINGS_JSON_ADDED, differences(before, snapshot(files)), "what the save changed");
                same(new ArrayList<String>(), store(files).load(v458()).drift, "and no drift is reported");
            }
        });

        test("first open, then Save without a change: no game file changes, also with the frame-rate counter standing", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                File files = phone("diag-fps-overlay");
                Map<String, String> before = snapshot(files);
                SettingsStore.Loaded loaded = store(files).load(v458());
                same(defaults().with("fpsOverlay", 1), loaded.settings, "the standing counter file is shown as On");
                saved(files, loaded.settings, v458());
                same(ONLY_SETTINGS_JSON_ADDED, differences(before, snapshot(files)), "what the save changed");
                // "Restore original" is the user's own choice to switch the counter off: that one file goes, nothing else
                SettingsStore.SaveResult restored = saved(files, defaults(), v458());
                same(Arrays.asList("private-trace/diag-fps-overlay"), restored.removed, "removed by Restore original");
                same(Arrays.asList("removed private-trace/diag-fps-overlay", "added settings", "added " + JSON), differences(before, snapshot(files)), "after Restore original");
            }
        });

        test("the whole first visit (capability scan, load, save of the defaults) adds two files and changes none", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                File files = phone();
                Map<String, String> before = snapshot(files);
                Capabilities capabilities = CapabilityScanner.load(files, true);
                SettingsStore.Loaded loaded = store(files).load(capabilities);
                saved(files, loaded.settings, capabilities);
                same(Arrays.asList("added settings", "added settings/capabilities.txt", "added " + JSON), differences(before, snapshot(files)), "what the visit changed");
                same(V458_DEFAULTS_TEXT, text(files, JSON), "settings.json");
            }
        });

        test("every value of every setting, saved one after the other: only setting files are ever touched, and Restore original brings the files back", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                // v458, a build with every lane, and a build that announces frame generation (so its two switch files are walked too).
                String[][] libraries = {V458_NAMES, ALL_NAMES, SetTestKit.FRAMEGEN_NAMES};
                String[] labels = {"v458", "all lanes", "frame generation"};
                for (int build = 0; build < libraries.length; build++) {
                    final boolean full = build > 0;
                    Capabilities capabilities = caps(libraries[build]);
                    File files = phoneFiles(tempDir(), libraries[build]);
                    Map<String, String> before = snapshot(files);
                    int saves = 0;
                    for (SettingSpec spec : SettingsSchema.all()) {
                        for (int value = spec.min; value <= spec.max; value += spec.step) {
                            Settings settings = defaults().with(spec.key, value);
                            saved(files, settings, capabilities);
                            String when = labels[build] + ", after saving " + spec.key + "=" + value;
                            onlySettingFilesDiffer(before, snapshot(files), when);
                            SettingsStore.Loaded loaded = store(files).load(capabilities);
                            check(loaded.fromFile, when + ": the saved file is read back");
                            same(settings, loaded.settings, when + ": what load returns");
                            same(new ArrayList<String>(), loaded.drift, when + ": drift right after the save");
                            saves++;
                        }
                    }
                    check(saves == 6 + 4 + 41 + 7 + 3 + 2 + 4 * 16 + 7 + 2, "the loop ran (" + saves + " saves)");
                    saved(files, defaults(), capabilities);
                    // All lanes: the listing is the one from before the walk. v458 differs in two documented ways:
                    // - the scaler was chosen through the two switch files, so they were removed and are now back as new
                    //   empty files ("changed" = same content, newer time);
                    // - the 30 fps cap was made by the game's limiter, which needs the standing 60 fps switch gone; that
                    //   switch is not put back (it makes no difference to a build newer than v307).
                    List<String> expected = full ? ONLY_SETTINGS_JSON_ADDED
                            : Arrays.asList("removed private-trace/diag-v280-limit60", "changed private-trace/diag-v409-scale-cubic",
                                    "changed private-trace/diag-v450-scale-fsr1", "added settings", "added " + JSON);
                    same(expected, differences(before, snapshot(files)), labels[build] + ": after Restore original");
                    sameBytes(new byte[0], bytes(files, "private-trace/diag-v409-scale-cubic"), "the cubic switch file is back, empty");
                    sameBytes(new byte[0], bytes(files, "private-trace/diag-v450-scale-fsr1"), "the FSR 1 switch file is back, empty");
                }
            }
        });

        test("frame generation in a build that announces it: Save writes the chosen order's switch under the 30 fps cap and nothing else; the dry run's files stay", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                // The phone's switch files plus what a developer left from the timing dry run. The library is a stand-in
                // that holds the names of the announced build; the scan below is the app's own.
                File files = phoneFiles(tempDir(), SetTestKit.FRAMEGEN_NAMES, "diag-fg-pair-ahead", "diag-fg-hold");
                Capabilities capabilities = CapabilityScanner.load(files, true);
                check(capabilities.valueSupported("frameGeneration", 1) && capabilities.valueSupported("frameGeneration", 2), "the scan finds the marker and both switch names");
                Settings shown = store(files).load(capabilities).settings;
                same(0, shown.get("frameGeneration"), "first visit: Off");
                Map<String, String> before = snapshot(files);
                same(Arrays.asList(JSON), saved(files, shown, capabilities).written, "a Save of what the screen shows writes the saved settings only");
                same(Arrays.asList("added " + JSON), differences(before, snapshot(files)), "and no file the game reads");

                Settings between = shown.with("frameCap", SettingsSchema.CAP_30).with("frameGeneration", 1);
                SettingsStore.SaveResult on = saved(files, between, capabilities);
                same(Arrays.asList(JSON, "private-trace/diag-pace-30", "private-trace/diag-fg-between"), on.written, "in-between frames under 30 fps: written");
                same(new ArrayList<String>(), on.removed, "nothing removed");
                sameBytes(new byte[0], bytes(files, "private-trace/diag-fg-between"), "the switch file is empty");
                check(!new File(files, "private-trace/diag-fg-ahead").exists(), "the other order's switch does not exist");
                SettingsStore.Loaded loaded = store(files).load(capabilities);
                same(between, loaded.settings, "read back");
                same(new ArrayList<String>(), loaded.drift, "nothing differs after the save");

                SettingsStore.SaveResult ahead = saved(files, between.with("frameGeneration", 2), capabilities);
                same(Arrays.asList(JSON, "private-trace/diag-fg-ahead"), ahead.written, "predicted frames: its own switch");
                same(Arrays.asList("private-trace/diag-fg-between"), ahead.removed, "and the other order's switch goes");

                // Another cap: the generator would make no picture, so no switch stands; the chosen order is kept in settings.json.
                Settings forty = between.with("frameGeneration", 2).with("frameCap", SettingsSchema.CAP_40);
                SettingsStore.SaveResult capped = saved(files, forty, capabilities);
                same(Arrays.asList("private-trace/diag-pace-30", "private-trace/diag-fg-ahead"), capped.removed, "40 fps: the 30 fps cap file and the switch go");
                check(text(files, JSON).contains("\"frameGeneration\":\"ahead\""), "the chosen order stays saved: " + text(files, JSON));
                same(forty, store(files).load(capabilities).settings, "and is read back");
                same(new ArrayList<String>(), store(files).load(capabilities).drift, "with nothing differing");
                same(Arrays.asList(JSON, "private-trace/diag-pace-30", "private-trace/diag-fg-ahead"),
                        saved(files, forty.with("frameCap", SettingsSchema.CAP_30), capabilities).written, "back at 30 fps the switch returns");

                saved(files, shown, capabilities);
                same(Arrays.asList("added " + JSON), differences(before, snapshot(files)),
                        "back at what the first visit showed: every file the game reads is as it was, byte and time");
                for (String developer : new String[] {"diag-fg-pair-ahead", "diag-fg-hold"}) {
                    same(before.get("private-trace/" + developer), snapshot(files).get("private-trace/" + developer), developer + " was never touched");
                }
            }
        });

        test("developer switches a row cannot show - a frame-generation switch under a grey row, the render-resolution stage A switch - are named in a warning and left alone", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                // A developer build of the generator: it reads the switch but does not announce itself, so the row is grey.
                String[] unannounced = SettingsCoreTestKit.minus(SetTestKit.FRAMEGEN_NAMES, SetTestKit.FRAMEGEN_MARKER);
                File files = phoneFiles(tempDir(), unannounced, "diag-fg-between");
                check(new File(files, "private-trace/diag-v450-fsr1-no-rcas").delete(), "test setup: no other developer switch");
                Map<String, String> before = snapshot(files);
                same(Arrays.asList(SettingsStore.WARNING_STANDING + "diag-fg-between (frame generation is switched on outside this screen)."),
                        store(files).load(caps(unannounced)).warnings, "the warning");
                saved(files, defaults().with("frameCap", SettingsSchema.CAP_30).with("frameGeneration", 2), caps(unannounced));
                same(before.get("private-trace/diag-fg-between"), snapshot(files).get("private-trace/diag-fg-between"), "a Save leaves the developer's switch as it is");
                // Where the build does not read the file (the phone's v464) it does nothing: no warning. Where the row is
                // live it is the row's own file: the row shows it, so no warning either.
                same(new ArrayList<String>(), store(files).load(SetTestKit.v464WithDryRun()).warnings, "v464 does not read it");
                same(null, SettingsStore.standingEffect("diag-fg-between", SetTestKit.framegen()), "a live row accounts for its own switch");
                same(null, SettingsStore.standingEffect("diag-fg-betweenx", caps(unannounced)), "a look-alike name is not the switch");

                // The game's own A/B switch for render resolution: under v464 it makes the row's modes untrue ("Native"
                // would go through 1280x720), so it is named; under v463, which has no other way, it means nothing.
                File stageA = phoneFiles(tempDir(), SettingsCoreTestKit.V464_NAMES, "diag-rs-stage-a");
                check(new File(stageA, "private-trace/diag-v450-fsr1-no-rcas").delete(), "test setup: no other developer switch");
                Map<String, String> untouched = snapshot(stageA);
                same(Arrays.asList(SettingsStore.WARNING_STANDING + "diag-rs-stage-a (every render size is shown through a 1280x720 picture, whatever Render Resolution says)."),
                        store(stageA).load(SettingsCoreTestKit.v464()).warnings, "v464: the warning");
                same(new ArrayList<String>(), store(stageA).load(SettingsCoreTestKit.v463()).warnings, "v463: no warning");
                saved(stageA, defaults().with("renderScalePercent", SettingsSchema.RENDER_SCALE_150), SettingsCoreTestKit.v464());
                same(untouched.get("private-trace/diag-rs-stage-a"), snapshot(stageA).get("private-trace/diag-rs-stage-a"), "a Save leaves the switch as it is");
            }
        });

        test("a save while the game runs is refused and writes nothing", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                File files = phone();
                Map<String, String> before = snapshot(files);
                SettingsStore.SaveResult result = new SettingsStore(files, RUNNING).save(defaults().with("fpsOverlay", 1).with("scaler", 3), v458());
                check(!result.ok && result.refusedGameRunning, "refused");
                same("The game is running. Close it, then save.", result.error, "the reason");
                check(result.written.isEmpty() && result.removed.isEmpty(), "nothing reported as written or removed");
                same(new ArrayList<String>(), differences(before, snapshot(files)), "what the refused save changed");
            }
        });

        test("a second identical save writes nothing: no file is rewritten, every modification time stays", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                File files = phone();
                Settings settings = defaults().with("fpsOverlay", 1).with("fsrSharpnessTenths", 5).with("frameCap", 3);
                SettingsStore.SaveResult first = saved(files, settings, v458());
                check(first.written.size() >= 4, "the first save wrote the files: " + first.written);
                age(files);
                Map<String, String> before = snapshot(files);
                SettingsStore.SaveResult second = saved(files, settings, v458());
                same(new ArrayList<String>(), second.written, "written by the second save");
                same(new ArrayList<String>(), second.removed, "removed by the second save");
                same(new ArrayList<String>(), differences(before, snapshot(files)), "what the second save changed");
            }
        });

        test("save reports what it wrote and removed, in the order of the fixed list", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                File files = phone();
                SettingsStore.SaveResult bicubic = saved(files, defaults().with("scaler", 2), v458());
                same(Arrays.asList(JSON), bicubic.written, "bicubic: written");
                same(Arrays.asList("private-trace/diag-v450-scale-fsr1"), bicubic.removed, "bicubic: removed");
                SettingsStore.SaveResult thirty = saved(files, defaults().with("frameCap", 3).with("fsrSharpnessTenths", 5), v458());
                same(Arrays.asList(JSON, "private-trace/diag-v280-limit30", "private-trace/diag-v450-scale-fsr1", "private-trace/diag-v450-fsr1-stops"), thirty.written, "30 fps: written");
                same(Arrays.asList("private-trace/diag-v280-limit60"), thirty.removed, "30 fps: removed");
                sameBytes(new byte[0], bytes(files, "private-trace/diag-v280-limit30"), "a switch file is empty");
                sameBytes(ascii("0.5\n"), bytes(files, "private-trace/diag-v450-fsr1-stops"), "the sharpness file");
            }
        });

        test("a value the installed build cannot honour is kept in settings.json and written to the game as the default", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                File files = phone();
                Map<String, String> before = snapshot(files);
                Settings wish = defaults().with("frameCap", 2).with("worldTraffic", 35);
                saved(files, wish, v458());
                same(ONLY_SETTINGS_JSON_ADDED, differences(before, snapshot(files)), "no game file changes for values v458 does not have");
                check(text(files, JSON).contains("\"frameCap\":\"40\"") && text(files, JSON).contains("\"worldTraffic\":35"), "the wish is kept: " + text(files, JSON));
                SettingsStore.Loaded loaded = store(files).load(v458());
                same(wish, loaded.settings, "load returns the wish");
                same(new ArrayList<String>(), loaded.drift, "and reports no drift");
                // when the build learns the values, the same saved file plans the real files
                same(Arrays.asList("settings/world-load.txt", "private-trace/diag-pace-40"), store(files).load(allLanes()).drift, "drift once a newer build is installed");
            }
        });

        test("drift: a file changed behind the store's back is named, and saving again repairs it", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                File files = phone();
                Settings settings = defaults().with("fpsOverlay", 1);
                saved(files, settings, v458());
                check(new File(files, "private-trace/diag-fps-overlay").delete(), "test setup: a script removes the counter switch");
                check(new File(files, "private-trace/diag-v280-limit60").delete(), "test setup: and the 60 switch, and leaves the 30 switch");
                put(files, "private-trace/diag-v280-limit30", "");
                put(files, "private-trace/diag-oc-queries-2", ""); // not a setting file: no business of drift
                SettingsStore.Loaded loaded = store(files).load(v458());
                same(settings, loaded.settings, "the saved settings are still shown");
                same(Arrays.asList("private-trace/diag-v280-limit30", "private-trace/diag-fps-overlay"), loaded.drift, "drift");
                SettingsStore.SaveResult again = saved(files, loaded.settings, v458());
                same(Arrays.asList("private-trace/diag-fps-overlay"), again.written, "Apply again: written");
                same(Arrays.asList("private-trace/diag-v280-limit30"), again.removed, "Apply again: removed");
                same(new ArrayList<String>(), store(files).load(v458()).drift, "drift after Apply again");
            }
        });

        test("settings.json that is damaged, empty or from a newer app version: load falls back and leaves the file alone", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                String newer = DEFAULTS_TEXT.replace("\"version\":1", "\"version\":2").replace("\"fpsOverlay\":false", "\"fpsOverlay\":true,\"someNewSetting\":7");
                String[] unusable = {"garbage, not JSON", "", DEFAULTS_TEXT.substring(0, DEFAULTS_TEXT.length() / 2), newer, "[]"};
                for (String content : unusable) {
                    String what = "settings.json holding <" + (content.length() > 24 ? content.substring(0, 24) + "..." : content) + ">";
                    File files = phone();
                    put(files, JSON, content);
                    age(files);
                    Map<String, String> before = snapshot(files);
                    SettingsStore.Loaded loaded = store(files).load(v458());
                    check(!loaded.fromFile, what + ": not used");
                    same(defaults(), loaded.settings, what + ": the defaults (what the game's files stand for)");
                    same(2, loaded.warnings.size(), what + ": the developer switch and one warning about the file " + loaded.warnings);
                    same(STANDING, loaded.warnings.get(0), what + ": the developer switch comes first");
                    same(new ArrayList<String>(), loaded.drift, what + ": no drift is claimed");
                    same(new ArrayList<String>(), differences(before, snapshot(files)), what + ": load must not delete or change anything");
                    same(content, text(files, JSON), what + ": the file is still there, byte for byte");
                }
                // the fallback is what the game's files say, not blindly the defaults: a standing counter file stays visible
                File files = phone("diag-fps-overlay");
                put(files, JSON, "garbage");
                same(defaults().with("fpsOverlay", 1), store(files).load(v458()).settings, "fallback with a standing counter file");
                // the user's next Save replaces the unusable file
                saved(files, defaults().with("fpsOverlay", 1), v458());
                check(store(files).load(v458()).fromFile, "after a save the file is valid again");
            }
        });

        test("a crash between the temporary file and the rename leaves the old settings in force", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                File files = phone();
                Settings old = defaults().with("fpsOverlay", 1);
                Settings next = defaults().with("scaler", 2).with("fsrSharpnessTenths", 7);
                saved(files, old, v458());
                age(files);
                Map<String, String> before = snapshot(files);
                // the save of 'next' gets as far as the complete temporary file of settings.json, then the process dies
                AtomicFiles.writeTemp(new File(files, JSON), ascii(SettingsJson.encode(next)));
                same(Arrays.asList("added settings/settings.json.tmp"), differences(before, snapshot(files)), "what the interrupted save left");
                SettingsStore.Loaded loaded = store(files).load(v458());
                check(loaded.fromFile, "the old file is read");
                same(old, loaded.settings, "the old settings are in force");
                same(new ArrayList<String>(), loaded.drift, "the game's files still match them");
                // the same with a temporary file that was only partly written
                put(files, JSON + ".tmp", SettingsJson.encode(next).substring(0, 40));
                same(old, store(files).load(v458()).settings, "a partial temporary file changes nothing either");
                // the next save goes through and leaves no temporary file
                saved(files, next, v458());
                same(next, store(files).load(v458()).settings, "after the next save");
                check(!new File(files, JSON + ".tmp").exists(), "the stray temporary file is gone");
            }
        });

        test("a failure in the middle is reported, stops the save, harms nothing, and saving again finishes the job", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                File files = phone();
                // a folder stands where the counter switch file should be written
                put(files, "private-trace/diag-fps-overlay/inside", "kept");
                age(files);
                Map<String, String> before = snapshot(files);
                Settings settings = defaults().with("fpsOverlay", 1).with("inputUnbuffered", 1);
                SettingsStore.SaveResult failed = store(files).save(settings, v458());
                check(!failed.ok && !failed.refusedGameRunning, "the save fails");
                check(failed.error != null && failed.error.contains("diag-fps-overlay"), "the error names the file: " + failed.error);
                same(Arrays.asList(JSON), failed.written, "written before the failure");
                same(Arrays.asList("added settings", "added " + JSON), differences(before, snapshot(files)),
                        "the folder in the way is untouched and the file behind it in the list (diag-lt2-unbuffered) was not written");
                same(settings, store(files).load(v458()).settings, "settings.json already holds the new choice");
                // (to the game, which only asks whether the name exists, the folder already counts as the counter switch,
                // so drift names only the file that is really missing)
                same(Arrays.asList("private-trace/diag-lt2-unbuffered"), store(files).load(v458()).drift, "drift shows what is missing");
                // the obstacle is cleared by hand; saving again completes
                check(new File(files, "private-trace/diag-fps-overlay/inside").delete() && new File(files, "private-trace/diag-fps-overlay").delete(), "test setup");
                SettingsStore.SaveResult again = saved(files, settings, v458());
                same(Arrays.asList("private-trace/diag-fps-overlay", "private-trace/diag-lt2-unbuffered"), again.written, "written by the second save");
                // a folder standing where a file must be absent is not removed either
                put(files, "private-trace/diag-wl-test-min/inside", "kept");
                SettingsStore.SaveResult blocked = store(files).save(settings, v458());
                check(!blocked.ok && blocked.error.contains("diag-wl-test-min"), "a folder is never removed: " + blocked.error);
                sameBytes(ascii("kept"), bytes(files, "private-trace/diag-wl-test-min/inside"), "the folder's content");
            }
        });

        test("the gate: a path that is not on the fixed list is never written or removed", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                final File files = phone();
                saved(files, defaults(), v458());
                age(files);
                Map<String, String> before = snapshot(files);
                String[] foreign = {"private-trace/diag-cexec-bools", "saves/slot0", "private-trace/../saves/slot0", "settings/../saves/slot0",
                        "private-trace/private-trace-live.so", "private-trace/run-0123456789abcdef0123456789abcdef/arguments.bin",
                        "settings/settings.json.tmp", "settings/capabilities-override.txt", "private-trace/diag-fps-overlay-old", "new-file", ""};
                for (final String path : foreign) {
                    for (final byte[] content : new byte[][] {null, ascii("overwritten")}) {
                        final LinkedHashMap<String, byte[]> plan = new LinkedHashMap<String, byte[]>();
                        plan.put(path, content);
                        throwsA(IOException.class, (content == null ? "removing " : "writing ") + "<" + path + ">", new SettingsCoreTestKit.Case() {
                            @Override public void run() throws IOException { store(files).apply(plan, new ArrayList<String>(), new ArrayList<String>()); }
                        });
                    }
                    throwsA(IOException.class, "ownedTarget of <" + path + ">", new SettingsCoreTestKit.Case() {
                        @Override public void run() throws IOException { SettingsStore.ownedTarget(files, path); }
                    });
                }
                same(new ArrayList<String>(), differences(before, snapshot(files)), "after all refused attempts");
                // the same call does work for a file of the list
                LinkedHashMap<String, byte[]> allowed = new LinkedHashMap<String, byte[]>();
                allowed.put("private-trace/diag-fps-overlay", new byte[0]);
                store(files).apply(allowed, new ArrayList<String>(), new ArrayList<String>());
                same(Arrays.asList("added private-trace/diag-fps-overlay"), differences(before, snapshot(files)), "a file of the list is written");
            }
        });

        test("symbolic links: nothing is written or removed through one, above all not a save", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                // 1. the settings folder is a link to the saves folder
                File files = phone();
                Files.createSymbolicLink(new File(files, "settings").toPath(), new File(files, "saves").toPath());
                Map<String, String> before = snapshot(files);
                SettingsStore.SaveResult result = store(files).save(defaults().with("scaler", 2), v458());
                check(!result.ok && result.error.contains("symbolic link"), "refused: " + result.error);
                same(new ArrayList<String>(), differences(before, snapshot(files)), "settings folder is a link: nothing changes");

                // 2. a setting file's name is a link to a save; the plan wants that name absent (defaults) or present (counter on)
                files = phone();
                Files.createSymbolicLink(new File(files, "private-trace/diag-fps-overlay").toPath(), new File(files, "saves/slot0").toPath());
                before = snapshot(files);
                for (Settings settings : new Settings[] {defaults(), defaults().with("fpsOverlay", 1)}) {
                    result = store(files).save(settings, v458());
                    check(!result.ok && result.error.contains("diag-fps-overlay"), "refused: " + result.error);
                    onlySettingFilesDiffer(before, snapshot(files), "setting file is a link");
                    same(before.get("saves/slot0"), snapshot(files).get("saves/slot0"), "the save behind the link");
                    same(before.get("private-trace/diag-fps-overlay"), snapshot(files).get("private-trace/diag-fps-overlay"), "the link itself");
                }

                // 3. the temporary name of a setting file is a link to a save
                files = phone();
                Files.createSymbolicLink(new File(files, "private-trace/diag-fps-overlay.tmp").toPath(), new File(files, "saves/slot0").toPath());
                before = snapshot(files);
                result = store(files).save(defaults().with("fpsOverlay", 1), v458());
                check(!result.ok && result.error.contains("diag-fps-overlay"), "refused: " + result.error);
                same(before.get("saves/slot0"), snapshot(files).get("saves/slot0"), "the save behind the temporary name");
            }
        });

        test("the files directory itself may be reached through a symbolic link (as on Android)", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                File files = phone();
                File link = new File(files.getParentFile(), "files-link");
                Files.createSymbolicLink(link.toPath(), files.toPath());
                SettingsStore.SaveResult result = new SettingsStore(link, NOT_RUNNING).save(defaults().with("fpsOverlay", 1), v458());
                check(result.ok, "save through the linked path: " + result.error);
                check(new File(files, "private-trace/diag-fps-overlay").isFile(), "the switch file is in the real directory");
                same(defaults().with("fpsOverlay", 1), new SettingsStore(link, NOT_RUNNING).load(v458()).settings, "load through the linked path");
            }
        });

        test("an import that cannot be exact is announced, and still nothing is written", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                File files = phone("diag-v409-scale-linear");
                check(new File(files, "private-trace/diag-v450-scale-fsr1").delete() && new File(files, "private-trace/diag-v409-scale-cubic").delete(), "test setup: only the bilinear switch stands");
                Map<String, String> before = snapshot(files);
                SettingsStore.Loaded loaded = store(files).load(v458());
                same(defaults().with("scaler", 3), loaded.settings, "bilinear is not offered and is shown as nearest");
                same(2, loaded.warnings.size(), "the rewrite warning and the developer switch: " + loaded.warnings);
                check(loaded.warnings.get(0).startsWith("Saving will also rewrite") && loaded.warnings.get(0).contains("diag-v409-scale-linear"),
                        "the FIRST warning names the file a save would change: " + loaded.warnings);
                same(STANDING, loaded.warnings.get(1), "the developer switch comes second");
                // Also with a damaged settings.json the rewrite warning stays first: it is the line the screen shows.
                put(files, JSON, "garbage, not JSON");
                List<String> withDamage = store(files).load(v458()).warnings;
                same(3, withDamage.size(), "three warnings: " + withDamage);
                check(withDamage.get(0).startsWith("Saving will also rewrite"), "the rewrite warning is first: " + withDamage);
                check(new File(files, JSON).delete() && new File(files, "settings").delete(), "test cleanup");
                same(new ArrayList<String>(), differences(before, snapshot(files)), "what load changed");
            }
        });

        test("save creates the settings folder but never the game's folder, and never throws", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                File files = new File(tempDir(), "files");
                check(files.mkdir(), "test setup: an empty files directory (no game installed)");
                SettingsStore.SaveResult result = store(files).save(defaults(), v458());
                check(!result.ok && result.error.contains("not installed"), "no game folder: the save is refused and says so: " + result.error);
                check(!result.error.contains("private-trace") && !result.error.contains("folder"), "in words a player knows, without a folder's name: " + result.error);
                check(!result.error.startsWith("Could not save"), "the store gives the reason only; the screen adds \"Could not save: \": " + result.error);
                same("[]", new TreeMap<String, String>(snapshot(files)).keySet().toString(), "nothing was written: not even settings.json, which would be a half-applied save");

                SettingsStore.SaveResult noSettings = store(files).save(null, v458());
                check(!noSettings.ok && noSettings.error != null, "null settings: an error result, not an exception");
                SettingsStore.SaveResult noCapabilities = store(files).save(defaults(), null);
                check(!noCapabilities.ok && noCapabilities.error != null, "null capabilities: an error result, not an exception");
                File phone = phone();
                Map<String, String> before = snapshot(phone);
                SettingsStore.SaveResult broken = new SettingsStore(phone, new SettingsStore.GameRunning() {
                    @Override public boolean check() { throw new IllegalStateException("cannot tell"); }
                }).save(defaults().with("fpsOverlay", 1), v458());
                check(!broken.ok, "when it cannot be told whether the game runs, nothing is saved");
                same(new ArrayList<String>(), differences(before, snapshot(phone)), "and nothing is written");
            }
        });

        test("developer switch files that overrule a row are named in a warning, and never touched", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                File files = phone("diag-pace-lowlat", "diag-pace-400", "diag-fg-pair-ahead", "diag-v460-scale-gsr1", "diag-fg-pair");
                Map<String, String> before = snapshot(files);
                List<String> warnings = store(files).load(v461()).warnings;
                same(Arrays.asList(SettingsStore.WARNING_STANDING + "diag-fg-pair-ahead (a frame-pair timing test runs under the 30 fps cap), "
                        + "diag-pace-lowlat (frame pacing is changed), "
                        + "diag-v450-fsr1-no-rcas (FSR sharpness has no effect), "
                        + "diag-v460-scale-gsr1 (Snapdragon GSR 1 is the scaler, whatever the Scaler row says)."),
                        warnings, "the four overruling files, sorted, each with its effect; look-alike names are not listed");
                saved(files, defaults().with("fpsOverlay", 1), v461());
                List<String> afterSave = store(files).load(v461()).warnings;
                same(warnings, afterSave, "a save does not make the warning go: the files still stand");
                for (String line : differences(before, snapshot(files))) {
                    check(!line.contains("diag-fg-") && !line.contains("diag-pace-") && !line.contains("no-rcas") && !line.contains("gsr1"),
                            "a developer switch file was touched: " + line);
                }
                // The GSR 1 switch is only a warning where nothing else accounts for it: v458 does not read it, and on
                // v462 the Scaler row shows it and scaler.txt overrules it.
                check(!store(files).load(v458()).warnings.toString().contains("gsr1"), "v458 does not read the GSR 1 switch: no warning about it");
                check(!store(files).load(v462()).warnings.toString().contains("whatever the Scaler row says"), "v462: the row accounts for the switch");
                // The stale-switch case of the GSR lane's review: FSR 1 is saved, a GSR 1 switch file was left behind, and
                // the build reads scaler.txt. The screen reports the difference, and applying the saved settings writes
                // the word fsr1, which overrules the switch; the switch file itself is not touched.
                same(Arrays.asList("settings/scaler.txt"), store(files).load(v462()).drift, "v462: the saved FSR 1 against the standing GSR 1 switch is reported");
                SettingsStore.SaveResult applied = saved(files, defaults().with("fpsOverlay", 1), v462());
                same(Arrays.asList("settings/scaler.txt"), applied.written, "v462: Apply again writes the word");
                sameBytes(ascii("fsr1\n"), bytes(files, "settings/scaler.txt"), "v462: scaler.txt");
                check(new File(files, "private-trace/diag-v460-scale-gsr1").isFile(), "the developer switch still stands");
                same(new ArrayList<String>(), store(files).load(v462()).drift, "v462: nothing differs afterwards");
                check(new File(files, "settings/scaler.txt").delete(), "test setup: back to the state without the word");
                for (String name : new String[] {"diag-v450-fsr1-no-rcas", "diag-pace-lowlat", "diag-fg-pair-ahead", "diag-v460-scale-gsr1"}) {
                    check(new File(files, "private-trace/" + name).delete(), "test setup: remove " + name);
                }
                same(new ArrayList<String>(), store(files).load(v461()).warnings, "none standing: no warning");
                same(null, SettingsStore.standingEffect("diag-fps-overlay", v461()), "a setting file is not a developer switch");
                same(null, SettingsStore.standingEffect("diag-wl-test", v461()), "a leftover world test file is removed by Save, not warned about");
            }
        });

        test("leftover test files of the world sliders: a Save removes them and nothing else, with any game library", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                String[] leftovers = {"diag-wl-test", "diag-wl-test-min", "diag-wl-test-vehicles", "diag-wl-test-peds", "diag-wl-test-parked",
                        "diag-wl-test-scenario", "diag-wl-test-lod", "diag-wl-lod-unlock"};
                // look-alikes that must stay: not one of the game's eight names
                File files = phoneFiles(tempDir(), SettingsCoreTestKit.V669_NAMES, SettingsCoreTestKit.plus(leftovers, "diag-wl-test-zero", "diag-wl-tes", "diag-wl-lod"));
                Map<String, String> before = snapshot(files);
                Capabilities capabilities = CapabilityScanner.load(files, true);
                SettingsStore.Loaded loaded = store(files).load(capabilities);
                check(loaded.warnings.get(0).startsWith("Saving will also rewrite game setting files") && loaded.warnings.get(0).contains("diag-wl-test-min"),
                        "the screen says beforehand that a Save removes them: " + loaded.warnings);
                same(Arrays.asList("added settings", "added settings/capabilities.txt"), differences(before, snapshot(files)), "loading wrote nothing but the scan cache");
                SettingsStore.SaveResult result = saved(files, loaded.settings, capabilities);
                ArrayList<String> expectedRemoved = new ArrayList<String>();
                ArrayList<String> expected = new ArrayList<String>();
                for (String name : leftovers) expectedRemoved.add("private-trace/" + name);
                java.util.Collections.sort(expectedRemoved);
                for (String path : expectedRemoved) expected.add("removed " + path);
                expected.addAll(Arrays.asList("added settings", "added settings/capabilities.txt", "added " + JSON));
                ArrayList<String> removed = new ArrayList<String>(result.removed);
                java.util.Collections.sort(removed);
                same(expectedRemoved, removed, "removed by the save");
                same(expected, differences(before, snapshot(files)), "everything that changed");
            }
        });

        test("today's phone (standing library v669, the recorded switch files): the first visit and a Save change no file the game reads", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                for (String[] library : new String[][] {SettingsCoreTestKit.V669_NAMES, V458_NAMES, SettingsCoreTestKit.V461_NAMES, SettingsCoreTestKit.V462_NAMES}) {
                    // the fixture's standing files are the phone's (FSR 1 + cubic switches, limit60) plus the counter switch
                    File files = phoneFiles(tempDir(), library, "diag-fps-overlay");
                    Map<String, String> before = snapshot(files);
                    Capabilities capabilities = CapabilityScanner.load(files, true);
                    SettingsStore.Loaded loaded = store(files).load(capabilities);
                    String build = "library with " + library.length + " names";
                    for (String warning : loaded.warnings) check(!warning.startsWith("Saving will also rewrite"), build + ": no rewrite warning: " + warning);
                    SettingsStore.SaveResult result = saved(files, loaded.settings, capabilities);
                    same(Arrays.asList(JSON), result.written, build + ": written by the save");
                    same(new ArrayList<String>(), result.removed, build + ": removed by the save");
                    same(Arrays.asList("added settings", "added settings/capabilities.txt", "added " + JSON), differences(before, snapshot(files)),
                            build + ": the whole visit (the app's own two files; diag-v280-limit60 and every game file as before, times included)");
                    // "Restore original" then Save: with the old library still nothing the game reads changes
                    if (library.length == 0) {
                        SettingsStore.SaveResult restored = saved(files, defaults(), capabilities);
                        same(new ArrayList<String>(), restored.removed, "v669: Restore original removes nothing");
                        same(Arrays.asList("added settings", "added settings/capabilities.txt", "added " + JSON), differences(before, snapshot(files)), "v669: after Restore original");
                    }
                }
            }
        });

        test("a Save under one game library, another live later: a row that was grey is not treated as chosen (review, major)", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                // The phone: library v669 (reads no setting file), the counter switch standing.
                File files = phoneFiles(tempDir(), SettingsCoreTestKit.V669_NAMES, "diag-fps-overlay");
                Map<String, String> phone = snapshot(files);
                Capabilities v669 = SettingsCoreTestKit.v669();
                // Ordinary use today: the one live row is switched on and saved, then off and saved.
                Settings shown = store(files).load(v669).settings;
                saved(files, shown.with("inputUnbuffered", 1), v669);
                saved(files, store(files).load(v669).settings.with("inputUnbuffered", 0), v669);
                same("{\"version\":1,\"inputUnbuffered\":false}\n", text(files, JSON), "settings.json records only the row that could be changed");
                same(ONLY_SETTINGS_JSON_ADDED, differences(phone, snapshot(files)), "after both saves");
                for (Capabilities newer : new Capabilities[] {v458(), v461(), v462()}) {
                    SettingsStore.Loaded loaded = store(files).load(newer);
                    check(loaded.fromFile, "the saved file is used");
                    same(1, loaded.settings.get("fpsOverlay"), "the counter row shows what the standing switch says: On");
                    same(new ArrayList<String>(), loaded.drift, "no file differs from the saved settings");
                    same(Arrays.asList(STANDING), loaded.warnings, "no warning about a missing row");
                    Map<String, String> before = snapshot(files);
                    // a Save with nothing edited (Start), under the newer library
                    SettingsStore.SaveResult nothingEdited = store(files).save(loaded.settings, newer);
                    same(new ArrayList<String>(), nothingEdited.removed, "Start with nothing edited removes nothing");
                    same(Arrays.asList(JSON), nothingEdited.written, "only settings.json is rewritten (it now names this build's rows)");
                    same(Arrays.asList("changed " + JSON), differences(before, snapshot(files)),
                            "diag-fps-overlay and every other game file keep bytes and modification time");
                    check(text(files, JSON).contains("\"fpsOverlay\":true"), "the counter is now recorded as it stands: " + text(files, JSON));
                    put(files, JSON, "{\"version\":1,\"inputUnbuffered\":false}\n"); // back to the file saved under v669
                }
                // the reviewer's case: under v462 pick 40 fps and save
                Settings forty = store(files).load(v462()).settings.with("frameCap", SettingsSchema.CAP_40);
                SettingsStore.SaveResult result = saved(files, forty, v462());
                same(new ArrayList<String>(), result.removed, "40 fps under v462: nothing is removed");
                same(Arrays.asList(JSON, "private-trace/diag-pace-40"), result.written, "40 fps under v462: written");
                check(new File(files, "private-trace/diag-fps-overlay").isFile(), "the counter switch stands");
                // and back under v669 the choice made under v462 stays on record
                saved(files, store(files).load(v669).settings, v669);
                check(text(files, JSON).contains("\"frameCap\":\"40\"") && text(files, JSON).contains("\"fpsOverlay\":true"),
                        "a Save under v669 keeps the values chosen under v462: " + text(files, JSON));
                same(SettingsSchema.CAP_40, store(files).load(v462()).settings.get("frameCap"), "v462 again: 40 fps");
                same(new ArrayList<String>(), store(files).load(v462()).drift, "v462 again: nothing differs");
            }
        });

        test("the game appearing during a save is reported; a save that changes nothing does not ask twice", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                File files = phone();
                final int[] asked = {0};
                SettingsStore appearing = new SettingsStore(files, new SettingsStore.GameRunning() {
                    @Override public boolean check() { return ++asked[0] > 1; }   // not running at the start, running afterwards
                });
                SettingsStore.SaveResult result = appearing.save(defaults().with("fpsOverlay", 1), v458());
                check(result.ok, "the save itself completed: " + result.error);
                same(SettingsStore.NOTE_GAME_APPEARED, result.error, "the note that the game appeared");
                same(2, asked[0], "asked before the first write and after the last");
                check(new File(files, "private-trace/diag-fps-overlay").isFile(), "the file was written");

                asked[0] = 0;
                SettingsStore.SaveResult again = appearing.save(defaults().with("fpsOverlay", 1), v458());
                check(again.ok && again.error == null && again.written.isEmpty() && again.removed.isEmpty(), "same settings again: nothing to write, no note");
                same(1, asked[0], "nothing was written, so nothing is asked afterwards");
            }
        });

        SettingsCoreTestKit.end();
    }
}
