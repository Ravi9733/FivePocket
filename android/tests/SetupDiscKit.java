package dev.recompbootstrap.smoke;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Test support for the two-disc setup: a made-up ship build whose pinned disc files are the synthetic discs' files
 * of Disc2DiscsTest, and a Device that does what SetupDiscScreen does with the model's actions, synchronously and
 * without Android (documents are DiscSources by address). It is a twin of the screen's glue, not the screen itself.
 */
final class SetupDiscKit {
    private SetupDiscKit() {}

    static final byte[][] PLAY_FILES = {Disc2DiscsTest.EXE, Disc2DiscsTest.COMMON, Disc2DiscsTest.META, Disc2DiscsTest.AUDIO};
    static final byte[][] INSTALL_FILES = {Disc2DiscsTest.PART0, Disc2DiscsTest.STAMP0, Disc2DiscsTest.PART1, Disc2DiscsTest.STAMP1,
            Disc2DiscsTest.PART2, Disc2DiscsTest.STAMP2, Disc2DiscsTest.PART3, Disc2DiscsTest.STAMP3};

    static List<SetupCatalog.Item> items(String disc, byte[][] files) {
        List<SetupCatalog.Item> out = new ArrayList<SetupCatalog.Item>();
        for (int i = 0; i < files.length; ++i) out.add(new SetupCatalog.Item(disc, disc + "-" + i, files[i].length, SetupTestKit.sha256(files[i])));
        return out;
    }

    static final List<SetupCatalog.Item> PLAY_ITEMS = items(SetupCatalog.PLAY, PLAY_FILES);
    static final List<SetupCatalog.Item> INSTALL_ITEMS = items(SetupCatalog.INSTALL, INSTALL_FILES);

    static byte[] playImage() {
        return Disc2DiscsTest.playImage(Disc2DiscsTest.EXE, Disc2DiscsTest.COMMON).data;
    }

    static byte[] installImage() {
        return Disc2DiscsTest.installImage(Disc2DiscsTest.packages()).data;
    }

    /** A made-up app version whose 12 not-bundled entries are the two synthetic discs' files. */
    static ShipTestKit.Version version(File files) {
        ShipTestKit.Version v = new ShipTestKit.Version(files);
        v.inputs.clear();
        for (byte[] f : PLAY_FILES) v.inputs.add(f);
        for (byte[] f : INSTALL_FILES) v.inputs.add(f);
        v.bundledPaths.addAll(java.util.Arrays.asList(ShipTestKit.GENERATED));
        return v.build();
    }

    /** One device: its files directory, the ship build, the documents the player can choose, the page. */
    static final class Device {
        final File files;
        final ShipTestKit.Version version;
        final Map<String, DiscSource> documents = new LinkedHashMap<String, DiscSource>();
        final Map<String, String> names = new LinkedHashMap<String, String>();
        final List<String> log = new ArrayList<String>();
        VerifiedExtractor.Space space = SetupTestKit.PLENTY;
        SetupDiscModel model = new SetupDiscModel();
        /** What the next chooser returns: a document address, or null (closed without a choice). */
        String nextChoice;
        /** Whether the provider offers a lasting permission for the next choice. */
        boolean persistable = true;
        /** The progress of the next copy (tests stop or "kill" it). */
        SetupDiscCopy.Progress copyProgress;
        int choosersOpened;
        final String[] chosenNow = new String[SetupDiscModel.DISCS];
        /**
         * setup-fixes: the process is killed while the next copy runs (set from a test's Progress). The copy's own
         * work ends where it is (SetupDiscCopy.run returns as after a stop), and nothing of what the screen does after
         * the copy runs: no marker removed, no choice forgotten, no copyFinished. The test then calls restart().
         */
        boolean killNow;
        /** setup-fixes: this simulated process's token (SetupDiscStore's); restart() gives a new one, reopen() keeps it. */
        String process = "process-1";
        private int processes = 1;
        /** The marker of the running copy, as SetupDiscScreen keeps it. */
        SetupDiscStore.Marker marker;

        Device(String label, boolean firstRunDone) throws IOException {
            files = SetupTestKit.newFilesDir(label);
            version = version(files);
            if (firstRunDone) firstRun();
        }

        void firstRun() {
            ShipFirstRun.Result r = ShipFirstRun.run(version.bundle, files, ShipTestKit.QUIET, ShipTestKit.PLENTY);
            if (!r.ok()) throw new IllegalStateException("first run: " + r.error);
        }

