package dev.recompbootstrap.smoke;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.SystemClock;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.util.Log;
import android.view.View;
import java.io.File;
import java.io.IOException;

/**
 * The two-disc setup page of a build that carries its game code (SetupScreen hands every call to it then). Thin:
 * every decision is SetupDiscModel's, every file operation Disc2Discs' and SetupDiscCopy's, the remembered choices
 * SetupDiscStore's (all pure Java, host tested). This class draws the model through SetupDiscPage, opens the system
 * file chooser, takes the persistable read permission of the chosen document, runs the work off the UI thread and
 * writes one log line per event under the tag RecompSetup (SetupDiscLog). Everything the work writes lies under
 * files/private-trace (inputs/ and disc-staging/) and files/settings (the remembered choices).
 */
final class SetupDiscScreen implements Screen {
    private static final int[] REQUEST = {SetupPickerRoute.PLAY, SetupPickerRoute.INSTALL};
    private static final long TICK_MS = 250;
    /** Where the chooser opens when the device's provider knows it: the shared Download folder. A hint only. */
    private static final String DOWNLOADS = "content://com.android.externalstorage.documents/document/primary%3ADownload";

    private final SetupDiscModel model = new SetupDiscModel();
    private ScreenHost host;
    private UiKit.SceneView root;
    private boolean shown;
    /** Per disc: the image chosen in this run of the page (used when the provider offered no lasting permission). */
    private final Uri[] chosenNow = new Uri[SetupDiscModel.DISCS];

    // written by the worker thread, read by the UI tick
    private volatile boolean cancel;
    /** The marker of the running copy (SetupDiscStore.Marker), or null. */
    private SetupDiscStore.Marker marker;
    private volatile long bytesDone, bytesTotal;
    private volatile int fileIndex, files;
    private volatile String currentName = "";

    private final SetupDiscCopy.Progress progress = new SetupDiscCopy.Progress() {
        @Override public void update(long done, long total, int index, int count, String name) {
            bytesDone = done;
            bytesTotal = total;
            fileIndex = index;
            files = count;
            currentName = name;
        }

        @Override public boolean cancelled() {
            return cancel;
        }
    };

    private final Runnable tick = new Runnable() {
        @Override public void run() {
            if (!shown || model.copyingRow() < 0) return;
            model.progress(bytesDone, bytesTotal, fileIndex, files, currentName, SystemClock.elapsedRealtime());
            render();
            root.postDelayed(this, TICK_MS);
        }
    };

    @Override public View createView(ScreenHost screenHost) {
        host = screenHost;
        root = new UiKit.SceneView(host.activity(), host.unit(), new UiKit.Listener() {
            @Override public void onHit(int id) {
                int base = id / 100 * 100, index = id - base;
                if (base == MenuLayout.HIT_ROW) {
                    if (SetupDiscPage.tap(model, index)) onNav(NavInput.CONFIRM, false);
                    else render();
                } else if (base == MenuLayout.HIT_ALERT_BUTTON) {
                    if (SetupDiscPage.answerTap(model, index)) {
                        model.overlayTap(index);
                        perform(model.takeAction());
                    }
                    render();
                } else if (base == MenuLayout.HIT_PROMPT) {
                    if (!onNav(index, false) && index == NavInput.BACK && shown) host.pop();
                }
            }

            @Override public void onScroll(int lines) {
                // Nothing on this page scrolls.
            }

            @Override public void onResized() {
                render();
            }
        });
        LauncherScreen.showBackdrop(host, root);
        render();
        return root;
    }

    @Override public void onShown() {
        shown = true;
        if (model.copyingRow() >= 0) {
            root.removeCallbacks(tick);
            root.postDelayed(tick, TICK_MS);
        } else {
            SetupDiscModel.Look look = model.lookStarts();
            if (look != null) inspect(true, look);
        }
        render();
    }

    @Override public void onHidden() {
        shown = false;
        if (root != null) root.removeCallbacks(tick);
        // Leaving the page stops a running copy at the next block; finished files stay, the row offers Continue.
        if (model.copyingRow() >= 0) cancel = true;
    }

