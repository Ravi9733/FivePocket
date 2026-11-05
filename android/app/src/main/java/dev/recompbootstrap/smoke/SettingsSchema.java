package dev.recompbootstrap.smoke;

import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/**
 * The twelve user-facing settings of schema version 1 (design sections 2.5 and 3.1). Pure Java.
 *
 * Every default but the scaler's is "the original game / what the game does with no setting file": 60 fps cap (no cap
 * file), sharpness 0.2 stops, 100 % render resolution, no frame generation (an opt-in: no switch file), no
 * frame-rate counter, every world slider at Original, buffered stick input. The scaler's default is FSR 1, which is
 * what an install has standing through its two switch files (diag-v450-scale-fsr1 and diag-v409-scale-cubic; the
 * install kit's list of standing switches names both). Game build v464 with neither switch and no scaler.txt scales
 * with Nearest (scaler_settings.h, poll): on such a files directory the row shows Nearest, and Restore Original
 * followed by Save writes settings/scaler.txt with "fsr1". "Restore original" is every key at its default.
 *
 * Ranges follow the game runtime's readers (local-analysis/agents/app-ui-2/CONTRACTS.md names the source lines):
 * the four density sliders 25 to 100 (world_load.h DensityFloor), draw distance 70 to 100 (LodFloor), render
 * resolution the named steps of render_scale.h (each an even number of sixteenths of 1280x720).
 */
final class SettingsSchema {
    private SettingsSchema() { }

    static final int VERSION = 1;

    static final String FRAME_CAP = "frameCap", SCALER = "scaler", FSR_SHARPNESS = "fsrSharpnessTenths",
            RENDER_SCALE = "renderScalePercent", FRAME_GENERATION = "frameGeneration", FPS_OVERLAY = "fpsOverlay",
            WORLD_TRAFFIC = "worldTraffic", WORLD_PEDESTRIANS = "worldPedestrians", WORLD_PARKED_CARS = "worldParkedCars",
            WORLD_SCENARIO_PEDESTRIANS = "worldScenarioPedestrians", WORLD_DRAW_DISTANCE = "worldDrawDistance",
            INPUT_UNBUFFERED = "inputUnbuffered";

    /** Values (choice indexes) of frameCap. */
    static final int CAP_OFF = 0, CAP_60 = 1, CAP_40 = 2, CAP_30 = 3, CAP_24 = 4, CAP_AUTO = 5;
    /** Values of scaler. */
    static final int SCALER_FSR1 = 0, SCALER_GSR1 = 1, SCALER_BICUBIC = 2, SCALER_NEAREST = 3;
    /** Values of frameGeneration: off, or one of the two orders of frame-gen/DESIGN-stage1.md section 1. */
    static final int FG_OFF = 0, FG_BETWEEN = 1, FG_AHEAD = 2;
    /** Default of fsrSharpnessTenths: 0.2 stops, the runtime's own default (fsr1::DefaultStops). */
    static final int SHARPNESS_DEFAULT = 2;
    /** Value of renderScalePercent that stands for 100 %. */
    static final int RENDER_SCALE_100 = 3;
    /** The other values of renderScalePercent (the places of "50", "75", "88", "115", "125" and "150" among its ids). */
    static final int RENDER_SCALE_50 = 0, RENDER_SCALE_75 = 1, RENDER_SCALE_88 = 2, RENDER_SCALE_115 = 4, RENDER_SCALE_125 = 5,
            RENDER_SCALE_150 = 6;

    /** Lowest value of the four density sliders and of draw distance: the runtime raises anything lower to these. */
    static final int WORLD_DENSITY_MIN = 25, WORLD_DRAW_DISTANCE_MIN = 70;

