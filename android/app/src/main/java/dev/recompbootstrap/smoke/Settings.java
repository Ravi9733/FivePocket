package dev.recompbootstrap.smoke;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/** The user's choice for every setting of SettingsSchema. Immutable; every value is valid for its setting. Pure Java. */
final class Settings {
    private static final Settings DEFAULTS = makeDefaults();

    /** One value per setting, in the order of SettingsSchema.all(). */
    private final int[] values;

    private Settings(int[] values) { this.values = values; }

    private static Settings makeDefaults() {
        List<SettingSpec> specs = SettingsSchema.all();
        int[] values = new int[specs.size()];
        for (int i = 0; i < values.length; i++) values[i] = specs.get(i).defaultValue;
        return new Settings(values);
    }

    /** Every setting at its default: the original game / the phone's behaviour without this screen. */
    static Settings defaults() {
        return DEFAULTS;
    }

    /** IllegalArgumentException for an unknown key. */
    int get(String key) {
        return values[SettingsSchema.indexOf(key)];
    }

    /** A copy with one value changed. IllegalArgumentException for an unknown key or a value the setting cannot hold. */
    Settings with(String key, int value) {
        int index = SettingsSchema.indexOf(key);
        if (!SettingsSchema.all().get(index).valid(value)) {
            throw new IllegalArgumentException(value + " is not a value of the setting " + key);
        }
        if (values[index] == value) return this;
        int[] copy = values.clone();
        copy[index] = value;
        return new Settings(copy);
    }

    boolean isDefault() {
        return equals(DEFAULTS);
    }

    /** Keys whose values differ between this and other, in schema order. */
    List<String> changedKeys(Settings other) {
        ArrayList<String> keys = new ArrayList<String>();
        List<SettingSpec> specs = SettingsSchema.all();
        for (int i = 0; i < values.length; i++) {
            if (values[i] != other.values[i]) keys.add(specs.get(i).key);
        }
        return Collections.unmodifiableList(keys);
    }

    @Override public boolean equals(Object o) {
        return o instanceof Settings && Arrays.equals(values, ((Settings) o).values);
    }

    @Override public int hashCode() {
        return Arrays.hashCode(values);
    }

    /** For logs and test messages: the JSON form without its line feed. */
    @Override public String toString() {
        return SettingsJson.encode(this).trim();
    }
}
