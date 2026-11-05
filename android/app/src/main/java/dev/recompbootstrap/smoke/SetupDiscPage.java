package dev.recompbootstrap.smoke;

import java.util.Locale;

/**
 * The two-disc setup page in the menu's frame: SetupDiscModel into a MenuScenes.Page, as ShellPages.setup does for
 * SetupModel (same heading, rows, info block with its bar, help strip, prompts and question). Decides nothing itself.
 * Pure Java.
 */
final class SetupDiscPage {
    private SetupDiscPage() {}

    /** Touch on a row: a tap on a row that is not highlighted only moves the highlight there (false); on the highlighted one it is A. */
    static boolean tap(SetupDiscModel model, int row) {
        if (model.focused() == row) return true;
        model.focus(row);
        return false;
    }

    /** Touch on an answer of the open question: the same rule. */
    static boolean answerTap(SetupDiscModel model, int answer) {
        int count = model.overlayButtons().length;
        if (answer < 0 || answer >= count) return false;
        if (model.overlayFocus() == answer) return true;
        for (int steps = count; steps > 0 && model.overlayFocus() != answer; --steps) {
            model.nav(model.overlayFocus() < answer ? NavInput.RIGHT : NavInput.LEFT, false);
        }
        return false;
    }

    static MenuScenes.Page page(SetupDiscModel model) {
        MenuScenes.Page page = new MenuScenes.Page();
        page.title = ShellPages.TITLE;
        page.tabs = new String[] {model.title().toUpperCase(Locale.ROOT)};
        page.shoulders = false;
        boolean question = model.overlay() != SetupDiscModel.OVERLAY_NONE;
        int focused = model.focused(), working = -1;
        for (int row = 0; row < SetupDiscModel.ROWS; ++row) {
            MenuScenes.Row r = new MenuScenes.Row();
            int state = model.state(row);
            if (state == SetupDiscModel.COPYING) working = row;
            r.label = Scene.titleCase(model.label(row));
            r.value = Scene.titleCase(model.stateWord(row));
            r.enabled = model.actionable(row) || state == SetupDiscModel.COPYING || state == SetupDiscModel.READING;
            r.valueTone = state == SetupDiscModel.PROBLEM ? MenuScenes.TONE_ERROR
                    : state == SetupDiscModel.DONE ? MenuScenes.TONE_GOOD : MenuScenes.TONE_NORMAL;
            page.rows.add(r);
        }
        if (!question) page.rowFocused = focused;
        // While a disc is copied: what is happening, with its bar, above the rows.
        page.infoBar = model.progressFraction();
        if (page.infoBar >= 0f && working >= 0) page.info.add(new MenuScenes.Line(model.detail(working), MenuScenes.TONE_NORMAL));
        String detail = model.detail(focused);
        if (detail != null && (page.info.isEmpty() || focused != working)) {
            page.help.add(new MenuScenes.Line(detail, model.detailIsProblem(focused) ? MenuScenes.TONE_ERROR : MenuScenes.TONE_NORMAL));
        }
        page.prompts.addAll(MenuScenes.legendPrompts(model.footer()));
        if (question) {
            page.alert = new ConfirmOverlay(model.overlayHeading(), model.overlayText(), null, model.overlayButtons(), model.overlayFocus());
        }
        return page;
    }
}
