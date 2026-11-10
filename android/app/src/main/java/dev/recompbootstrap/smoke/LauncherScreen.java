package dev.recompbootstrap.smoke;

import android.app.Activity;
import android.content.Intent;
import android.content.Context;
import android.graphics.Bitmap;
import android.hardware.input.InputManager;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.View;
import java.io.File;
import java.util.TimeZone;

/**
 * The app's frame (local-analysis/agents/app-ui-4/DESIGN.md): a header, the tab strip PLAY / SETTINGS / CONTROLLER /
 * ABOUT, the current tab's page and the button prompts, all painted by one view from a Scene. This class decides
 * nothing: where the highlight goes is ShellNav's, what Play and the controller check do is LauncherMenu's, the
 * settings are MenuModel's and SettingsPages', what is drawn is MenuScenes'. It forwards commands and taps to them
 * and carries out what they return (start the game, look at the installed files again, write nothing by itself).
 * After a start it made it looks, on its return, at what became of that start (LaunchOutcome) and tells the menu.
 */
final class LauncherScreen implements Screen, UiKit.Listener {
    /** The class that can start the game activity. It exists in debug builds only, so it is looked up by name. */
    private static final String LAUNCH_HELPER = "dev.recompbootstrap.smoke.DebugGameLauncher";

    private ScreenHost host;
    private GameLauncher gameLauncher;   // null when this build has no launch helper
    private LauncherMenu menu;
    private final ShellNav nav = new ShellNav();
    private SettingsScreen settings;
    private AboutScreen about;
    private UiKit.SceneView view;
    private int detection;               // only the newest detection's result is shown
    private String controller;           // the gamepad's name when the screen was last shown, or null
    /** The last start this screen made, until its outcome has been looked at. */
    private final LaunchOutcome.Watch lastStart = new LaunchOutcome.Watch();
    /**
     * The look at a start's outcome is made this long after the return: by then the game window has ended its
     * process and Android has recorded how it ended.
     */
    private static final long OUTCOME_DELAY_MS = 1000;

    @Override public View createView(ScreenHost host) {
        this.host = host;
        gameLauncher = findGameLauncher();
        menu = new LauncherMenu(gameLauncher != null, ShipApplication.playerApp(), ShipApplication.publicApp());
        view = new UiKit.SceneView(host.activity(), host.unit(), this);
        settings = new SettingsScreen(host, this, nav);
        settings.attach(view);
        about = new AboutScreen(host, this, nav);
        showBackdrop(host, view);
        return view;
    }

    /** Reads the user's own backdrop picture, if there is one, off the UI thread and gives it to a view. */
    static void showBackdrop(ScreenHost host, final UiKit.SceneView target) {
        final File filesDir = host.filesDir();
        final Bitmap[] picture = new Bitmap[1];
        host.background(new Runnable() {
            @Override public void run() {
                picture[0] = UiKit.loadBackdrop(filesDir);
            }
        }, new Runnable() {
            @Override public void run() {
                if (picture[0] != null) target.setBackdrop(picture[0]);
            }
        });
    }

    private static GameLauncher findGameLauncher() {
        try {
            return (GameLauncher) Class.forName(LAUNCH_HELPER).getDeclaredConstructor().newInstance();
        } catch (ReflectiveOperationException | RuntimeException | LinkageError absent) {
            return null;   // Play and Controller check are then shown as "Not available in this build"
        }
    }

    /**
     * A controller was connected or taken away while this screen is shown: the Controller row, the header's line and
     * the Play page's sentence "Connect a controller to play" follow at once, not only at the next return.
     */
    private final InputManager.InputDeviceListener controllers = new InputManager.InputDeviceListener() {
        @Override public void onInputDeviceAdded(int id) {
            controllerChanged();
        }

        @Override public void onInputDeviceRemoved(int id) {
            controllerChanged();
        }

        @Override public void onInputDeviceChanged(int id) {
            controllerChanged();
        }
    };
    private boolean watchingControllers;

    private void controllerChanged() {
        controller = controllerName();
        render();
    }

    private void watchControllers(boolean watch) {
        if (watch == watchingControllers) {
            return;
        }
        InputManager manager = (InputManager) host.activity().getSystemService(Context.INPUT_SERVICE);
        if (manager == null) {
            return;
        }
        if (watch) {
            manager.registerInputDeviceListener(controllers, null);
        } else {
            manager.unregisterInputDeviceListener(controllers);
        }
        watchingControllers = watch;
    }

