package dev.recompbootstrap.smoke;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.pm.ApplicationInfo;
import android.graphics.Color;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.system.Os;
import android.system.OsConstants;
import android.util.Log;
import android.widget.TextView;
import android.widget.FrameLayout;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.view.KeyEvent;
import android.view.MotionEvent;
import java.io.InputStream;
import java.io.ByteArrayOutputStream;
import java.io.FileDescriptor;
import java.io.FileOutputStream;
import java.security.MessageDigest;

/**
 * The game window. No network. Compiled into every build type since ship week (it was debug-only before, and
 * refused to start unless the app was debuggable; that one condition is gone).
 *
 * What is drawn over the game is GameWindowStatus's decision: no build draws a developer label for any start, only
 * two plain lines when a start was refused or the game has stopped. The debug build draws its developer label
 * again while the switch file files/private-trace/diag-window-banner exists (looked for once, in onCreate).
 * Select held, then A, takes a picture of the game (CaptureController).
 *
 * What still guards a start, in the order they are asked: the dedicated process (the single-instance rule), a
 * well-formed request (mode, 64-digit SHA-256 of the game code, 32-digit run id, 64-digit SHA-256 of the start
 * arguments), a run id that was never used, and then, on one open descriptor, the loader's file rule and the game
 * code's SHA-256 against the one the request names. In the shipped build the launcher is the only caller (the
 * activity is not exported) and names the SHA-256 it has just compared with settings/qualified-code.txt, which
 * ShipFirstRun wrote from the record inside the APK.
 */
