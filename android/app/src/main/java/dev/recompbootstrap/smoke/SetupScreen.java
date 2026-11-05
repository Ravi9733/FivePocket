package dev.recompbootstrap.smoke;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Intent;
import android.net.Uri;
import android.os.SystemClock;
import android.view.View;
import android.widget.FrameLayout;
import java.io.Closeable;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;

/**
 * First-run setup (design section 2.6). Thin: every decision is SetupModel's (pure Java, host tested); every file
 * operation is DiscIdentifier's, VerifiedExtractor's or CodeBundleImporter's (pure Java, host tested). This class
 * draws the model, forwards commands to it, opens the system file chooser and runs the work off the UI thread.
 * It writes nothing itself; everything the work writes lies under files/game-install (InstallGuard).
 *
 * In a build that carries its game code (ShipApplication.carriesGameCode) the page is the two-disc setup instead:
 * every call below goes to SetupDiscScreen, and nothing of this class's own path runs.
 */
final class SetupScreen implements Screen {
    private static final int REQUEST_PLAY_IMAGE = 0x5E01, REQUEST_PLAY_FOLDER = 0x5E02, REQUEST_BUNDLE = 0x5E03;
    private static final long TICK_MS = 250;

    /** The two-disc setup of a build that carries its game code; null in a developer build (this page as it was). */
    private Screen twoDisc;
    private FrameLayout publicContainer;
    private final SetupModel model = new SetupModel();
    private ScreenHost host;
    private UiKit.SceneView root;
    private boolean shown;

    // the chosen play disc (kept only while the screen lives; after a restart the player chooses it again)
    private Uri playUri;
    private boolean playIsFolder;

    // written by the worker thread, read by the UI tick
    private volatile boolean cancel;
    private volatile long bytesDone, bytesTotal;
    private volatile int filesDone, filesTotal;
    private volatile String currentPath = "";