    @Override public void onShown() {
        watchControllers(true);
        refresh();
    }

    @Override public void onHidden() {
        watchControllers(false);
        settings.hidden();
    }

    /** The activity came back to the front (after the game or the controller check): look at the files again. */
    @Override public void onAppBack() {
        refresh();
    }

    private void refresh() {
        controller = controllerName();
        menu.shown();
        detect();
        judgeLastStart();
        entered(nav.tab());
        render();
    }

    /** The later look at a start that was left too soon to judge. Only while this screen is still in front: the game window is then not. */
    private final Runnable judgeAgain = new Runnable() {
        @Override public void run() {
            if (host.activity().hasWindowFocus()) {
                judgeLastStart();
            }
        }
    };

    /**
     * Back on this screen after a start it made: looks once at what became of that start. Not before the game
     * window had the time to accept the request (a return sooner than that is another event; the look is then left
     * for the next return).
     */
    private void judgeLastStart() {
        long now = SystemClock.uptimeMillis();
        final String run = lastStart.due(now);
        if (run == null) {
            // Back sooner than a start can be judged (a refusal the player left at once): look again when it can be.
            long wait = lastStart.settlesIn(now);
            view.removeCallbacks(judgeAgain);
            if (wait > 0) {
                view.postDelayed(judgeAgain, wait);
            }
            return;
        }
        final long wallMs = lastStart.startedWallMs();
        view.postDelayed(new Runnable() {
            @Override public void run() {
                final File filesDir = host.filesDir();
                final Activity activity = host.activity();
                final LaunchOutcome[] outcome = new LaunchOutcome[1];
                host.background(new Runnable() {
                    @Override public void run() {
                        long[] ranMs = {-1};
                        LaunchOutcome.Exit exit = LaunchExit.since(activity, wallMs, ranMs);
                        outcome[0] = LaunchOutcome.read(filesDir, run, exit, ranMs[0]);
                    }
                }, new Runnable() {
                    @Override public void run() {
                        menu.startOutcome(outcome[0]);
                        render();
                    }
                });
            }
        }, OUTCOME_DELAY_MS);
    }

    /** Looks at the installed files off the UI thread; the screen keeps showing the previous answer meanwhile. */
    private void detect() {
        final int ticket = ++detection;
        final File filesDir = host.filesDir();
        final String apkPath = host.activity().getApplicationInfo().sourceDir;
        final InstallState[] found = new InstallState[1];
        final ShipReady[] ship = new ShipReady[1];
        // Back from the game, the two-disc setup page or the first screen: this look reads the game code file and
        // the shader file whole again. Any other look answers that part from the last whole read (ShipVerify).
        final boolean wholeLook = menu.takeWholeLook();
        host.background(new Runnable() {
            @Override public void run() {
                // In a build that carries its game code, "ready" also needs that build's own rule (ShipReady): its
                // game code in place and every pinned game data file there. In any other build nothing changes.
                if (wholeLook) ShipVerify.again();
                ship[0] = ShipReady.remembered(apkPath, filesDir, ShipVerify.WHY_MENU);
                found[0] = InstallState.detect(filesDir, ship[0]);
            }
        }, new Runnable() {
            @Override public void run() {
                if (ticket == detection) {
                    // The page of a build that carries its game code says what is missing in that rule's numbers.
                    menu.setInstall(found[0], ship[0].bundled() ? ship[0] : null);
                    render();
                }
            }
        });
    }

    /** The text measure of the view that paints the frame. */
    Scene.Measure measure() {
        return view;
    }

    // ------------------------------------------------------------------------------------------ commands

    @Override public boolean onNav(int command, boolean repeat) {
        boolean asked = settings.capturing();
        handle(command, repeat);
        // A question this command opened (Save changes?, Restore original?, the files list) or a save it started ends
        // a direction that is still held: its repeats must not walk the answers (menu-input-fixes M2).
        if (!asked && settings.capturing()) host.endHold();
        return true;   // this is the bottom screen: B never leaves the app
    }

