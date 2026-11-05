package android.view;
public class MotionEvent {
    private final long time; private final int device,source,action; private final java.util.Map<Integer,Float> axes;
    public MotionEvent(long eventTime,int device,int source,int action,java.util.Map<Integer,Float> axes){
        time=eventTime;this.device=device;this.source=source;this.action=action;this.axes=axes;}
    public int getDeviceId(){return device;} public boolean isFromSource(int s){return (source&s)==s;}
    public int getActionMasked(){return action;} public long getEventTime(){return time;}
    public InputDevice getDevice(){return InputDevice.getDevice(device);}
    public float getAxisValue(int axis){Float v=axes.get(axis);return v==null?0:v;}
    public static String axisToString(int axis){return "AXIS_"+axis;}
}
