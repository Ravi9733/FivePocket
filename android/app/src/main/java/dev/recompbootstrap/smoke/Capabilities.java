package dev.recompbootstrap.smoke;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Set;
import java.util.TreeSet;

/**
 * Which setting values the installed game build can honour (design section 3.3). Pure Java, immutable.
 *
 * The knowledge comes from the names of the input files the game library contains (CapabilityScanner finds them).
 * A default value is always supported. With no game library nothing but the defaults is supported.
 */
final class Capabilities {
    /**
     * Marker strings. A marker is not a file: it is a text a game build carries (zero-terminated, like a file name)
     * to announce a feature that a file name alone cannot prove.
     * GSR1_MARKER: the build contains the Snapdragon GSR 1 pass AND takes the word gsr1 from settings/scaler.txt
     * (in the library from v462: direct_present.h, the RENDER_DIRECT_GSR log format ends with it).
     * WORLD_LOD_MARKER: the build honours draw_distance in world-load.txt without a developer file (proposed by lane
     * world-load, SETTINGS-SCREEN.md section 6; in no build yet).
     * FRAME_GENERATION_MARKER: the build contains the picture generator of frame-generation stage 1b and is meant to be
     * switched by this screen (local-analysis/agents/frame-gen/DESIGN-stage1.md sections 1, 6 and 7): it makes a second
     * picture per drawn frame while files/private-trace/diag-fg-between or diag-fg-ahead exists (both: between) and
     * the pacer's fixed 30 fps cap (diag-pace-30) paces the game; with neither file every decision is as without the
     * generator. No build carries it yet (v464 holds only the timing dry run, diag-fg-pair-ahead / diag-fg-pair-between,
     * which makes no picture and is deliberately not looked at). The marker alone enables nothing: a value is offered
     * only when the build also contains its switch file's name and the name of the 30 fps cap file.
     * FRAME_GENERATION_AUTO_MARKER: the build makes the generator's in-between picture also while the automatic cap
     * (diag-pace-auto) paces the game, that cap's lower range of 20 and 15 fps included, when diag-fg-between and
     * diag-pace-auto-low exist beside the cap's file (written for game build v524: fg_gen.h
     * RECOMP_FGG_AUTO_CAPABILITY). Builds before it (v522 and v523 among them) read diag-pace-auto-low as a developer's
     * switch and do not carry the marker: for them nothing changes. This marker alone enables nothing either: the build
     * must also contain the name of diag-pace-auto-low (generationUnderAutomatic), and the in-between order must be a
     * value it has.
     * RENDER_STAGE_B_MARKER: the build shows a render size as it is drawn (render-resolution stage B, in the library
     * from v464: the end of a RENDER_DIRECT_SOURCE log format). With FSR 1 or GSR 1 the scaler works from the render
     * size, and exactly 1920x1080 on a 1920x1080 screen is shown 1:1 with no scaler and no sharpening. A build
     * without it (v463) brings every render size to 1280x720 first.
     */
    static final String GSR1_MARKER = "recomp-cap:gsr1";
    static final String WORLD_LOD_MARKER = "recomp-cap:wl-lod";
    static final String FRAME_GENERATION_MARKER = "recomp-cap:framegen1";
    static final String FRAME_GENERATION_AUTO_MARKER = "recomp-cap:framegen-auto1";
    static final String RENDER_STAGE_B_MARKER = "recomp-cap:render-stage-b";

    static final String REASON_NO_LIBRARY = "Game code is not installed";
    static final String REASON_NOT_IN_BUILD = "Not in the installed game build";
    static final String REASON_NO_UNCAPPED = "No game build can run without a frame cap yet";
    static final String REASON_INPUT = "Needs Android 11 or newer and an app build that has this option";
    static final String REASON_NOT_A_VALUE = "Not a value of this setting";
    static final String REASON_NOT_BUILT = "Not in this version";
    static final String REASON_NEEDS_463 = REASON_NOT_IN_BUILD + " (needs game build v463 or newer)";
    static final String REASON_LOD_LOCKED = "Not in this version";