    /**
     * The app left the front (Home, another app, the screen switched off). Before the second repair nothing told the
     * page, and the copy went on for as long as Android let the process run, while the documents said it stopped.
     * Now it stops, as with B: SetupDiscModel.appLeft has the rule and the reason. Nothing else is touched here: an
     * image that is only being read writes nothing. model.appLeft(marker) tells the running copy's marker first (it then
     * records "left", check finding M1), and only then is the copy's cancel flag set (check-2 minor 2).
     */
    @Override public void onAppLeft() {
        int disc = model.copyingRow();
        if (model.appLeft(marker)) {
            cancel = true;
            Log.i(SetupDiscLog.TAG, SetupDiscLog.stopAsked(disc, "app-left"));
            render();
        }
    }

    /**
     * The app is in front again: what is in place and the free space are looked at again, so that the figures on the
     * page are the ones of now (the player may have deleted a disc image in the Files app meanwhile, as the page
     * suggests). Not while an image is read or a copy runs or winds down: their own ends look.
     */
    @Override public void onAppBack() {
        SetupDiscModel.Look look = shown ? model.lookStarts() : null;
        if (look != null) inspect(false, look);
    }

    /**
     * The window has the focus again (the notification shade, a dialog): the free space is looked at again as well, so
     * that a "not enough space" row shows the figures of now (setup-fixes, C3-1: the player freed space and the row
     * still said what was free when the copy failed). A on such a row looks again too (SetupDiscModel.confirm).
     */
    @Override public void onWindowBack() {
        SetupDiscModel.Look look = shown ? model.lookStarts() : null;
        if (look != null) inspect(false, look);
    }

    @Override public boolean onNav(int command, boolean repeat) {
        boolean handled = model.nav(command, repeat);
        perform(model.takeAction());
        render();
        return handled;
    }

    @Override public void onActivityResult(int requestCode, int resultCode, Intent data) {
        int row = requestCode == REQUEST[0] ? SetupDiscModel.PLAY : requestCode == REQUEST[1] ? SetupDiscModel.INSTALL : -1;
        if (row < 0) return;
        Uri uri = resultCode == Activity.RESULT_OK && data != null ? data.getData() : null;
        if (uri == null) {
            Log.i(SetupDiscLog.TAG, SetupDiscLog.nothingChosen(row));
            model.nothingChosen(row);
            render();
            return;
        }
        boolean permission = false;
        try {
            // So that a copy can continue after the app was stopped without choosing again.
            host.activity().getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            permission = true;
        } catch (RuntimeException notOffered) {
            // The provider offers no lasting permission: the copy works now; after a restart the row asks again.
        }
        identify(row, uri, false, permission);
    }

    // ---- actions -----------------------------------------------------------------------------------------------------

    private void perform(int action) {
        int d = model.actionDisc();
        switch (action) {
            case SetupDiscModel.ACT_PICK:
                pick(d);
                break;
            case SetupDiscModel.ACT_CONTINUE: {
                SetupDiscStore.Choice choice = model.disk().remembered[d];
                if (choice != null) identify(d, Uri.parse(choice.uri), true, true);
                else if (chosenNow[d] != null) identify(d, chosenNow[d], true, false);
                else pick(d);
                break;
            }
            case SetupDiscModel.ACT_START:
                startCopy(d);
                break;
            case SetupDiscModel.ACT_STOP:
                cancel = true;
                break;
            case SetupDiscModel.ACT_RECHECK:
                SetupDiscModel.Look look = model.lookStarts();
                if (look != null) inspect(false, look);
                break;
            case SetupDiscModel.ACT_LEAVE:
                host.pop();
                break;
            default:
                break;
        }
    }

    private void pick(int row) {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        // Only storage on or in this device: a file of a cloud service would be downloaded whole first.
        intent.putExtra(Intent.EXTRA_LOCAL_ONLY, true);
        intent.putExtra(DocumentsContract.EXTRA_INITIAL_URI, Uri.parse(DOWNLOADS));
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        try {
            host.startForResult(intent, REQUEST[row]);
            Log.i(SetupDiscLog.TAG, SetupDiscLog.chooser(row, true));
        } catch (RuntimeException none) {
            Log.i(SetupDiscLog.TAG, SetupDiscLog.chooser(row, false));
            model.nothingChosen(row);
        }
    }