    private void handle(int command, boolean repeat) {
        int tab = nav.tab();
        int route = ShellNav.route(tab, command, settings.capturing(), settings.ready(), menu.asking());
        if (route == ShellNav.TO_SETTINGS_MODEL) {
            settings.command(command, repeat);   // a question is open, a save runs, or Start / Y
            return;
        }
        if (route == ShellNav.TO_PLAY_QUESTION) {
            menu.leavePlayItem();   // answers the open question with "no"
            render();
            return;
        }
        carryOut(tab, nav.command(command, repeat, enterable(tab), settings.dirty()), command, repeat);
    }

    /** True when the highlight can go from the tab's highlighted column item into its pane. */
    private boolean enterable(int tab) {
        return tab == ShellNav.SETTINGS ? settings.enterable() : tab == ShellNav.ABOUT && about.enterable();
    }

    /** Does what ShellNav answered. tab is the tab that was current when the command arrived. */
    private void carryOut(int tab, int answer, int command, boolean repeat) {
        // The highlight left the Play item (to the strip or to another tab): an open "start anyway?" is answered no.
        ShellPages.afterMove(tab, nav, menu);
        switch (answer) {
            case ShellNav.TAB:
                // L1 / R1 or a tap brought another tab: a direction still held stops here (vm-pad-hold D5).
                if (ShellNav.endsHold(answer, command)) host.endHold();
                entered(nav.tab());
                break;
            case ShellNav.ENTERED:
                if (tab == ShellNav.SETTINGS) settings.enter();
                break;
            case ShellNav.ACTIVATE:
                activate(tab);
                break;
            case ShellNav.PANE_COMMAND:
                if (tab == ShellNav.SETTINGS) {
                    settings.paneCommand(command, repeat);
                    return;   // it draws itself
                }
                if (tab == ShellNav.ABOUT) about.paneCommand(command);
                break;
            case ShellNav.ASK:
                settings.askLeave();
                return;
            default:
                break;
        }
        render();
    }

    /** The settings question was answered with Save or Discard; answer is what ShellNav then did. */
    void settingsLeft(int answer) {
        if (ShellNav.endsHold(answer, NavInput.NONE)) host.endHold();
        if (answer == ShellNav.TAB) entered(nav.tab());
        render();
    }

    private void entered(int tab) {
        if (tab == ShellNav.SETTINGS) settings.selected();
        else if (tab == ShellNav.ABOUT) about.selected();
    }

    /** A on the column item of a tab that has no pane to enter. */
    private void activate(int tab) {
        if (tab == ShellNav.PLAY) {
            if (nav.column(ShellNav.PLAY) == 1) {
                host.push(new SetupScreen());
            } else {
                perform(menu.tap(LauncherMenu.PRIMARY, host.gameRunning()));
            }
        } else if (tab == ShellNav.CONTROLLER) {
            perform(menu.tap(LauncherMenu.CONTROLLER_CHECK, host.gameRunning()));
        } else if (tab == ShellNav.ABOUT && nav.column(ShellNav.ABOUT) == ShellPages.ABOUT_SELF_TEST) {
            about.startSelfTest();
        } else if (tab == ShellNav.ABOUT && nav.column(ShellNav.ABOUT) == ShellPages.ABOUT_GPU_DRIVERS) {
            about.startGpuDrivers();
        } else if (tab == ShellNav.ABOUT && nav.column(ShellNav.ABOUT) == ShellPages.ABOUT_CAPTURES) {
            about.copyCaptures();   // "Copy To Pictures"; does nothing when there is nothing to copy (no prompt then)
        }
    }

    // ------------------------------------------------------------------------------------------ touch

    @Override public void onHit(int id) {
        int base = id / 100 * 100, index = id - base, tab = nav.tab();
        if (base == MenuLayout.HIT_PROMPT) {
            onNav(index, false);
            return;
        }
        if (tab == ShellNav.SETTINGS && settings.hit(base, index)) return;
        if (tab == ShellNav.SETTINGS && settings.capturing()) return;
        if (base == MenuLayout.HIT_TAB) {
            carryOut(tab, nav.tapTab(index, settings.dirty()), NavInput.NONE, false);
        } else if (base == MenuLayout.HIT_COLUMN) {
            // The highlighted item: as A. Any other: the highlight only moves there.
            carryOut(tab, nav.tapColumn(index, enterable(tab)), NavInput.NONE, false);
        }
    }

