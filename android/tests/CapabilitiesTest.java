package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SettingsCoreTestKit.ALL_NAMES;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.V458_NAMES;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.caps;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.check;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.minus;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.names;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.same;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.test;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.throwsA;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.TreeSet;

/** Host test of Capabilities: which setting values a game build supports (design section 3.3). Run with java -ea. */
public final class CapabilitiesTest {
    /**
     * Every non-default value of every setting as "key=value", for those the capabilities report as supported.
     * A slider whose non-default values are all supported is listed once as "key=*".
     */
    private static List<String> supported(Capabilities capabilities) {
        ArrayList<String> out = new ArrayList<String>();
        for (SettingSpec spec : SettingsSchema.all()) {
            ArrayList<String> yes = new ArrayList<String>();
            int total = 0;
            for (int value = spec.min; value <= spec.max; value += spec.step) {
                if (value == spec.defaultValue) continue;
                total++;
                if (capabilities.valueSupported(spec.key, value)) {
                    yes.add(spec.key + "=" + (spec.kind == SettingSpec.Kind.CHOICE ? spec.choiceIds[value] : String.valueOf(value)));
                }
            }
            if (spec.kind == SettingSpec.Kind.SLIDER && yes.size() == total) out.add(spec.key + "=*");
            else out.addAll(yes);
        }
        return out;
    }