    /**
     * Reads what is in place (read-only) off the UI thread, then gives it to the model. look: the token from
     * model.lookStarts(); only this look releases the gate, and a look asked for meanwhile then starts.
     */
    private void inspect(final boolean logPage, final SetupDiscModel.Look look) {
        if (look == null) return;   // no token from lookStarts(), no look (check-2 minor 1)
        final File filesDir = host.filesDir();
        final String apk = host.activity().getApplicationInfo().sourceDir;
        final SetupDiscModel.Disk[] found = new SetupDiscModel.Disk[1];
        host.background(new Runnable() {
            @Override public void run() {
                found[0] = look(apk, filesDir);
            }
        }, new Runnable() {
            @Override public void run() {
                boolean again = model.lookDone(look);
                if (found[0] != null) {
                    model.setDisk(found[0]);
                    if (logPage) {
                        Log.i(SetupDiscLog.TAG, SetupDiscLog.page(found[0]));
                        Log.i(SetupDiscLog.TAG, SetupDiscLog.finish(found[0]));
                    }
                }
                if (again) {
                    SetupDiscModel.Look next = model.lookStarts();
                    if (next != null) inspect(false, next);
                }
                render();
            }
        });
    }

    private static SetupDiscModel.Disk look(String apk, File filesDir) {
        // The game code file and the shader file are not read whole again for every look of this page (the copy from
        // the discs never writes them): ShipVerify answers from the last whole read while they are the same files.
        SetupDiscModel.Disk disk = SetupDiscModel.inspect(filesDir,
                ShipReady.remembered(apk, filesDir, ShipVerify.WHY_SETUP_PAGE), new SetupDiscStore(filesDir), VerifiedExtractor.DISK);
        if (PublicGameCode.isPublic(apk) && disk.codeProblem != null) {
            return new SetupDiscModel.Disk(disk.pins, disk.inPlace, disk.notTrusted, disk.freeBytes, disk.remembered,
                    "Return to Set Up Game (B) and choose Install Game Code to finish setting up.", false, disk.lastCopy, disk.leftoverBytes);
        }
        return disk;
    }

    private void identify(final int row, final Uri uri, final boolean fromMemory, final boolean permission) {
        final ContentResolver resolver = host.activity().getContentResolver();
        final File filesDir = host.filesDir();
        final String apk = host.activity().getApplicationInfo().sourceDir;
        final Object[] result = new Object[4];   // Disk, Disc2Discs.Disc, Recognition, String name; or [1] = failure reason
        model.readingStarted(row, fromMemory);
        render();
        host.background(new Runnable() {
            @Override public void run() {
                SetupDiscModel.Disk disk = look(apk, filesDir);
                result[0] = disk;
                String name = displayName(resolver, uri);
                UriDiscSource image;
                try {
                    image = UriDiscSource.open(resolver, uri);
                } catch (IOException | RuntimeException e) {
                    result[1] = VerifiedExtractor.reason(e);
                    if (!fromMemory && SetupDiscModel.releaseGrant(permission, -1)) {
                        release(resolver, uri);   // could not even be opened: as a refused image (C4-4)
                        Log.i(SetupDiscLog.TAG, SetupDiscLog.released(row, "unreadable"));
                    }
                    return;
                }
                try {
                    long bytes = image.size();
                    Disc2Discs.Disc found = Disc2Discs.open(image, SetupCatalog.game());
                    SetupDiscModel.Recognition r = SetupDiscModel.recognise(found, disk.pins, disk.inPlace, bytes);
                    result[1] = found;
                    result[2] = r;
                    result[3] = name;
                    SetupDiscStore store = new SetupDiscStore(filesDir);
                    if (r.disc >= 0 && permission) {
                        SetupDiscStore.Choice before = store.load(r.disc);
                        if (before != null && !before.uri.equals(uri.toString())) release(resolver, Uri.parse(before.uri));
                        try {
                            store.save(r.disc, new SetupDiscStore.Choice(uri.toString(), name, bytes));
                            Log.i(SetupDiscLog.TAG, SetupDiscLog.choice(r.disc, true, null));
                        } catch (IOException e) {
                            Log.i(SetupDiscLog.TAG, SetupDiscLog.choice(r.disc, false, "not-saved"));
                        }
                    } else if (r.disc < 0 && fromMemory && store.forget(row)) {
                        release(resolver, uri);
                        Log.i(SetupDiscLog.TAG, SetupDiscLog.choice(row, false, "not-usable"));
                    } else if (!fromMemory && SetupDiscModel.releaseGrant(permission, r.disc)) {
                        // A new choice that is not one of the two discs: nothing remembers it, so nothing would ever
                        // give its read permission back (setup-fixes, C4-4).
                        release(resolver, uri);
                        Log.i(SetupDiscLog.TAG, SetupDiscLog.released(row, "refused"));
                    }
                    result[0] = look(apk, filesDir);   // the remembered choices as they are now
                } catch (IOException | RuntimeException e) {
                    result[1] = VerifiedExtractor.reason(e);
                } catch (Error e) {   // a failure of the device itself (out of memory) must not leave the row "Reading"
                    result[1] = e.getClass().getSimpleName();
                } finally {
                    try {
                        image.close();
                    } catch (IOException ignored) {
                        // it was only read
                    }
                }
            }
        }, new Runnable() {
            @Override public void run() {
                SetupDiscModel.Disk disk = (SetupDiscModel.Disk) result[0];
                model.setDisk(disk);
                if (result[1] instanceof String) {
                    String reason = (String) result[1];
                    Log.i(SetupDiscLog.TAG, SetupDiscLog.rememberedGone(row, reason));
                    if (fromMemory) {
                        model.rememberedGone(row, reason);
                        forgetLater(row, uri, "unreadable");
                    } else {
                        model.imageRead(row, new SetupDiscModel.Recognition(-1, Disc2Discs.Kind.UNREADABLE,
                                "The file you chose could not be opened (" + reason + ").", Disc2Discs.NEXT_RECONNECT, 0, -1), null);
                    }
                } else {
                    if (!fromMemory) Log.i(SetupDiscLog.TAG, SetupDiscLog.chosen(row, ((SetupDiscModel.Recognition) result[2]).imageBytes, permission));
                    Log.i(SetupDiscLog.TAG, SetupDiscLog.recognised(row, (Disc2Discs.Disc) result[1], (SetupDiscModel.Recognition) result[2], fromMemory));
                    SetupDiscModel.Recognition r = (SetupDiscModel.Recognition) result[2];
                    if (r.disc >= 0) chosenNow[r.disc] = uri;
                    model.imageRead(row, r, (String) result[3]);
                }
                perform(model.takeAction());   // continuing: the copy starts at once
                render();
            }
        });
    }

