package dev.recompbootstrap.smoke;

import android.app.Activity;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.Rect;
import android.hardware.input.InputManager;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.PixelCopy;
import android.view.Surface;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewTreeObserver;
import android.widget.FrameLayout;
import android.widget.TextView;
import java.io.File;
import java.io.FileOutputStream;
import java.util.List;
import java.util.function.Predicate;

/**
 * The game window's capture button: Select held, then A (CaptureCombo decides; this class is the Android side).
 *
 * The main thread only runs the combo and hands a capture to the thread "capture-io", which does, in this order:
 * logs CAPTURE_START; copies the game's Surface as it is on screen with PixelCopy into a bitmap of the Surface's
 * size; asks the renderer for a census (CaptureCensus; armed right after the copy rather than after the save, so the
 * census frame is as close to the moment as it can be); writes cap-NNN.png (.part, then renamed); shows "Captured n"
 * in the top right corner for half a second once the file is written (a View above the game, so it is never in a
 * picture: PixelCopy of a Surface reads only the game's buffer); keeps the newest pictures (CaptureStore.prune) and
 * says so on screen when old ones were removed; logs the CAPTURE line. Log tag RecompNativeInput (the harness logger
 * keeps it).
 *
 * Every combo is answered on the screen (the words are CaptureBadge's): the number of the picture that was written,
 * or one line that says why there is none (too soon after the last, the last not finished, no picture of the game
 * yet, no memory, storage). n is the number in the file's name, in the log and on the screen alike; a capture that
 * left no file does not hand its number to the next one.
 *
 * Nothing that goes wrong while a picture is copied, encoded, written or pruned may leave capture-io as an exception:
 * on Android that would end the game's process. Such a capture ends as CAPTURE_ABORTED with a line on the screen.
 */
final class CaptureController implements CaptureCombo.Output<KeyEvent>, InputManager.InputDeviceListener {
    static final String TAG="RecompNativeInput";
    static final String PHYSICAL_CAPTURE="PHYSICAL capture combo (Select held, then A)";
    private final Activity activity;
    private final SurfaceView surface;
    private final Predicate<KeyEvent> game;
    private final CaptureCombo<KeyEvent> combo=new CaptureCombo<>(this);
    private final CaptureStore.Gate gate=new CaptureStore.Gate(CaptureStore.MIN_INTERVAL_MS);
    private final Handler main=new Handler(Looper.getMainLooper());
    private final HandlerThread thread=new HandlerThread("capture-io");
    private final Handler io;
    private final TextView badge;
    private final InputManager inputs;
    private final File root;
    private final String run;
    private final CaptureCensus census;
    private final Runnable tick=this::tick, hide;
    private final ViewTreeObserver.OnWindowFocusChangeListener focus;
    private boolean busy; // main thread: a capture is still being copied / written
    private volatile boolean closed; // the window is gone: nothing more is shown
    private int lastNumber; // capture-io: the number of this window's last capture (0: none yet)

    /**
     * Called once by PrivateTraceActivity where the game's SurfaceView is made. `content` is the window's layout
     * (the badge goes on top); `game` gives an event to the game (PrivateGamepadInput.key).
     */
    static CaptureController attach(Activity activity,SurfaceView surface,FrameLayout content,Predicate<KeyEvent> game){
        return new CaptureController(activity,surface,content,game);
    }