    private static final List<SettingSpec> ALL = Collections.unmodifiableList(Arrays.asList(
            SettingSpec.choice(FRAME_CAP, "Picture", "Frame rate cap",
                    new String[] {"off", "60", "40", "30", "24", "auto"},
                    new String[] {"Off", "60 fps (default)", "40 fps", "30 fps (original console cap)", "24 fps", "Automatic"},
                    CAP_60, false),
            SettingSpec.choice(SCALER, "Picture", "Scaler",
                    new String[] {"fsr1", "gsr1", "bicubic", "nearest"},
                    new String[] {"FSR 1 (default)", "Snapdragon GSR 1", "Bicubic", "Nearest"},
                    SCALER_FSR1, false),
            SettingSpec.slider(FSR_SHARPNESS, "Picture", "FSR sharpness", 0, 40, 1, SHARPNESS_DEFAULT, true),
            // The named steps of lane fsr-modes (render_scale.h: 8, 12, 14, 16, 18, 20 and 24 sixteenths of 1280x720).
            // 100 must stay at index RENDER_SCALE_100.
            SettingSpec.choice(RENDER_SCALE, "Picture", "Render resolution",
                    new String[] {"50", "75", "88", "100", "115", "125", "150"},
                    new String[] {"50 % (640x360)", "75 % (960x540, Performance)", "88 % (1120x630, Balanced)",
                            "100 % (1280x720, original)", "115 % (1440x810, supersampling)",
                            "125 % (1600x900, supersampling)", "150 % (1920x1080, supersampling)"},
                    RENDER_SCALE_100, true),
            SettingSpec.choice(FRAME_GENERATION, "Picture", "Frame generation (experimental)",
                    new String[] {"off", "between", "ahead"},
                    new String[] {"Off", "In-Between Frames", "Predicted Frames"},
                    FG_OFF, false),
            SettingSpec.toggle(FPS_OVERLAY, "Picture", "Frame-rate counter"),
            SettingSpec.slider(WORLD_TRAFFIC, "World", "Traffic", WORLD_DENSITY_MIN, 100, 5, 100, false),
            SettingSpec.slider(WORLD_PEDESTRIANS, "World", "Pedestrians", WORLD_DENSITY_MIN, 100, 5, 100, false),
            SettingSpec.slider(WORLD_PARKED_CARS, "World", "Parked cars", WORLD_DENSITY_MIN, 100, 5, 100, false),
            SettingSpec.slider(WORLD_SCENARIO_PEDESTRIANS, "World", "Scenario pedestrians", WORLD_DENSITY_MIN, 100, 5, 100, false),
            SettingSpec.slider(WORLD_DRAW_DISTANCE, "World", "Draw distance", WORLD_DRAW_DISTANCE_MIN, 100, 5, 100, false),
            SettingSpec.toggle(INPUT_UNBUFFERED, "Controller", "Low-delay stick input")));

    /** One line of the row (about 100 characters fit): what a render size above 100 % is in a build without stage B, and is not. */
    static final String HINT_SUPERSAMPLING = "Supersampling through a 1280x720 picture: smoother edges, more heat. Not native 1080p.";
    static final String HINT_SOFTER = "Softer and cooler: fewer pixels are drawn.";

    /**
     * Render resolution in a build with stage B (Capabilities.renderStageB): the five steps are named modes, and the
     * texts say what the phone showed and measured with game build v464 on 2026-10-05 (docs/fable/DISCOVERIES.md:
     * 08:29 the picture, 09:49 Native's cost, 11:39 the house frame of 115 % and 88 % - two boots each against one
     * boot at 100 %, no power figure; 06:22 and 07:38 the cost of 75 %, taken with v463). 125 % has no measurement,
     * so its text (HELP_MORE) says only what it is.
     */
    static final String HELP_NATIVE = "The game draws every pixel of this screen. Sharpest picture. Measured on an AYN Thor: "
            + "about 3 ms more per frame (49 to 43 fps indoors), slightly more power. Scaler and sharpness do nothing here.";
    static final String HELP_MORE = "More pixels than the original, then enlarged by the scaler.";
    static final String HELP_ULTRA = HELP_MORE + " Measured on an AYN Thor: about 1 to 1.5 ms more per frame indoors.";
    static final String HELP_QUALITY = "The original game's size, enlarged by the scaler.";
    static final String HELP_BALANCED = "Fewer pixels than the original, then enlarged by the scaler. Measured on an AYN Thor: "
            + "no faster than Quality indoors.";
    static final String HELP_PERFORMANCE = "Fewer pixels, enlarged by the scaler. Measured on an AYN Thor: "
            + "about half a millisecond faster, little or no power saved. Softer than Quality.";
    /** Before the text of 50 % and 125 %: values the game honours and the row shows, but does not offer. */
    static final String HELP_NOT_A_MODE = "Not one of the five modes. ";
    /** What the game does with a size it does not hand to the scaler as it is: 50 % always, the others with Bicubic or Nearest. */
    static final String HELP_THROUGH_720 = "The picture goes through 1280x720 first: softer.";
    /** The game reads render-scale.txt once, when it starts. */
    static final String HELP_RESTART = "A change needs a restart of the game.";