        String document(String name, byte[] data) {
            return document(name, new SetupTestKit.MemoryDisc(Disc2DiscsTest.XGD3, data));
        }

        String document(String name, DiscSource source) {
            String uri = "content://test.documents/document/" + name;
            documents.put(uri, source);
            names.put(uri, name);
            return uri;
        }

        File trace() {
            return new File(files, InstallState.PRIVATE_TRACE);
        }

        ShipReady ship() {
            return ShipReady.of(version.bundle, files);
        }

        SetupDiscModel.Disk look() {
            return SetupDiscModel.inspect(files, ship(), store(), space, PLAY_ITEMS, INSTALL_ITEMS);
        }

        /** The page is opened (or shown again). */
        Device open() {
            SetupDiscModel.Look token = model.lookStarts();   // SetupDiscScreen.onShown: no look while one runs or while busy
            if (token == null) return this;
            SetupDiscModel.Disk disk = look();
            model.lookDone(token);
            model.setDisk(disk);
            log.add(SetupDiscLog.page(disk));
            return this;
        }

        /** B to the menu and Set Up Game Files again: a new page model in the same process (LauncherScreen pushes a new page). */
        Device reopen() {
            model = new SetupDiscModel();
            return open();
        }

        /** The process ends and starts again: a new page model over the same storage, a new process token. */
        Device restart() {
            process = "process-" + (++processes);
            model = new SetupDiscModel();
            for (int d = 0; d < chosenNow.length; ++d) chosenNow[d] = null;
            return open();
        }

        /** The app left the front while the page is shown: what SetupDiscScreen.onAppLeft does. */
        void appLeft() {
            model.appLeft(marker);   // SetupDiscScreen.onAppLeft: the model tells the marker, then the copy is cancelled
        }

        /** The window got the focus back (or the app came back): what SetupDiscScreen.onWindowBack / onAppBack do. */
        void windowBack() {
            SetupDiscModel.Look token = model.lookStarts();
            if (token == null) return;
            model.setDisk(look());
            if (model.lookDone(token)) windowBack();   // a look asked for meanwhile
        }

        /** SetupDiscScreen.forgetLater's look: through the gate; while another look runs, it follows that one. */
        void lookAfterForget() {
            SetupDiscModel.Look token = model.lookStarts();
            if (token == null) {
                model.lookLater();
                return;
            }
            model.setDisk(look());
            if (model.lookDone(token)) windowBack();
        }

        SetupDiscStore store() {
            return new SetupDiscStore(files, process);
        }

        /** A button: what SetupDiscScreen.onNav does. */
        boolean press(int command) {
            boolean handled = model.nav(command, false);
            perform(model.takeAction());
            return handled;
        }

        void tapAnswer(int button) {
            model.overlayTap(button);
            perform(model.takeAction());
        }

        void perform(int action) {
            int d = model.actionDisc();
            switch (action) {
                case SetupDiscModel.ACT_PICK:
                    pick(d);
                    break;
                case SetupDiscModel.ACT_CONTINUE: {
                    SetupDiscStore.Choice c = model.disk().remembered[d];
                    if (c != null) identify(d, c.uri, true, true);
                    else if (chosenNow[d] != null) identify(d, chosenNow[d], true, false);
                    else pick(d);
                    break;
                }
                case SetupDiscModel.ACT_START:
                    copy(d);
                    break;
                case SetupDiscModel.ACT_RECHECK:
                    windowBack();   // the same gated look SetupDiscScreen runs
                    break;
                default:
                    break;
            }
        }

        void pick(int row) {
            ++choosersOpened;
            log.add(SetupDiscLog.chooser(row, true));
            String uri = nextChoice;
            nextChoice = null;
            if (uri == null) {
                log.add(SetupDiscLog.nothingChosen(row));
                model.nothingChosen(row);
                return;
            }
            identify(row, uri, false, persistable);
        }

