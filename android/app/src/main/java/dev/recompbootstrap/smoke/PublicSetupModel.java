package dev.recompbootstrap.smoke;

/** Pure setup state: no setup action is enabled while an import is in flight or the game is running. */
final class PublicSetupModel {
    static final int NONE = 0, PICK = 1, DISCS = 2, LEAVE = 3, STOP = 4;
    int focus;
    boolean busy, ready;
    String notice = PublicCodeWords.HELP;
    long done, total;
    int confirm(boolean gameRunning) {
        if (busy) return NONE;
        if (gameRunning) { notice = PublicCodeWords.RUNNING; return NONE; }
        if (focus == 0) return PICK;
        if (focus == 1) return DISCS;
        return LEAVE;
    }
    int back() { return busy ? STOP : LEAVE; }
    void move(int delta, boolean repeat) {
        if (busy) return;
        int next = focus + delta;
        if (next < 0 || next > 2) { if (repeat) return; next = (next + 3) % 3; }
        focus = next;
    }
    void started() { busy = true; done = total = 0; notice = "Checking and installing the game code. B: stop."; }
    void finished(VerifiedExtractor.Outcome outcome, String activationProblem) {
        busy = false;
        notice = activationProblem == null ? PublicCodeWords.outcome(outcome)
                : "The bundle was kept, but setup could not finish. " + activationProblem;
    }
    MenuScenes.Page page() {
        MenuScenes.Page p = new MenuScenes.Page();
        p.title = ShellPages.TITLE;
        p.tabs = new String[]{"SET UP GAME"}; p.shoulders = false;
        for (String label : new String[]{PublicCodeWords.INSTALL, "Copy Disc Files", ready ? "Ready — Back to Menu" : "Back to Menu"}) {
            MenuScenes.Row r = new MenuScenes.Row(); r.label = label; r.enabled = !busy; p.rows.add(r);
        }
        p.rowFocused = focus;
        p.help.add(new MenuScenes.Line(notice, MenuScenes.TONE_NORMAL));
        if (busy && total > 0) p.infoBar = Math.max(0f, Math.min(1f, (float)((double)done / total)));
        p.prompts.addAll(MenuScenes.legendPrompts(busy ? "B Stop" : "A Select   B Back"));
        return p;
    }
}
