package dev.recompbootstrap.smoke;

import java.util.Locale;

/**
 * Every word of the controller check (ControllerCheckActivity, debug source set), which only draws what this class
 * returns. Pure Java. The screen is for a player who wants to see that every button, trigger and stick reaches the
 * game: it names what is pressed now and what has been seen so far, and gives the one way out. It carries no
 * developer wording (no snapshot dump, no packet counter, no notes about test methods).
 *
 * Buttons are the game's (Xbox 360) button bits, as the game receives them.
 */
final class ControllerCheckText {
    private ControllerCheckText() {}

    static final String TITLE = "Controller Check";
    static final String TEXT_DO = "Press every button, move both sticks all the way round and squeeze both triggers.";
    /** The one way out: B is one of the buttons under test, so the screen is left with a held pair. */
    static final String TEXT_LEAVE = "To leave, hold Select + Start for one second.";
    /** Without a controller the held pair cannot be pressed: the page then says what is the case and names Android's own way out. */
    static final String TEXT_NO_CONTROLLER = "No controller is connected. Connect one and this page shows its buttons, sticks and triggers.";
    static final String TEXT_LEAVE_WITHOUT = "To leave now, use this device's Back button or Back gesture.";
    static final String TEXT_NAMES = "The controller's buttons and the game's:\n"
            + "A, B, X, Y = the same letters     L1, R1 = LB, RB     L2, R2 = LT, RT\n"
            + "Select = Back     Start = Start     L3, R3 = the stick clicks";
    static final String TEXT_UNREADABLE = "The controller cannot be read at the moment.";
    /**
     * On the page that cannot read the controller, after the held pair: that page is shown whatever is connected, also
     * with no controller at all, and then the held pair cannot be pressed. Android's Back leaves in every case (the
     * window takes keys of the selected controller only).
     */
    static final String TEXT_LEAVE_ALSO = "If that does nothing, use this device's Back button or Back gesture.";
    static final String NOTHING = "nothing", NOTHING_YET = "nothing yet";

    private static final int[] MASKS = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 4096, 8192, 16384, 32768};
    private static final String[] NAMES = {"Up", "Down", "Left", "Right", "Start", "Back", "L3", "R3", "LB", "RB",
            "Guide", "A", "B", "X", "Y"};
    /** The buttons a check is complete with: every named one but Guide, which few devices have. */
    static final int ALL_BUTTONS = 1 | 2 | 4 | 8 | 16 | 32 | 64 | 128 | 256 | 512 | 4096 | 8192 | 16384 | 32768;

    /** The names of the buttons whose bits are set, in a fixed order, separated by spaces; "" for none. */
    static String buttonNames(int bits) {
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < MASKS.length; ++i) {
            if ((bits & MASKS[i]) != 0) {
                out.append(out.length() > 0 ? " " : "").append(NAMES[i]);
            }
        }
        return out.toString();
    }

    /**
     * The controller's own name out of the input path's description ("AYN Thor / Odin -> Xbox 360, player 1\nOdin
     * Controller (id 5)\n..."): its second line without the device number. null when there is no controller.
     */
    static String controllerName(String description) {
        if (description == null) {
            return null;
        }
        String[] lines = description.split("\n");
        if (lines.length < 2) {
            return null;
        }
        String name = lines[1];
        int id = name.lastIndexOf(" (id ");
        name = (id >= 0 ? name.substring(0, id) : name).trim();
        return name.isEmpty() ? null : name;
    }

    /**
     * The whole screen. buttons, the triggers (0 to 255) and the sticks (-32768 to 32767) are what the game receives
     * now; seenButtons, the two trigger flags and stickDirections (one bit per direction of each of the four stick
     * axes, eight in all) are what a physical press has shown since the screen was opened.
     */
    static String screen(String description, int buttons, int leftTrigger, int rightTrigger, int leftX, int leftY,
            int rightX, int rightY, int seenButtons, boolean leftTriggerSeen, boolean rightTriggerSeen,
            int stickDirections) {
        String name = controllerName(description);
        String pressed = buttonNames(buttons), seen = buttonNames(seenButtons);
        String missing = buttonNames(ALL_BUTTONS & ~seenButtons);
        if (name == null) {
            // Nothing can be pressed: no rows of zeros, and not the way out that needs two buttons.
            return TITLE + "\n" + TEXT_NO_CONTROLLER + "\n" + TEXT_LEAVE_WITHOUT + "\n\n"
                    + "Controller: none found\n\n" + TEXT_NAMES;
        }
        return TITLE + "\n" + TEXT_DO + "\n" + TEXT_LEAVE + "\n\n"
                + "Controller: " + name + "\n\n"
                + "Pressed now: " + (pressed.isEmpty() ? NOTHING : pressed) + "\n"
                + String.format(Locale.ROOT, "Left trigger: %d     Right trigger: %d\n", leftTrigger, rightTrigger)
                + String.format(Locale.ROOT, "Left stick: %d, %d     Right stick: %d, %d\n\n", leftX, leftY, rightX, rightY)
                + "Buttons seen so far: " + (seen.isEmpty() ? NOTHING_YET : seen) + "\n"
                + "Still to press: " + (missing.isEmpty() ? NOTHING : missing) + "\n"
                + "Triggers seen: left " + (leftTriggerSeen ? "yes" : "no") + ", right " + (rightTriggerSeen ? "yes" : "no")
                + "     Stick directions seen: " + Integer.bitCount(stickDirections & 255) + " of 8\n\n"
                + TEXT_NAMES;
    }

    /**
     * The screen while the game's view of the controller cannot be read: that, and both ways out. The page cannot
     * tell here whether a controller is connected, so it names the held pair and the way that needs no controller.
     */
    static String unreadable() {
        return TITLE + "\n" + TEXT_UNREADABLE + "\n" + TEXT_LEAVE + "\n" + TEXT_LEAVE_ALSO;
    }
}