    public static void main(String[] args) {
        SettingsCoreTestKit.begin("CapabilitiesTest");

        test("the four real builds: what the settings screen may offer on each", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                same(Arrays.asList("inputUnbuffered=1"), supported(SettingsCoreTestKit.v669()), "v669, the phone's standing library: it reads no setting file");
                same(Arrays.asList("frameCap=30", "scaler=bicubic", "scaler=nearest", "fsrSharpnessTenths=*", "fpsOverlay=1", "inputUnbuffered=1"),
                        supported(caps(V458_NAMES)), "v458");
                same(Arrays.asList("frameCap=40", "frameCap=30", "frameCap=24", "frameCap=auto", "scaler=bicubic", "scaler=nearest",
                        "fsrSharpnessTenths=*", "fpsOverlay=1", "worldTraffic=*", "worldPedestrians=*", "worldParkedCars=*",
                        "worldScenarioPedestrians=*", "inputUnbuffered=1"), supported(SettingsCoreTestKit.v461()), "v461");
                same(Arrays.asList("frameCap=40", "frameCap=30", "frameCap=24", "frameCap=auto", "scaler=gsr1", "scaler=bicubic", "scaler=nearest",
                        "fsrSharpnessTenths=*", "fpsOverlay=1", "worldTraffic=*", "worldPedestrians=*", "worldParkedCars=*",
                        "worldScenarioPedestrians=*", "inputUnbuffered=1"), supported(SettingsCoreTestKit.v462()), "v462");
            }
        });

        test("a future build with everything: all but 'frame cap off' and frame generation", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                same(Arrays.asList("frameCap=40", "frameCap=30", "frameCap=24", "frameCap=auto", "scaler=gsr1", "scaler=bicubic",
                        "scaler=nearest", "fsrSharpnessTenths=*", "renderScalePercent=50", "renderScalePercent=75", "renderScalePercent=88",
                        "renderScalePercent=115", "renderScalePercent=125", "renderScalePercent=150",
                        "fpsOverlay=1", "worldTraffic=*", "worldPedestrians=*", "worldParkedCars=*", "worldScenarioPedestrians=*",
                        "worldDrawDistance=*", "inputUnbuffered=1"), supported(caps(ALL_NAMES)), "supported non-default values");
            }
        });

        test("each value needs exactly the file names of the support table", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                // key, value, then the alternatives: each alternative is a group of names that must all be there
                Object[][] rows = {
                        {"frameCap", 3, "diag-pace-30", "diag-v280-limit30"},
                        {"frameCap", 2, "diag-pace-40"},
                        {"frameCap", 4, "diag-pace-24"},
                        {"frameCap", 5, "diag-pace-auto"},
                        {"scaler", 2, "scaler.txt", "diag-v409-scale-cubic"},
                        {"scaler", 1, "scaler.txt+recomp-cap:gsr1"},
                        {"scaler", 3, "scaler.txt", "diag-v409-scale-cubic", "diag-v450-scale-fsr1"},
                        {"fsrSharpnessTenths", 5, "fsr-sharpness.txt", "diag-v450-fsr1-stops"},
                        {"fsrSharpnessTenths", 0, "fsr-sharpness.txt", "diag-v450-fsr1-stops"},
                        {"renderScalePercent", 0, "render-scale.txt"},
                        {"renderScalePercent", 6, "render-scale.txt"},
                        // Frame generation (experimental): the marker, the order's own switch file and the 30 fps cap file, all three.
                        {"frameGeneration", 1, "recomp-cap:framegen1+diag-fg-between+diag-pace-30"},
                        {"frameGeneration", 2, "recomp-cap:framegen1+diag-fg-ahead+diag-pace-30"},
                        {"fpsOverlay", 1, "diag-fps-overlay"},
                        {"worldTraffic", 35, "world-load.txt"},
                        {"worldPedestrians", 25, "world-load.txt"},
                        {"worldParkedCars", 95, "world-load.txt"},
                        {"worldScenarioPedestrians", 50, "world-load.txt"},
                        {"worldDrawDistance", 70, "world-load.txt+recomp-cap:wl-lod"}};
                for (Object[] row : rows) {
                    String key = (String) row[0];
                    int value = (Integer) row[1];
                    TreeSet<String> everyNeeded = new TreeSet<String>();
                    for (int a = 2; a < row.length; a++) {
                        String[] group = ((String) row[a]).split("\\+");
                        everyNeeded.addAll(Arrays.asList(group));
                        check(caps(group).valueSupported(key, value), key + "=" + value + " must be supported with only " + row[a]);
                        for (String missing : group) {
                            check(!caps(minus(group, missing)).valueSupported(key, value),
                                    key + "=" + value + " must not be supported with " + row[a] + " lacking " + missing);
                        }
                    }
                    // every other name present, all of the needed ones absent: still not supported
                    String[] others = minus(ALL_NAMES, everyNeeded.toArray(new String[0]));
                    check(!caps(others).valueSupported(key, value), key + "=" + value + " must not be supported by unrelated names");
                }
                check(!caps(ALL_NAMES).valueSupported("frameCap", 0), "no build supports 'frame cap off'");
                check(!caps("diag-v460-scale-gsr1", "diag-v450-scale-fsr1", "recomp-cap:gsr1").valueSupported("scaler", 1),
                        "GSR 1 by its developer switch alone (v461) is not selectable: it needs scaler.txt");
            }
        });

        test("frame generation: never live by the dry-run names or by file names alone; the reason says which of the two is missing", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                // What v460 to v464 really contain (the timing dry run), the two order switches without a marker (a
                // developer build of the generator), and the marker beside the dry run only: none of them offers a value.
                String[][] sets = {{}, V458_NAMES, ALL_NAMES, SetTestKit.DRY_RUN, {"diag-fg-ahead", "diag-fg-between", "diag-pace-30"},
                        SettingsCoreTestKit.plus(SettingsCoreTestKit.V464_NAMES, SetTestKit.DRY_RUN),
                        SettingsCoreTestKit.plus(SettingsCoreTestKit.plus(ALL_NAMES, SetTestKit.DRY_RUN), "diag-fg-ahead", "diag-fg-between"),
                        SettingsCoreTestKit.plus(SettingsCoreTestKit.plus(ALL_NAMES, SetTestKit.DRY_RUN), "recomp-cap:framegen1")};
                for (String[] set : sets) {
                    for (int value = 1; value <= 2; value++) {
                        check(!caps(set).valueSupported("frameGeneration", value), "frame generation " + value + " with " + Arrays.asList(set));
                    }
                    check(!caps(set).anyChoice("frameGeneration"), "the row is disabled with " + Arrays.asList(set));
                }
                same("Not in this version", caps(SetTestKit.DRY_RUN).reason("frameGeneration", 1), "the reason with the dry run in the build");
                same("Not in this version", caps().reason("frameGeneration", 2), "the reason with nothing in the build");
                same("Not in this version", caps("diag-fg-ahead", "diag-fg-between", "diag-pace-30").reason("frameGeneration", 1),
                        "the reason with the switch names but no marker: the build does not announce a generator");
                same("Not in the installed game build", caps("recomp-cap:framegen1", "diag-fg-ahead", "diag-pace-30").reason("frameGeneration", 1),
                        "the marker and the other order only: this order is not in the build");
                same("recomp-cap:framegen1", Capabilities.FRAME_GENERATION_MARKER, "the marker name the frame-generation lane is told (CONTRACTS.md, this lane's report)");

                // The build of the design: both orders can be chosen, and a build with one order offers that one only.
                Capabilities announced = SetTestKit.framegen();
                check(announced.valueSupported("frameGeneration", 1) && announced.valueSupported("frameGeneration", 2), "both orders with the marker and both names");
                same(null, announced.reason("frameGeneration", 2), "a supported order has no reason");
                Capabilities oneOrder = caps(SettingsCoreTestKit.minus(SetTestKit.FRAMEGEN_NAMES, "diag-fg-between"));
                check(!oneOrder.valueSupported("frameGeneration", 1) && oneOrder.valueSupported("frameGeneration", 2), "a build with the ahead order only");
                same(0, oneOrder.effective("frameGeneration", 1), "the order it lacks counts as Off");
            }
        });

        test("the low-delay input option depends on the app, not on the game library", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                check(Capabilities.of(names(), true, true).valueSupported("inputUnbuffered", 1), "option in the app: supported");
                check(!Capabilities.of(names(ALL_NAMES), true, false).valueSupported("inputUnbuffered", 1), "option not in the app: not supported");
            }
        });

        test("defaults are always supported; without a game library nothing else is", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                Capabilities none = Capabilities.of(names(ALL_NAMES), false, true);
                for (Capabilities capabilities : new Capabilities[] {none, caps(), caps(V458_NAMES), caps(ALL_NAMES)}) {
                    for (SettingSpec spec : SettingsSchema.all()) {
                        check(capabilities.valueSupported(spec.key, spec.defaultValue), spec.key + ": the default is supported");
                        same(null, capabilities.reason(spec.key, spec.defaultValue), spec.key + ": no reason for the default");
                    }
                }
                same(new ArrayList<String>(), supported(none), "supported without a library");
                for (SettingSpec spec : SettingsSchema.all()) {
                    int other = spec.defaultValue == spec.min ? spec.max : spec.min;
                    same("Game code is not installed", none.reason(spec.key, other), spec.key + ": reason without a library");
                    check(!none.anyChoice(spec.key), spec.key + ": nothing to choose without a library");
                }
                same(new ArrayList<String>(), none.names(), "no names without a library");
                check(!none.has("diag-fps-overlay"), "has() without a library");
            }
        });

        test("reason: the line shown for an unsupported value", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                Capabilities bare = Capabilities.of(names(), true, false);
                String notInBuild = "Not in the installed game build";
                same("No game build can run without a frame cap yet", bare.reason("frameCap", 0), "frame cap off");
                same("No game build can run without a frame cap yet", caps(ALL_NAMES).reason("frameCap", 0), "frame cap off, full build");
                same(notInBuild + " (needs game build v460 or newer)", bare.reason("frameCap", 3), "30 fps");
                same(notInBuild + " (needs game build v460 or newer)", bare.reason("frameCap", 2), "40 fps");
                same(notInBuild + " (needs game build v460 or newer)", caps(V458_NAMES).reason("frameCap", 4), "24 fps");
                same(notInBuild + " (needs game build v460 or newer)", bare.reason("frameCap", 5), "automatic");
                same(notInBuild, bare.reason("scaler", 2), "bicubic");
                same(notInBuild, bare.reason("scaler", 3), "nearest in a build that reads no scaler input");
                same(notInBuild + " (needs game build v462 or newer)", SettingsCoreTestKit.v461().reason("scaler", 1), "GSR 1 on v461");
                same(notInBuild, bare.reason("fsrSharpnessTenths", 5), "sharpness");
                same(notInBuild + " (needs game build v463 or newer)", SettingsCoreTestKit.v462().reason("renderScalePercent", 0), "render scale");
                same(notInBuild, bare.reason("fpsOverlay", 1), "overlay");
                same(notInBuild + " (needs game build v461 or newer)", caps(V458_NAMES).reason("worldTraffic", 50), "world slider");
                same(notInBuild + " (needs game build v461 or newer)", caps(V458_NAMES).reason("worldDrawDistance", 80), "draw distance without the world file");
                same("Not in this version", SettingsCoreTestKit.v461().reason("worldDrawDistance", 80),
                        "draw distance on v461: the file is read, the slider is locked");
                check(bare.reason("inputUnbuffered", 1) != null && bare.reason("inputUnbuffered", 1).contains("Android 11"), "input option");
                same(null, caps(V458_NAMES).reason("frameCap", 3), "a supported value has no reason");
                check(bare.reason("worldTraffic", 33) != null && !bare.valueSupported("worldTraffic", 33), "a value off the step is not supported");
                check(!caps(ALL_NAMES).valueSupported("worldTraffic", 33), "a value off the step is not supported by a full build either");
                check(!caps(ALL_NAMES).valueSupported("worldTraffic", 20), "a density below the game's floor of 25 is not a value");
                check(!caps(ALL_NAMES).valueSupported("worldDrawDistance", 65), "a draw distance below the game's floor of 70 is not a value");
                for (SettingSpec spec : SettingsSchema.all()) {
                    for (Capabilities build : new Capabilities[] {bare, caps(V458_NAMES), SettingsCoreTestKit.v461(), SettingsCoreTestKit.v462(),
                            caps("recomp-cap:framegen1")}) {
                        for (int value = spec.min; value <= spec.max; value += spec.step) {
                            String reason = build.reason(spec.key, value);
                            if (reason == null) continue;
                            for (String internal : new String[] {"pace-caps", "world-load", "fsr-modes", "frame-gen", "lane", "diag-", ".txt"}) {
                                check(!reason.contains(internal), "the reason shows an internal name: " + reason);
                            }
                        }
                    }
                }
                throwsA(IllegalArgumentException.class, "unknown key", new SettingsCoreTestKit.Case() {
                    @Override public void run() { caps().valueSupported("noSuchSetting", 0); }
                });
            }
        });

        test("effective and anyChoice", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                Capabilities v458 = caps(V458_NAMES);
                same(3, v458.effective("frameCap", 3), "supported: the value itself");
                same(1, v458.effective("frameCap", 2), "40 fps on v458: the default (60)");
                same(0, v458.effective("scaler", 1), "GSR 1 on v458: the default (FSR 1)");
                same(100, v458.effective("worldTraffic", 35), "world slider on v458: 100");
                same(35, caps(ALL_NAMES).effective("worldTraffic", 35), "world slider on a full build");
                ArrayList<String> rows = new ArrayList<String>();
                for (SettingSpec spec : SettingsSchema.all()) {
                    if (v458.anyChoice(spec.key)) rows.add(spec.key);
                }
                same(Arrays.asList("frameCap", "scaler", "fsrSharpnessTenths", "fpsOverlay", "inputUnbuffered"), rows, "rows that are not disabled on v458");
                rows.clear();
                for (SettingSpec spec : SettingsSchema.all()) {
                    if (SettingsCoreTestKit.v669().anyChoice(spec.key)) rows.add(spec.key);
                }
                same(Arrays.asList("inputUnbuffered"), rows, "rows that are not disabled with the phone's standing library");
                check(!Capabilities.of(names(V458_NAMES), true, false).anyChoice("inputUnbuffered"), "input row without the app option");
            }
        });

        test("names: sorted, complete, unmodifiable", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                final Capabilities capabilities = caps("diag-fps-overlay", "world-load.txt", "diag-pace-40");
                same(Arrays.asList("diag-fps-overlay", "diag-pace-40", "world-load.txt"), capabilities.names(), "names");
                check(capabilities.has("world-load.txt") && !capabilities.has("scaler.txt"), "has()");
                throwsA(UnsupportedOperationException.class, "names() is unmodifiable", new SettingsCoreTestKit.Case() {
                    @Override public void run() { capabilities.names().add("x"); }
                });
            }
        });

        SettingsCoreTestKit.end();
    }
}