    private final VerifiedExtractor.Progress progress = new VerifiedExtractor.Progress() {
        @Override
        public void update(long done, long total, int files, int filesOf, String current) {
            bytesDone = done;
            bytesTotal = total;
            filesDone = files;
            filesTotal = filesOf;
            currentPath = current;
        }

        @Override
        public boolean cancelled() {
            return cancel;
        }
    };

    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            if (!shown || !model.busy()) return;
            model.progress(bytesDone, bytesTotal, filesDone, filesTotal, currentPath, SystemClock.elapsedRealtime());
            render();
            root.postDelayed(this, TICK_MS);
        }
    };

    public SetupScreen() { this(false); }

    SetupScreen(boolean discsOnly) {
        twoDisc = discsOnly ? new SetupDiscScreen() : ShipApplication.publicApp() ? new PublicSetupScreen()
                : ShipApplication.carriesGameCode() ? new SetupDiscScreen() : null;
    }

    @Override
    public View createView(ScreenHost screenHost) {
        if (ShipApplication.publicApp() && twoDisc != null) {
            host = screenHost;
            publicContainer = new FrameLayout(host.activity());
            publicContainer.addView(twoDisc.createView(host));
            return publicContainer;
        }
        if (twoDisc != null) return twoDisc.createView(screenHost);
        host = screenHost;
        // The same frame as the tabs, painted from a Scene: ShellPages.setup says what it shows.
        root = new UiKit.SceneView(host.activity(), host.unit(), new UiKit.Listener() {
            @Override
            public void onHit(int id) {
                int base = id / 100 * 100, index = id - base;
                if (base == MenuLayout.HIT_ROW) {
                    // The highlighted step: as A. Any other: the highlight only moves there.
                    if (ShellPages.setupTap(model, index)) command(NavInput.CONFIRM);
                    else render();
                } else if (base == MenuLayout.HIT_ALERT_BUTTON) {
                    if (ShellPages.setupAnswerTap(model, index)) {
                        model.overlayTap(index);
                        perform(model.takeAction());
                    }
                    render();
                } else if (base == MenuLayout.HIT_PROMPT) {
                    // B on the prompt line: as the button, an unhandled Back leaves the page.
                    if (!onNav(index, false) && index == NavInput.BACK && shown) host.pop();
                }
            }

            @Override
            public void onResized() {
                render();
            }

            @Override
            public void onScroll(int lines) {
                // Nothing on this page scrolls.
            }
        });
        LauncherScreen.showBackdrop(host, root);
        render();
        return root;
    }

    @Override
    public void onShown() {
        if (twoDisc != null) {
            twoDisc.onShown();
            return;
        }
        shown = true;
        if (model.busy()) {
            root.removeCallbacks(tick);
            root.postDelayed(tick, TICK_MS);
        } else {
            inspect(null);
        }
        render();
    }

    @Override
    public void onHidden() {
        if (twoDisc != null) {
            twoDisc.onHidden();
            return;
        }
        shown = false;
        if (root != null) root.removeCallbacks(tick);
        // Leaving the screen stops running work at the next block; finished files stay, the rest is copied next time.
        if (model.busy()) cancel = true;
    }

    /** The app left the front: the two-disc page stops a running copy (its own rule). The developer page as before. */
    @Override
    public void onAppLeft() {
        if (twoDisc != null) twoDisc.onAppLeft();
    }

    /** The app is in front again: the two-disc page looks at the files and the free space again. */
    @Override
    public void onAppBack() {
        if (twoDisc != null) twoDisc.onAppBack();
    }

    /** The window has the focus again: the two-disc page looks at the files and the free space again (setup-fixes, C3-1). */
    @Override
    public void onWindowBack() {
        if (twoDisc != null) twoDisc.onWindowBack();
    }

    @Override
    public boolean onNav(int command, boolean repeat) {
        if (twoDisc != null) return twoDisc.onNav(command, repeat);
        boolean handled = model.nav(command, repeat);
        perform(model.takeAction());
        render();
        return handled;
    }

    private void command(int command) {
        onNav(command, false);
    }

    @Override
    public void onActivityResult(int requestCode, int resultCode, Intent data) {
        // The existing host restores a SetupScreen under an outstanding picker. Its request code identifies
        // which public child was waiting; replace only the view, keeping the host's restored result target.
        if (publicContainer != null && SetupPickerRoute.restoreDiscs(requestCode, twoDisc instanceof PublicSetupScreen)) {
            twoDisc.onHidden();
            twoDisc = new SetupDiscScreen();
            publicContainer.removeAllViews();
            publicContainer.addView(twoDisc.createView(host));
            twoDisc.onShown();
        }
        if (twoDisc != null) {
            twoDisc.onActivityResult(requestCode, resultCode, data);
            return;
        }
        boolean play = requestCode == REQUEST_PLAY_IMAGE || requestCode == REQUEST_PLAY_FOLDER;
        if (!play && requestCode != REQUEST_BUNDLE) return;
        Uri uri = resultCode == Activity.RESULT_OK && data != null ? data.getData() : null;
        if (uri == null) {
            // The chooser was closed without a choice, or the device has no app that can show one (the host then
            // reports "cancelled" as well). Nothing changes; the step says so and what can be done.
            model.nothingChosen(play ? SetupModel.STEP_PLAY : SetupModel.STEP_IMPORT);
            render();
            return;
        }
        if (play) identify(uri, requestCode == REQUEST_PLAY_FOLDER);
        else startImport(uri);
    }

    private void perform(int action) {
        switch (action) {
            case SetupModel.ACT_PICK_PLAY_IMAGE:
                host.startForResult(openDocument(), REQUEST_PLAY_IMAGE);
                break;
            case SetupModel.ACT_PICK_PLAY_FOLDER:
                host.startForResult(localOnly(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE)), REQUEST_PLAY_FOLDER);
                break;
            case SetupModel.ACT_PICK_BUNDLE:
                host.startForResult(openDocument(), REQUEST_BUNDLE);
                break;
            case SetupModel.ACT_START_COPY:
                startCopy();
                break;
            case SetupModel.ACT_STOP_WORK:
                cancel = true;
                break;
            case SetupModel.ACT_LEAVE:
                host.pop();
                break;
            default:
                break;
        }
    }

    private static Intent openDocument() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        return localOnly(intent);
    }

    /**
     * Only storage that is on or in this device is offered: a file in a cloud service would be downloaded whole
     * (gigabytes) before the first byte could be read, with nothing on this page to show for it.
     */
    private static Intent localOnly(Intent intent) {
        intent.putExtra(Intent.EXTRA_LOCAL_ONLY, true);
        return intent;
    }

    /** Reads what is in place (read-only) off the UI thread, then gives it to the model and runs then. */
    private void inspect(final Runnable then) {
        final File filesDir = host.filesDir();
        final SetupModel.Disk[] found = new SetupModel.Disk[1];
        host.background(new Runnable() {
            @Override
            public void run() {
                found[0] = SetupModel.inspect(filesDir);
            }
        }, new Runnable() {
            @Override
            public void run() {
                if (found[0] != null) model.setDisk(found[0]);
                if (then != null) then.run();
                render();
            }
        });
    }

    private void identify(final Uri uri, final boolean folder) {
        final ContentResolver resolver = host.activity().getContentResolver();
        final DiscIdentifier.Result[] result = new DiscIdentifier.Result[1];
        model.identifyStarted();
        render();
        host.background(new Runnable() {
            @Override
            public void run() {
                try {
                    if (folder) {
                        result[0] = DiscIdentifier.identifyFolder(UriDiscSource.folder(resolver, uri));
                        return;
                    }
                    UriDiscSource image = UriDiscSource.open(resolver, uri);
                    try {
                        result[0] = DiscIdentifier.identifyImage(image);
                    } finally {
                        closeQuietly(image);
                    }
                } catch (IOException e) {
                    result[0] = DiscIdentifier.unreadable(e);
                } catch (RuntimeException e) {   // a file provider that misbehaves must not leave the step "Working" for ever
                    result[0] = DiscIdentifier.unreadable(e);
                } catch (Error e) {              // nor must a failure of the device itself (out of memory)
                    result[0] = DiscIdentifier.unreadable(new IOException(e.getClass().getSimpleName()));
                }
            }
        }, new Runnable() {
            @Override
            public void run() {
                playUri = uri;
                playIsFolder = folder;
                model.playDiscIdentified(result[0]);
                render();
            }
        });
    }

    private void startCopy() {
        if (playUri == null) return;
        final ContentResolver resolver = host.activity().getContentResolver();
        final File gameInstall = InstallGuard.root(host.filesDir());
        final Uri uri = playUri;
        final boolean folder = playIsFolder;
        final VerifiedExtractor.Outcome[] outcome = new VerifiedExtractor.Outcome[1];
        beginWork(SetupModel.STEP_COPY);
        host.background(new Runnable() {
            @Override
            public void run() {
                try {
                    if (folder) {
                        outcome[0] = VerifiedExtractor.extract(SetupCatalog.items(),
                                VerifiedExtractor.folderReader(UriDiscSource.folder(resolver, uri)), gameInstall, progress);
                        return;
                    }
                    UriDiscSource image = UriDiscSource.open(resolver, uri);
                    try {
                        outcome[0] = VerifiedExtractor.extract(SetupCatalog.items(), VerifiedExtractor.imageReader(image),
                                gameInstall, progress);
                    } finally {
                        closeQuietly(image);
                    }
                } catch (IOException e) {
                    outcome[0] = discCopyGone(e);
                } catch (RuntimeException e) {
                    outcome[0] = discCopyGone(e);
                } catch (Error e) {   // the step must not stay "Working" for ever, whatever ended the work
                    outcome[0] = VerifiedExtractor.Outcome.failed("The copy stopped unexpectedly (" + e.getClass().getSimpleName() + ").");
                }
            }
        }, new Runnable() {
            @Override
            public void run() {
                endWork(outcome[0]);
            }
        });
    }

    private void startImport(final Uri uri) {
        final ContentResolver resolver = host.activity().getContentResolver();
        final File gameInstall = InstallGuard.root(host.filesDir());
        final VerifiedExtractor.Outcome[] outcome = new VerifiedExtractor.Outcome[1];
        beginWork(SetupModel.STEP_IMPORT);
        host.background(new Runnable() {
            @Override
            public void run() {
                try {
                    InputStream in = resolver.openInputStream(uri);
                    if (in == null) throw new IOException("the file cannot be opened");
                    try {
                        outcome[0] = CodeBundleImporter.importBundle(in, gameInstall, progress);
                    } finally {
                        closeQuietly(in);
                    }
                } catch (IOException e) {
                    outcome[0] = bundleFileGone(e);
                } catch (RuntimeException e) {
                    outcome[0] = bundleFileGone(e);
                } catch (Error e) {
                    outcome[0] = VerifiedExtractor.Outcome.failed("The import stopped unexpectedly (" + e.getClass().getSimpleName() + ").");
                }
            }
        }, new Runnable() {
            @Override
            public void run() {
                endWork(outcome[0]);
            }
        });
    }

    /** The chosen disc copy could not even be opened (storage removed, permission gone since it was chosen). */
    private static VerifiedExtractor.Outcome discCopyGone(Exception e) {
        return VerifiedExtractor.Outcome.failed(VerifiedExtractor.Problem.SOURCE_UNREADABLE,
                "The disc copy you chose could not be opened (" + VerifiedExtractor.reason(e) + ").");
    }

    private static VerifiedExtractor.Outcome bundleFileGone(Exception e) {
        return VerifiedExtractor.Outcome.failed(VerifiedExtractor.Problem.SOURCE_UNREADABLE,
                "The file you chose could not be opened (" + VerifiedExtractor.reason(e) + ").");
    }

    /** The sources are read-only: a failing close loses nothing and must not hide the result. */
    private static void closeQuietly(Closeable source) {
        try {
            source.close();
        } catch (IOException ignored) {
            // nothing was written through it
        }
    }

    private void beginWork(int step) {
        cancel = false;
        bytesDone = 0;
        bytesTotal = 0;
        filesDone = 0;
        filesTotal = 0;
        currentPath = "";
        model.workStarted(step, SystemClock.elapsedRealtime());
        root.removeCallbacks(tick);
        root.postDelayed(tick, TICK_MS);
        render();
    }

    /** The work ended: look at what is really in place first, then tell the model. */
    private void endWork(final VerifiedExtractor.Outcome outcome) {
        root.removeCallbacks(tick);
        inspect(new Runnable() {
            @Override
            public void run() {
                model.workFinished(outcome != null ? outcome : VerifiedExtractor.Outcome.failed("The work ended without a result."));
            }
        });
    }

    private void render() {
        if (root == null) return;
        MenuScenes.Page page = ShellPages.setup(model);
        page.touchIcons = root.touchIcons();
        root.show(MenuScenes.build(page, root, root.frame()));
        root.setKeepScreenOn(model.busy());   // a long copy must not be cut short by the screen timeout
    }
}
