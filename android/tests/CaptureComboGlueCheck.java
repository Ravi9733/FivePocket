package dev.recompbootstrap.smoke;

/**
 * Compiled against android.jar, run on the host: the combo's key codes are Android's (javac copies the KeyEvent
 * constants into this class, so no Android class is loaded when it runs).
 */
public final class CaptureComboGlueCheck {
    public static void main(String[] args) {
        boolean ok = CaptureCombo.SELECT == android.view.KeyEvent.KEYCODE_BUTTON_SELECT
                && CaptureCombo.A == android.view.KeyEvent.KEYCODE_BUTTON_A
                && XboxGamepadMapper.button(CaptureCombo.SELECT) == 0x20 && XboxGamepadMapper.button(CaptureCombo.A) == 0x1000;
        System.out.println((ok ? "PASS     " : "FAIL     ") + "combo key codes are KEYCODE_BUTTON_SELECT / KEYCODE_BUTTON_A (guest Back 0x20 / A 0x1000)");
        if (!ok) System.exit(1);
    }
}
