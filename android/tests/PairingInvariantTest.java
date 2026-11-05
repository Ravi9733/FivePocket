package dev.recompbootstrap.smoke;

import java.util.ArrayList;
import java.util.List;
import java.util.Random;

/**
 * The D-pad pairing invariant (menu-input-fixes round 3, M1), tested with a generator instead of case by case:
 *
 *   every physical D-pad press yields exactly one command (repeats aside), however many of the two reports (key, HAT)
 *   it produces and in whatever order and batch they arrive; two physical presses yield two; a press held longer than
 *   LONG_MS repeats before the next press; and when every press is over, nothing repeats any more.
 *
 * 20,000 sequences, seed 20261006. Each: one pad kind (keys only, HAT only, both); 1 to 8 presses of the four
 * directions, 1 to 400 ms long (one in ten 900 to 1500 ms), 1 to 300 ms apart; for "both", the later of the two reports (fixed per pad, chosen at
 * random) comes 0 to 40 ms after the other (random per press, never before the same source's previous release).
 * Keys are delivered at their own time; HAT samples in batches at frame ends, frames 5 to 400 ms long (random per
 * frame); a key and a batch at the same time: the key first. The commands are compared with the physical presses.
 */
public final class PairingInvariantTest {
    private static final String S = "PairingInvariant";
    static final int SEQUENCES = 20000;
    static final long SEED = 20261006L;
    private static final int KEYS_ONLY = 0, HAT_ONLY = 1, BOTH = 2;
    private static final int[] KEY_CODE = {0, 19, 20, 21, 22};
    /** Longer than the first repeat (400 ms) plus the largest report delay (40) and frame (400) plus a margin. */
    static final long LONG_MS = 850;

    /** {time, kind 0 = key / 1 = batch, ...}: keys {code, down}; batches hold samples {time, hatX, hatY}. */
    private static final class Delivery {
        final long time;
        final int kind, code;
        final boolean down;
        final List<float[]> samples = new ArrayList<float[]>();

        Delivery(long time, int kind, int code, boolean down) {
            this.time = time;
            this.kind = kind;
            this.code = code;
            this.down = down;
        }
    }

    static final class Outcome {
        int sequences, presses, commands, failures, stuck, longPresses, longWithoutRepeat;
        final int[] perKind = new int[3];
        final List<String> first = new ArrayList<String>();
    }

    private static float[] hat(long time, int direction) {
        float x = direction == NavInput.LEFT ? -1 : direction == NavInput.RIGHT ? 1 : 0;
        float y = direction == NavInput.UP ? -1 : direction == NavInput.DOWN ? 1 : 0;
        return new float[] {time, x, y};
    }

