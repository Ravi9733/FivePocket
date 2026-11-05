package dev.recompbootstrap.smoke;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;

/**
 * vm-pad-hold D1 (major): the Graphics Self-Test set up Vulkan in its SurfaceHolder callback on the main thread
 * (shared with the menu); vkCreateSwapchainKHR waited over 5 s and Android showed "isn't responding" with Close app
 * focused. SelfTestRun hands every native call to one worker and keeps the window answering: started, running,
 * finished, failed, slow, left while running. The fakes below run worker jobs and UI tasks only when the test says
 * so, and the fake native side fails the test when it is called from anywhere but the worker.
 */
public final class SelfTestRunTest {
    private static final String S = "SelfTestRun";
    private static final String REPORT = "Native library loaded\nRenderer: presented\nSurface: 1920x1080\nVulkan: Clear/present succeeded at 1920x1080";

    private static final class Fake implements SelfTestRun.Native, SelfTestRun.Worker, SelfTestRun.Ui {
        final ArrayDeque<Runnable> jobs = new ArrayDeque<Runnable>(), uiTasks = new ArrayDeque<Runnable>();
        final List<String> calls = new ArrayList<String>();
        Runnable delayed;
        long delayedMs;
        boolean onWorker;
        boolean createOk = true, attachOk = true;
        /** Like smoke_jni.cpp: create refuses while a window is attached. */
        boolean nativeAttached, strictCreate;
        RuntimeException createThrows;
        LinkageError createLinkage;
        int changes;
        long awaitedMs = -1;

        private void native_(String call) {
            if (!onWorker) throw new AssertionError("native call " + call + " made outside the worker");
            calls.add(call);
        }

        @Override public boolean create() {
            native_("create");
            if (strictCreate && nativeAttached) return false;
            if (createThrows != null) throw createThrows;
            if (createLinkage != null) throw createLinkage;
            return createOk;
        }

        @Override public boolean attach(Object surface, int width, int height) {
            native_("attach " + surface + " " + width + "x" + height);
            nativeAttached = attachOk;
            return attachOk;
        }

        @Override public boolean resize(int width, int height) {
            native_("resize " + width + "x" + height);
            return true;
        }

        @Override public void detach() {
            native_("detach");
            nativeAttached = false;
        }

        @Override public void pause() {
            native_("pause");
        }

        @Override public void resume() {
            native_("resume");
        }

        @Override public void destroy() {
            native_("destroy");
        }

        @Override public String describe() {
            native_("describe");
            return createOk ? REPORT : "Native runtime initialization failed";
        }

        @Override public void run(Runnable job) {
            jobs.add(job);
        }

        @Override public boolean await(long ms) {
            awaitedMs = ms;
            return jobs.isEmpty();   // a stuck worker: the UI thread does not wait for it beyond ms
        }

        @Override public void post(Runnable task) {
            uiTasks.add(task);
        }

        @Override public void postDelayed(Runnable task, long delayMs) {
            delayed = task;
            delayedMs = delayMs;
        }

        @Override public void changed() {
            changes++;
        }

        /** The worker runs one job; what it posted waits for drainUi(). */
        void jobOnly() {
            onWorker = true;
            try {
                jobs.poll().run();
            } finally {
                onWorker = false;
            }
        }

        void drainUi() {
            while (!uiTasks.isEmpty()) uiTasks.poll().run();
        }

        /** The worker runs one job; then the UI thread runs what it posted. */
        void workOne() {
            onWorker = true;
            try {
                jobs.poll().run();
            } finally {
                onWorker = false;
            }
            while (!uiTasks.isEmpty()) uiTasks.poll().run();
        }

        Fake() {
            SelfTestRun.processEnded();   // each case is a fresh process
        }

        void workAll() {
            while (!jobs.isEmpty()) workOne();
        }

        String takeCalls() {
            String joined = String.join(", ", calls);
            calls.clear();
            return joined;
        }
    }

    private static String stateName(int state) {
        return new String[] {"STARTED", "RUNNING", "FINISHED", "FAILED", "SLOW", "LEFT"}[state];
    }

