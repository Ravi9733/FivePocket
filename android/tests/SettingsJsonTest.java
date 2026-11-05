package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SettingsCoreTestKit.check;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.same;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.test;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.throwsA;

import java.util.ArrayList;
import java.util.Arrays;

/** Host test of the settings model (SettingSpec, SettingsSchema, Settings) and its JSON form (SettingsJson). Run with java -ea. */
public final class SettingsJsonTest {
    /** settings.json with every setting at its default, as design section 3.1 defines it. */
    private static final String DEFAULTS_TEXT = "{\"version\":1,\"frameCap\":\"60\",\"scaler\":\"fsr1\",\"fsrSharpnessTenths\":2,"
            + "\"renderScalePercent\":100,\"frameGeneration\":\"off\",\"fpsOverlay\":false,\"worldTraffic\":100,"
            + "\"worldPedestrians\":100,\"worldParkedCars\":100,\"worldScenarioPedestrians\":100,\"worldDrawDistance\":100,"
            + "\"inputUnbuffered\":false}\n";
    /** A file in which no setting is at its default. */
    private static final String CHANGED_TEXT = "{\"version\":1,\"frameCap\":\"30\",\"scaler\":\"bicubic\",\"fsrSharpnessTenths\":5,"
            + "\"renderScalePercent\":75,\"frameGeneration\":\"between\",\"fpsOverlay\":true,\"worldTraffic\":35,"
            + "\"worldPedestrians\":40,\"worldParkedCars\":25,\"worldScenarioPedestrians\":95,\"worldDrawDistance\":70,"
            + "\"inputUnbuffered\":true}\n";

    private static Settings changed() {
        return Settings.defaults().with("frameCap", 3).with("scaler", 2).with("fsrSharpnessTenths", 5).with("renderScalePercent", 1)
                .with("frameGeneration", 1).with("fpsOverlay", 1).with("worldTraffic", 35).with("worldPedestrians", 40)
                .with("worldParkedCars", 25).with("worldScenarioPedestrians", 95).with("worldDrawDistance", 70).with("inputUnbuffered", 1);
    }

    /** CHANGED_TEXT with the JSON value of one key replaced (or the whole pair removed when json is null). */
    private static String changedWith(String key, String json) {
        int start = CHANGED_TEXT.indexOf("\"" + key + "\":");
        check(start > 0, "test table: unknown key " + key);
        int valueStart = start + key.length() + 3;
        int end = CHANGED_TEXT.indexOf(',', valueStart);
        if (end < 0) end = CHANGED_TEXT.indexOf('}', valueStart);
        if (json != null) return CHANGED_TEXT.substring(0, valueStart) + json + CHANGED_TEXT.substring(end);
        return CHANGED_TEXT.charAt(end) == ','
                ? CHANGED_TEXT.substring(0, start) + CHANGED_TEXT.substring(end + 1)
                : CHANGED_TEXT.substring(0, start - 1) + CHANGED_TEXT.substring(end);
    }

    private static void illegal(String what, SettingsCoreTestKit.Case body) {
        throwsA(IllegalArgumentException.class, what, body);
    }

