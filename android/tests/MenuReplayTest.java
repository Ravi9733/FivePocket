package dev.recompbootstrap.smoke;

import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * The checker's 127 replay scripts and the lane's added ones (MenuReplay) as a regression: every script must end as
 * MenuReplayExpected says - today's end state unless the table names the fixes it differs by (menu-input-fixes
 * RESULT.md lists them by defect). A script that ends otherwise fails with its name and both states.
 */
public final class MenuReplayTest {
    private static final String S = "MenuReplay";
    private static final Set<String> KNOWN = new HashSet<String>(java.util.Arrays.asList(
            "D2", "D3", "D5", "D6", "D7", "M2", "M3", "M4", "minor2-horizontal", "minor2-removal", "r3-drift", "r3-minor6"));

    public static void main(String[] args) {
        MenuFixKit.requireAssertions();
        final Map<String, String[]> rows = new HashMap<String, String[]>();
        for (String[] row : MenuReplayExpected.ROWS) rows.put(row[0], row);
        final List<MenuReplay.Script> scripts = MenuReplay.scripts();
        MenuFixKit.test(S, "the table: one row per script, and every difference from today names known fixes", () -> {
            MenuFixKit.eq(scripts.size(), rows.size(), "rows");
            int checker = 0;
            for (MenuReplay.Script s : scripts) {
                String[] row = rows.get(s.name);
                MenuFixKit.check(row != null, "a row for " + s.name);
                if (!s.name.startsWith("lane/")) checker++;
                if (row[2] == null) continue;
                MenuFixKit.check(!row[2].equals(row[1]), s.name + ": a listed difference that is none");
                for (String fix : row[3].split("\\+")) MenuFixKit.check(KNOWN.contains(fix), s.name + ": unknown fix " + fix);
                for (String fix : row[4].split("\\+")) MenuFixKit.check(KNOWN.contains(fix), s.name + ": unknown fix " + fix);
            }
            MenuFixKit.eq(127, checker, "the checker's scripts");
        });
        MenuFixKit.test(S, "every script ends as the table says (today's end state, or the listed fix's)", () -> {
            StringBuilder wrong = new StringBuilder();
            int n = 0;
            for (MenuReplay.Script s : scripts) {
                String[] row = rows.get(s.name);
                String expected = row[2] != null ? row[2] : row[1];
                String actual;
                try {
                    actual = MenuReplay.run(s, false).state();
                } catch (RuntimeException e) {
                    actual = "EXCEPTION " + e;
                }
                if (!expected.equals(actual)) {
                    if (n++ < 5) wrong.append("\n  ").append(s.name).append("\n    expected ").append(expected).append("\n    actual   ").append(actual);
                }
            }
            MenuFixKit.check(n == 0, n + " script(s) end otherwise:" + wrong);
        });
        MenuFixKit.test(S, "the checker's undeclared consequences are gone (M2, M3, M4) and the added cases hold", () -> {
            String[][] cases = {
                // M2: a held direction does not walk the answers of the question it opened
                {"inv/RB during held LEFT on Traffic, one repeat inside the question, then A", "overlay=0", "saves=0", "dirty=true"},
                {"inv/RB during held RIGHT from Traffic 65 (dirty stays), one repeat, A", "overlay=0", "saves=0", "dirty=true"},
                // M3: a pad that reports its D-pad as keys and as the HAT: one tap, one step
                {"inv/pad with D-pad keys AND HAT: 10 ms DOWN tap in Licence", "licence=1"},
                {"inv/pad with D-pad keys AND HAT: 120 ms DOWN tap, 300 ms frames", "licence=1"},
                {"lane/M3 keys and HAT: two 30 ms taps in one 300 ms frame", "licence=2"},
                {"lane/M3 keys and HAT: HAT batch first (unbuffered order), 10 ms", "licence=1"},
                {"lane/M3 keys only: 10 ms DOWN tap in Licence", "licence=1"},
                {"lane/M3 HAT only: 10 ms DOWN tap in Licence", "licence=1"},
                // M4: a push that sweeps past the diagonal changes no value
                {"inv/stick ramp through diagonal to right", "dirty=false", "Scaler=FSR 1 (default)", "FSR sharpness=0.2"},
                {"inv/stick ramp through diagonal in one slow frame", "dirty=false", "Scaler=FSR 1 (default)", "FSR sharpness=0.2"},
                // minor 2: a pad taken away ends the hold
                {"lane/minor2 pad taken away mid-hold, then a stick tap", "licence=0"},
                {"lane/M2 LEFT held on Traffic, Y (restore question), B, LEFT still held", "Traffic=90 %"},
                // round 3: M1 (two presses, keys and HAT, one slow frame: two steps), minor 6 (a D-pad press under a held stick)
                {"lane/r3 M1 keys and HAT: LEFT 5-200 and 250-330 ms in one 300 ms frame on Traffic", "Traffic=90 %"},
                {"lane/r3 minor6 stick LEFT held on Traffic, RB opens the question, D-pad LEFT", "overlay=2:Discard"},
            };
            for (String[] c : cases) {
                MenuReplay.Script script = null;
                for (MenuReplay.Script s : scripts) if (s.name.equals(c[0])) script = s;
                MenuFixKit.check(script != null, "script " + c[0]);
                String state = MenuReplay.run(script, false).state();
                for (int i = 1; i < c.length; ++i) MenuFixKit.check(state.contains(c[i]), c[0] + ": " + c[i] + " in " + state);
            }
        });
        MenuFixKit.finish(S);
    }
}
