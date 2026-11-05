package dev.recompbootstrap.smoke;

/**
 * Run with java -ea. The rule that says which connected controller plays, on the device lists a DualShock, an Xbox
 * Series pad and a handheld with a built-in pad give. Does not show that Android lists those devices this way on a
 * given phone: that is observed on a device (DEVICE-STEPS of the alpha build).
 */
public final class ControllerChoiceTest {
    private static final int NOT = 0, JOYSTICK = 1, PAD = 2, BUILT_IN = 3;

    public static void main(String[] args) {
        boolean assertions = false;
        assert assertions = true;
        if (!assertions) throw new IllegalStateException("Run with -ea");

        // ranks
        assert ControllerChoice.rank(false, true, true) == NOT : "an unsupported device is never a controller";
        assert ControllerChoice.rank(true, true, false) == BUILT_IN && ControllerChoice.rank(true, true, true) == BUILT_IN;
        assert ControllerChoice.rank(true, false, true) == PAD && ControllerChoice.rank(true, false, false) == JOYSTICK;

        // nothing listed, nothing chosen
        assert ControllerChoice.pick(ControllerChoice.NONE, new int[] {}, new int[] {}) == ControllerChoice.NONE;
        assert ControllerChoice.pick(ControllerChoice.NONE, new int[] {4, 5}, new int[] {NOT, NOT}) == ControllerChoice.NONE;
        assert ControllerChoice.pick(7, new int[] {4}, new int[] {NOT}) == ControllerChoice.NONE : "the chosen pad went away";

        // a phone and one Xbox Series pad: the pad (id 9) among a keyboard and a fingerprint reader
        assert ControllerChoice.pick(ControllerChoice.NONE, new int[] {2, 3, 9}, new int[] {NOT, NOT, PAD}) == 9;

        // a DualShock listed as motion sensors (a joystick without buttons, id 11) BEFORE its pad (id 12): the pad plays
        assert ControllerChoice.pick(ControllerChoice.NONE, new int[] {11, 12, 13}, new int[] {JOYSTICK, PAD, NOT}) == 12;
        // ... and the sensors alone are still better than nothing
        assert ControllerChoice.pick(ControllerChoice.NONE, new int[] {11}, new int[] {JOYSTICK}) == 11;

        // a handheld: its built-in pad (id 6) is the start, whatever the order
        assert ControllerChoice.pick(ControllerChoice.NONE, new int[] {12, 6}, new int[] {PAD, BUILT_IN}) == 6;
        assert ControllerChoice.pick(ControllerChoice.NONE, new int[] {6, 12}, new int[] {BUILT_IN, PAD}) == 6;
        // two equal pads: the first Android lists
        assert ControllerChoice.pick(ControllerChoice.NONE, new int[] {12, 14}, new int[] {PAD, PAD}) == 12;

        // the chosen pad stays while it is listed: a built-in pad does not take the game back by itself
        assert ControllerChoice.pick(12, new int[] {6, 12}, new int[] {BUILT_IN, PAD}) == 12;
        // ... it left: the built-in pad again
        assert ControllerChoice.pick(12, new int[] {6}, new int[] {BUILT_IN}) == 6;
        // ... it came back under a new number (a Bluetooth reconnect): the best listed
        assert ControllerChoice.pick(12, new int[] {6, 15}, new int[] {BUILT_IN, PAD}) == 6;
        // a listed device that stopped being a controller is not kept
        assert ControllerChoice.pick(12, new int[] {12, 15}, new int[] {NOT, PAD}) == 15;
        // negative numbers are never devices
        assert ControllerChoice.pick(-1, new int[] {-1, 15}, new int[] {PAD, PAD}) == 15;

        // adopt on press: a button press on another controller takes the game ...
        assert ControllerChoice.adopts(6, 12, true, true, true, false);
        assert ControllerChoice.adopts(ControllerChoice.NONE, 12, true, true, true, false);
        // ... and nothing else does
        assert !ControllerChoice.adopts(12, 12, true, true, true, false) : "the same controller";
        assert !ControllerChoice.adopts(6, 12, true, true, false, false) : "a release";
        assert !ControllerChoice.adopts(6, 12, true, false, true, false) : "a key that is no gamepad button";
        assert !ControllerChoice.adopts(6, 12, false, true, true, false) : "a device that is no controller";
        assert !ControllerChoice.adopts(6, 12, true, true, true, true) : "a test event";
        assert !ControllerChoice.adopts(6, -1, true, true, true, false) : "no device";
        System.out.println("ControllerChoiceTest passed");
    }
}
