package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SetupTestKit.check;
import static dev.recompbootstrap.smoke.SetupTestKit.expect;
import static dev.recompbootstrap.smoke.SetupTestKit.expectEquals;

import java.util.List;

/**
 * Run with java -ea. SetupDiscPage: the model as a page of the menu (rows, tones, info block, help, prompts,
 * question) and the touch rules; every state is laid out by MenuScenes.build without a text cut off.
 */
public final class SetupDiscPageTest {
    /** A face a tenth wider than the layout tests' fixed one (as SetupModelTest's). */
    static final Scene.Measure WIDE = new Scene.Measure() {
        @Override public float width(String text, int sizePx, int face) {
            return text.length() * sizePx * 1.1f * (face == Scene.BOLD ? 0.6f : face == Scene.REGULAR ? 0.5f : 0.35f);
        }
    };

    /** The control: the page of a model whose help texts are three sentences longer. */
    static final class Longer {
        final SetupDiscModel model;

        Longer(SetupDiscModel model) {
            this.model = model;
        }
    }

    static void expectFits(Longer longer, String what) {
        MenuScenes.Page page = SetupDiscPage.page(longer.model);
        String extra = " This sentence is here only to make the text longer than any the page has. And another one of the same kind."
                + " And a third one, so that the strip cannot hold it.";
        MenuScenes.Line first = page.help.get(0);
        page.help.set(0, new MenuScenes.Line(first.text + extra + extra + extra + extra, first.tone));
        Scene scene = MenuScenes.build(page, WIDE);
        if (System.getenv("SHOW_CONTROL") != null) {
            for (Scene.Item it : scene.items) if (it.type == Scene.TEXT) System.out.println("TEXT " + it.y + " " + it.text);
        }
        fits(scene, what);
    }

    static void expectFits(SetupDiscModel m, String what) {
        int rows = m.overlay() == SetupDiscModel.OVERLAY_NONE ? SetupDiscModel.ROWS : 1;
        int kept = m.focused();
        for (int row = 0; row < rows; ++row) {
            if (rows > 1) m.focus(row);
            fits(MenuScenes.build(SetupDiscPage.page(m), WIDE), what + ", row " + row);
        }
        if (rows > 1) m.focus(kept);
    }

    static void fits(Scene scene, String what) {
        for (Scene.Item it : scene.items) {
            if (it.type != Scene.TEXT) continue;
            String where = what + ": <" + it.text + ">";
            expect(WIDE.width(it.text, it.size, it.face) <= it.w + 0.5f, "wider than its box: " + where);
            boolean own = it.text.endsWith("ing" + Scene.ELLIPSIS);
            expect(own || !it.text.endsWith(Scene.ELLIPSIS), "cut off: " + where);
            expect(it.y >= 0 && it.y + it.h <= MenuLayout.H + 0.5f, "outside the screen: " + where);
        }
    }
    /** The heading MenuScenes draws over a page's question: the first text of the alert, in the title face. */
    static String drawnHeading(MenuScenes.Page page) {
        for (Scene.Item it : MenuScenes.build(page, WIDE).items) {
            if (it.type == Scene.TEXT && it.face == Scene.HEAVY) return it.text;   // (the alert's heading face)
        }
        return null;
    }

