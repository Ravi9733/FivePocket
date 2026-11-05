package dev.recompbootstrap.smoke;

import android.app.Activity;
import android.content.pm.ApplicationInfo;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.view.Gravity;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.FrameLayout;
import android.widget.TextView;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.file.LinkOption;
import java.nio.file.StandardOpenOption;
import java.io.File;
import android.app.ActivityManager;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.graphics.drawable.ColorDrawable;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.os.SystemClock;
import android.util.DisplayMetrics;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Map;

/** Asset-free launcher used to prove Java, JNI, and ARM64 native startup. */
public final class BootstrapActivity extends Activity {
    private TextView statusView;
    /** The self-test's title (the status lines are statusView); both keep out of the system bars (WindowFit). */
    private TextView headingView;
    private boolean nativeCreated;
    private boolean nativeSurfaceAttached;
    private volatile String diagnosticHeading;   // written by the self-test's worker (configureDiagnostic)
    /**
     * The app's front end (launcher, settings, setup, about). null in the self-test mode, which is everything this
     * class did before the front end existed and still does, unchanged, when the start intent asks for it.
     */
    private LauncherHost launcher;
    /** The self-test's native work, on its own thread (vm-pad-hold D1); null in launcher mode. */
    private SelfTestRun selfTest;
    private SelfTestUi selfTestUi;
    private TextView selfTestHeading;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (!ScreenStack.smokeRequested(new IntentExtras())) {
            // Launcher mode. NativeBridge is not touched on this path, so the self-test library is not loaded.
            launcher = new LauncherHost();
            launcher.create(savedInstanceState == null ? null : savedInstanceState.getString(ScreenStack.AWAITING_KEY));
            return;
        }

        // The runtime, the surface and the swapchain are set up on the self-test's own thread: on a slow device the
        // window says "Running" and B goes back, instead of the menu's main thread waiting (vm-pad-hold D1).
        selfTestUi = new SelfTestUi();
        selfTest = new SelfTestRun(new SelfTestNative(getFilesDir().getAbsolutePath(), getCacheDir().getAbsolutePath()),
                SelfTestRun.processWorker(), selfTestUi);
        selfTest.start();

        FrameLayout content = new FrameLayout(this);
        int padding = (int) (24.0f * getResources().getDisplayMetrics().density);

