package dev.recompbootstrap.smoke;

import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * The text form of Settings: files/settings/settings.json, schema version 1 (design section 3.1). Pure Java.
 *
 * One flat JSON object: "version" first, then the keys in schema order, one line feed at the end.
 * JSON forms: a CHOICE is its id as a string (render resolution: as a number), a SLIDER is a whole number, a TOGGLE is
 * true or false.
 *
 * The file records what the user could choose, and nothing else: the key of a row the installed game build cannot
 * change is left out when it is saved (encode with capabilities). A key that is missing is no fault: SettingsStore
 * takes that row from what the game's files say. Without this, a Save made under a build where the frame-rate
 * counter row is grey would record "counter off" as if chosen, and the next Save under a newer build would remove the
 * standing counter switch.
 */
final class SettingsJson {
    private SettingsJson() { }

    static final String VERSION_KEY = "version";

    static final class Decoded {
        /** Never null. Keys that were missing or invalid hold their defaults. */
        final Settings settings;
        /** The keys the object does not hold, in schema order. Unmodifiable. Empty when not valid. */
        final List<String> missing;
        /** False: the text is not a version-1 settings object. settings is then all defaults and the file counts as absent. */
        final boolean valid;
        /** One plain sentence per problem. Unmodifiable. */
        final List<String> warnings;

        Decoded(Settings settings, boolean valid, List<String> warnings, List<String> missing) {
            this.settings = settings;
            this.missing = Collections.unmodifiableList(missing);
            this.valid = valid;
            this.warnings = Collections.unmodifiableList(warnings);
        }
    }

    /** Every key (the text form of a Settings object, for logs and tests). Deterministic; ends with one line feed. */
    static String encode(Settings settings) {
        return encode(settings, null);
    }

    /**
     * What Save writes. A key is left out when the installed build cannot change its row (no supported value besides
     * the default) and the row holds a supported value, i.e. the default: nothing was chosen there. A value the build
     * does not support is kept: it was chosen under another build and comes back with it.
     */
    static String encode(Settings settings, Capabilities capabilities) {
        LinkedHashMap<String, Object> object = new LinkedHashMap<String, Object>();
        object.put(VERSION_KEY, Long.valueOf(SettingsSchema.VERSION));
        for (SettingSpec spec : SettingsSchema.all()) {
            int value = settings.get(spec.key);
            if (capabilities != null && !capabilities.anyChoice(spec.key) && capabilities.valueSupported(spec.key, value)) continue;
            Object json;
            if (spec.kind == SettingSpec.Kind.TOGGLE) json = Boolean.valueOf(value == 1);
            else if (spec.kind == SettingSpec.Kind.SLIDER) json = Long.valueOf(value);
            else if (spec.numericIds) json = Long.valueOf(Long.parseLong(spec.choiceIds[value]));
            else json = spec.choiceIds[value];
            object.put(spec.key, json);
        }
        return JsonLite.write(object) + "\n";
    }

    /**
     * Never throws. Text that is not a JSON object, or whose "version" is missing or not 1, is not valid: the result
     * holds the defaults and one warning. In a valid object, a key of the wrong type, out of range or off its step takes
     * its default and adds one warning; a missing key holds its default here and is named in missing (no warning);
     * the other keys are kept. Unknown keys are ignored.
     */
    static Decoded decode(String text) {
        ArrayList<String> warnings = new ArrayList<String>();
        String problem = null;
        Map<?, ?> object = null;
        if (text == null || text.trim().isEmpty()) {
            problem = "The saved settings file is empty.";
        } else {
            try {
                Object parsed = JsonLite.parse(text);
                if (parsed instanceof Map) object = (Map<?, ?>) parsed;
                else problem = "The saved settings file does not hold a settings object.";
            } catch (JsonLite.ParseException damaged) {
                problem = "The saved settings file is damaged.";
            }
        }
        if (object != null) {
            Object version = object.get(VERSION_KEY);
            if (version == null) {
                problem = "The saved settings file has no version number.";
            } else if (!(version instanceof Long)) {
                problem = "The saved settings file has an unreadable version number.";
            } else if (((Long) version).longValue() > SettingsSchema.VERSION) {
                problem = "The saved settings file was written by a newer version of this app (settings version "
                        + version + ").";
            } else if (((Long) version).longValue() != SettingsSchema.VERSION) {
                problem = "The saved settings file has an unknown version number (" + version + ").";
            }
        }
        if (problem != null) {
            warnings.add(problem + " It is not used; it is replaced when you save.");
            return new Decoded(Settings.defaults(), false, warnings, new ArrayList<String>());
        }
        ArrayList<String> missing = new ArrayList<String>();
        Settings settings = Settings.defaults();
        for (SettingSpec spec : SettingsSchema.all()) {
            Object json = object.get(spec.key);
            if (json == null) {
                missing.add(spec.key);
                continue;
            }
            int value = valueOf(spec, json);
            if (value == NOT_A_VALUE) {
                warnings.add("The saved value of \"" + spec.label + "\" is not valid; using "
                        + spec.format(spec.defaultValue) + ".");
                continue;
            }
            settings = settings.with(spec.key, value);
        }
        return new Decoded(settings, true, warnings, missing);
    }

    private static final int NOT_A_VALUE = Integer.MIN_VALUE;

    /** The setting's value for a parsed JSON value, or NOT_A_VALUE when the type or the value does not fit. */
    private static int valueOf(SettingSpec spec, Object json) {
        if (spec.kind == SettingSpec.Kind.TOGGLE) {
            return json instanceof Boolean ? (((Boolean) json).booleanValue() ? 1 : 0) : NOT_A_VALUE;
        }
        if (spec.kind == SettingSpec.Kind.CHOICE) {
            String id;
            if (spec.numericIds) {
                if (!(json instanceof Long)) return NOT_A_VALUE;
                id = json.toString();
            } else {
                if (!(json instanceof String)) return NOT_A_VALUE;
                id = (String) json;
            }
            int index = spec.indexOfId(id);
            return index < 0 ? NOT_A_VALUE : index;
        }
        if (!(json instanceof Long)) return NOT_A_VALUE;
        long number = ((Long) json).longValue();
        if (number < spec.min || number > spec.max || !spec.valid((int) number)) return NOT_A_VALUE;
        return (int) number;
    }
}
