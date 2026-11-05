package dev.recompbootstrap.smoke;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

/**
 * One run of the Graphics Self-Test (ABOUT > Graphics Self-Test), kept off the UI thread (vm-pad-hold D1). Pure Java.
 *
 * <p>The test's native work (the runtime, the Vulkan surface and swapchain, one cleared and presented frame) used to
 * run in the window's SurfaceHolder callbacks on the main thread. On the virtual device vkCreateSwapchainKHR waited
 * more than 5 s for a buffer; the main thread is the menu's too (same process), so Android showed "isn't responding"
 * with Close app focused. Here every native call runs on one worker thread, in the order the window asked for it; the
 * UI thread only hands the calls over and shows what came back. The window says "Running" with B as the way back
 * until an answer arrives; with no answer after SLOW_MS it says so (B still leaves). A failure ends in its message.
 *
 * <p>States (UI thread): STARTED (start() asked for the runtime), RUNNING (the runtime is up, the picture is being
 * set up), FINISHED (a frame was presented), FAILED (the runtime or the picture could not be set up), SLOW (no answer
 * yet after SLOW_MS), LEFT (the window was closed; whatever the worker still answers is ignored, and the worker
 * detaches the surface and ends the runtime after the job it is in).
 */
final class SelfTestRun {
    static final int STARTED = 0, RUNNING = 1, FINISHED = 2, FAILED = 3, SLOW = 4, LEFT = 5;
    /** No answer from the worker this long after start(): the window says so. */
    static final long SLOW_MS = 8000;
    /**
     * When Android takes the surface away, the UI thread waits this long at most for the worker to let go of it (a
     * SurfaceHolder callback should not return while the surface is still drawn to); a stuck worker is not waited for.
     */
    static final long DETACH_WAIT_MS = 500;
    static final String TEXT_RUNNING = "Running the graphics test…\nB: back to the menu.";
    static final String TEXT_SLOW = "The graphics test has had no answer from this device's graphics driver for "
            + (SLOW_MS / 1000) + " seconds.\nB: back to the menu.";
    /** The report of a runtime that did not start (what the window showed before; ShipWords.selfTest words it plainly). */
    static final String TEXT_NOT_STARTED = "Native runtime initialization failed";

    /** The native test (NativeBridge). Every call is made on the worker thread only. */
    interface Native {
        boolean create();
        boolean attach(Object surface, int width, int height);
        boolean resize(int width, int height);
        void detach();
        void pause();
        void resume();
        void destroy();
        String describe();
    }

    /** One background thread: jobs run one after another, in the order given, never on the UI thread. */
    interface Worker {
        void run(Runnable job);
        /** Waits at most ms for every job given so far to end; true when they all did. */
        boolean await(long ms);
    }

    /** The window's side, all on the UI thread. */
    interface Ui {
        void post(Runnable task);
        void postDelayed(Runnable task, long delayMs);
        /** The state or the report changed: show text(). */
        void changed();
    }

    private final Native gpu;
    private final Worker worker;
    private final Ui ui;
    private int state = STARTED;
    private String report = "";
    private boolean started;
    /** Bumped on the UI thread when the surface goes away or the window closes: older surface jobs then do nothing. */
    private volatile int surfaceGeneration;
    // Touched by worker jobs only.
    private boolean created, attached;
    /**
     * The run whose create() last set up the process's one native runtime (worker jobs only). A window's late jobs
     * (its onDestroy arriving after the next window's onCreate) must not detach or end the runtime the next window set
     * up: only the owner's jobs touch it (menu-input-fixes minor 3; today's code had the same flaw).
     */
    private static SelfTestRun owner;

    SelfTestRun(Native gpu, Worker worker, Ui ui) {
        this.gpu = gpu;
        this.worker = worker;
        this.ui = ui;
    }

    int state() {
        return state;
    }

    /** A new process: no run owns a runtime (host tests start each case so; the app never calls it). */
    static void processEnded() {
        owner = null;
    }

    /** What the window shows; ship: the build handed out (only the report's lines about the picture). */
    String text(boolean ship) {
        if (state == STARTED || state == RUNNING) return TEXT_RUNNING;
        if (state == SLOW) return TEXT_SLOW;
        return ship ? ShipWords.selfTest(report) : report;
    }

    /** The window was made: the runtime is started on the worker. Returns at once. */
    void start() {
        if (started) return;
        started = true;
        ui.changed();
        ui.postDelayed(new Runnable() {
            @Override public void run() {
                if (state == STARTED || state == RUNNING) {
                    state = SLOW;
                    ui.changed();
                }
            }
        }, SLOW_MS);
        worker.run(new Runnable() {
            @Override public void run() {
                boolean ok;
                try {
                    // The previous window's surface may still be attached (its surfaceDestroyed comes after this
                    // window's onCreate): the native create refuses while a window is attached, so it is let go first
                    // (round 3, minor 3; today's code had the same flaw).
                    SelfTestRun previous = owner;
                    if (previous != null && previous != SelfTestRun.this && previous.attached) {
                        gpu.detach();
                        previous.attached = false;
                    }
                    ok = gpu.create();
                } catch (RuntimeException | LinkageError failed) {
                    ok = false;   // the library could not be loaded, or the runtime threw: a failure, said as one
                }
                created = ok;
                if (ok) owner = SelfTestRun.this;
                final boolean up = ok;
                final String text = ok ? describe() : TEXT_NOT_STARTED;
                ui.post(new Runnable() {
                    @Override public void run() {
                        if (state == LEFT) return;
                        if (!up) {
                            end(FAILED, text);
                        } else if (state == STARTED) {
                            state = RUNNING;
                            report = text;
                            ui.changed();
                        }
                    }
                });
            }
        });
    }