    /** True for the five named modes of a stage B build: every step but 50 % and 125 %. */
    static boolean renderMode(int value) {
        return value != RENDER_SCALE_50 && value != RENDER_SCALE_125;
    }

    /** The text of a render-resolution value in a stage B build. The render size stands in every one of them. */
    static String renderModeLabel(int value) {
        switch (value) {
            case RENDER_SCALE_50: return "50 %: 640x360, upscaled";
            case RENDER_SCALE_75: return "Performance: 960x540, upscaled";
            case RENDER_SCALE_88: return "Balanced: 1120x630, upscaled";
            case RENDER_SCALE_100: return "Quality: 1280x720, original";
            case RENDER_SCALE_115: return "Ultra: 1440x810, upscaled";
            case RENDER_SCALE_125: return "125 %: 1600x900, upscaled";
            case RENDER_SCALE_150: return "Native: 1920x1080, no upscaling";
            default: throw new IllegalArgumentException(value + " is not a render resolution");
        }
    }

    /**
     * The help strip of the Render Resolution row in a stage B build, one entry per line: what the value is (with
     * what was measured), then - for a size the scaler in use does not take as it is - that the picture is softer,
     * then that a change needs a restart. scaler is the scaler in use (a value of the Scaler setting).
     */
    static String[] renderModeHelp(int value, int scaler) {
        String what = value == RENDER_SCALE_150 ? HELP_NATIVE : value == RENDER_SCALE_115 ? HELP_ULTRA
                : value == RENDER_SCALE_100 ? HELP_QUALITY : value == RENDER_SCALE_88 ? HELP_BALANCED
                : value == RENDER_SCALE_75 ? HELP_PERFORMANCE : value == RENDER_SCALE_50 ? HELP_THROUGH_720 : HELP_MORE;
        if (!renderMode(value)) what = HELP_NOT_A_MODE + what;
        // 50 % says it already; at 100 % nothing is resized before the scaler; Native has no scaler.
        boolean sized = value != RENDER_SCALE_50 && value != RENDER_SCALE_100 && value != RENDER_SCALE_150;
        if (sized && (scaler == SCALER_BICUBIC || scaler == SCALER_NEAREST)) {
            return new String[] {what, "With " + spec(SCALER).format(scaler) + " as the scaler the picture goes through 1280x720 first: softer.",
                    HELP_RESTART};
        }
        return new String[] {what, HELP_RESTART};
    }

    /**
     * 40 fps at Native, measured with game build v464 (DISCOVERIES 2026-10-05 10:02, one boot): in the house 67 % of
     * frames were shown for the cap's three refreshes and 8.4 % of slots were late; outdoors 59 % were late.
     */
    static final String HINT_40_NATIVE = "Measured on an AYN Thor at Native: uneven indoors (two frames in three evenly paced). Does not hold outdoors.";
    /**
     * 30 fps at Native, measured with game build v464 (DISCOVERIES 2026-10-05 12:06, one boot): in the house 70 % of
     * frames were shown for the cap's four refreshes and 0.3 % of slots were late; outdoors 82 % and 4.7 %.
     */
    static final String HINT_30_NATIVE = "Measured on an AYN Thor at Native: holds indoors and outdoors (70 to 82 % of frames evenly paced).";
    /** What "Measured:" becomes in a frame-cap note shown at another render size than the one it was measured at. */
    static final String MEASURED = "Measured on an AYN Thor:", MEASURED_AT_QUALITY = "Measured on an AYN Thor at Quality:";

    /**
     * The note under a frame cap in a stage B build, where the render resolution has modes; null when there is none.
     * The caps were measured at the original size (hint), so at every other render size the note says "at Quality";
     * 40 fps and 30 fps at Native have measurements of their own. render is a value of the Render Resolution setting.
     */
    static String capNote(int cap, int render) {
        if (cap == CAP_40 && render == RENDER_SCALE_150) return HINT_40_NATIVE;
        if (cap == CAP_30 && render == RENDER_SCALE_150) return HINT_30_NATIVE;
        String note = hint(FRAME_CAP, cap);
        return note == null || render == RENDER_SCALE_100 ? note : note.replace(MEASURED, MEASURED_AT_QUALITY);
    }

