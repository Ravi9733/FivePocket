package dev.recompbootstrap.smoke;

import android.os.Build;
import android.view.View;
import java.io.File;
import java.io.IOException;

/**
 * The Settings tab (local-analysis/agents/app-ui-4/DESIGN.md section 5). This class only forwards and fills in what
 * is drawn: every decision about values, questions and saving is {@link MenuModel}'s, the grouping into categories
 * is {@link SettingsPages}', both tested on the host. Files are read and written by CapabilityScanner and
 * SettingsStore, off the UI thread, and written only after the user chose Save (or "Apply again"). Opening and
 * leaving the tab writes no setting.
 */
final class SettingsScreen {
    private static final long SAVED_NOTE_MS = 2000;

    private final ScreenHost host;
    private final LauncherScreen owner;
    private final ShellNav nav;
    private final SettingsPages pages = new SettingsPages();
    private MenuModel model = new MenuModel();
    private Capabilities capabilities;
    private boolean saving;
    private int loads, alertScroll;
    private View timerView;

    private final Runnable clearSavedNote = new Runnable() {
        @Override public void run() {
            model.clearSavedStatus();
            owner.render();
        }
    };

    SettingsScreen(ScreenHost host, LauncherScreen owner, ShellNav nav) {
        this.host = host;
        this.owner = owner;
        this.nav = nav;
        nav.setColumnCount(ShellNav.SETTINGS, SettingsPages.COUNT);
    }

    /** The view whose message queue carries the "Saved." timer. */
    void attach(View view) {
        timerView = view;
    }

    // ------------------------------------------------------------------------------------------ state for the frame

    boolean ready() {
        return model.placeholder() == null;
    }

    boolean dirty() {
        return model.dirty();
    }

    /** True while every command belongs to the model: a question is open or a save is running. */
    boolean capturing() {
        return saving || model.overlay() != MenuModel.OVERLAY_NONE || model.pendingAction() != MenuModel.ACTION_NONE;
    }

    /** True when the highlight can go from the category's name into its rows. */
    boolean enterable() {
        return ready() && SettingsPages.rows(model, nav.column(ShellNav.SETTINGS)).length > 0;
    }

    /** The tab became the current one, or the app came back to the front: read the files again unless edits stand. */
    void selected() {
        if (!dirty() && !capturing()) load();
    }

    // ------------------------------------------------------------------------------------------ commands

    /** A command for the model itself: anything while a question is open, Start (save) and Y (restore) otherwise. */
    void command(int command, boolean repeat) {
        if (model.overlay() == MenuModel.OVERLAY_DRIFT_LIST && (command == NavInput.UP || command == NavInput.DOWN)) {
            alertScroll = ConfirmOverlay.scrolled(alertScroll, command == NavInput.UP ? -1 : 1,
                    MenuScenes.alertDetailLines(model.overlayDetail(), owner.measure()));
            owner.render();
            return;
        }
        forward(command, repeat);
    }

    /** A command while the highlight is in the rows. */
    void paneCommand(int command, boolean repeat) {
        if (!ready()) return;
        if (command == NavInput.UP || command == NavInput.DOWN) {
            pages.move(model, command == NavInput.UP ? -1 : 1, repeat);
            owner.render();
        } else if (command == NavInput.LEFT || command == NavInput.RIGHT || command == NavInput.CONFIRM) {
            forward(command, repeat);
        }
    }

    /** The highlight goes from the category's name into its rows. */
    void enter() {
        int category = nav.column(ShellNav.SETTINGS);
        if (!ready() || !pages.enter(model, category)) nav.place(ShellNav.SETTINGS, category, false);
    }

    /** Leaving with unsaved changes was asked for: the model puts its question. */
    void askLeave() {
        model.nav(NavInput.BACK, false);
        settle();
    }

    /** Touch. Returns true when the tap was this tab's. */
    boolean hit(int base, int index) {
        if (base == MenuLayout.HIT_ALERT_BUTTON) {
            pages.tapAnswer(model, nav, index);   // the highlighted answer: as A; another one: the highlight moves to it
            settle();
            return true;
        }
        if (!ready() || capturing()) return false;
        if (base != MenuLayout.HIT_ROW && base != MenuLayout.HIT_ARROW_LEFT && base != MenuLayout.HIT_ARROW_RIGHT) return false;
        if (!pages.tap(model, nav, base, index)) return false;
        settle();
        return true;
    }

    private void forward(int command, boolean repeat) {
        pages.forward(model, nav, command, repeat);
        settle();
    }