    static Outcome run(int sequences, long seed) {
        Random random = new Random(seed);
        Outcome out = new Outcome();
        for (int sequence = 0; sequence < sequences; ++sequence) {
            int kind = random.nextInt(3);
            boolean hatLate = random.nextBoolean();
            int count = 1 + random.nextInt(8);
            long t = 10 + random.nextInt(50);
            long lateEnd = -1, lateDelay = 0;
            List<Integer> physical = new ArrayList<Integer>();
            List<Boolean> isLong = new ArrayList<Boolean>();
            List<long[]> keys = new ArrayList<long[]>();      // time, code, down
            List<float[]> hats = new ArrayList<float[]>();    // time, x, y
            StringBuilder story = new StringBuilder();
            for (int p = 0; p < count; ++p) {
                int direction = 1 + random.nextInt(4);
                long duration = random.nextInt(10) == 0 ? 900 + random.nextInt(601) : 1 + random.nextInt(400);
                long delay = 0;
                if (kind == BOTH) {
                    delay = random.nextInt(41);
                    // never before the late source's previous release (a source reports its presses in order)
                    if (t + delay <= lateEnd) delay = lateEnd - t + 1;
                    if (delay > 40) throw new IllegalStateException("generator: delay " + delay);
                    lateDelay = delay;
                }
                long keyAt = t + (kind == BOTH && !hatLate ? delay : 0), hatAt = t + (kind == BOTH && hatLate ? delay : 0);
                if (kind != HAT_ONLY) {
                    keys.add(new long[] {keyAt, KEY_CODE[direction], 1});
                    keys.add(new long[] {keyAt + duration, KEY_CODE[direction], 0});
                }
                if (kind != KEYS_ONLY) {
                    hats.add(hat(hatAt, direction));
                    hats.add(hat(hatAt + duration, NavInput.NONE));
                }
                if (kind == BOTH) lateEnd = t + lateDelay + duration;
                physical.add(direction);
                isLong.add(duration > LONG_MS);
                story.append(MenuFixKit.NAMES[direction]).append('[').append(t).append(',').append(t + duration).append(" d").append(delay).append("] ");
                t += duration + 1 + random.nextInt(300);
            }
            // frames
            List<Long> ends = new ArrayList<Long>();
            long frame = 0;
            while (frame <= t + 400) {
                frame += 5 + random.nextInt(396);
                ends.add(frame);
            }
            List<Delivery> deliveries = new ArrayList<Delivery>();
            for (long[] k : keys) deliveries.add(new Delivery(k[0], 0, (int) k[1], k[2] == 1));
            Delivery batch = null;
            for (float[] sample : hats) {
                long at = 0;
                for (long end : ends) if (end > (long) sample[0]) { at = end; break; }
                if (batch == null || batch.time != at) {
                    batch = new Delivery(at, 1, 0, false);
                    deliveries.add(batch);
                }
                batch.samples.add(sample);
            }
            deliveries.sort((a, b) -> a.time != b.time ? Long.compare(a.time, b.time) : Integer.compare(a.kind, b.kind));

            final List<Integer> commands = new ArrayList<Integer>();
            final List<Integer> repeatsAfter = new ArrayList<Integer>();   // repeats after each command
            final NavInputTest.FakeTimer timer = new NavInputTest.FakeTimer();
            NavInput input = new NavInput((command, repeat) -> {
                if (!repeat) {
                    commands.add(command);
                    repeatsAfter.add(0);
                } else if (!repeatsAfter.isEmpty()) {
                    repeatsAfter.set(repeatsAfter.size() - 1, repeatsAfter.get(repeatsAfter.size() - 1) + 1);
                }
            }, timer);
            for (Delivery d : deliveries) {
                timer.advance(d.time - timer.now);
                if (d.kind == 0) {
                    input.key(d.code, d.down, 0, d.time);
                } else {
                    final List<float[]> samples = d.samples;
                    input.motion(new NavInput.Motion() {
                        @Override public int history() {
                            return samples.size() - 1;
                        }

                        @Override public float value(int axis, int position) {
                            return axis == NavInput.AXIS_HAT_X ? samples.get(position)[1] : axis == NavInput.AXIS_HAT_Y ? samples.get(position)[2] : 0f;
                        }

                        @Override public long time(int position) {
                            return (long) samples.get(position)[0];
                        }
                    });
                }
            }
            timer.advance(3000);
            boolean stuck = timer.pending();
            out.sequences++;
            out.perKind[kind]++;
            out.presses += physical.size();
            out.commands += commands.size();
            boolean lostRepeat = false;
            for (int p = 0; p < isLong.size(); ++p) {
                if (!isLong.get(p)) continue;
                out.longPresses++;
                if (p < repeatsAfter.size() && repeatsAfter.get(p) == 0) lostRepeat = true;
            }
            if (lostRepeat) out.longWithoutRepeat++;
            if (!commands.equals(physical) || stuck || lostRepeat) {
                out.failures++;
                if (stuck) out.stuck++;
                if (out.first.size() < 5) {
                    out.first.add("seq " + sequence + " kind " + kind + (kind == BOTH ? (hatLate ? " HAT late" : " key late") : "")
                            + ": " + story + "-> " + names(commands) + (stuck ? " (a repeat still runs)" : "") + (lostRepeat ? " (a long press did not repeat)" : ""));
                }
            }
        }
        return out;
    }

    private static String names(List<Integer> commands) {
        StringBuilder s = new StringBuilder();
        for (int c : commands) s.append(MenuFixKit.NAMES[c]).append(' ');
        return s.toString().trim();
    }

    public static void main(String[] args) {
        MenuFixKit.requireAssertions();
        MenuFixKit.test(S, SEQUENCES + " generated sequences, three pad kinds: one command per physical press, nothing left repeating", () -> {
            Outcome o = run(SEQUENCES, SEED);
            System.out.println("      sequences " + o.sequences + " (keys only " + o.perKind[KEYS_ONLY] + ", HAT only " + o.perKind[HAT_ONLY]
                    + ", both " + o.perKind[BOTH] + "), physical presses " + o.presses + ", commands " + o.commands
                    + ", presses longer than " + LONG_MS + " ms " + o.longPresses
                    + ", sequences that break the invariant " + o.failures + " (a repeat left running: " + o.stuck
                    + ", a long press without a repeat: " + o.longWithoutRepeat + ")");
            for (String f : o.first) System.out.println("      " + f);
            MenuFixKit.eq(0, o.failures, "sequences that break the invariant");
            MenuFixKit.eq(o.presses, o.commands, "commands");
        });
        MenuFixKit.finish(S);
    }
}