        void identify(int row, String uri, boolean fromMemory, boolean permission) {
            model.readingStarted(row, fromMemory);
            SetupDiscModel.Disk disk = look();
            DiscSource image = documents.get(uri);
            SetupDiscStore store = new SetupDiscStore(files);
            if (image == null) {
                model.setDisk(disk);
                log.add(SetupDiscLog.rememberedGone(row, "the file cannot be opened"));
                if (fromMemory) {
                    model.rememberedGone(row, "the file cannot be opened");
                    if (store.forget(row)) log.add(SetupDiscLog.choice(row, false, "unreadable"));
                    lookAfterForget();
                } else {
                    model.imageRead(row, new SetupDiscModel.Recognition(-1, Disc2Discs.Kind.UNREADABLE, "The file you chose could not be opened (gone).",
                            Disc2Discs.NEXT_RECONNECT, 0, -1), null);
                }
                perform(model.takeAction());
                return;
            }
            long bytes;
            try {
                bytes = image.size();
            } catch (IOException e) {
                bytes = -1;
            }
            Disc2Discs.Disc found = Disc2Discs.open(image, Disc2DiscsTest.GAME);
            SetupDiscModel.Recognition r = SetupDiscModel.recognise(found, disk.pins, disk.inPlace, bytes);
            if (r.disc >= 0 && permission) {
                try {
                    store.save(r.disc, new SetupDiscStore.Choice(uri, names.get(uri), bytes));
                    log.add(SetupDiscLog.choice(r.disc, true, null));
                } catch (IOException e) {
                    throw new IllegalStateException(e);
                }
            } else if (r.disc < 0 && fromMemory && store.forget(row)) {
                log.add(SetupDiscLog.choice(row, false, "not-usable"));
            }
            model.setDisk(look());
            log.add(SetupDiscLog.recognised(row, found, r, fromMemory));
            if (r.disc >= 0) chosenNow[r.disc] = uri;
            model.imageRead(row, r, names.get(uri));
            perform(model.takeAction());
        }

        SetupDiscCopy.Outcome lastCopy;

        void copy(int disc) {
            SetupDiscStore.Choice c = model.disk().remembered[disc];
            String uri = c != null ? c.uri : chosenNow[disc];
            if (uri == null) {
                pick(disc);
                return;
            }
            model.copyStarted(disc, 0);
            marker = store().marker(disc);
            log.add(SetupDiscLog.copyStart(disc, model.disk(), model.spaceNeed(disc)));
            if (!marker.begin()) log.add(SetupDiscLog.markerNotWritten(disc));   // SetupDiscScreen.startCopy: before anything is written
            SetupDiscModel.Disk disk = look();
            DiscSource image = documents.get(uri);
            SetupDiscCopy.Outcome o;
            if (image == null) {
                o = new SetupDiscCopy.Outcome(false, false, VerifiedExtractor.Problem.SOURCE_UNREADABLE, "The disc image could not be opened (gone).",
                        0, 0, 0, 0, 0, new ArrayList<SetupDiscCopy.FileResult>(), new ArrayList<Disc2Discs.Pin>());
            } else {
                Disc2Discs.Disc found = Disc2Discs.open(image, Disc2DiscsTest.GAME);
                final SetupDiscModel model = this.model;
                SetupDiscCopy.Progress progress = copyProgress != null ? copyProgress : new SetupDiscCopy.Progress() {
                    @Override public void update(long done, long total, int index, int count, String name) {
                        model.progress(done, total, index, count, name, 0);
                    }

                    @Override public boolean cancelled() {
                        return false;
                    }
                };
                o = SetupDiscCopy.run(found, disk.pins.of(disc), disk.pins.all, disk.notTrusted, trace(), SetupDiscCopy.disk(trace()), progress, space,
                        new SetupDiscCopy.Report() {
                            @Override public void file(SetupDiscCopy.FileResult f) {
                                log.add(SetupDiscLog.file(f));
                            }
                        });
            }
            if (killNow) {
                killNow = false;
                lastCopy = o;
                return;   // the process is gone: the page never saw the end
            }
            SetupDiscStore store = new SetupDiscStore(files);
            marker.end(o.stopped && o.error == null);   // SetupDiscScreen.startCopy, after the copy
            if ((o.complete || o.problem == VerifiedExtractor.Problem.DISC_COPY_BAD) && store.forget(disc)) {
                log.add(SetupDiscLog.choice(disc, false, o.complete ? "done" : "bad-copy"));
            }
            lastCopy = o;
            log.add(SetupDiscLog.copyEnd(disc, o));
            model.setDisk(look());
            model.copyFinished(o);
            log.add(SetupDiscLog.finish(model.disk()));
        }

        /** Every text the page shows now: rows, details, footer, question. */
        List<String> texts() {
            List<String> out = new ArrayList<String>();
            for (int r = 0; r < SetupDiscModel.ROWS; ++r) {
                out.add(model.label(r));
                out.add(model.stateWord(r));
                out.add(model.detail(r));
            }
            out.add(model.footer());
            out.add(model.overlayText());
            Collections.addAll(out, model.overlayButtons());
            return out;
        }
    }
}