    /** Draws the model's new state and does what it asks for. */
    private void settle() {
        int action = model.pendingAction();
        if (action == MenuModel.ACTION_SAVE) {
            save();
            return;
        }
        if (action == MenuModel.ACTION_LEAVE) {
            // The question was answered with Save or Discard. That model is finished: start from the files again.
            model = new MenuModel();
            alertScroll = 0;
            load();
            owner.settingsLeft(nav.leaveGranted());
            return;
        }
        if (nav.asking() && !capturing()) nav.leaveCancelled();   // Keep editing, or the save failed
        if (model.overlay() == MenuModel.OVERLAY_NONE) alertScroll = 0;
        owner.render();
    }

    // ------------------------------------------------------------------------------------------ work off the UI thread

    /** Reads what the installed game build supports and the saved settings. Writes no setting. */
    private void load() {
        final int ticket = ++loads;
        final File filesDir = host.filesDir();
        final boolean inputOption = inputOptionInApp();
        final Object[] result = new Object[2];
        final String[] failure = new String[1];
        host.background(new Runnable() {
            @Override public void run() {
                try {
                    Capabilities found = CapabilityScanner.load(filesDir, inputOption);
                    result[0] = found;
                    result[1] = store(filesDir).load(found);
                } catch (IOException | RuntimeException problem) {
                    failure[0] = describe(problem);
                }
            }
        }, new Runnable() {
            @Override public void run() {
                // Only the newest read counts, and never over edits or a question that appeared meanwhile.
                if (ticket != loads || dirty() || capturing()) return;
                MenuModel fresh = new MenuModel();
                SettingsStore.Loaded loaded = (SettingsStore.Loaded) result[1];
                if (failure[0] != null || loaded == null) {
                    fresh.loadFailed(failure[0] == null ? "no result" : failure[0]);
                } else {
                    capabilities = (Capabilities) result[0];
                    fresh.setGameRunning(host.gameRunning());
                    fresh.loaded(loaded.settings, loaded.fromFile, loaded.warnings, loaded.drift, capabilities);
                }
                model = fresh;
                pages.reloaded(model, nav);
                owner.render();
            }
        });
    }

    /** The one place a setting is written: the model asked for it after the user chose Save or "Apply again". */
    private void save() {
        ++loads;   // a read that began before this save holds what the files said before it: its result no longer counts
        final File filesDir = host.filesDir();
        final boolean onDriftRow = SettingsPages.onDriftRow(model);
        final Settings settings = model.beginSave();
        final Capabilities supported = capabilities;
        final SettingsStore.SaveResult[] result = new SettingsStore.SaveResult[1];
        final String[] failure = new String[1];
        saving = true;
        owner.render();
        host.background(new Runnable() {
            @Override public void run() {
                try {
                    result[0] = store(filesDir).save(settings, supported);
                } catch (RuntimeException problem) {
                    failure[0] = describe(problem);
                }
            }
        }, new Runnable() {
            @Override public void run() {
                saving = false;
                if (result[0] != null) model.saveFinished(result[0].ok, result[0].refusedGameRunning, result[0].error);
                else model.saveFinished(false, false, failure[0]);
                if (MenuModel.TEXT_SAVED.equals(model.status()) && timerView != null) {
                    timerView.removeCallbacks(clearSavedNote);
                    timerView.postDelayed(clearSavedNote, SAVED_NOTE_MS);
                }
                pages.afterSave(model, nav, onDriftRow);
                settle();
            }
        });
    }

    void hidden() {
        if (timerView != null) timerView.removeCallbacks(clearSavedNote);
    }

    private SettingsStore store(File filesDir) {
        return new SettingsStore(filesDir, new SettingsStore.GameRunning() {
            @Override public boolean check() {
                return host.gameRunning();
            }
        });
    }

    private static boolean inputOptionInApp() {
        if (Build.VERSION.SDK_INT < 30) return false;
        try {
            Class.forName("dev.recompbootstrap.smoke.InputDispatchOption");
            return true;
        } catch (ClassNotFoundException | LinkageError missing) {
            return false;
        }
    }

    /** The words for an exception: SettingsPages.failure (plain words first, the technical cause in brackets). */
    private static String describe(Exception problem) {
        return SettingsPages.failure(problem);
    }

    /** Fills in what the tab shows. */
    void fill(MenuScenes.Page page) {
        pages.fill(page, model, nav, alertScroll);
    }
}