    private void forgetLater(final int disc, final Uri uri, final String why) {
        final File filesDir = host.filesDir();
        final ContentResolver resolver = host.activity().getContentResolver();
        host.background(new Runnable() {
            @Override public void run() {
                if (new SetupDiscStore(filesDir).forget(disc)) Log.i(SetupDiscLog.TAG, SetupDiscLog.choice(disc, false, why));
                release(resolver, uri);
            }
        }, new Runnable() {
            @Override public void run() {
                // through the gate as every look (check-2 minor 3); while one runs, another follows it
                SetupDiscModel.Look look = model.lookStarts();
                if (look != null) inspect(false, look);
                else model.lookLater();
            }
        });
    }

    private void startCopy(final int disc) {
        final SetupDiscStore.Choice choice = model.disk().remembered[disc];
        final Uri uri = choice != null ? Uri.parse(choice.uri) : chosenNow[disc];
        if (uri == null) {
            pick(disc);
            return;
        }
        final ContentResolver resolver = host.activity().getContentResolver();
        final File filesDir = host.filesDir();
        final File trace = new File(filesDir, InstallState.PRIVATE_TRACE);
        final String apk = host.activity().getApplicationInfo().sourceDir;
        final SetupDiscCopy.Outcome[] outcome = new SetupDiscCopy.Outcome[1];
        final SetupDiscModel.Disk[] after = new SetupDiscModel.Disk[1];
        cancel = false;
        final SetupDiscStore.Marker running = new SetupDiscStore(filesDir).marker(disc);
        marker = running;
        bytesDone = bytesTotal = 0;
        fileIndex = files = 0;
        currentName = "";
        model.copyStarted(disc, SystemClock.elapsedRealtime());
        Log.i(SetupDiscLog.TAG, SetupDiscLog.copyStart(disc, model.disk(), model.spaceNeed(disc)));
        root.removeCallbacks(tick);
        root.postDelayed(tick, TICK_MS);
        render();
        host.background(new Runnable() {
            @Override public void run() {
                // Before anything is written: if this run of the app ends before the page sees the copy end (killed,
                // the device restarted), the next run finds the marker and the row says so (setup-fixes, C1-1).
                if (!running.begin()) Log.i(SetupDiscLog.TAG, SetupDiscLog.markerNotWritten(disc));
                try {
                    SetupDiscModel.Disk disk = look(apk, filesDir);
                    UriDiscSource image = UriDiscSource.open(resolver, uri);
                    try {
                        Disc2Discs.Disc found = Disc2Discs.open(image, SetupCatalog.game());
                        int serves = found.kind == Disc2Discs.Kind.PLAY ? SetupDiscModel.PLAY
                                : found.kind == Disc2Discs.Kind.INSTALL ? SetupDiscModel.INSTALL : -1;
                        if (serves != disc) {
                            // The log's sentence keeps the name the page's sentence leaves out (Disc2Discs.Disc.named).
                            String changed = "The disc image you chose has changed since it was chosen: ";
                            outcome[0] = new SetupDiscCopy.Outcome(false, false, VerifiedExtractor.Problem.DISC_COPY_BAD,
                                    changed + found.logged(), changed + found.message, 0, 0, 0, 0, 0,
                                    new java.util.ArrayList<SetupDiscCopy.FileResult>(), disk.missing(disc));
                        } else {
                            outcome[0] = SetupDiscCopy.run(found, disk.pins.of(disc), disk.pins.all, disk.notTrusted, trace,
                                    SetupDiscCopy.disk(trace), progress, VerifiedExtractor.DISK, new SetupDiscCopy.Report() {
                                        @Override public void file(SetupDiscCopy.FileResult f) {
                                            Log.i(SetupDiscLog.TAG, SetupDiscLog.file(f));
                                        }
                                    });
                        }
                    } finally {
                        try {
                            image.close();
                        } catch (IOException ignored) {
                            // it was only read
                        }
                    }
                } catch (IOException | RuntimeException e) {
                    outcome[0] = new SetupDiscCopy.Outcome(false, false, VerifiedExtractor.Problem.SOURCE_UNREADABLE,
                            "The disc image could not be opened (" + VerifiedExtractor.reason(e) + ").", 0, 0, 0, 0, 0,
                            new java.util.ArrayList<SetupDiscCopy.FileResult>(), new java.util.ArrayList<Disc2Discs.Pin>());
                } catch (Error e) {   // the row must not stay "Copying" for ever, whatever ended the work
                    outcome[0] = new SetupDiscCopy.Outcome(false, false, VerifiedExtractor.Problem.OTHER,
                            "The copy stopped unexpectedly (" + e.getClass().getSimpleName() + ").", 0, 0, 0, 0, 0,
                            new java.util.ArrayList<SetupDiscCopy.FileResult>(), new java.util.ArrayList<Disc2Discs.Pin>());
                }
                SetupDiscStore store = new SetupDiscStore(filesDir);
                // The end was seen: removed after a copy that ended by itself, "stopped" or "left" after a stop.
                running.end(outcome[0].stopped && outcome[0].error == null);
                if ((outcome[0].complete || outcome[0].problem == VerifiedExtractor.Problem.DISC_COPY_BAD) && store.forget(disc)) {
                    // Done: the image is not needed again. A bad copy: continuing with it would fail at the same file.
                    release(resolver, uri);
                    Log.i(SetupDiscLog.TAG, SetupDiscLog.choice(disc, false, outcome[0].complete ? "done" : "bad-copy"));
                }
                after[0] = look(apk, filesDir);
            }
        }, new Runnable() {
            @Override public void run() {
                root.removeCallbacks(tick);
                Log.i(SetupDiscLog.TAG, SetupDiscLog.copyEnd(disc, outcome[0]));
                model.setDisk(after[0]);
                model.copyFinished(outcome[0]);
                Log.i(SetupDiscLog.TAG, SetupDiscLog.finish(after[0]));
                render();
            }
        });
    }

    private static void release(ContentResolver resolver, Uri uri) {
        try {
            resolver.releasePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (RuntimeException ignored) {
            // there was none to release
        }
    }

    /** The document's name for the player, or null. */
    private static String displayName(ContentResolver resolver, Uri uri) {
        try (Cursor cursor = resolver.query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                String name = cursor.getString(0);
                if (name != null && !name.isEmpty()) return name;
            }
        } catch (RuntimeException ignored) {
            // the name is only for a sentence
        }
        return null;
    }

    private void render() {
        if (root == null) return;
        MenuScenes.Page page = SetupDiscPage.page(model);
        page.touchIcons = root.touchIcons();
        root.show(MenuScenes.build(page, root, root.frame()));
        root.setKeepScreenOn(model.copyingRow() >= 0);   // a long copy must not be cut short by the screen timeout
    }
}