    /**
     * What each order of frame generation is, as its design defines it (frame-gen/DESIGN-stage1.md section 1: between
     * shows the drawn frame two refreshes later; ahead shows it when the cap alone would, and its made picture is a
     * prediction, wrong for one picture after an abrupt camera change). Shown only in a build that announces its
     * generator; nothing here is a measurement.
     */
    static final String HINT_FG_BETWEEN = "Experimental; may switch itself off. Made pictures go between drawn frames, two refreshes late.";
    static final String HINT_FG_AHEAD = "Experimental; may switch itself off. A predicted picture follows each frame; one wrong on a turn.";

    /**
     * True when frame generation can make its second picture under this frame cap in every build that has it: only the
     * fixed 30 fps cap gives the four-refresh frame a pair needs (frame-gen/DESIGN-stage1.md section 6, gate condition 1;
     * under Automatic the pacer may pick another rate, so the gate stays closed there - but for a build that announces
     * pairs under the automatic cap, which Capabilities.generationPaced adds). cap is a value of the Frame Rate Cap setting.
     */
    static boolean generationPaced(int cap) {
        return cap == CAP_30;
    }

    /**
     * True for the frame caps under which the frame-rate counter has been seen on the screen with game build v464: the
     * 60 fps cap only (SettingsPages.counterNote has the evidence). When a cap has been looked at on a device, this is
     * the one place to change. cap is a value of the Frame Rate Cap setting.
     */
    static boolean counterSeenUnder(int cap) {
        return cap == CAP_60;
    }

    /**
     * One line that says what a value means, shown under the row when nothing more urgent stands there; null when
     * there is nothing to say. The frame-cap figures were measured on the phone (40: game builds v460 and v462,
     * DISCOVERIES 2026-10-05 01:53, 05:55 and 06:30; 30 and Automatic: v460).
     */
    static String hint(String key, int value) {
        if (key.equals(FRAME_CAP)) {
            return value == CAP_40 ? "Measured on an AYN Thor: holds indoors. The Thor's second-screen dashboard makes some frames uneven."
                    : value == CAP_30 ? "Measured on an AYN Thor: holds while driving (84 % of frames evenly paced)."
                    : value == CAP_AUTO ? "Picks the cap itself. Measured on an AYN Thor: 40 indoors, 24 while driving." : null;
        }
        if (key.equals(RENDER_SCALE)) {
            return value > RENDER_SCALE_100 ? HINT_SUPERSAMPLING : value < RENDER_SCALE_100 ? HINT_SOFTER : null;
        }
        if (key.equals(FRAME_GENERATION)) {
            return value == FG_BETWEEN ? HINT_FG_BETWEEN : value == FG_AHEAD ? HINT_FG_AHEAD : null;
        }
        if (value == 100) return null;
        if (key.equals(WORLD_TRAFFIC)) return "Moving cars the game adds around you.";
        if (key.equals(WORLD_PEDESTRIANS)) return "People walking around.";
        if (key.equals(WORLD_PARKED_CARS)) return "Cars parked along streets and in car parks.";
        if (key.equals(WORLD_SCENARIO_PEDESTRIANS)) return "People at fixed spots. Multiplies with Pedestrians.";
        if (key.equals(WORLD_DRAW_DISTANCE)) return "Also shortens how far away traffic and people are simulated.";
        return null;
    }

    /** The twelve settings in the row order of the settings screen (rows 1-12). Unmodifiable. */
    static List<SettingSpec> all() {
        return ALL;
    }

    /** IllegalArgumentException for an unknown key. */
    static SettingSpec spec(String key) {
        return ALL.get(indexOf(key));
    }

    /** Position of a key in all(). IllegalArgumentException for an unknown key. */
    static int indexOf(String key) {
        for (int i = 0; i < ALL.size(); i++) {
            if (ALL.get(i).key.equals(key)) return i;
        }
        throw new IllegalArgumentException("unknown setting: " + key);
    }
}