    /** SurfaceHolder.Callback.surfaceCreated. */
    void surfaceCreated(final Object surface, final int width, final int height) {
        surfaceJob(surface, width, height, false);
    }

    /** SurfaceHolder.Callback.surfaceChanged: a new size for an attached surface, else the first attach. */
    void surfaceChanged(Object surface, int width, int height) {
        if (width <= 0 || height <= 0) return;
        surfaceJob(surface, width, height, true);
    }

    private void surfaceJob(final Object surface, final int width, final int height, final boolean resize) {
        if (state == LEFT) return;
        final int generation = surfaceGeneration;
        worker.run(new Runnable() {
            @Override public void run() {
                // No runtime, a runtime another window set up since, or the surface is gone.
                if (!created || owner != SelfTestRun.this || generation != surfaceGeneration) return;
                boolean ok;
                try {
                    if (resize && attached) {
                        ok = gpu.resize(width, height);
                    } else {
                        ok = gpu.attach(surface, width, height);
                        attached = ok;
                    }
                } catch (RuntimeException | LinkageError failed) {
                    ok = false;
                }
                final boolean presented = ok;
                final String text = describe();
                ui.post(new Runnable() {
                    @Override public void run() {
                        if (state != LEFT) end(presented ? FINISHED : FAILED, text);
                    }
                });
            }
        });
    }

    /** SurfaceHolder.Callback.surfaceDestroyed: the worker lets go of the surface; waited for DETACH_WAIT_MS at most. */
    void surfaceDestroyed() {
        surfaceGeneration++;
        worker.run(new Runnable() {
            @Override public void run() {
                if (!attached || owner != SelfTestRun.this) return;
                try {
                    gpu.detach();
                } catch (RuntimeException | LinkageError ignored) {
                    // nothing is left to show it to
                }
                attached = false;
                report(describe());
            }
        });
        worker.await(DETACH_WAIT_MS);
    }

    /** Activity.onResume / onPause: handed to the runtime in order with everything else. */
    void resume() {
        lifecycle(true);
    }

    void pause() {
        lifecycle(false);
    }

    private void lifecycle(final boolean resume) {
        if (state == LEFT) return;
        worker.run(new Runnable() {
            @Override public void run() {
                if (!created || owner != SelfTestRun.this) return;
                try {
                    if (resume) gpu.resume();
                    else gpu.pause();
                } catch (RuntimeException | LinkageError ignored) {
                    // the report says what the runtime's state is
                }
                report(describe());
            }
        });
    }

    /** Activity.onDestroy: nothing more is shown; the worker detaches and ends the runtime after its current job. */
    void leave() {
        state = LEFT;
        surfaceGeneration++;
        worker.run(new Runnable() {
            @Override public void run() {
                try {
                    if (owner == SelfTestRun.this) {
                        if (attached) gpu.detach();
                        if (created) gpu.destroy();
                    }
                } catch (RuntimeException | LinkageError ignored) {
                    // the window is gone
                }
                if (owner == SelfTestRun.this) owner = null;
                attached = false;
                created = false;
            }
        });
    }

    private void end(int how, String text) {
        state = how;
        report = text;
        ui.changed();
    }

    /** A new report from the worker: shown once the test has ended (while it runs the window says Running). */
    private void report(final String text) {
        ui.post(new Runnable() {
            @Override public void run() {
                if (state == FINISHED || state == FAILED) {
                    report = text;
                    ui.changed();
                }
            }
        });
    }

    private String describe() {
        try {
            String text = gpu.describe();
            return text == null ? "" : text;
        } catch (RuntimeException | LinkageError failed) {
            return TEXT_NOT_STARTED;
        }
    }

    /** The process's one worker: every self-test window's native calls run on this thread, one after another. */
    static Worker processWorker() {
        return SerialWorker.INSTANCE;
    }

    private static final class SerialWorker implements Worker {
        static final SerialWorker INSTANCE = new SerialWorker();
        private final ExecutorService executor = Executors.newSingleThreadExecutor(job -> {
            Thread thread = new Thread(job, "self-test-graphics");
            thread.setDaemon(true);
            return thread;
        });

        @Override public void run(Runnable job) {
            executor.execute(job);
        }

        @Override public boolean await(long ms) {
            CountDownLatch done = new CountDownLatch(1);
            executor.execute(done::countDown);
            try {
                return done.await(ms, TimeUnit.MILLISECONDS);
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
                return false;
            }
        }
    }
}