    private final TreeSet<String> names;
    private final boolean libraryPresent;
    private final boolean inputOptionInApp;

    private Capabilities(Set<String> names, boolean libraryPresent, boolean inputOptionInApp) {
        this.names = new TreeSet<String>(names);
        this.libraryPresent = libraryPresent;
        this.inputOptionInApp = inputOptionInApp;
    }

    /**
     * namesInLibrary: the probe names found in the game library (after the lead's override file was applied).
     * libraryPresent: false when there is no game library; the names are then ignored.
     * inputOptionInApp: the low-delay input option exists in this app build and the device runs Android 11 or newer.
     */
    static Capabilities of(Set<String> namesInLibrary, boolean libraryPresent, boolean inputOptionInApp) {
        return new Capabilities(libraryPresent ? namesInLibrary : Collections.<String>emptySet(), libraryPresent, inputOptionInApp);
    }

    /** True when the game library contains that name (a base name such as "diag-pace-40", or the GSR 1 marker). */
    boolean has(String name) {
        return names.contains(name);
    }

    /** has() for a path of RuntimeMapping, e.g. "private-trace/diag-pace-40". */
    boolean reads(String ownedPath) {
        return has(RuntimeMapping.name(ownedPath));
    }

    /** IllegalArgumentException for an unknown key. A value the setting cannot hold is not supported. */
    boolean valueSupported(String key, int value) {
        return reason(key, value) == null;
    }

    /** null when the value is supported; otherwise the one-line reason shown under the row. */
    String reason(String key, int value) {
        SettingSpec spec = SettingsSchema.spec(key);
        if (!spec.valid(value)) return REASON_NOT_A_VALUE;
        if (value == spec.defaultValue) return null;
        if (!libraryPresent) return REASON_NO_LIBRARY;
        if (key.equals(SettingsSchema.FRAME_CAP)) {
            if (value == SettingsSchema.CAP_OFF) return REASON_NO_UNCAPPED;
            if (value == SettingsSchema.CAP_30) {
                return reads(RuntimeMapping.PACE_30) || reads(RuntimeMapping.LIMIT30) ? null : needs(460);
            }
            String file = value == SettingsSchema.CAP_40 ? RuntimeMapping.PACE_40
                    : value == SettingsSchema.CAP_24 ? RuntimeMapping.PACE_24 : RuntimeMapping.PACE_AUTO;
            return reads(file) ? null : needs(460);
        }
        if (key.equals(SettingsSchema.SCALER)) {
            // Nearest is "no scaler input says otherwise": a build that reads no scaler input at all cannot be told.
            if (value == SettingsSchema.SCALER_NEAREST) return scalerInputs() ? null : REASON_NOT_IN_BUILD;
            if (value == SettingsSchema.SCALER_BICUBIC) {
                return reads(RuntimeMapping.SCALER_TXT) || reads(RuntimeMapping.SCALE_CUBIC) ? null : REASON_NOT_IN_BUILD;
            }
            return reads(RuntimeMapping.SCALER_TXT) && has(GSR1_MARKER) ? null : needs(462);
        }
        if (key.equals(SettingsSchema.FSR_SHARPNESS)) {
            return reads(RuntimeMapping.SHARPNESS_TXT) || reads(RuntimeMapping.FSR1_STOPS) ? null : REASON_NOT_IN_BUILD;
        }
        if (key.equals(SettingsSchema.RENDER_SCALE)) {
            return reads(RuntimeMapping.RENDER_SCALE_TXT) ? null : REASON_NEEDS_463;
        }
        if (key.equals(SettingsSchema.FRAME_GENERATION)) {
            // Live only in a build that announces its generator; the dry-run names prove nothing (see the marker's text).
            if (!has(FRAME_GENERATION_MARKER)) return REASON_NOT_BUILT;
            String file = value == SettingsSchema.FG_BETWEEN ? RuntimeMapping.FG_BETWEEN : RuntimeMapping.FG_AHEAD;
            // The generator makes its pictures only under the pacer's 30 fps cap, so that file must be in the build too.
            return reads(file) && reads(RuntimeMapping.PACE_30) ? null : REASON_NOT_IN_BUILD;
        }
        if (key.equals(SettingsSchema.FPS_OVERLAY)) {
            return reads(RuntimeMapping.FPS_OVERLAY) ? null : REASON_NOT_IN_BUILD;
        }
        if (key.equals(SettingsSchema.INPUT_UNBUFFERED)) {
            return inputOptionInApp ? null : REASON_INPUT;
        }
        // the world sliders share one input file; draw distance is also locked inside the runtime
        if (!reads(RuntimeMapping.WORLD_LOAD_TXT)) return needs(461);
        if (key.equals(SettingsSchema.WORLD_DRAW_DISTANCE)) return has(WORLD_LOD_MARKER) ? null : REASON_LOD_LOCKED;
        return null;
    }