    @Override public void onResized() {
        render();
    }

    @Override public void onScroll(int lines) {
        if (nav.tab() == ShellNav.ABOUT) {
            about.scrollBy(lines);
            render();
        }
    }

    // ------------------------------------------------------------------------------------------ actions

    private void perform(LauncherMenu.Action action) {
        switch (action) {
            case PLAY:
                play();
                break;
            case RECHECK:
                detect();   // "Check again": the answer arrives through setInstall, as every detection's does
                break;
            case OPEN_GAME_FILES_SETUP:
                // "Set Up Game Files": the two-disc setup page, in the menu's frame (SetupScreen hands every call to
                // SetupDiscScreen in this build). B on it pops it; this screen is then shown again and looks at the
                // files again (onShown), so the first item and the state line follow what the page copied.
                host.push(new SetupScreen());
                break;
            case OPEN_FIRST_SCREEN:
                // "Finish Setting Up": one of the app's own pieces is not in place. The first screen checks every
                // piece again and puts back what is missing; coming back from it runs the detection (onAppBack).
                host.activity().startActivity(new Intent(host.activity(), ShipStartActivity.class)
                        .putExtra(ShipStartActivity.EXTRA_FROM_LAUNCHER, true));
                break;
            case OPEN_CONTROLLER_CHECK:
                try {
                    gameLauncher.startControllerCheck(host.activity());
                } catch (RuntimeException refused) {
                    menu.controllerCheckFailed(LaunchAdvice.cannotOpen("controller check", refused.getClass().getSimpleName()));
                }
                break;
            default:
                break;
        }
    }

    /** Prepares the start request off the UI thread (it hashes the game code file), then starts the game. */
    private void play() {
        final File filesDir = host.filesDir();
        final boolean acceptUnqualified = menu.acceptsUnqualified();
        final String[] error = new String[1];
        final String[] made = new String[1];
        host.background(new Runnable() {
            @Override public void run() {
                error[0] = gameLauncher.prepare(filesDir, acceptUnqualified);
                if (error[0] == null) {
                    made[0] = LaunchRequest.newestLauncherRun(filesDir);   // the run that prepare has just made
                }
            }
        }, new Runnable() {
            @Override public void run() {
                Activity activity = host.activity();
                if (menu.prepared(error[0], host.gameRunning(), activity.hasWindowFocus())) {
                    try {
                        gameLauncher.start(activity);
                        lastStart.started(made[0], SystemClock.uptimeMillis(), System.currentTimeMillis());
                    } catch (RuntimeException refused) {
                        menu.startFailed(LaunchAdvice.cannotOpen("game window", refused.getClass().getSimpleName()));
                    }
                } else if (error[0] != null && !LaunchRequest.UNQUALIFIED.equals(error[0])) {
                    // What stopped the start may be a piece that went missing since the last look: the state line
                    // then names it (and a failure that is not about the files stays on the page).
                    detect();
                }
                render();
            }
        });
    }

    // ------------------------------------------------------------------------------------------ drawing

    void render() {
        if (view == null) {
            return;
        }
        TimeZone zone = TimeZone.getDefault();
        MenuScenes.Page page = ShellPages.frame(nav, menu, controller, zone);
        switch (nav.tab()) {
            case ShellNav.PLAY:
                ShellPages.play(page, nav, menu, controller, zone);
                break;
            case ShellNav.SETTINGS:
                settings.fill(page);
                break;
            case ShellNav.CONTROLLER:
                ShellPages.controller(page, nav, menu, controller);
                break;
            default:
                about.fill(page);
                break;
        }
        // The build that is handed out: no sentence names the owner's phone, the kind of build or an install kit.
        if (menu.selfInstalling()) ShipWords.page(page);
        if (!UiKit.touchScreen(host.activity())) page.touchIcons = false;   // (no touch screen: the strip names the buttons)
        view.show(MenuScenes.build(page, view, view.frame()));
    }

    /** The name of the first real gamepad or joystick Android reports, or null. */
    private static String controllerName() {
        return UiKit.controllerName();
    }

    @Override public void onActivityResult(int requestCode, int resultCode, Intent data) {
        // This screen starts nothing for a result.
    }
}
