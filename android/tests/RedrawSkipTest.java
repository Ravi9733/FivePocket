package dev.recompbootstrap.smoke;

import java.util.ArrayList;
import java.util.List;
import java.util.TimeZone;

/**
 * vm-pad-hold D4: every repeat rebuilt and redrew the whole 1920 x 1080 scene, also when the Licence text was already
 * at its end. UiKit's SceneView.show now draws only when Scene.needsDraw says the new scene differs from the one shown.
 */
public final class RedrawSkipTest {
    private static final String S = "RedrawSkip";
    private static final TimeZone UTC = TimeZone.getTimeZone("UTC");

    private static Scene sample(int color, String text, float x, int hitId, String key, float... points) {
        Scene scene = new Scene();
        scene.contentKey = key;
        scene.backdrop(0xFF000000, 0xFF101010);
        scene.rect(x, 20, 300, 40, color);
        scene.text(text, x, 20, 300, 40, 28, Scene.BOLD, Scene.LEFT, 0xFFFFFFFF, true);
        scene.poly(color, points);
        scene.hit(x, 20, 300, 40, hitId);
        return scene;
    }

    /** The About tab with the Licence text in the pane, scrolled to scroll, built as LauncherScreen.render builds it. */
    private static Scene about(ShellNav nav, List<String> lines, int scroll) {
        MenuScenes.Page page = ShellPages.frame(nav, new LauncherMenu(true), "Pad", UTC);
        ShellPages.about(page, nav, lines, scroll);
        return MenuScenes.build(page, MenuLayoutTest.FIXED);
    }

    public static void main(String[] args) {
        MenuFixKit.requireAssertions();
        MenuFixKit.test(S, "nothing shown yet: draw; the same scene again: no draw", () -> {
            Scene shown = sample(0xFF203040, "Scaler", 100, 7, "a", 0, 0, 10, 0, 5, 8);
            MenuFixKit.check(Scene.needsDraw(null, shown), "the first scene is drawn");
            MenuFixKit.check(!Scene.needsDraw(shown, sample(0xFF203040, "Scaler", 100, 7, "a", 0, 0, 10, 0, 5, 8)), "an equal scene is not");
        });
        MenuFixKit.test(S, "any one difference is drawn: colour, text, place, touch id, shape, content key, one more item", () -> {
            Scene shown = sample(0xFF203040, "Scaler", 100, 7, "a", 0, 0, 10, 0, 5, 8);
            MenuFixKit.check(Scene.needsDraw(shown, sample(0xFF203041, "Scaler", 100, 7, "a", 0, 0, 10, 0, 5, 8)), "colour");
            MenuFixKit.check(Scene.needsDraw(shown, sample(0xFF203040, "Scaler.", 100, 7, "a", 0, 0, 10, 0, 5, 8)), "text");
            MenuFixKit.check(Scene.needsDraw(shown, sample(0xFF203040, "Scaler", 100.5f, 7, "a", 0, 0, 10, 0, 5, 8)), "place");
            MenuFixKit.check(Scene.needsDraw(shown, sample(0xFF203040, "Scaler", 100, 8, "a", 0, 0, 10, 0, 5, 8)), "touch id");
            MenuFixKit.check(Scene.needsDraw(shown, sample(0xFF203040, "Scaler", 100, 7, "a", 0, 0, 10, 0, 5, 9)), "shape");
            MenuFixKit.check(Scene.needsDraw(shown, sample(0xFF203040, "Scaler", 100, 7, "b", 0, 0, 10, 0, 5, 8)), "content key (the fade)");
            Scene moved = sample(0xFF203040, "Scaler", 100, 7, "a", 0, 0, 10, 0, 5, 8);
            moved.hits.clear();
            moved.hit(140, 20, 300, 40, 7);
            MenuFixKit.check(Scene.needsDraw(shown, moved), "a touch rectangle moved, same id, same drawing");
            Scene more = sample(0xFF203040, "Scaler", 100, 7, "a", 0, 0, 10, 0, 5, 8);
            more.disc(5, 5, 2, 0xFFFFFFFF);
            MenuFixKit.check(Scene.needsDraw(shown, more), "one more item");
        });
        MenuFixKit.test(S, "Licence text at its end: a held DOWN's repeat builds the same scene, so nothing is drawn", () -> {
            ShellNav nav = new ShellNav();
            nav.setColumnCount(ShellNav.ABOUT, 5);
            nav.command(NavInput.PAGE_PREV, false, true, false);   // PLAY -> ABOUT
            nav.command(NavInput.RIGHT, false, true, false);       // into the pane
            List<String> lines = new ArrayList<String>();
            for (int i = 0; i < 200; ++i) lines.add("Licence line " + i);
            int end = ShellPages.aboutScrolled(0, 10000, lines.size());
            Scene shown = about(nav, lines, end);
            int after = ShellPages.aboutScrolled(end, 3, lines.size());
            MenuFixKit.eq(end, after, "the text is clamped at its end");
            MenuFixKit.check(!Scene.needsDraw(shown, about(nav, lines, after)), "the repeat at the end draws nothing");
            MenuFixKit.check(Scene.needsDraw(shown, about(nav, lines, ShellPages.aboutScrolled(end, -3, lines.size()))),
                    "control: UP from the end moves the text and is drawn");
        });
        MenuFixKit.finish(S);
    }
}
