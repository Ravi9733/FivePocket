package android.view;
public class KeyEvent {
    private final long time; private final int action,code,repeat,device,source;
    public KeyEvent(long eventTime,int action,int code,int repeat,int device,int source){
        time=eventTime;this.action=action;this.code=code;this.repeat=repeat;this.device=device;this.source=source;}
    public int getAction(){return action;} public int getKeyCode(){return code;} public int getRepeatCount(){return repeat;}
    public int getDeviceId(){return device;} public long getEventTime(){return time;}
    public boolean isFromSource(int s){return (source&s)==s;}
    public static String keyCodeToString(int code){
        switch(code){
            case 19:return "KEYCODE_DPAD_UP";case 20:return "KEYCODE_DPAD_DOWN";case 21:return "KEYCODE_DPAD_LEFT";case 22:return "KEYCODE_DPAD_RIGHT";
            case 96:return "KEYCODE_BUTTON_A";case 97:return "KEYCODE_BUTTON_B";case 99:return "KEYCODE_BUTTON_X";case 100:return "KEYCODE_BUTTON_Y";
            case 102:return "KEYCODE_BUTTON_L1";case 103:return "KEYCODE_BUTTON_R1";case 104:return "KEYCODE_BUTTON_L2";case 105:return "KEYCODE_BUTTON_R2";
            case 106:return "KEYCODE_BUTTON_THUMBL";case 107:return "KEYCODE_BUTTON_THUMBR";case 108:return "KEYCODE_BUTTON_START";
            case 109:return "KEYCODE_BUTTON_SELECT";case 110:return "KEYCODE_BUTTON_MODE";default:return "KEYCODE_"+code;}
    }
}