    public static void main(String[] args) {
        SettingsCoreTestKit.begin("SettingsJsonTest");

        test("schema: the twelve settings in screen order, with their kinds, groups and value lists", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                String[] expected = {
                        "frameCap|CHOICE|Picture|Frame rate cap|off,60,40,30,24,auto|1",
                        "scaler|CHOICE|Picture|Scaler|fsr1,gsr1,bicubic,nearest|0",
                        "fsrSharpnessTenths|SLIDER|Picture|FSR sharpness|0..40 step 1|2",
                        "renderScalePercent|CHOICE|Picture|Render resolution|50,75,88,100,115,125,150|3",
                        "frameGeneration|CHOICE|Picture|Frame generation (experimental)|off,between,ahead|0",
                        "fpsOverlay|TOGGLE|Picture|Frame-rate counter|0..1 step 1|0",
                        "worldTraffic|SLIDER|World|Traffic|25..100 step 5|100",
                        "worldPedestrians|SLIDER|World|Pedestrians|25..100 step 5|100",
                        "worldParkedCars|SLIDER|World|Parked cars|25..100 step 5|100",
                        "worldScenarioPedestrians|SLIDER|World|Scenario pedestrians|25..100 step 5|100",
                        "worldDrawDistance|SLIDER|World|Draw distance|70..100 step 5|100",
                        "inputUnbuffered|TOGGLE|Controller|Low-delay stick input|0..1 step 1|0"};
                ArrayList<String> actual = new ArrayList<String>();
                for (SettingSpec spec : SettingsSchema.all()) {
                    String values = spec.kind == SettingSpec.Kind.CHOICE ? String.join(",", spec.choiceIds)
                            : spec.min + ".." + spec.max + " step " + spec.step;
                    actual.add(spec.key + "|" + spec.kind + "|" + spec.group + "|" + spec.label + "|" + values + "|" + spec.defaultValue);
                    same(spec.choiceIds.length, spec.choiceLabels.length, spec.key + ": one label per choice");
                    if (spec.kind == SettingSpec.Kind.CHOICE) {
                        same(0, spec.min, spec.key + ": a choice value is an index");
                        same(spec.choiceIds.length - 1, spec.max, spec.key + ": a choice value is an index");
                    }
                    check(SettingsSchema.spec(spec.key) == spec, "spec() finds " + spec.key);
                }
                same(Arrays.asList(expected), actual, "schema");
                same(1, SettingsSchema.VERSION, "schema version");
                illegal("spec of an unknown key", new SettingsCoreTestKit.Case() {
                    @Override public void run() { SettingsSchema.spec("noSuchSetting"); }
                });
                throwsA(UnsupportedOperationException.class, "all() is unmodifiable", new SettingsCoreTestKit.Case() {
                    @Override public void run() { SettingsSchema.all().remove(0); }
                });
            }
        });

        test("defaults: the file for the original game is exactly the text of design 3.1", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                same(DEFAULTS_TEXT, SettingsJson.encode(Settings.defaults()), "encode(defaults)");
                check(Settings.defaults().isDefault(), "defaults().isDefault()");
            }
        });

        test("format: the text shown for a value", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                same("40 fps", SettingsSchema.spec("frameCap").format(2), "frame cap 40");
                same("60 fps (default)", SettingsSchema.spec("frameCap").format(1), "frame cap 60");
                same("85 %", SettingsSchema.spec("worldTraffic").format(85), "traffic 85");
                same("Original", SettingsSchema.spec("worldTraffic").format(100), "a world slider at 100 is the original game");
                same("Original", SettingsSchema.spec("worldDrawDistance").format(100), "draw distance at 100");
                same("25 %", SettingsSchema.spec("worldPedestrians").format(25), "the lowest density the game accepts");
                illegal("a density below the game's floor", new SettingsCoreTestKit.Case() {
                    @Override public void run() { SettingsSchema.spec("worldTraffic").format(20); }
                });
                illegal("a draw distance below the game's floor", new SettingsCoreTestKit.Case() {
                    @Override public void run() { SettingsSchema.spec("worldDrawDistance").format(65); }
                });
                same("150 % (1920x1080, supersampling)", SettingsSchema.spec("renderScalePercent").format(6), "render scale 150 says supersampling");
                same("115 % (1440x810, supersampling)", SettingsSchema.spec("renderScalePercent").format(4), "render scale 115 says supersampling");
                same("0.5 (0 = sharpest)", SettingsSchema.spec("fsrSharpnessTenths").format(5), "sharpness 5 tenths");
                same("4.0 (0 = sharpest)", SettingsSchema.spec("fsrSharpnessTenths").format(40), "sharpness 40 tenths");
                same("On", SettingsSchema.spec("fpsOverlay").format(1), "toggle on");
                same("Off", SettingsSchema.spec("inputUnbuffered").format(0), "toggle off");
                same("100 % (1280x720, original)", SettingsSchema.spec("renderScalePercent").format(3), "render scale 100");
                same("75 % (960x540, Performance)", SettingsSchema.spec("renderScalePercent").format(1), "render scale 75");
                illegal("format of a value off the step", new SettingsCoreTestKit.Case() {
                    @Override public void run() { SettingsSchema.spec("worldTraffic").format(33); }
                });
            }
        });

        test("Settings: with() changes one key in a copy and refuses what the setting cannot hold", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                final Settings defaults = Settings.defaults();
                Settings changed = defaults.with("worldTraffic", 35);
                same(35, changed.get("worldTraffic"), "changed value");
                same(100, defaults.get("worldTraffic"), "the original is not changed");
                same(100, Settings.defaults().get("worldTraffic"), "defaults() is not changed");
                check(!changed.isDefault() && !changed.equals(defaults), "a changed copy is not the default");
                for (SettingSpec spec : SettingsSchema.all()) {
                    if (!spec.key.equals("worldTraffic")) same(spec.defaultValue, changed.get(spec.key), spec.key + " untouched");
                }
                int[][] bad = {{33}, {-5}, {105}, {101}};
                for (final int[] value : bad) {
                    illegal("worldTraffic = " + value[0], new SettingsCoreTestKit.Case() {
                        @Override public void run() { defaults.with("worldTraffic", value[0]); }
                    });
                }
                illegal("fsrSharpnessTenths = 41", new SettingsCoreTestKit.Case() {
                    @Override public void run() { defaults.with("fsrSharpnessTenths", 41); }
                });
                illegal("frameCap = 6", new SettingsCoreTestKit.Case() {
                    @Override public void run() { defaults.with("frameCap", 6); }
                });
                illegal("fpsOverlay = 2", new SettingsCoreTestKit.Case() {
                    @Override public void run() { defaults.with("fpsOverlay", 2); }
                });
                illegal("with() of an unknown key", new SettingsCoreTestKit.Case() {
                    @Override public void run() { defaults.with("noSuchSetting", 0); }
                });
                illegal("get() of an unknown key", new SettingsCoreTestKit.Case() {
                    @Override public void run() { defaults.get("noSuchSetting"); }
                });
            }
        });

        test("Settings: equality and changedKeys", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                Settings a = Settings.defaults().with("scaler", 2).with("fpsOverlay", 1);
                Settings b = Settings.defaults().with("fpsOverlay", 1).with("scaler", 2);
                check(a.equals(b) && a.hashCode() == b.hashCode(), "the same values are equal, whatever the order of the changes");
                check(a.with("scaler", 0).with("fpsOverlay", 0).isDefault(), "back at the defaults is the default");
                same(Arrays.asList("scaler", "fpsOverlay"), a.changedKeys(Settings.defaults()), "changed keys in schema order");
                same(Arrays.asList("scaler", "fpsOverlay"), Settings.defaults().changedKeys(b), "changedKeys is symmetric");
                same(new ArrayList<String>(), a.changedKeys(b), "no change");
                same(Arrays.asList("frameCap", "worldDrawDistance"), a.changedKeys(a.with("worldDrawDistance", 70).with("frameCap", 5)), "first and last key");
            }
        });

        test("round trip: every value of every setting survives encode and decode", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                int cases = 0;
                for (SettingSpec spec : SettingsSchema.all()) {
                    for (int value = spec.min; value <= spec.max; value += spec.step) {
                        for (Settings base : new Settings[] {Settings.defaults(), changed()}) {
                            Settings settings = base.with(spec.key, value);
                            SettingsJson.Decoded decoded = SettingsJson.decode(SettingsJson.encode(settings));
                            check(decoded.valid, spec.key + "=" + value + ": valid");
                            same(new ArrayList<String>(), decoded.warnings, spec.key + "=" + value + ": warnings");
                            same(settings, decoded.settings, spec.key + "=" + value);
                            cases++;
                        }
                    }
                }
                check(cases == 2 * (6 + 4 + 41 + 7 + 3 + 2 + 4 * 16 + 7 + 2), "the loop ran over every value (" + cases + " cases)");
            }
        });

        test("encode: the JSON forms (string ids, numeric render scale, numbers, booleans), key order, final line feed", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                same(CHANGED_TEXT, SettingsJson.encode(changed()), "encode(changed)");
                SettingsJson.Decoded decoded = SettingsJson.decode(CHANGED_TEXT);
                check(decoded.valid && decoded.warnings.isEmpty(), "the text decodes cleanly");
                same(changed(), decoded.settings, "decode(changed text)");
            }
        });

        test("decode: a value of the wrong type, out of range or off its step becomes that key's default, with one warning", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                String[][] bad = {
                        {"frameCap", "60"}, {"frameCap", "\"90\""}, {"frameCap", "\"Off\""}, {"frameCap", "true"}, {"frameCap", "null"},
                        {"scaler", "\"fsr2\""}, {"scaler", "0"}, {"scaler", "[\"fsr1\"]"},
                        {"fsrSharpnessTenths", "41"}, {"fsrSharpnessTenths", "-1"}, {"fsrSharpnessTenths", "\"5\""}, {"fsrSharpnessTenths", "0.5"},
                        {"fsrSharpnessTenths", "5.0"}, {"fsrSharpnessTenths", "4294967301"},
                        {"renderScalePercent", "\"75\""}, {"renderScalePercent", "80"}, {"renderScalePercent", "67"}, {"renderScalePercent", "2"}, {"renderScalePercent", "75.0"},
                        {"frameGeneration", "\"on\""}, {"frameGeneration", "1"},
                        {"fpsOverlay", "1"}, {"fpsOverlay", "\"true\""}, {"fpsOverlay", "null"},
                        {"worldTraffic", "33"}, {"worldTraffic", "20"}, {"worldTraffic", "0"}, {"worldDrawDistance", "65"}, {"worldTraffic", "105"}, {"worldTraffic", "-5"}, {"worldTraffic", "\"35\""}, {"worldTraffic", "{}"},
                        {"worldDrawDistance", "101"}, {"inputUnbuffered", "0"}, {"inputUnbuffered", "\"on\""}};
                for (String[] entry : bad) {
                    String what = entry[0] + " = " + entry[1];
                    SettingsJson.Decoded decoded = SettingsJson.decode(changedWith(entry[0], entry[1]));
                    check(decoded.valid, what + ": the file as a whole stays valid");
                    same(1, decoded.warnings.size(), what + ": number of warnings " + decoded.warnings);
                    SettingSpec spec = SettingsSchema.spec(entry[0]);
                    check(decoded.warnings.get(0).contains("\"" + spec.label + "\""), what + ": the warning names the row: " + decoded.warnings);
                    same(changed().with(entry[0], spec.defaultValue), decoded.settings, what + ": only that key falls back");
                }
            }
        });

        test("encode for a Save: the key of a row the installed build cannot change is left out; a chosen value is kept", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                // today's phone (v669): only the input row can be changed
                same("{\"version\":1,\"inputUnbuffered\":false}\n", SettingsJson.encode(Settings.defaults(), SettingsCoreTestKit.v669()), "v669, defaults");
                same("{\"version\":1,\"inputUnbuffered\":true}\n", SettingsJson.encode(Settings.defaults().with("inputUnbuffered", 1), SettingsCoreTestKit.v669()), "v669, low delay on");
                // values chosen under another build are not supported here and are kept: they come back with that build
                same("{\"version\":1,\"frameCap\":\"40\",\"worldTraffic\":50,\"inputUnbuffered\":false}\n",
                        SettingsJson.encode(Settings.defaults().with("frameCap", SettingsSchema.CAP_40).with("worldTraffic", 50), SettingsCoreTestKit.v669()), "v669, 40 fps and traffic 50 from another build");
                same("{\"version\":1,\"frameCap\":\"60\",\"scaler\":\"fsr1\",\"fsrSharpnessTenths\":2,\"fpsOverlay\":false,\"inputUnbuffered\":false}\n",
                        SettingsJson.encode(Settings.defaults(), SettingsCoreTestKit.v458()), "v458: its three picture rows, the counter and the input row");
                same("{\"version\":1,\"frameCap\":\"60\",\"scaler\":\"fsr1\",\"fsrSharpnessTenths\":2,\"fpsOverlay\":false,\"worldTraffic\":100,"
                        + "\"worldPedestrians\":100,\"worldParkedCars\":100,\"worldScenarioPedestrians\":100,\"inputUnbuffered\":false}\n",
                        SettingsJson.encode(Settings.defaults(), SettingsCoreTestKit.v462()), "v462: no render resolution, frame generation or draw distance");
                // the full form is unchanged, and what a Save writes decodes to the same settings
                same(DEFAULTS_TEXT, SettingsJson.encode(Settings.defaults(), null), "without capabilities: every key");
                SettingsJson.Decoded back = SettingsJson.decode(SettingsJson.encode(changed(), SettingsCoreTestKit.v669()));
                check(back.valid && back.warnings.isEmpty(), "what a Save writes is a valid file without warnings");
                same(changed().with("fpsOverlay", 1), back.settings, "v669 keeps every chosen value (all but the input row are unsupported there)");
            }
        });

        test("decode: a missing key is named in missing, without a warning; unknown keys are ignored and not written back", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                for (SettingSpec spec : SettingsSchema.all()) {
                    SettingsJson.Decoded decoded = SettingsJson.decode(changedWith(spec.key, null));
                    check(decoded.valid, spec.key + " missing: valid");
                    same(new ArrayList<String>(), decoded.warnings, spec.key + " missing: no warning (the row was grey when the file was saved)");
                    same(Arrays.asList(spec.key), decoded.missing, spec.key + " missing: named");
                    same(changed().with(spec.key, spec.defaultValue), decoded.settings, spec.key + " missing");
                }
                same(new ArrayList<String>(), SettingsJson.decode(CHANGED_TEXT).missing, "a complete file: nothing missing");
                same(new ArrayList<String>(), SettingsJson.decode("{}").missing, "an invalid file: nothing is called missing");
                same(Arrays.asList("frameCap", "scaler"), SettingsJson.decode(changedWith("frameCap", null).replace("\"scaler\":\"bicubic\",", "")).missing, "two keys, in schema order");
                String withUnknown = CHANGED_TEXT.replace("\"frameCap\"", "\"futureSetting\":{\"a\":[1,2]},\"frameCap\"");
                SettingsJson.Decoded decoded = SettingsJson.decode(withUnknown);
                check(decoded.valid, "unknown key: valid");
                same(new ArrayList<String>(), decoded.warnings, "unknown key: no warning");
                same(changed(), decoded.settings, "unknown key: settings");
                same(CHANGED_TEXT, SettingsJson.encode(decoded.settings), "the unknown key is not written back");
            }
        });

        test("decode: a damaged, empty, foreign or newer-version file is not valid and gives the defaults", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                String[] unusable = {null, "", "   \n", "garbage", "[1]", "\"x\"", "3", "null", "{}",
                        CHANGED_TEXT.replace("\"version\":1,", ""),
                        CHANGED_TEXT.replace("\"version\":1", "\"version\":2"),
                        CHANGED_TEXT.replace("\"version\":1", "\"version\":0"),
                        CHANGED_TEXT.replace("\"version\":1", "\"version\":-1"),
                        CHANGED_TEXT.replace("\"version\":1", "\"version\":\"1\""),
                        CHANGED_TEXT.replace("\"version\":1", "\"version\":1.0"),
                        CHANGED_TEXT.replace("\"version\":1", "\"version\":null"),
                        CHANGED_TEXT.replace("\"version\":1", "\"version\":1,\"version\":1"),
                        CHANGED_TEXT + "x", "\u0000\u0000\u0000\u0000"};
                for (String text : unusable) {
                    String what = text == null ? "null" : text.length() > 30 ? text.substring(0, 30) + "..." : text;
                    SettingsJson.Decoded decoded = SettingsJson.decode(text);
                    check(!decoded.valid, "<" + what + ">: must not be valid");
                    same(Settings.defaults(), decoded.settings, "<" + what + ">: settings");
                    same(1, decoded.warnings.size(), "<" + what + ">: warnings " + decoded.warnings);
                }
                String newer = SettingsJson.decode(CHANGED_TEXT.replace("\"version\":1", "\"version\":2")).warnings.get(0);
                check(newer.contains("newer version") && newer.contains("2"), "the warning says the file is from a newer version: " + newer);
            }
        });

        test("decode: every truncation of a valid file is not valid, and nothing throws", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                // the last two characters are '}' and the line feed; cutting only the line feed leaves a complete object
                for (int length = 0; length < CHANGED_TEXT.length() - 1; length++) {
                    SettingsJson.Decoded decoded = SettingsJson.decode(CHANGED_TEXT.substring(0, length));
                    check(!decoded.valid, "cut after " + length + " characters: must not be valid");
                    same(Settings.defaults(), decoded.settings, "cut after " + length + " characters");
                }
                check(SettingsJson.decode(CHANGED_TEXT.substring(0, CHANGED_TEXT.length() - 1)).valid, "without the final line feed the file is still valid");
                StringBuilder deep = new StringBuilder();
                for (int i = 0; i < 200000; i++) deep.append('[');
                check(!SettingsJson.decode(deep.toString()).valid, "absurd nesting is not valid");
            }
        });

        SettingsCoreTestKit.end();
    }
}