public final class PrivateTraceActivity extends Activity {
    private static final String TAG="RecompPrivateTrace";
    private boolean privateProcess;
    private TextView status;
    private boolean gameRequested;
    private boolean previewRequested;
    private boolean liveRequested;
    private int frameMode;
    private boolean runnerStarted;
    private boolean frameSurfaceAttached;
    private Runnable startRunner;
    private PrivateGamepadInput gamepadInput;
    private InputDispatchOption inputDispatch; // null until a request was accepted; see InputDispatchOption
    private boolean closedUsedLauncherRequest; // see InputDispatchOption.closesUsedRequest
    // A start the launcher made (a player pressed Play): its refusal or stop is left for the launcher. Script starts: false.
    private boolean launcherStart;
    // No developer label in this window (GameWindowStatus.plainWindow: every build, unless the debug build's switch file exists).
    private boolean plainWindow;
    private String acceptedLauncherRequest; // the launcher's request once this window has claimed it; null before, and for scripts
    private boolean leaveWithB; // a refused or stopped start in a plain window or made by the launcher: B closes the window
    private CaptureController capture; // Select held + A: a picture of the game (CaptureController); windows with a picture only
    static native void updateGamepad(boolean connected,int buttons,int leftTrigger,int rightTrigger,
            int leftX,int leftY,int rightX,int rightY);
    static native void updateGamepadCapabilities(int buttons,int axes,int triggers);
    static native int[] readGamepadForDiagnostics();

    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        // Nothing runs that could want the button: B goes back to the launcher, as the window's text says.
        if(leaveWithB && event.getKeyCode()==KeyEvent.KEYCODE_BUTTON_B){if(event.getAction()==KeyEvent.ACTION_UP)finish();return true;}
        if(capture!=null && capture.key(event))return true;
        return (gamepadInput!=null && gamepadInput.key(event)) || super.dispatchKeyEvent(event);
    }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent event) {
        return (gamepadInput!=null && gamepadInput.motion(event)) || super.dispatchGenericMotionEvent(event);
    }
    @Override public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if(!focused && gamepadInput!=null)gamepadInput.clear();
        if(focused)fullPanel(); // (bars shown by a swipe hide again)
        if(inputDispatch!=null && inputDispatch.takeRequest(focused))requestUnbufferedJoystick();
    }
    /**
     * Low-delay stick input (files/private-trace/diag-lt2-unbuffered, off unless that file exists): asks the window's
     * root view to deliver joystick MotionEvents (sticks, triggers, HAT) as they arrive instead of holding them until
     * the next frame callback. Reached only through InputDispatchOption.takeRequest, which is true at most once per
     * run and never below API 30. Buttons are KeyEvents and are not affected.
     */
    @android.annotation.TargetApi(30)
    private void requestUnbufferedJoystick(){
        getWindow().getDecorView().requestUnbufferedDispatch(InputDevice.SOURCE_CLASS_JOYSTICK);
        Log.i(InputDispatchOption.LOG_TAG,InputDispatchOption.LOG_LINE);
    }
    /** Looks for the low-delay input switch file once; called from onCreate only, after the request was accepted. */
    private void readInputDispatchOption(){
        inputDispatch=InputDispatchOption.forRun(gameRequested,getFilesDir(),android.os.Build.VERSION.SDK_INT);
    }
    /** Live game windows use the whole panel: system bars hidden, shown transiently by an edge swipe. */
    private void fullPanel(){
        if(!liveRequested)return;
        if(android.os.Build.VERSION.SDK_INT>=28)getWindow().getAttributes().layoutInDisplayCutoutMode=
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        if(android.os.Build.VERSION.SDK_INT>=30){
            getWindow().setDecorFitsSystemWindows(false);
            WindowInsetsController controller=getWindow().getInsetsController();
            if(controller!=null){
                controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                controller.hide(WindowInsets.Type.systemBars());
            }
        }else getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY|View.SYSTEM_UI_FLAG_FULLSCREEN|
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION|View.SYSTEM_UI_FLAG_LAYOUT_STABLE|View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION|
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
    }
    @Override protected void onDestroy() {
        if(capture!=null){capture.close();capture=null;}
        if(gamepadInput!=null){gamepadInput.close();gamepadInput=null;}
        super.onDestroy();
        // A used launcher request closed this screen before it was ever shown, so onStop (which ends the process
        // for every other exit) did not run: end the process here, or it would look like a running game.
        if(closedUsedLauncherRequest && privateProcess)Process.killProcess(Process.myPid());
    }

    private static native boolean prepareProcess(boolean traceRun);
    private static native int claimRequest(String filesDirectory, String requestId);
    private static native int openLibrary(String filesDirectory,int frameMode);
    private static native int checkAbi(int descriptor, String filesDirectory);
    private static native int openArguments(String filesDirectory, String requestId);
    private static native byte[] runTrace(int descriptor, String filesDirectory, String requestId, byte[] manifest,int frameMode);
    private static native boolean attachFrameSurface(Surface surface,int width,int height,boolean reject,boolean failAfterPresent,boolean rejectSecond);
    private static native void resizeFrameSurface(int width,int height);
    private static native void detachFrameSurface();
    private static native void finishCheck();

    @Override public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // Decided once, before anything is shown: no developer label, unless this is the debug build and its switch file exists.
        plainWindow=GameWindowStatus.plainWindow((getApplicationInfo().flags&ApplicationInfo.FLAG_DEBUGGABLE)!=0,getFilesDir());
        status=new TextView(this);
        status.setTextSize(20);
        status.setTextColor(Color.WHITE);
        status.setBackgroundColor(Color.rgb(24,28,36));
        status.setPadding(40,40,40,40);
        setContentView(status);
        if(plainWindow){ // black until the game draws, instead of the theme's window and the grey status page
            getWindow().setBackgroundDrawable(new android.graphics.drawable.ColorDrawable(Color.BLACK));
            status.setBackgroundColor(Color.BLACK);status.setGravity(Gravity.CENTER);status.setVisibility(View.GONE);
        }
        ActivityManager manager=(ActivityManager)getSystemService(ACTIVITY_SERVICE);
        if(manager!=null && manager.getRunningAppProcesses()!=null){
            for(ActivityManager.RunningAppProcessInfo info:manager.getRunningAppProcesses()){
                if(info.pid==Process.myPid() && info.uid==Process.myUid() &&
                        (getPackageName()+":private_trace").equals(info.processName))privateProcess=true;
            }
        }
        String mode=getIntent().getStringExtra("private_trace");
        String expected=getIntent().getStringExtra("expected_sha256");
        String requestId=getIntent().getStringExtra("request_id");
        String manifestHash=getIntent().getStringExtra("manifest_sha256");
        boolean displayRequested="run-native-display".equals(mode) || "run-native-display-reject".equals(mode) ||
                "run-native-display-reject-second".equals(mode) || "run-native-display-post-accept-fail".equals(mode);
        boolean failAfterPresent="run-native-swap-post-accept-fail".equals(mode) || "run-native-display-post-accept-fail".equals(mode);
        boolean rejectSecond="run-native-display-reject-second".equals(mode);
        liveRequested="run-native-swap".equals(mode) || "run-native-swap-reject".equals(mode) || failAfterPresent || displayRequested;
        boolean rejectPreview="run-stopped-preview-reject".equals(mode) || "run-native-swap-reject".equals(mode) ||
                "run-native-display-reject".equals(mode);
        previewRequested="run-stopped-preview".equals(mode) || rejectPreview || liveRequested;
        frameMode=displayRequested?4:(liveRequested?3:(previewRequested?2:0));
        gameRequested="run-trace".equals(mode) || previewRequested;
        if(!privateProcess ||
                !("abi-check".equals(mode) || gameRequested) || expected==null || !expected.matches("[0-9a-f]{64}") ||
                requestId==null || !requestId.matches("[0-9a-f]{32}") ||
                (gameRequested && (manifestHash==null || !manifestHash.matches("[0-9a-f]{64}")))){
            show("REJECT_REQUEST");return;
        }
        // closesUsedRequest(1,...) is true exactly when the id is a line of settings/launcher-runs.txt.
        launcherStart=InputDispatchOption.closesUsedRequest(1,getFilesDir(),requestId);
        System.loadLibrary("recomp_private_debug");
        if(gameRequested)gamepadInput=new PrivateGamepadInput(this);
        if(gameRequested)getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        int claimed=claimRequest(getFilesDir().getAbsolutePath(),requestId);
        if(InputDispatchOption.closesUsedRequest(claimed,getFilesDir(),requestId)){
            // Play, Home, back to the app: Android restarted this activity with the request that was already used.
            // Return to the launcher instead of a refusal screen the gamepad cannot leave. Script starts never get here.
            Log.i(TAG,InputDispatchOption.CLOSED_LINE+" pid="+Process.myPid());
            closedUsedLauncherRequest=true;finish();return;
        }
        if(claimed!=0){show(claimed==1?"REJECT_USED_REQUEST":"REJECT_REQUEST_STORAGE");return;}
        if(launcherStart)acceptedLauncherRequest=requestId; // from here on a refusal is reported to the launcher (show)
        if(!prepareProcess(gameRequested)){show("REJECT_PROCESS");return;}
        readInputDispatchOption();
        startRunner=()->{
            if(runnerStarted)return;
            runnerStarted=true;show("CHECKING_PRIVATE_LIBRARY");
            new Thread(()->{
                String result;
                try {result=verifyAndCheck(expected,requestId,manifestHash);}
                catch(Exception error){result="REJECT_IO_"+error.getClass().getSimpleName();}
                finally {finishCheck();}
                String completed=result;
                runOnUiThread(()->show(completed));
            },"private-library-check").start();
        };
        if(previewRequested){
            FrameLayout content=new FrameLayout(this);
            SurfaceView surface=new SurfaceView(this);
            surface.getHolder().addCallback(new SurfaceHolder.Callback(){
                private void attach(SurfaceHolder holder,int width,int height){
                    if(frameSurfaceAttached || width<1 || height<1)return;
                    frameSurfaceAttached=attachFrameSurface(holder.getSurface(),width,height,rejectPreview,failAfterPresent,rejectSecond);
                    if(frameSurfaceAttached)startRunner.run();
                    else {show("REJECT_SURFACE");finishCheck();}
                }
                @Override public void surfaceCreated(SurfaceHolder holder){
                    attach(holder,holder.getSurfaceFrame().width(),holder.getSurfaceFrame().height());
                }
                @Override public void surfaceChanged(SurfaceHolder holder,int format,int width,int height){
                    if(frameSurfaceAttached)resizeFrameSurface(width,height);else attach(holder,width,height);
                }
                @Override public void surfaceDestroyed(SurfaceHolder holder){
                    if(frameSurfaceAttached){detachFrameSurface();frameSurfaceAttached=false;}
                }
            });
            content.addView(surface,new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,ViewGroup.LayoutParams.MATCH_PARENT));
            ((ViewGroup)status.getParent()).removeView(status);
            status.setTextSize(11);status.setPadding(12,6,12,6);status.setBackgroundColor(0x99000000);
            content.addView(status,new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,ViewGroup.LayoutParams.WRAP_CONTENT,
                    plainWindow?Gravity.CENTER:Gravity.TOP|Gravity.LEFT));
            capture=CaptureController.attach(this,surface,content,e->gamepadInput!=null && gamepadInput.key(e));
            setContentView(content);fullPanel();show(liveRequested?"WAITING_FOR_NATIVE_SWAP_SURFACE":"WAITING_FOR_STOPPED_PREVIEW_SURFACE");
        }else startRunner.run();
    }

    private static String hex(byte[] digest){
        StringBuilder actual=new StringBuilder(64);
        for(byte part:digest)actual.append(String.format(java.util.Locale.ROOT,"%02x",part&255));
        return actual.toString();
    }

    private String verifyAndCheck(String expected,String requestId,String manifestHash) throws Exception {
        String directory=getFilesDir().getAbsolutePath();
        int raw=openLibrary(directory,frameMode);
        if(raw<0)return "REJECT_LIBRARY_"+(-raw);
        // Hash and dlopen use the SAME open descriptor, never a reopened path.
        // The known digest comes from the explicit local developer request;
        // this is an integrity check, not authentication against hostile same-UID code.
        ParcelFileDescriptor descriptor=ParcelFileDescriptor.adoptFd(raw);
        try(InputStream input=new ParcelFileDescriptor.AutoCloseInputStream(descriptor)){
            MessageDigest sha=MessageDigest.getInstance("SHA-256");
            byte[] buffer=new byte[65536];
            long bytes=0;
            for(int count;(count=input.read(buffer))!=-1;){
                if(count==0 || (bytes+=count)>512L*1024*1024)return "REJECT_SIZE";
                sha.update(buffer,0,count);
            }
            if(!hex(sha.digest()).equals(expected))return "REJECT_HASH";
            if(gameRequested){
                int manifestFd=openArguments(directory,requestId);
                if(manifestFd<0)return "REJECT_MANIFEST_FILE";
                byte[] manifest;
                try(InputStream source=new ParcelFileDescriptor.AutoCloseInputStream(ParcelFileDescriptor.adoptFd(manifestFd))){
                    ByteArrayOutputStream owned=new ByteArrayOutputStream();
                    for(int count;(count=source.read(buffer))!=-1;){
                        if(count==0 || owned.size()+count>65536)return "REJECT_MANIFEST_SIZE";
                        owned.write(buffer,0,count);
                    }
                    manifest=owned.toByteArray();
                }
                if(!hex(MessageDigest.getInstance("SHA-256").digest(manifest)).equals(manifestHash))return "REJECT_MANIFEST_HASH";
                runOnUiThread(()->show("RUNNING_GUEST"));
                byte[] result=runTrace(descriptor.getFd(),directory,requestId,manifest,frameMode);
                if(result==null || result.length<4)return "REJECT_NATIVE_RUN";
                int code=((result[0]&255)<<24)|((result[1]&255)<<16)|((result[2]&255)<<8)|(result[3]&255);
                if(code!=0)return "REJECT_RUN_SETUP_"+code;
                if(result.length==4)return "REJECT_EMPTY_PROTOCOL";
                // Only the bounded protocol is saved. No raw output goes to
                // logcat; the host must run its strict parser before claiming a result.
                String destination=directory+"/private-trace/run-"+requestId+"/trace-protocol.txt";
                FileDescriptor file=Os.open(destination,OsConstants.O_WRONLY|OsConstants.O_CREAT|
                        OsConstants.O_EXCL|OsConstants.O_NOFOLLOW|OsConstants.O_CLOEXEC,0600);
                try(FileOutputStream saved=new FileOutputStream(file)){saved.write(result,4,result.length-4);}
                return "TRACE_RETURNED_PROTOCOL_SAVED";
            }
            int code=checkAbi(descriptor.getFd(),directory);
            return code==0?"ABI_PASS":"REJECT_ABI_"+code;
        }
    }

    private void show(String result){
        Log.i(TAG,result+" pid="+Process.myPid()+" uid="+Process.myUid());
        // A refusal or a stop of a request the launcher made and this window accepted is left for the launcher to read.
        GameWindowStatus.report(getFilesDir(),acceptedLauncherRequest,result);
        String text=GameWindowStatus.label(result);
        // Nothing runs that could want the button once a start was refused or the game has stopped: B then leaves.
        leaveWithB=(plainWindow||launcherStart) && text!=null;
        if(plainWindow) {
            // Nothing over the picture while the game is checked, waited for or running; plain words otherwise.
            status.setVisibility(text==null?View.GONE:View.VISIBLE);
            if(text!=null){status.setTextSize(18);status.setPadding(48,32,48,32);status.setText(text);}
            return;
        }
        if(liveRequested) {
            String state="RUNNING_GUEST".equals(result)?"Running native game code":
                    "TRACE_RETURNED_PROTOCOL_SAVED".equals(result)?"STOPPED — last frame only; protocol saved":
                    result.replace('_',' ');
            status.setText("Development build · "+state+"\nGameplay unverified · Android Back exits");
            return;
        }
        status.setText((liveRequested?"Experimental ordered native swap":
                (previewRequested?"STOPPED image consumer — not live game presentation":"Private native-library diagnostic"))+"\n\n"+result+
                (gameRequested?"\n\nGuarded game startup diagnostic.\nProtocol requires host verification.\nTitle screen and gameplay not verified.":
                    "\n\nSeparate debug process; bounded ABI check only.\nNo game execution, native swap or gameplay.")+
                "\nBack exits this process.");
    }

    @Override protected void onStop(){
        super.onStop();
        // This entry does not continue work in the background or offer reload.
        // Process death also bounds native/TLS lifetime after the one-shot check.
        if(privateProcess)Process.killProcess(Process.myPid());
    }
}