    /** True when the build reads any input that chooses the scaler (the word file or one of the two switch files). */
    boolean scalerInputs() {
        return reads(RuntimeMapping.SCALER_TXT) || reads(RuntimeMapping.SCALE_FSR1) || reads(RuntimeMapping.SCALE_CUBIC);
    }

    private static String needs(int build) {
        return REASON_NOT_IN_BUILD + " (needs game build v" + build + " or newer)";
    }

    /** True when the build reads render-scale.txt and shows the render size as it is drawn (RENDER_STAGE_B_MARKER). */
    boolean renderStageB() {
        return has(RENDER_STAGE_B_MARKER) && reads(RuntimeMapping.RENDER_SCALE_TXT);
    }

    /**
     * True when the build makes frame generation's in-between picture under the automatic cap too: it announces that
     * (FRAME_GENERATION_AUTO_MARKER) and reads the switch file of that cap's lower range, which RuntimeMapping then
     * writes and removes together with the generation switch.
     */
    boolean generationUnderAutomatic() {
        return has(FRAME_GENERATION_AUTO_MARKER) && reads(RuntimeMapping.PACE_AUTO_LOW);
    }

    /**
     * True when frame generation makes its second picture under this frame cap with this order chosen: either order
     * under the fixed 30 fps cap (SettingsSchema.generationPaced), and the in-between order under the automatic cap in
     * a build of generationUnderAutomatic(). The predicted order under the automatic cap is as in every build before:
     * no picture. cap and generation are values of the Frame Rate Cap and Frame Generation settings.
     */
    boolean generationPaced(int cap, int generation) {
        if (SettingsSchema.generationPaced(cap)) return true;
        return cap == SettingsSchema.CAP_AUTO && generation == SettingsSchema.FG_BETWEEN && generationUnderAutomatic();
    }

    /**
     * True when the row offers the value as a step. Every supported value is offered, with one exception: in a
     * stage B build the Render Resolution row has five named modes, and 50 % and 125 % are not among them. Those two
     * stay supported - the build honours them, so a saved one is shown and written as it is - but the row does not
     * step onto them.
     */
    boolean offered(String key, int value) {
        if (!valueSupported(key, value)) return false;
        return !(key.equals(SettingsSchema.RENDER_SCALE) && renderStageB() && !SettingsSchema.renderMode(value));
    }

    /** The value itself when it is supported, otherwise the setting's default. */
    int effective(String key, int value) {
        return valueSupported(key, value) ? value : SettingsSchema.spec(key).defaultValue;
    }

    /** True when at least one non-default value is supported (false: the row is disabled). */
    boolean anyChoice(String key) {
        SettingSpec spec = SettingsSchema.spec(key);
        for (int value = spec.min; value <= spec.max; value += spec.step) {
            if (value != spec.defaultValue && valueSupported(key, value)) return true;
        }
        return false;
    }

    /** The names found in the game library, sorted (for the About screen). Unmodifiable. */
    List<String> names() {
        return Collections.unmodifiableList(new ArrayList<String>(names));
    }
}