    public static void main(String[] args) throws Exception {
        SetupTestKit.requireAssertions();

        check("rows, values, tones, help and prompts follow the model; no import row; no status line", () -> {
            SetupDiscKit.Device d = new SetupDiscKit.Device("page", true).open();
            d.nextChoice = d.document("other.iso", Disc2DiscsTest.playImage(Disc2DiscsTest.OTHER_EXE, Disc2DiscsTest.COMMON).data);
            d.press(NavInput.CONFIRM);
            MenuScenes.Page page = SetupDiscPage.page(d.model);
            expectEquals(3, page.rows.size(), "three rows");
            expectEquals("1  Play Disc (Disc 2)", page.rows.get(0).label, "label");
            expectEquals("Problem", page.rows.get(0).value, "value");
            expectEquals(MenuScenes.TONE_ERROR, page.rows.get(0).valueTone, "problem tone");
            expectEquals(MenuScenes.TONE_ERROR, page.help.get(0).tone, "problem text tone");
            expectEquals(d.model.detail(0), page.help.get(0).text, "help is the focused row's text");
            d.model.focus(2);
            expectEquals(d.model.detail(2), SetupDiscPage.page(d.model).help.get(0).text, "also on another row");
            expect(!page.rows.get(2).enabled, "Finish not enabled");
            for (MenuScenes.Row r : page.rows) expect(!r.label.toLowerCase(java.util.Locale.ROOT).contains("import"), "no import row");
            expect(page.status.isEmpty(), "no build words at the top");
            List<MenuScenes.Prompt> prompts = page.prompts;
            expectEquals(2, prompts.size(), "B and A");
            expectEquals("B", prompts.get(0).glyph, "B first");
            expectEquals(-1f, page.infoBar, "no bar");
        });

        check("while copying: the info block with the bar shows the progress; the question replaces the focus", () -> {
            SetupDiscModel m = new SetupDiscKit.Device("page-copy", true).open().model;
            m.copyStarted(0, 0);
            m.progress(500, 1000, 1, 4, "common.rpf", 0);
            MenuScenes.Page page = SetupDiscPage.page(m);
            expectEquals(0.5f, page.infoBar, "bar");
            expectEquals(1, page.info.size(), "info");
            expect(page.help.isEmpty(), "the same text is not repeated as help");
            expectEquals("Copying", page.rows.get(0).value, "word");
            expect(page.rows.get(0).enabled, "the working row is drawn enabled");
            m.nav(NavInput.BACK, false);
            page = SetupDiscPage.page(m);
            expect(page.alert != null && page.rowFocused == -1, "question over the page");
            expectEquals("Stop Copying", drawnHeading(page), "the question about stopping is headed by what it asks, not by the frame's 'Alert'");
        });

        check("the question before a copy is headed Copy; a question of another page keeps the frame's own heading", () -> {
            SetupDiscKit.Device d = new SetupDiscKit.Device("page-heading", true).open();
            d.nextChoice = d.document("Disc2-Play.iso", SetupDiscKit.playImage());
            d.press(NavInput.CONFIRM);
            expectEquals(SetupDiscModel.OVERLAY_START, d.model.overlay(), "(the question is open)");
            expectEquals("Copy", drawnHeading(SetupDiscPage.page(d.model)), "the heading that is drawn");
            MenuScenes.Page other = new MenuScenes.Page();
            other.alert = new ConfirmOverlay("Save changes?", null, new String[] {"Save", "Discard"}, 0);
            expectEquals(MenuScenes.ALERT_HEADING, drawnHeading(other), "a question without a heading of its own");
        });

        check("touch: one tap selects and activates a setup row; answers still require focus then confirm", () -> {
            SetupDiscKit.Device d = new SetupDiscKit.Device("page-touch", true).open();
            expect(SetupDiscPage.tap(d.model, 1), "one tap activates");
            expectEquals(1, d.model.focused(), "the tapped row is focused");
            d.nextChoice = d.document("Disc2-Play.iso", SetupDiscKit.playImage());
            d.model.focus(0);
            d.press(NavInput.CONFIRM);
            expect(!SetupDiscPage.answerTap(d.model, 1), "moves to Not now");
            expectEquals(1, d.model.overlayFocus(), "focus");
            expect(SetupDiscPage.answerTap(d.model, 1), "then it is A");
            expect(!SetupDiscPage.answerTap(d.model, 5), "no such answer");
        });

        check("layout: every state, each row highlighted in turn, in a face a tenth wider: no text is cut or leaves its box", () -> {
            int states = 0;
            for (java.util.Map.Entry<String, SetupDiscModel> e : SetupDiscPreviewStates.named().entrySet()) {
                expectFits(e.getValue(), e.getKey());
                ++states;
            }
            expect(states >= 20, "all states: " + states);
            // the check can fail: a text three sentences longer than the longest is reported as cut
            SetupDiscModel m = SetupDiscPreviewStates.named().get("17-document-gone");
            boolean cut = false;
            try {
                expectFits(new Longer(m), "control");
            } catch (AssertionError expected) {
                cut = expected.getMessage().startsWith("cut off") || expected.getMessage().startsWith("wider");
            }
            expect(cut, "control: a longer text is reported");
        });

        SetupTestKit.finish("SetupDiscPageTest");
    }
}