    public static void main(String[] args) {
        MenuFixKit.requireAssertions();
        MenuFixKit.test(S, "started: start() returns at once, no native call on the UI thread, the window says Running with B", () -> {
            Fake f = new Fake();
            SelfTestRun run = new SelfTestRun(f, f, f);
            run.start();
            run.surfaceCreated("surface", 1920, 1080);
            run.surfaceChanged("surface", 1920, 1080);
            MenuFixKit.eq("", f.takeCalls(), "native calls before the worker ran");
            MenuFixKit.eq("STARTED", stateName(run.state()), "state");
            MenuFixKit.check(run.text(false).startsWith("Running") && run.text(false).contains("B: back"), "text: " + run.text(false));
            MenuFixKit.eq(run.text(false), run.text(true), "the same words in the build handed out");
            MenuFixKit.eq(SelfTestRun.SLOW_MS, f.delayedMs, "the slow check is set");
        });
        MenuFixKit.test(S, "running then finished: create, attach, resize on the worker in order; the report is shown", () -> {
            Fake f = new Fake();
            SelfTestRun run = new SelfTestRun(f, f, f);
            run.start();
            run.surfaceCreated("surface", 1920, 1080);
            run.surfaceChanged("surface", 1920, 1080);
            f.workOne();
            MenuFixKit.eq("RUNNING", stateName(run.state()), "state after create");
            MenuFixKit.check(run.text(false).startsWith("Running"), "still Running");
            f.workAll();
            MenuFixKit.eq("create, describe, attach surface 1920x1080, describe, resize 1920x1080, describe", f.takeCalls(), "worker calls");
            MenuFixKit.eq("FINISHED", stateName(run.state()), "state");
            MenuFixKit.eq(REPORT, run.text(false), "developer build: the whole report");
            MenuFixKit.eq(ShipWords.selfTest(REPORT), run.text(true), "build handed out: the picture's lines");
            f.delayed.run();
            MenuFixKit.eq("FINISHED", stateName(run.state()), "the slow check after the end changes nothing");
        });
        MenuFixKit.test(S, "failed: the runtime does not start, throws, or cannot be loaded; or the picture is not presented", () -> {
            Fake f = new Fake();
            f.createOk = false;
            SelfTestRun run = new SelfTestRun(f, f, f);
            run.start();
            run.surfaceCreated("surface", 1920, 1080);
            f.workAll();
            MenuFixKit.eq("create", f.takeCalls(), "no surface work without a runtime");
            MenuFixKit.eq("FAILED", stateName(run.state()), "state");
            MenuFixKit.eq(SelfTestRun.TEXT_NOT_STARTED, run.text(false), "developer text, as before");
            MenuFixKit.eq(ShipWords.SELF_TEST_FAILED, run.text(true), "plain words in the build handed out");

            Fake thrown = new Fake();
            thrown.createThrows = new IllegalStateException("boom");
            SelfTestRun second = new SelfTestRun(thrown, thrown, thrown);
            second.start();
            thrown.workAll();
            MenuFixKit.eq("FAILED", stateName(second.state()), "a runtime that throws");

            Fake unlinked = new Fake();
            unlinked.createLinkage = new UnsatisfiedLinkError("no library");
            SelfTestRun third = new SelfTestRun(unlinked, unlinked, unlinked);
            third.start();
            unlinked.workAll();
            MenuFixKit.eq("FAILED", stateName(third.state()), "a library that cannot be loaded");

            Fake dark = new Fake();
            dark.attachOk = false;
            SelfTestRun fourth = new SelfTestRun(dark, dark, dark);
            fourth.start();
            fourth.surfaceCreated("surface", 1920, 1080);
            dark.workAll();
            MenuFixKit.eq("FAILED", stateName(fourth.state()), "a surface that was not presented");
            MenuFixKit.eq(REPORT, fourth.text(false), "its report is shown");
        });
        MenuFixKit.test(S, "slow: no answer after SLOW_MS says so (B still leaves); a late answer still ends the run", () -> {
            Fake f = new Fake();
            SelfTestRun run = new SelfTestRun(f, f, f);
            run.start();
            run.surfaceCreated("surface", 1920, 1080);
            f.workOne();
            f.delayed.run();
            MenuFixKit.eq("SLOW", stateName(run.state()), "state");
            MenuFixKit.eq(SelfTestRun.TEXT_SLOW, run.text(false), "text");
            MenuFixKit.check(run.text(true).contains("B: back to the menu"), "the way back is named");
            f.workAll();
            MenuFixKit.eq("FINISHED", stateName(run.state()), "the late answer");
        });
        MenuFixKit.test(S, "left while running: B during a stuck swapchain returns at once; later answers are ignored; the worker cleans up", () -> {
            Fake f = new Fake();
            SelfTestRun run = new SelfTestRun(f, f, f);
            run.start();
            run.surfaceCreated("surface", 1920, 1080);
            f.workOne();   // the runtime is up; the attach job is the one that hangs on the device
            run.pause();
            run.surfaceDestroyed();
            MenuFixKit.eq(SelfTestRun.DETACH_WAIT_MS, f.awaitedMs, "the surface's end waits a bounded time only");
            run.leave();
            int changes = f.changes;
            MenuFixKit.eq("LEFT", stateName(run.state()), "state");
            MenuFixKit.eq("create, describe", f.takeCalls(), "nothing more was called on the UI thread");
            f.workAll();
            MenuFixKit.eq("pause, describe, destroy", f.takeCalls(), "the attach of the gone surface is skipped; the runtime is ended");
            MenuFixKit.eq(changes, f.changes, "nothing is shown after leaving");
            MenuFixKit.eq("LEFT", stateName(run.state()), "still left");
            f.delayed.run();
            MenuFixKit.eq("LEFT", stateName(run.state()), "the slow check after leaving changes nothing");
        });
        MenuFixKit.test(S, "left after it finished: the surface is detached and the runtime ended, on the worker", () -> {
            Fake f = new Fake();
            SelfTestRun run = new SelfTestRun(f, f, f);
            run.start();
            run.resume();
            run.surfaceCreated("surface", 1280, 720);
            f.workAll();
            f.takeCalls();
            run.pause();
            run.surfaceDestroyed();
            run.leave();
            MenuFixKit.eq("", f.takeCalls(), "nothing on the UI thread");
            f.workAll();
            MenuFixKit.eq("pause, describe, detach, describe, destroy", f.takeCalls(), "in order on the worker");
        });
        MenuFixKit.test(S, "an answer the worker posted just before the window left is not shown", () -> {
            Fake f = new Fake();
            SelfTestRun run = new SelfTestRun(f, f, f);
            run.start();
            run.surfaceCreated("surface", 1920, 1080);
            f.workOne();
            f.jobOnly();   // the attach ran and posted FINISHED; the UI thread has not run it yet
            run.leave();
            int changes = f.changes;
            f.drainUi();
            MenuFixKit.eq("LEFT", stateName(run.state()), "state");
            MenuFixKit.eq(changes, f.changes, "nothing shown");
        });
        MenuFixKit.test(S, "minor 3: the old window's late onDestroy does not end the runtime the new window set up", () -> {
            Fake f = new Fake();
            SelfTestRun first = new SelfTestRun(f, f, f);
            first.start();
            first.surfaceCreated("surface1", 1920, 1080);
            f.workAll();
            f.takeCalls();
            first.pause();
            first.surfaceDestroyed();
            SelfTestRun second = new SelfTestRun(f, f, f);   // the new window's onCreate comes before the old one's onDestroy
            second.start();
            second.surfaceCreated("surface2", 1920, 1080);
            first.leave();
            f.workAll();
            MenuFixKit.eq("pause, describe, detach, describe, create, describe, attach surface2 1920x1080, describe", f.takeCalls(),
                    "the old window's leave touches nothing of the new runtime");
            MenuFixKit.eq("FINISHED", stateName(second.state()), "the new window");
            second.pause();
            second.leave();
            f.workAll();
            MenuFixKit.eq("pause, describe, detach, destroy", f.takeCalls(), "the new window's own leave ends it");
        });
        MenuFixKit.test(S, "round 3 minor 3: the old window's surface goes away after the new window's onCreate: the new one still starts", () -> {
            Fake f = new Fake();
            f.strictCreate = true;
            SelfTestRun first = new SelfTestRun(f, f, f);
            first.start();
            first.surfaceCreated("surface1", 1920, 1080);
            f.workAll();
            f.takeCalls();
            SelfTestRun second = new SelfTestRun(f, f, f);
            second.start();                       // its create job runs while surface1 is still attached
            second.surfaceCreated("surface2", 1920, 1080);
            f.workAll();
            first.surfaceDestroyed();             // late
            first.leave();
            f.workAll();
            MenuFixKit.eq("FINISHED", stateName(second.state()), "the new window");
            MenuFixKit.eq("detach, create, describe, attach surface2 1920x1080, describe", f.takeCalls(),
                    "surface1 let go before the create; the old window's late jobs touch nothing");
        });
        MenuFixKit.test(S, "a foreign run's late surface job does nothing to the new owner's runtime", () -> {
            Fake f = new Fake();
            SelfTestRun first = new SelfTestRun(f, f, f);
            first.start();
            f.workAll();
            SelfTestRun second = new SelfTestRun(f, f, f);
            second.start();
            second.surfaceCreated("surface2", 1280, 720);
            f.workAll();
            f.takeCalls();
            first.surfaceCreated("surface1", 1920, 1080);   // the old window's surface comes late
            first.surfaceChanged("surface1", 1920, 1080);
            f.workAll();
            MenuFixKit.eq("", f.takeCalls(), "no attach, no resize for the run that no longer owns the runtime");
        });
        MenuFixKit.test(S, "the process worker runs jobs off the calling thread, in order", () -> {
            final List<String> seen = java.util.Collections.synchronizedList(new ArrayList<String>());
            final Thread caller = Thread.currentThread();
            SelfTestRun.Worker worker = SelfTestRun.processWorker();
            for (int i = 0; i < 5; ++i) {
                final int n = i;
                worker.run(() -> seen.add(n + (Thread.currentThread() == caller ? " caller" : " worker")));
            }
            MenuFixKit.check(worker.await(5000), "the jobs ended");
            MenuFixKit.eq("[0 worker, 1 worker, 2 worker, 3 worker, 4 worker]", seen.toString(), "jobs");
            final Object gate = new Object();
            final boolean[] release = {false};
            worker.run(() -> {
                synchronized (gate) {
                    while (!release[0]) {
                        try {
                            gate.wait();
                        } catch (InterruptedException stop) {
                            return;
                        }
                    }
                }
            });
            long before = System.nanoTime();
            boolean ended = worker.await(SelfTestRun.DETACH_WAIT_MS);
            long waitedMs = (System.nanoTime() - before) / 1000000;
            synchronized (gate) {
                release[0] = true;
                gate.notifyAll();
            }
            MenuFixKit.check(!ended && waitedMs < 2000, "a stuck job is waited for " + waitedMs + " ms only");
            MenuFixKit.check(worker.await(5000), "and ends when it is released");
        });
        MenuFixKit.finish(S);
    }
}
