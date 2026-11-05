package dev.recompbootstrap.smoke;

import android.app.Activity;
import android.app.ActivityManager;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.os.SystemClock;
import android.view.KeyEvent;
import android.util.TypedValue;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;

/** No game code/assets. Reads the same JNI snapshot used by guest ABI5; every word it shows is ControllerCheckText's.
 *  In every build type since ship week. */
public final class ControllerCheckActivity extends Activity {
    private PrivateGamepadInput input;
    private TextView status;
    private int seenButtons;
    private boolean leftTriggerSeen,rightTriggerSeen;
    private int stickDirections;
    // Leaving without touch: B is one of the tested buttons here, so the exit is Select + Start held for one second
    // (Xbox bits: Back = 32, Start = 16). The timer checks again while both are held, because a held button need not
    // produce further input events.
    private final HoldChord exitChord=new HoldChord(32,16,1000);
    private final Handler exitTimer=new Handler(Looper.getMainLooper());
    private final Runnable exitCheck=this::checkExit;
    private boolean privateProcess;
    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        ActivityManager manager=(ActivityManager)getSystemService(ACTIVITY_SERVICE);
        if(manager!=null && manager.getRunningAppProcesses()!=null){
            for(ActivityManager.RunningAppProcessInfo info:manager.getRunningAppProcesses()){
                if(info.pid==Process.myPid() && info.uid==Process.myUid() &&
                        (getPackageName()+":private_trace").equals(info.processName))privateProcess=true;
            }
        }
        System.loadLibrary("recomp_private_debug");
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().setBackgroundDrawable(new android.graphics.drawable.ColorDrawable(Color.BLACK));
        status=new TextView(this);status.setTextColor(Color.WHITE);
        status.setPadding(48,32,48,32);status.setLineSpacing(0,1.15f);
        fitText();
        // The whole text on one screen at every font size, out of the system bars (setup-fixes, C7-1: at the largest
        // font the button table was off the screen, and the pad cannot scroll; Android 15 draws edge to edge). The
        // text keeps its size while it fits and is made smaller only when it would not (WindowFit.CHECK_TEXT_*).
        FrameLayout frame=new FrameLayout(this);frame.setBackgroundColor(Color.BLACK);
        frame.addView(status,new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,FrameLayout.LayoutParams.MATCH_PARENT));
        final int[] base={48,32,48,32};
        frame.setOnApplyWindowInsetsListener(new View.OnApplyWindowInsetsListener(){
            @Override public WindowInsets onApplyWindowInsets(View view,WindowInsets insets){
                int[] p=WindowFit.padded(base,BootstrapActivity.barInsets(insets),WindowFit.ALL);
                status.setPadding(p[0],p[1],p[2],p[3]);
                return insets;
            }
        });
        setContentView(frame);
        input=new PrivateGamepadInput(this);input.observe(this::render);
    }
    /** Controller Check's text size: as before while it fits, smaller (down to a floor) at a large system font size. */
    private void fitText() {
        status.setTextSize(TypedValue.COMPLEX_UNIT_SP,WindowFit.CHECK_TEXT_SP);
        status.setAutoSizeTextTypeUniformWithConfiguration(WindowFit.CHECK_TEXT_MIN_SP,WindowFit.CHECK_TEXT_SP,1,TypedValue.COMPLEX_UNIT_SP);
    }
    /** fontScale is in the manifest's configChanges: a font change no longer ends this screen (its onStop ends the process). */
    @Override public void onConfigurationChanged(android.content.res.Configuration newConfig){super.onConfigurationChanged(newConfig);if(status!=null)fitText();}
    private void render() {
        int[] s=PrivateTraceActivity.readGamepadForDiagnostics();
        if(s==null||s.length!=9){status.setText(ControllerCheckText.unreadable());checkExit();return;}
        // Coverage excludes injected values, even if another physical event arrives while they are held.
        if(input.lastEvent().startsWith("PHYSICAL")) {
            int[] p=input.physicalState();
            seenButtons|=p[0];
            leftTriggerSeen|=p[1]>20;rightTriggerSeen|=p[2]>20;
            for(int i=0;i<4;++i){if(p[3+i]<-16000)stickDirections|=1<<(i*2);if(p[3+i]>16000)stickDirections|=2<<(i*2);}
        }
        status.setText(ControllerCheckText.screen(input.description(),s[2],s[3],s[4],s[5],s[6],s[7],s[8],
                seenButtons,leftTriggerSeen,rightTriggerSeen,stickDirections));
        checkExit();
    }
    /** Finishes the screen once Select + Start have been held together for one second. */
    private void checkExit() {
        int[] s=PrivateTraceActivity.readGamepadForDiagnostics();
        int buttons=(s!=null&&s.length==9)?s[2]:(input!=null?input.physicalState()[0]:0);
        exitTimer.removeCallbacks(exitCheck);
        if(exitChord.update(buttons,SystemClock.uptimeMillis())){finish();return;}
        if(exitChord.pending())exitTimer.postDelayed(exitCheck,50);
    }
    @Override public boolean dispatchKeyEvent(KeyEvent event){return (input!=null&&input.key(event))||super.dispatchKeyEvent(event);}
    @Override public boolean dispatchGenericMotionEvent(MotionEvent event){return (input!=null&&input.motion(event))||super.dispatchGenericMotionEvent(event);}
    @Override public void onWindowFocusChanged(boolean focused){super.onWindowFocusChanged(focused);if(!focused&&input!=null)input.clear();}
    @Override protected void onStop(){
        super.onStop();
        // This screen shares the game's process (the manifest gives both the name ...:private_trace). The launcher and
        // the settings screen treat that process as "the game is running", so it must not outlive this screen:
        // it ends here, the same way the game activity ends its process when it is left.
        if(privateProcess)Process.killProcess(Process.myPid());
    }
    @Override protected void onDestroy(){exitTimer.removeCallbacks(exitCheck);if(input!=null){input.observe(null);input.close();input=null;}super.onDestroy();}
}
