package dev.recompbootstrap.smoke;

/**
 * vm-pad-hold D3: D-pad taps of 359 / 322 / 108 ms and a 66 ms stick tap were lost while frames took 0.7-1.4 s:
 * Android batches joystick MOVE events per frame and the menu read only the newest sample, so a press and its release
 * inside one batch cancelled out. NavInput.motion reads every sample of the event, the batched ones first.
 */
public final class BatchedTapTest {
    private static final String S = "BatchedTap";

    /** A joystick event: samples[position] = {hatX, hatY, stickX, stickY}; the last one is the current sample. */
    private static NavInput.Motion event(final float[]... samples) {
        return new NavInput.Motion() {
            @Override public int history() {
                return samples.length - 1;
            }

            @Override public float value(int axis, int position) {
                float[] sample = samples[position];
                switch (axis) {
                    case NavInput.AXIS_HAT_X: return sample[0];
                    case NavInput.AXIS_HAT_Y: return sample[1];
                    case NavInput.AXIS_X: return sample[2];
                    case NavInput.AXIS_Y: return sample[3];
                    default: return 0f;
                }
            }

            @Override public long time(int position) {
                return position;   // 1 ms apart, oldest first (only D-pad key pairing reads it: PadKindsTest)
            }
        };
    }

    private static final float[] CENTRE = {0f, 0f, 0f, 0f}, HAT_DOWN = {0f, 1f, 0f, 0f};

    public static void main(String[] args) {
        MenuFixKit.requireAssertions();
        MenuFixKit.test(S, "a D-pad tap whose press and release came in one event moves once", () -> {
            MenuFixKit.Rig rig = new MenuFixKit.Rig();
            MenuFixKit.check(rig.input.motion(event(HAT_DOWN, CENTRE)), "the event is reported as a change");
            rig.at(2000);
            MenuFixKit.eq("DOWN@0", rig.take(), "one step, no repeat");
        });
        MenuFixKit.test(S, "two D-pad taps in one event (the Frame Rate rows case) move twice", () -> {
            MenuFixKit.Rig rig = new MenuFixKit.Rig();
            rig.input.motion(event(HAT_DOWN, CENTRE, HAT_DOWN, CENTRE));
            rig.at(2000);
            MenuFixKit.eq("DOWN@0 DOWN@0", rig.take(), "two steps");
        });
        MenuFixKit.test(S, "a 66 ms stick tap (-0.58) inside one event moves once", () -> {
            MenuFixKit.Rig rig = new MenuFixKit.Rig();
            rig.input.motion(event(new float[] {0f, 0f, -0.3f, 0f}, new float[] {0f, 0f, -0.58f, 0f}, CENTRE));
            MenuFixKit.eq("LEFT@0", rig.take(), "one step");
        });
        MenuFixKit.test(S, "samples are read oldest first", () -> {
            MenuFixKit.Rig rig = new MenuFixKit.Rig();
            rig.input.motion(event(new float[] {0f, 0f, 0f, -0.9f}, CENTRE, new float[] {0f, 0f, 0f, 0.9f}));
            MenuFixKit.eq("UP@0 DOWN@0", rig.take(), "UP, then DOWN (held)");
        });
        MenuFixKit.test(S, "samples are read oldest first: five samples, two taps, in their order", () -> {
            MenuFixKit.Rig rig = new MenuFixKit.Rig();
            rig.input.motion(event(new float[] {0f, -1f, 0f, 0f}, CENTRE, HAT_DOWN, CENTRE, CENTRE));
            MenuFixKit.eq("UP@0 DOWN@0", rig.take(), "UP, then DOWN");
        });
        MenuFixKit.test(S, "control: a held stick batched many times is one press; an event without history is one sample", () -> {
            MenuFixKit.Rig rig = new MenuFixKit.Rig();
            float[] held = {0f, 0f, 0.6f, 0f};
            rig.input.motion(event(held, held, held, held));
            rig.input.motion(event(held));
            rig.at(450);
            MenuFixKit.eq("RIGHT@0 RIGHT+@400", rig.take(), "one press, then the normal repeat");
            MenuFixKit.check(!rig.input.motion(event(held)), "an unchanged sample is no change");
        });
        MenuFixKit.test(S, "the axis numbers are android.view.MotionEvent's", () -> {
            MenuFixKit.eq(android.view.MotionEvent.AXIS_X, NavInput.AXIS_X, "AXIS_X");
            MenuFixKit.eq(android.view.MotionEvent.AXIS_Y, NavInput.AXIS_Y, "AXIS_Y");
            MenuFixKit.eq(android.view.MotionEvent.AXIS_HAT_X, NavInput.AXIS_HAT_X, "AXIS_HAT_X");
            MenuFixKit.eq(android.view.MotionEvent.AXIS_HAT_Y, NavInput.AXIS_HAT_Y, "AXIS_HAT_Y");
        });
        MenuFixKit.finish(S);
    }
}