        SurfaceView surfaceView = new SurfaceView(this);
        surfaceView.getHolder().addCallback(new SmokeSurfaceCallback());
        content.addView(surfaceView, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));

        TextView heading = new TextView(this);
        headingView = heading;
        heading.setText(R.string.smoke_heading);
        if(diagnosticHeading!=null)heading.setText(diagnosticHeading);
        heading.setTextSize(24.0f);
        heading.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        heading.setGravity(Gravity.CENTER);
        heading.setTextColor(Color.WHITE);
        heading.setShadowLayer(4.0f, 0.0f, 2.0f, Color.BLACK);
        FrameLayout.LayoutParams headingLayout = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.TOP);
        heading.setPadding(padding, padding, padding, padding / 2);
        content.addView(heading, headingLayout);
        selfTestHeading = heading;

        statusView = new TextView(this);
        statusView.setTextSize(15.0f);
        statusView.setGravity(Gravity.START);
        statusView.setTextColor(Color.WHITE);
        statusView.setBackgroundColor(0x99000000);
        statusView.setPadding(padding, padding / 2, padding, padding / 2);
        FrameLayout.LayoutParams statusLayout = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.BOTTOM);
        content.addView(statusView, statusLayout);

        // Android 15 draws this window edge to edge (targetSdk 35): the title keeps out of the status bar and the
        // cutout, the status lines out of the navigation bar (setup-fixes, C7-1: the last lines lay under the bar's
        // scrim). The picture behind them stays full size: the test reports the surface's real size.
        final int[] headingBase = {padding, padding, padding, padding / 2};
        final int[] statusBase = {padding, padding / 2, padding, padding / 2};
        content.setOnApplyWindowInsetsListener(new View.OnApplyWindowInsetsListener() {
            @Override public WindowInsets onApplyWindowInsets(View view, WindowInsets insets) {
                int[] bars = barInsets(insets);
                int[] top = WindowFit.padded(headingBase, bars, WindowFit.LEFT | WindowFit.TOP | WindowFit.RIGHT);
                int[] bottom = WindowFit.padded(statusBase, bars, WindowFit.LEFT | WindowFit.BOTTOM | WindowFit.RIGHT);
                headingView.setPadding(top[0], top[1], top[2], top[3]);
                statusView.setPadding(bottom[0], bottom[1], bottom[2], bottom[3]);
                return insets;
            }
        });

        setContentView(content);
        refreshStatus();
    }

    /** What the system bars and a display cutout cover, {left, top, right, bottom} in pixels. */
    @SuppressWarnings("deprecation")   // getSystemWindowInset* are the calls that exist before API 30
    static int[] barInsets(WindowInsets insets) {
        if (Build.VERSION.SDK_INT >= 30) {
            android.graphics.Insets bars = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            return new int[] {bars.left, bars.top, bars.right, bars.bottom};
        }
        return new int[] {insets.getSystemWindowInsetLeft(), insets.getSystemWindowInsetTop(), insets.getSystemWindowInsetRight(),
                insets.getSystemWindowInsetBottom()};
    }

    /**
     * The manifest names fontScale in configChanges (setup-fixes, defect 8: a font change made the menu anew on the
     * PLAY tab, and stopped a running disc copy as if the app had been left). The menu is drawn at its own sizes and
     * needs nothing; the self-test's two text views take the new font size.
     */
    @Override
    public void onConfigurationChanged(android.content.res.Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        if (headingView != null) headingView.setTextSize(24.0f);
        if (statusView != null) statusView.setTextSize(15.0f);
    }

    private void configureDiagnostic() {
        String mode=getIntent().getStringExtra("rgba_diagnostic");
        if(mode==null || !nativeCreated)return;
        // No input is bundled, no automatic discovery, and no title is run.
        // Only the debug APK accepts an explicitly selected private preview.
        if((getApplicationInfo().flags&ApplicationInfo.FLAG_DEBUGGABLE)==0){
            diagnosticHeading="RGBA diagnostic rejected: debug build required";return;
        }
        try {
            byte[] rgba=new byte[1280*720*4];
            boolean rejectReadback="reject-readback".equals(mode);
            if("pattern".equals(mode) || rejectReadback){
                for(int y=0;y<720;++y)for(int x=0;x<1280;++x){
                    int at=(y*1280+x)*4;
                    // Red/green above blue/yellow distinguishes both axes and R/B.
                    rgba[at]=(byte)((x<640)==(y<360)?255:0);
                    rgba[at+1]=(byte)(x>=640?255:0);
                    rgba[at+2]=(byte)(x<640 && y>=360?255:0);
                    rgba[at+3]=(byte)255;
                }
                diagnosticHeading="RGBA upload test — not gameplay";
                if(rejectReadback)diagnosticHeading="Injected readback rejection — must not present";
            } else if("stopped-preview".equals(mode)){
                File input=new File(getFilesDir(),"native-swap.rgba");
                try(FileChannel file=FileChannel.open(input.toPath(),StandardOpenOption.READ,LinkOption.NOFOLLOW_LINKS)){
                    if(file.size()!=rgba.length)throw new IllegalArgumentException("incorrect image size");
                    ByteBuffer target=ByteBuffer.wrap(rgba);
                    while(target.hasRemaining())if(file.read(target)<=0)throw new IllegalArgumentException("short image");
                    if(file.size()!=rgba.length)throw new IllegalArgumentException("image changed");
                }
                diagnosticHeading="Stopped image preview — not live gameplay";
            } else throw new IllegalArgumentException("unknown diagnostic mode");
            if(!NativeBridge.configureRgbaDiagnostic(rgba,rejectReadback))throw new IllegalArgumentException("native input rejected");
        } catch(Exception error){
            diagnosticHeading="RGBA diagnostic rejected: "+error.getClass().getSimpleName();
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (selfTest != null) {
            selfTest.resume();
        }
        if (launcher != null) {
            launcher.resumed();
        }
    }

    /**
     * Android may destroy this activity while another app answers for one of its screens (the system's file chooser,
     * opened by the setup page) and make it anew for the answer. What is saved here lets the new activity show the
     * setup page again and hand it the answer; without it the chosen file was dropped without a word.
     */
    @Override
    protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        if (launcher != null) {
            String awaiting = launcher.awaiting();
            if (awaiting != null) {
                outState.putString(ScreenStack.AWAITING_KEY, awaiting);
            }
        }
    }

    @Override
    protected void onStop() {
        if (launcher != null) {
            launcher.stopped();
        }
        super.onStop();
    }

    @Override
    protected void onPause() {
        if (launcher != null) {
            launcher.paused();
        }
        if (selfTest != null) {
            selfTest.pause();
        }
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        if (launcher != null) {
            launcher.destroy();
        }
        if (selfTest != null) {
            selfTest.leave();   // the worker detaches and ends the runtime; nothing here waits for it
            selfTestUi.clear();
        }
        super.onDestroy();
    }

    private void refreshStatus() {
        if (statusView == null || selfTest == null) {
            return;
        }
        // "Running", the slow-device message, or the report: the build that is handed out shows the report's lines a
        // player can read, the developer build the whole text.
        if (ShipApplication.publicApp()) statusView.setText(selfTest.text(true));
        else statusView.setText(selfTest.text(ShipApplication.carriesGameCode()));
    }

    /** The window's surface goes to SelfTestRun, which hands it to the worker; nothing here calls the native side. */
    private final class SmokeSurfaceCallback implements SurfaceHolder.Callback {
        @Override
        public void surfaceCreated(SurfaceHolder holder) {
            selfTest.surfaceCreated(holder.getSurface(), holder.getSurfaceFrame().width(),
                    holder.getSurfaceFrame().height());
        }

        @Override
        public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
            selfTest.surfaceChanged(holder.getSurface(), width, height);
        }

        @Override
        public void surfaceDestroyed(SurfaceHolder holder) {
            selfTest.surfaceDestroyed();
        }
    }

    /** NativeBridge for SelfTestRun. Called on the self-test's worker thread only. */
    private final class SelfTestNative implements SelfTestRun.Native {
        private final String dataDirectory, cacheDirectory;

        SelfTestNative(String dataDirectory, String cacheDirectory) {
            this.dataDirectory = dataDirectory;
            this.cacheDirectory = cacheDirectory;
        }

        @Override public boolean create() {
            nativeCreated = NativeBridge.create(dataDirectory, cacheDirectory);
            configureDiagnostic();
            final String title = diagnosticHeading;
            if (title != null) {
                selfTestUi.post(new Runnable() {
                    @Override public void run() {
                        if (selfTestHeading != null) selfTestHeading.setText(title);
                    }
                });
            }
            return nativeCreated;
        }

        @Override public boolean attach(Object surface, int width, int height) {
            nativeSurfaceAttached = NativeBridge.surfaceCreated((android.view.Surface) surface, width, height);
            return nativeSurfaceAttached;
        }

        @Override public boolean resize(int width, int height) {
            return NativeBridge.surfaceChanged(width, height);
        }

        @Override public void detach() {
            NativeBridge.surfaceDestroyed();
            nativeSurfaceAttached = false;
        }

        @Override public void pause() {
            NativeBridge.pause();
        }

        @Override public void resume() {
            NativeBridge.resume();
        }

        @Override public void destroy() {
            NativeBridge.destroy();
            nativeCreated = false;
        }

        @Override public String describe() {
            return NativeBridge.describe();
        }
    }

    /** SelfTestRun's UI side: the main thread's queue and the status line. */
    private final class SelfTestUi implements SelfTestRun.Ui {
        private final Handler handler = new Handler(Looper.getMainLooper());

        @Override public void post(Runnable task) {
            handler.post(task);
        }

        @Override public void postDelayed(Runnable task, long delayMs) {
            handler.postDelayed(task, delayMs);
        }

        @Override public void changed() {
            refreshStatus();
        }

        void clear() {
            handler.removeCallbacksAndMessages(null);
        }
    }

    // ---------------------------------------------------------------------------------------------------------
    // Launcher mode (design section 2.3). Everything below does nothing in the self-test mode: launcher is null
    // there and each override falls through to the framework's own behaviour.

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (launcher != null && launcher.key(event)) {
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (launcher != null && launcher.motion(event)) {
            return true;
        }
        return super.dispatchGenericMotionEvent(event);
    }

    @Override
    public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if (launcher != null) {
            launcher.focusChanged(focused);
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (launcher != null) {
            launcher.result(requestCode, resultCode, data);
        }
    }

    /** Reads the start intent for ScreenStack.smokeRequested and ScreenStack.setupRequested. */
    private final class IntentExtras implements ScreenStack.Extras {
        @Override public String text(String name) {
            return getIntent().getStringExtra(name);
        }

        @Override public boolean flag(String name) {
            return getIntent().getBooleanExtra(name, false);
        }
    }

    /**
     * Hosts the stack of screens: gives them a place to draw, turns gamepad and keyboard input into menu commands
     * (NavInput) for the top screen, and pops the top screen on an unhandled BACK. The bottom screen is the
     * launcher and is never popped: leaving the app is the system's Home gesture.
     */
    private final class LauncherHost
            implements ScreenHost, ScreenStack.Listener<Screen>, NavInput.Sink, NavInput.Timer {
        private final Handler handler = new Handler(Looper.getMainLooper());
        private final ScreenStack<Screen> stack = new ScreenStack<Screen>(this);
        private final Map<Screen, View> views = new IdentityHashMap<Screen, View>();
        private final NavInput navInput = new NavInput(this, this);
        /** A pad taken away mid-hold sends no release: every hold ends with it (menu-input-fixes minor 2). */
        private final android.hardware.input.InputManager.InputDeviceListener padRemoved =
                new android.hardware.input.InputManager.InputDeviceListener() {
                    @Override public void onInputDeviceAdded(int id) { }

                    @Override public void onInputDeviceRemoved(int id) {
                        navInput.reset();
                    }

                    @Override public void onInputDeviceChanged(int id) { }
                };
        private FrameLayout container;
        private float unit = 1f;
        private Runnable repeatTask;
        private Screen resultTarget;
        private boolean resumedBefore;
        private final FocusRelay focusRelay = new FocusRelay();
        private boolean destroyed;

        /** savedAwaiting: what awaiting() gave an earlier instance of this activity that Android destroyed, or null. */
        void create(String savedAwaiting) {
            android.hardware.input.InputManager pads = (android.hardware.input.InputManager) getSystemService(INPUT_SERVICE);
            if (pads != null) {
                pads.registerInputDeviceListener(padRemoved, handler);
            }
            getWindow().setBackgroundDrawable(new ColorDrawable(UiKit.BACKGROUND));
            unit = measureUnit();
            container = new FrameLayout(BootstrapActivity.this);
            container.setBackgroundColor(UiKit.BACKGROUND);
            setContentView(container);
            fullPanel();
            // Joystick moves are handed over as they come, not batched to the next frame: a slow frame does not hold
            // the menu's input back (vm-pad-hold D3/D4). Posted: the window is attached by then.
            container.post(new Runnable() {
                @Override public void run() {
                    unbufferedJoystick();
                }
            });
            stack.push(new LauncherScreen());
            if (ScreenStack.restoresSetupPage(savedAwaiting)) {
                // The activity was made anew under the file chooser: the setup page is shown again and is the one the
                // chooser's answer goes to (onActivityResult arrives after onCreate and before onResume).
                Screen setup = new SetupScreen();
                stack.push(setup);
                resultTarget = setup;
                android.util.Log.i(SetupDiscLog.TAG, SetupDiscLog.restored());
            } else if (ScreenStack.setupRequested(new IntentExtras(), (getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0)) {
                stack.push(new SetupScreen());   // a device check of the setup page; see ScreenStack.setupRequested
            }
        }

        /** What onSaveInstanceState keeps: ScreenStack.awaiting for the screen that waits for another app's answer. */
        String awaiting() {
            Screen target = resultTarget;
            return ScreenStack.awaiting(target != null && stack.contains(target), target instanceof SetupScreen);
        }

        @SuppressWarnings("deprecation")   // getDefaultDisplay / getRealMetrics are the calls that exist before API 30
        private float measureUnit() {
            int width, height;
            if (Build.VERSION.SDK_INT >= 30) {
                android.graphics.Rect bounds = getWindowManager().getCurrentWindowMetrics().getBounds();
                width = bounds.width();
                height = bounds.height();
            } else {
                DisplayMetrics metrics = new DisplayMetrics();
                getWindowManager().getDefaultDisplay().getRealMetrics(metrics);
                width = metrics.widthPixels;
                height = metrics.heightPixels;
            }
            return ScreenStack.unit(width, height);
        }

        /** The whole panel, system bars hidden and shown for a moment by an edge swipe: the game window's look. */
        @SuppressWarnings("deprecation")   // setSystemUiVisibility is the call that exists before API 30
        private void fullPanel() {
            Window window = getWindow();
            if (Build.VERSION.SDK_INT >= 28) {
                WindowManager.LayoutParams attributes = window.getAttributes();
                attributes.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
                window.setAttributes(attributes);
            }
            if (Build.VERSION.SDK_INT >= 30) {
                window.setDecorFitsSystemWindows(false);
                WindowInsetsController controller = window.getInsetsController();
                if (controller != null) {
                    controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                    controller.hide(WindowInsets.Type.systemBars());
                }
            } else {
                window.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                        | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
            }
        }

        // ---- activity events ----

        void resumed() {
            navInput.reset();
            // Coming back from the game, the controller check, Home or the file chooser does not pass through the
            // stack, so the visible screen is told here: the launcher screen looks at the installed files again, the
            // two-disc setup page at the files and the free space.
            Screen top = stack.top();
            if (resumedBefore && top != null) {
                top.onAppBack();
            }
            resumedBefore = true;
        }

        /** The app is no longer visible (Home, another app, the screen off): the visible screen is told (onStop). */
        void stopped() {
            Screen top = stack.top();
            if (top != null && !destroyed) {
                top.onAppLeft();
            }
        }

        void paused() {
            navInput.reset();
        }

        void focusChanged(boolean focused) {
            if (focused) {
                fullPanel();   // bars shown by a swipe hide again
            } else {
                navInput.reset();
            }
            // setup-fixes, C3-1: the visible screen is told when the focus comes back (FocusRelay has the rule).
            Screen top = stack.top();
            if (focusRelay.changed(focused) && top != null && !destroyed) top.onWindowBack();
        }

        void destroy() {
            android.hardware.input.InputManager pads = (android.hardware.input.InputManager) getSystemService(INPUT_SERVICE);
            if (pads != null) {
                pads.unregisterInputDeviceListener(padRemoved);
            }
            destroyed = true;
            navInput.reset();
            handler.removeCallbacksAndMessages(null);
            stack.clear();
        }

        boolean key(KeyEvent event) {
            if (ScreenStack.ignoresKey(event.getKeyCode(), event.getFlags())) {
                return true;   // a stick click, or the made-up key Android would send for it: not a menu command
            }
            int action = event.getAction();
            if (action != KeyEvent.ACTION_DOWN && action != KeyEvent.ACTION_UP) {
                return false;
            }
            return navInput.key(event.getKeyCode(), action == KeyEvent.ACTION_DOWN, event.getRepeatCount(), event.getEventTime());
        }

        /** On the window's root view: its own request is the one Android's window reads (API 30 and newer). */
        private void unbufferedJoystick() {
            if (Build.VERSION.SDK_INT >= 30 && !destroyed) {
                getWindow().getDecorView().requestUnbufferedDispatch(InputDevice.SOURCE_CLASS_JOYSTICK);
            }
        }

        boolean motion(final MotionEvent event) {
            if (!event.isFromSource(InputDevice.SOURCE_JOYSTICK) || event.getActionMasked() != MotionEvent.ACTION_MOVE) {
                return false;
            }
            // Every sample of the event, the ones Android batched first: a short tap whose press and release came in
            // one event still counts (vm-pad-hold D3).
            navInput.motion(new NavInput.Motion() {
                @Override public int history() {
                    return event.getHistorySize();
                }

                @Override public float value(int axis, int position) {
                    return position < event.getHistorySize() ? event.getHistoricalAxisValue(axis, position)
                            : event.getAxisValue(axis);
                }

                @Override public long time(int position) {
                    // The sample's own time: a D-pad key and its HAT report are paired by it (NavInput.PAIR_MS).
                    return position < event.getHistorySize() ? event.getHistoricalEventTime(position) : event.getEventTime();
                }
            });
            // Always consumed: Android turns an unconsumed stick or D-pad movement into made-up D-pad key presses.
            return true;
        }

        void result(int requestCode, int resultCode, Intent data) {
            Screen target = resultTarget;
            resultTarget = null;
            if (target != null && stack.contains(target)) {
                target.onActivityResult(requestCode, resultCode, data);
            }
        }

        // ---- NavInput.Sink and NavInput.Timer ----

        @Override public void onNav(int command, boolean repeat) {
            Screen top = stack.top();
            if (top == null || destroyed) {
                return;
            }
            boolean handled = top.onNav(command, repeat);
            if (!handled && command == NavInput.BACK && stack.size() > 1 && top == stack.top()) {
                navInput.reset();
                stack.backNotHandledBy(top);
            }
        }

        @Override public long now() {
            return SystemClock.uptimeMillis();
        }

        @Override public void schedule(Runnable task, long delayMs) {
            cancel();
            repeatTask = task;
            handler.postDelayed(task, delayMs);
        }

        @Override public void cancel() {
            if (repeatTask != null) {
                handler.removeCallbacks(repeatTask);
                repeatTask = null;
            }
        }

        // ---- ScreenStack.Listener: what a stack change means for the views ----

        @Override public void attach(Screen screen) {
            View view = screen.createView(this);
            views.put(screen, view);
            container.addView(view, new FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        }

        @Override public void detach(Screen screen) {
            View view = views.remove(screen);
            if (view != null) {
                container.removeView(view);
            }
        }

        @Override public void shown(Screen screen) {
            View view = views.get(screen);
            if (view != null) {
                view.setVisibility(View.VISIBLE);
            }
            screen.onShown();
        }

        @Override public void hidden(Screen screen) {
            screen.onHidden();
            View view = views.get(screen);
            if (view != null) {
                view.setVisibility(View.GONE);
            }
        }

        // ---- ScreenHost ----

        @Override public Activity activity() {
            return BootstrapActivity.this;
        }

        @Override public File filesDir() {
            return getFilesDir();
        }

        @Override public float unit() {
            return unit;
        }

        @Override public void push(Screen screen) {
            navInput.reset();
            stack.push(screen);
        }

        @Override public void pop() {
            navInput.reset();
            stack.pop();
        }

        @Override public void endHold() {
            navInput.endHold();
        }

        @Override public boolean gameRunning() {
            ActivityManager manager = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
            List<ActivityManager.RunningAppProcessInfo> processes =
                    manager == null ? null : manager.getRunningAppProcesses();
            List<String> names = null;
            if (processes != null) {
                names = new java.util.ArrayList<String>();
                for (ActivityManager.RunningAppProcessInfo info : processes) {
                    if (info.uid == Process.myUid()) {
                        names.add(info.processName);
                    }
                }
            }
            return ScreenStack.gameProcessListed(names, getPackageName());
        }

        @Override public void startForResult(Intent intent, final int requestCode) {
            final Screen caller = stack.top();
            resultTarget = caller;
            try {
                startActivityForResult(intent, requestCode);
            } catch (ActivityNotFoundException nothingToOpen) {
                // No app on the device can show what was asked for: the screen gets a "cancelled" result instead.
                resultTarget = null;
                handler.post(new Runnable() {
                    @Override public void run() {
                        if (!destroyed && caller != null && stack.contains(caller)) {
                            caller.onActivityResult(requestCode, RESULT_CANCELED, null);
                        }
                    }
                });
            }
        }

        @Override public void background(final Runnable work, final Runnable done) {
            new Thread(new Runnable() {
                @Override public void run() {
                    work.run();
                    handler.post(new Runnable() {
                        @Override public void run() {
                            if (!destroyed) {
                                done.run();
                            }
                        }
                    });
                }
            }, "launcher-background").start();
        }
    }
}
