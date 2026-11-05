package dev.recompbootstrap.smoke;

import android.content.Context;
import android.hardware.input.InputManager;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

/** One actual Android controller mapped to guest slot zero. No user sign-in. */
final class PrivateGamepadInput implements InputManager.InputDeviceListener {
    private static final String TAG="RecompNativeInput";
    private final InputManager manager;
    private final XboxGamepadMapper mapper=new XboxGamepadMapper();
    private final PhysicalMotionLog motionLog=new PhysicalMotionLog(); // stick-only play leaves a line too, at most one in 5 s
    // Android sends no joystick event while no value changes (a stick held against its end stop): the line of a stick
    // that is held still is written by this timer, at the time PhysicalMotionLog names.
    private final Handler motionTimer=new Handler(Looper.getMainLooper());
    private final Runnable heldCheck=this::heldCheck;
    private long heldCheckAt=Long.MIN_VALUE;
    private int deviceId=-1, rightAxisX=-1, rightAxisY=-1, leftAxis=-1, rightAxis=-1;
    private Runnable observer;
    private String description="No controller", lastEvent="No input received";
    PrivateGamepadInput(Context context) {
        manager=(InputManager)context.getSystemService(Context.INPUT_SERVICE);
        if(manager!=null)manager.registerInputDeviceListener(this,motionTimer);
        refresh(true);
    }
    void observe(Runnable listener) {observer=listener;publish();}
    String description() {return description;}
    String lastEvent() {return lastEvent;}
    int[] physicalState() {return new int[]{mapper.physicalButtons(),mapper.physicalLeftTrigger(),mapper.physicalRightTrigger(),
            mapper.leftX,mapper.leftY,mapper.rightX,mapper.rightY};}
    private static boolean supported(InputDevice device) {
        return device!=null && !device.isVirtual() &&
                (device.supportsSource(InputDevice.SOURCE_GAMEPAD) || device.supportsSource(InputDevice.SOURCE_JOYSTICK));
    }
    private static boolean thor(InputDevice device) {
        return supported(device) && device.getVendorId()==0x2020 && device.getProductId()==0x0111;
    }
    private static boolean hasAxis(InputDevice device,int axis) {
        return device.getMotionRange(axis,InputDevice.SOURCE_JOYSTICK)!=null;
    }
    private static int pickAxis(InputDevice device,int primary,int fallback) {
        return hasAxis(device,primary)?primary:(hasAxis(device,fallback)?fallback:-1);
    }
    /**
     * The device Android has under this number. A number below 0 is never a controller and Android is not asked for
     * it: InputDevice.getDevice(-1) answers with Android's own built-in "Virtual" keyboard on every device, which
     * would then stand on the controller check page as the controller's name.
     */
    private static InputDevice controller(int id) {
        return id<0?null:InputDevice.getDevice(id);
    }
    /** A device with a pad's buttons (a DualShock's touchpad and motion sensors are listed beside its pad without them). */
    private static boolean gamepadButtons(InputDevice device) {
        return device!=null && device.supportsSource(InputDevice.SOURCE_GAMEPAD) && device.hasKeys(KeyEvent.KEYCODE_BUTTON_A)[0];
    }
    private static boolean gamepadButton(int code) {
        return code==KeyEvent.KEYCODE_BUTTON_L2 || code==KeyEvent.KEYCODE_BUTTON_R2 || XboxGamepadMapper.button(code)!=0;
    }
    /** Which controller plays: ControllerChoice (the last one given a button press; at first the best one listed). */
    private void refresh(boolean changed) {
        int[] ids=InputDevice.getDeviceIds();
        int[] ranks=new int[ids.length];
        for(int i=0;i<ids.length;++i) {
            InputDevice candidate=controller(ids[i]);
            ranks[i]=ControllerChoice.rank(supported(candidate),thor(candidate),gamepadButtons(candidate));
        }
        int selected=ControllerChoice.pick(deviceId,ids,ranks);
        if(selected==deviceId && !changed)return;
        use(selected);
    }
    private void use(int selected) {
        deviceId=selected;
        InputDevice device=controller(deviceId);
        rightAxisX=rightAxisY=leftAxis=rightAxis=-1;
        int buttons=0,axes=0,triggers=0;
        if(device!=null) {
            // Thor: Z/RZ and BRAKE/GAS. Only use RX/RY if a complete fallback pair exists.
            if(hasAxis(device,MotionEvent.AXIS_Z)&&hasAxis(device,MotionEvent.AXIS_RZ)) {
                rightAxisX=MotionEvent.AXIS_Z;rightAxisY=MotionEvent.AXIS_RZ;
            } else if(hasAxis(device,MotionEvent.AXIS_RX)&&hasAxis(device,MotionEvent.AXIS_RY)) {
                rightAxisX=MotionEvent.AXIS_RX;rightAxisY=MotionEvent.AXIS_RY;
            }
            leftAxis=pickAxis(device,MotionEvent.AXIS_LTRIGGER,MotionEvent.AXIS_BRAKE);
            rightAxis=pickAxis(device,MotionEvent.AXIS_RTRIGGER,MotionEvent.AXIS_GAS);
            int[] codes={KeyEvent.KEYCODE_DPAD_UP,KeyEvent.KEYCODE_DPAD_DOWN,KeyEvent.KEYCODE_DPAD_LEFT,KeyEvent.KEYCODE_DPAD_RIGHT,
                    KeyEvent.KEYCODE_BUTTON_START,KeyEvent.KEYCODE_BUTTON_SELECT,KeyEvent.KEYCODE_BUTTON_THUMBL,KeyEvent.KEYCODE_BUTTON_THUMBR,
                    KeyEvent.KEYCODE_BUTTON_L1,KeyEvent.KEYCODE_BUTTON_R1,KeyEvent.KEYCODE_BUTTON_MODE,
                    KeyEvent.KEYCODE_BUTTON_A,KeyEvent.KEYCODE_BUTTON_B,KeyEvent.KEYCODE_BUTTON_X,KeyEvent.KEYCODE_BUTTON_Y};
            boolean[] present=device.hasKeys(codes);
            for(int i=0;i<codes.length;++i)if(present[i])buttons|=XboxGamepadMapper.button(codes[i]);
            if(hasAxis(device,MotionEvent.AXIS_HAT_X))buttons|=12;
            if(hasAxis(device,MotionEvent.AXIS_HAT_Y))buttons|=3;
            if(hasAxis(device,MotionEvent.AXIS_X))axes|=1;
            if(hasAxis(device,MotionEvent.AXIS_Y))axes|=2;
            if(rightAxisX>=0)axes|=12;
            boolean[] triggerKeys=device.hasKeys(KeyEvent.KEYCODE_BUTTON_L2,KeyEvent.KEYCODE_BUTTON_R2);
            triggers=(triggerKeys[0]||leftAxis>=0?1:0)|(triggerKeys[1]||rightAxis>=0?2:0);
            description=(thor(device)?"AYN Thor / Odin":"Android gamepad")+" -> Xbox 360, player 1\n"+
                    device.getName()+" (id "+deviceId+")\nRight stick: "+MotionEvent.axisToString(rightAxisX)+" / "+
                    MotionEvent.axisToString(rightAxisY)+"; LT/RT: "+MotionEvent.axisToString(leftAxis)+" / "+MotionEvent.axisToString(rightAxis);
        } else description="No controller connected";
        mapper.configure(leftAxis>=0,rightAxis>=0);
        motionLog.released();scheduleHeldCheck(); // the game was just given a controller at rest
        PrivateTraceActivity.updateGamepadCapabilities(buttons,axes,triggers);
        Log.i(TAG,description.replace('\n',' ')+" capabilities="+Integer.toHexString(buttons)+" axes="+axes+" triggers="+triggers);
        publish();
    }
    void clear() {mapper.clear();motionLog.released();scheduleHeldCheck();lastEvent="Input released (focus/device change)";publish();}
    private void publish() {
        PrivateTraceActivity.updateGamepad(deviceId>=0,mapper.buttons(),mapper.leftTrigger(),mapper.rightTrigger(),
                mapper.leftX,mapper.leftY,mapper.rightX,mapper.rightY);
        if(observer!=null)observer.run();
    }
    boolean key(KeyEvent event) {
        // Test events never release a physically held button, and remain labelled synthetic.
        boolean injected=event.getDeviceId()==-1 && event.isFromSource(InputDevice.SOURCE_GAMEPAD);
        // A button press on another connected controller makes that one the player's (a DualShock or an Xbox pad beside a
        // handheld's own pad, a pad that came back under a new number): the game is given it at rest, then this press.
        if(ControllerChoice.adopts(deviceId,event.getDeviceId(),supported(event.getDevice()),gamepadButton(event.getKeyCode()),
                event.getAction()==KeyEvent.ACTION_DOWN,injected))use(event.getDeviceId());
        if(deviceId<0 || (event.getDeviceId()!=deviceId && !injected) ||
                (event.getAction()!=KeyEvent.ACTION_DOWN && event.getAction()!=KeyEvent.ACTION_UP))return false;
        if(!mapper.key(event.getKeyCode(),event.getAction()==KeyEvent.ACTION_DOWN,injected))return false;
        lastEvent=(injected?"SYNTHETIC":"PHYSICAL")+" "+KeyEvent.keyCodeToString(event.getKeyCode())+
                (event.getAction()==KeyEvent.ACTION_DOWN?" down":" up");
        if(event.getRepeatCount()==0)Log.i(TAG,lastEvent);
        publish();return true;
    }
    private static float axis(MotionEvent event,int axis,boolean trigger) {
        if(axis<0)return 0;
        InputDevice device=event.getDevice();
        InputDevice.MotionRange range=device==null?null:device.getMotionRange(axis,InputDevice.SOURCE_JOYSTICK);
        if(range==null)return 0;
        float value=event.getAxisValue(axis);
        return trigger?XboxGamepadMapper.trigger(value,range.getMin(),range.getMax(),range.getFlat()):
                XboxGamepadMapper.centered(value,range.getMin(),range.getMax(),range.getFlat());
    }
    boolean motion(MotionEvent event) {
        if(deviceId<0 || event.getDeviceId()!=deviceId || !event.isFromSource(InputDevice.SOURCE_JOYSTICK) ||
                event.getActionMasked()!=MotionEvent.ACTION_MOVE)return false;
        mapper.motion(axis(event,MotionEvent.AXIS_X,false),axis(event,MotionEvent.AXIS_Y,false),
                axis(event,rightAxisX,false),axis(event,rightAxisY,false),
                axis(event,MotionEvent.AXIS_HAT_X,false),axis(event,MotionEvent.AXIS_HAT_Y,false),
                axis(event,leftAxis,true),axis(event,rightAxis,true));
        lastEvent="PHYSICAL stick / D-pad / trigger motion";
        // The one log line of stick-only play (button presses have their own lines in key()): PhysicalMotionLog decides.
        if(motionLog.moved(event.getDeviceId(),deviceId,mapper.leftX,mapper.leftY,mapper.rightX,mapper.rightY,mapper.physicalHat(),
                mapper.physicalLeftTrigger(),mapper.physicalRightTrigger(),event.getEventTime()))Log.i(TAG,PhysicalMotionLog.LINE);
        scheduleHeldCheck();
        publish();return true;
    }
    /** Asks again at the time the next line of a held stick falls due; nothing is scheduled while everything rests. */
    private void scheduleHeldCheck() {
        long due=motionLog.nextLineAt();
        if(due==heldCheckAt)return;
        motionTimer.removeCallbacks(heldCheck);
        heldCheckAt=due;
        if(due!=Long.MIN_VALUE)motionTimer.postAtTime(heldCheck,due);
    }
    private void heldCheck() {
        heldCheckAt=Long.MIN_VALUE;
        if(motionLog.stillHeld(SystemClock.uptimeMillis()))Log.i(TAG,PhysicalMotionLog.LINE);
        scheduleHeldCheck();
    }
    void close() {if(manager!=null)manager.unregisterInputDeviceListener(this);deviceId=-1;clear();motionTimer.removeCallbacks(heldCheck);}
    @Override public void onInputDeviceAdded(int id){refresh(false);}
    @Override public void onInputDeviceRemoved(int id){refresh(id==deviceId);}
    @Override public void onInputDeviceChanged(int id){refresh(id==deviceId);}
}
