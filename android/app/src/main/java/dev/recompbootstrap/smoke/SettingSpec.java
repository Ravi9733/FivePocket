package dev.recompbootstrap.smoke;

/**
 * One user-facing setting: its JSON key, its row on the settings screen and its legal values (design sections 2.5 and
 * 3.1). A setting's value is always an int: the index into choiceIds for a CHOICE, the number itself for a SLIDER,
 * 0 (off) or 1 (on) for a TOGGLE. Pure Java, immutable.
 */
final class SettingSpec {
    enum Kind { CHOICE, SLIDER, TOGGLE }

    /** JSON key. */
    final String key;
    final Kind kind;
    /** "Picture", "World" or "Controller". */
    final String group;
    /** Row label on the settings screen. */
    final String label;
    /** CHOICE: the JSON forms in display order; otherwise empty. */
    final String[] choiceIds;
    /** CHOICE: the texts shown, same length as choiceIds; otherwise empty. */
    final String[] choiceLabels;
    /** SLIDER: range and step. CHOICE: 0, choiceIds.length - 1, 1. TOGGLE: 0, 1, 1. */
    final int min, max, step;
    final int defaultValue;
    /** CHOICE only: the ids are numbers and are written to JSON as numbers (render resolution is the one such setting). */
    final boolean numericIds;
    /**
     * SLIDER only: the value counts tenths and is shown as "0.5 (0 = sharpest)"; otherwise it is a percentage of the
     * original game, shown as "85 %" and, at the default (100), as "Original".
     */
    private final boolean tenths;

    private SettingSpec(String key, Kind kind, String group, String label, String[] choiceIds, String[] choiceLabels,
            int min, int max, int step, int defaultValue, boolean numericIds, boolean tenths) {
        this.key = key;
        this.kind = kind;
        this.group = group;
        this.label = label;
        this.choiceIds = choiceIds;
        this.choiceLabels = choiceLabels;
        this.min = min;
        this.max = max;
        this.step = step;
        this.defaultValue = defaultValue;
        this.numericIds = numericIds;
        this.tenths = tenths;
        if (step < 1 || max < min || !valid(defaultValue) || choiceIds.length != choiceLabels.length) {
            throw new IllegalArgumentException("bad definition of the setting " + key);
        }
    }

    static SettingSpec choice(String key, String group, String label, String[] ids, String[] labels, int defaultIndex,
            boolean numericIds) {
        return new SettingSpec(key, Kind.CHOICE, group, label, ids, labels, 0, ids.length - 1, 1, defaultIndex, numericIds, false);
    }

    static SettingSpec slider(String key, String group, String label, int min, int max, int step, int defaultValue,
            boolean tenths) {
        return new SettingSpec(key, Kind.SLIDER, group, label, new String[0], new String[0], min, max, step, defaultValue, false, tenths);
    }

    static SettingSpec toggle(String key, String group, String label) {
        return new SettingSpec(key, Kind.TOGGLE, group, label, new String[0], new String[0], 0, 1, 1, 0, false, false);
    }

    /** True for a value this setting can hold: inside the range and on the step. */
    boolean valid(int value) {
        return value >= min && value <= max && (value - min) % step == 0;
    }

    /** Text for a value, e.g. "40 fps", "85 %", "Original", "0.5 (0 = sharpest)", "On". IllegalArgumentException for an invalid value. */
    String format(int value) {
        if (!valid(value)) throw new IllegalArgumentException(value + " is not a value of the setting " + key);
        if (kind == Kind.CHOICE) return choiceLabels[value];
        if (kind == Kind.TOGGLE) return value == 1 ? "On" : "Off";
        if (tenths) return value / 10 + "." + value % 10 + " (0 = sharpest)";
        return value == defaultValue ? "Original" : value + " %";
    }

    /** CHOICE: the value (index) whose JSON form is id, or -1 if there is none. */
    int indexOfId(String id) {
        for (int i = 0; i < choiceIds.length; i++) {
            if (choiceIds[i].equals(id)) return i;
        }
        return -1;
    }
}