    private CaptureController(Activity activity,SurfaceView surface,FrameLayout content,Predicate<KeyEvent> game){
        this.activity=activity;this.surface=surface;this.game=game;
        thread.start();io=new Handler(thread.getLooper());
        root=new File(activity.getFilesDir(),CaptureStore.FOLDER);
        run=CaptureStore.runName(activity.getIntent().getStringExtra("request_id"),
                System.currentTimeMillis()-SystemClock.elapsedRealtime());
        boolean censusOn=(activity.getApplicationInfo().flags&ApplicationInfo.FLAG_DEBUGGABLE)!=0; // ship build: off
        census=new CaptureCensus(activity.getFilesDir(),censusOn);
        io.post(()->{census.cleanUpEarlierRun();
            // What a game process that ended while it wrote a picture left behind (no capture is being written now).
            int unfinished=CaptureStore.removeUnfinished(root);
            if(unfinished>0)Log.i(TAG,"CAPTURE_LEFT_OVERS removed="+unfinished);
            Log.i(TAG,"CAPTURE_READY combo=select+a window_ms="+CaptureCombo.WINDOW_MS+" dir="+CaptureStore.FOLDER+"/"+run+
                    " keep="+CaptureStore.KEEP_COUNT+" census="+(censusOn?"on":"off"));});
        badge=new TextView(activity);
        badge.setTextSize(14);badge.setTextColor(Color.WHITE);badge.setBackgroundColor(0x99000000);badge.setPadding(14,6,14,6);
        badge.setVisibility(View.GONE);
        content.addView(badge,new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP|Gravity.RIGHT));
        hide=()->badge.setVisibility(View.GONE);
        inputs=(InputManager)activity.getSystemService(Context.INPUT_SERVICE);
        if(inputs!=null)inputs.registerInputDeviceListener(this,main);
        focus=focused->{if(!focused)reset();};
        activity.getWindow().getDecorView().getViewTreeObserver().addOnWindowFocusChangeListener(focus);
    }

    /** From dispatchKeyEvent, before the game's input: true when the combo took the event. */
    boolean key(KeyEvent event){
        int code=event.getKeyCode(),action=event.getAction();
        if((code!=CaptureCombo.SELECT && code!=CaptureCombo.A) ||
                (action!=KeyEvent.ACTION_DOWN && action!=KeyEvent.ACTION_UP) ||
                !(event.isFromSource(InputDevice.SOURCE_GAMEPAD) || event.isFromSource(InputDevice.SOURCE_JOYSTICK)))return false;
        // The event's own time and the time of this moment: they differ when the main thread was busy (CaptureCombo).
        boolean taken=combo.key(event,code,action==KeyEvent.ACTION_DOWN,event.getRepeatCount(),event.getEventTime(),
                SystemClock.uptimeMillis(),event.getDeviceId());
        schedule();
        return taken;
    }

    private void schedule(){
        main.removeCallbacks(tick);
        long due=combo.nextDeadline();
        if(due!=Long.MIN_VALUE)main.postAtTime(tick,due);
    }
    private void tick(){combo.advance(SystemClock.uptimeMillis());schedule();}
    private void reset(){combo.reset();schedule();}

    /** A held-back or delayed Select (only Select is ever given late): to the game, and one log line on capture-io. */
    @Override public void forward(KeyEvent event){
        boolean taken=game.test(event);
        long late=SystemClock.uptimeMillis()-event.getEventTime();
        String what=event.getAction()==KeyEvent.ACTION_DOWN?"down":"up";
        io.post(()->Log.i(TAG,"CAPTURE_SELECT_LATE "+what+" by_ms="+late+" taken="+(taken?1:0)));
    }

    @Override public void capture(long at){
        // Neither press of a capture reaches the game, so neither leaves its "PHYSICAL KEYCODE" line. A real
        // controller's capture leaves this line instead: the lead's stop guard reads "PHYSICAL" lines to know that
        // somebody is playing, and a player who only takes pictures must count. Injected test input (device -1) does not.
        if(combo.device()>=0)io.post(()->Log.i(TAG,PHYSICAL_CAPTURE));
        long wall=System.currentTimeMillis()-(SystemClock.uptimeMillis()-at);
        if(busy || !gate.allow(at)){
            boolean wasBusy=busy;
            io.post(()->Log.i(TAG,"CAPTURE_SKIPPED reason="+(wasBusy?"busy":"rate")+" at="+at));
            badge(CaptureBadge.refused(wasBusy),CaptureBadge.LONG_MS); // the player pressed the combo: say why nothing is taken
            return;
        }
        busy=true;
        io.post(()->{
            try{run(at,wall);}
            catch(RuntimeException|Error e){aborted(at,e,null,null);}
        });
    }

    /** capture-io: a capture that ended in an exception. Its half file and its bitmap go; the next capture is possible. */
    private void aborted(long at,Throwable error,Bitmap bitmap,File part){
        try{
            if(bitmap!=null)bitmap.recycle();
            if(part!=null)part.delete();
        }catch(RuntimeException ignored){
            // nothing more can be done for this capture
        }
        Log.w(TAG,"CAPTURE_ABORTED at="+at+" error="+error.getClass().getSimpleName());
        badge(error instanceof OutOfMemoryError?CaptureBadge.NO_MEMORY:CaptureBadge.FAILED,CaptureBadge.LONG_MS);
        main.post(()->busy=false);
    }

    /** capture-io. */
    private void run(long at,long wall){
        File dir=new File(root,run);
        if(!dir.isDirectory() && !dir.mkdirs()){
            Log.w(TAG,"CAPTURE_ABORTED at="+at+" error=no-folder");
            badge(CaptureBadge.NO_STORAGE,CaptureBadge.LONG_MS);
            main.post(()->busy=false);return;
        }
        int n=lastNumber=CaptureStore.nextNumber(dir,lastNumber);
        String name=CaptureStore.pictureName(n);
        Log.i(TAG,"CAPTURE_START n="+n+" file="+name+" dir="+CaptureStore.FOLDER+"/"+run+" at="+at+" wall="+wall);
        Surface target=surface.getHolder().getSurface();
        Rect frame=surface.getHolder().getSurfaceFrame();
        if(target==null || !target.isValid() || frame.width()<1 || frame.height()<1){
            badge(CaptureBadge.NO_PICTURE_YET,CaptureBadge.LONG_MS);
            finish(n,name,at,wall,"no-surface",0,null);return;
        }
        Bitmap bitmap;
        try{bitmap=Bitmap.createBitmap(frame.width(),frame.height(),Bitmap.Config.ARGB_8888);}
        catch(OutOfMemoryError e){
            badge(CaptureBadge.NO_MEMORY,CaptureBadge.LONG_MS);
            finish(n,name,at,wall,"no-memory",0,null);return;
        }
        long asked=SystemClock.uptimeMillis();
        File part=new File(dir,name+".part");
        try{
            // PixelCopy's answer arrives on capture-io as a message of its own, outside the try/catch of capture():
            // it has its own, or an exception while the picture is encoded or written would end the game's process.
            PixelCopy.request(target,bitmap,result->{
                try{copied(n,name,at,wall,asked,result,bitmap);}
                catch(RuntimeException|Error e){aborted(at,e,bitmap,part);}
            },io);
        }catch(RuntimeException|Error e){aborted(at,e,bitmap,null);} // (the Surface went away between the check and the copy)
    }

    /** capture-io, PixelCopy's answer. */
    private void copied(int n,String name,long at,long wall,long asked,int result,Bitmap bitmap){
        long copyMs=SystemClock.uptimeMillis()-asked;
        if(result!=PixelCopy.SUCCESS){
            bitmap.recycle();
            badge(CaptureBadge.noPicture(n),CaptureBadge.LONG_MS);
            finish(n,name,at,wall,"copy-"+copyResult(result),copyMs,census.arm(SystemClock.uptimeMillis()));
            return;
        }
        String censusWord=census.arm(SystemClock.uptimeMillis());
        if("armed".equals(censusWord))io.postDelayed(census::disarm,CaptureCensus.HOLD_MS);
        File dir=new File(root,run),part=new File(dir,name+".part"),done=new File(dir,name);
        String size=bitmap.getWidth()+"x"+bitmap.getHeight();
        long started=SystemClock.uptimeMillis();
        String state="saved";
        try(FileOutputStream out=new FileOutputStream(part)){
            if(!bitmap.compress(Bitmap.CompressFormat.PNG,100,out))state="encode-refused";
        }catch(java.io.IOException e){state="write-"+e.getClass().getSimpleName();}
        bitmap.recycle();
        if("saved".equals(state) && !part.renameTo(done))state="rename-refused";
        if(!"saved".equals(state))part.delete();
        long saveMs=SystemClock.uptimeMillis()-started;
        // The confirmation says "Captured" only now, when the file is written; a full storage never reads as a capture.
        boolean saved="saved".equals(state);
        badge(saved?CaptureBadge.saved(n):CaptureBadge.notSaved(n),saved?CaptureBadge.SHORT_MS:CaptureBadge.LONG_MS,0);
        List<CaptureStore.Picture> dropped=CaptureStore.prune(root,CaptureStore.KEEP_COUNT,CaptureStore.KEEP_BYTES);
        // (after the line above has had its time, so that both are read)
        if(!dropped.isEmpty())badge(CaptureBadge.removedOldest(dropped.size()),CaptureBadge.LONG_MS,
                saved?CaptureBadge.SHORT_MS:CaptureBadge.LONG_MS);
        Log.i(TAG,"CAPTURE n="+n+" file="+("saved".equals(state)?name:"none")+" at="+at+" now="+SystemClock.uptimeMillis()+
                " dir="+CaptureStore.FOLDER+"/"+run+" wall="+wall+" size="+size+
                " bytes="+("saved".equals(state)?done.length():0)+" copy_ms="+copyMs+" save_ms="+saveMs+" state="+state+
                " census="+censusWord+" dropped="+dropped.size());
        main.post(()->busy=false);
    }

    /** capture-io: a capture without a picture still leaves its CAPTURE line. */
    private void finish(int n,String name,long at,long wall,String state,long copyMs,String censusWord){
        if(censusWord==null)censusWord="not-asked";
        else if("armed".equals(censusWord))io.postDelayed(census::disarm,CaptureCensus.HOLD_MS);
        Log.i(TAG,"CAPTURE n="+n+" file=none at="+at+" now="+SystemClock.uptimeMillis()+" dir="+CaptureStore.FOLDER+"/"+run+
                " wall="+wall+" bytes=0 copy_ms="+copyMs+" state="+state+" census="+censusWord+" dropped=0");
        main.post(()->busy=false);
    }

    private static String copyResult(int code){
        switch(code){
            case PixelCopy.ERROR_SOURCE_NO_DATA:return "source-no-data";
            case PixelCopy.ERROR_SOURCE_INVALID:return "source-invalid";
            case PixelCopy.ERROR_DESTINATION_INVALID:return "destination-invalid";
            case PixelCopy.ERROR_TIMEOUT:return "timeout";
            default:return "error"+code;
        }
    }

    private void badge(String text,long ms){badge(text,ms,0);}

    /** Shows `text` for `ms`, starting `afterMs` from now; a line shown later replaces the one that is there. From any thread. */
    private void badge(String text,long ms,long afterMs){
        main.postDelayed(()->{
            if(closed)return;
            badge.setText(text);badge.setVisibility(View.VISIBLE);main.removeCallbacks(hide);main.postDelayed(hide,ms);
        },afterMs);
    }

    @Override public void onInputDeviceAdded(int id){}
    @Override public void onInputDeviceChanged(int id){}
    /** That controller's held-back Select or waiting release is settled; another controller's presses are not touched. */
    @Override public void onInputDeviceRemoved(int id){combo.reset(id);schedule();}

    /** From onDestroy. A capture being written finishes on capture-io; a census trigger is removed at once. */
    void close(){
        closed=true;
        main.removeCallbacks(tick);main.removeCallbacks(hide);
        if(inputs!=null)inputs.unregisterInputDeviceListener(this);
        ViewTreeObserver observer=activity.getWindow().getDecorView().getViewTreeObserver();
        if(observer.isAlive())observer.removeOnWindowFocusChangeListener(focus);
        io.post(census::disarm);
        thread.quitSafely();
    }
}
