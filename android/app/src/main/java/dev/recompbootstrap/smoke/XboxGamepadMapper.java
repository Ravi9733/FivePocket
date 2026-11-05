package dev.recompbootstrap.smoke;

import android.view.KeyEvent;

/** Android events -> Xbox 360 state. No game actions, profiles, or OS key remaps. */
final class XboxGamepadMapper {
    private int physicalKeys, syntheticKeys, hat;
    private boolean physicalLeft, physicalRight, syntheticLeft, syntheticRight;
    private boolean analogLeft, analogRight;
    int leftX, leftY, rightX, rightY;
    private int leftValue, rightValue;

    void configure(boolean leftAnalog, boolean rightAnalog) {
        clear(); analogLeft=leftAnalog; analogRight=rightAnalog;
    }
    void clear() {
        physicalKeys=syntheticKeys=hat=leftX=leftY=rightX=rightY=leftValue=rightValue=0;
        physicalLeft=physicalRight=syntheticLeft=syntheticRight=false;
    }
    int buttons() { return physicalKeys|syntheticKeys|hat; }
    int physicalButtons() {return physicalKeys|hat;}
    /** The D-pad as the hat axes give it (four bits), without the D-pad's key events. */
    int physicalHat() {return hat;}
    int physicalLeftTrigger() {return analogLeft?leftValue:(physicalLeft?255:0);}
    int physicalRightTrigger() {return analogRight?rightValue:(physicalRight?255:0);}
    int leftTrigger() { return syntheticLeft?255:physicalLeftTrigger(); }
    int rightTrigger() { return syntheticRight?255:physicalRightTrigger(); }

    static int button(int key) {
        switch(key) {
            case KeyEvent.KEYCODE_DPAD_UP:return 1;
            case KeyEvent.KEYCODE_DPAD_DOWN:return 2;
            case KeyEvent.KEYCODE_DPAD_LEFT:return 4;
            case KeyEvent.KEYCODE_DPAD_RIGHT:return 8;
            case KeyEvent.KEYCODE_BUTTON_START:return 0x10;
            case KeyEvent.KEYCODE_BUTTON_SELECT:return 0x20;
            case KeyEvent.KEYCODE_BUTTON_THUMBL:return 0x40;
            case KeyEvent.KEYCODE_BUTTON_THUMBR:return 0x80;
            case KeyEvent.KEYCODE_BUTTON_L1:return 0x100;
            case KeyEvent.KEYCODE_BUTTON_R1:return 0x200;
            case KeyEvent.KEYCODE_BUTTON_MODE:return 0x400;
            case KeyEvent.KEYCODE_BUTTON_A:return 0x1000;
            case KeyEvent.KEYCODE_BUTTON_B:return 0x2000;
            case KeyEvent.KEYCODE_BUTTON_X:return 0x4000;
            case KeyEvent.KEYCODE_BUTTON_Y:return 0x8000;
            default:return 0;
        }
    }
    boolean key(int code, boolean down, boolean synthetic) {
        if(code==KeyEvent.KEYCODE_BUTTON_L2) {
            if(synthetic)syntheticLeft=down;else physicalLeft=down;
        } else if(code==KeyEvent.KEYCODE_BUTTON_R2) {
            if(synthetic)syntheticRight=down;else physicalRight=down;
        } else {
            int mask=button(code);
            if(mask==0)return false;
            if(synthetic)syntheticKeys=down?syntheticKeys|mask:syntheticKeys&~mask;
            else physicalKeys=down?physicalKeys|mask:physicalKeys&~mask;
        }
        return true;
    }
    // Use the driver's calibration/deadzone; GTA V retains its own sensitivity.
    static float centered(float value,float min,float max,float flat) {
        if(!Float.isFinite(value)||!Float.isFinite(min)||!Float.isFinite(max)||
                !Float.isFinite(flat)||min>=0||max<=0||flat<0)return 0;
        if(Math.abs(value)<=flat)return 0;
        return Math.max(-1,Math.min(1,value/(value<0?-min:max)));
    }
    static float trigger(float value,float min,float max,float flat) {
        if(!Float.isFinite(value)||!Float.isFinite(min)||!Float.isFinite(max)||
                !Float.isFinite(flat)||max<=min||flat<0)return 0;
        if(value-min<=flat)return 0;
        return Math.max(0,Math.min(1,(value-min)/(max-min)));
    }
    static int stick(float value) {
        if(!Float.isFinite(value))return 0;
        value=Math.max(-1,Math.min(1,value));
        return Math.round(value*(value<0?32768:32767));
    }
    void motion(float lx,float ly,float rx,float ry,float hx,float hy,float lt,float rt) {
        leftX=stick(lx);leftY=stick(-ly);rightX=stick(rx);rightY=stick(-ry);
        hat=(hx<-.5f?4:hx>.5f?8:0)|(hy<-.5f?1:hy>.5f?2:0);
        leftValue=Math.round(255*(Float.isFinite(lt)?Math.max(0,Math.min(1,lt)):0));
        rightValue=Math.round(255*(Float.isFinite(rt)?Math.max(0,Math.min(1,rt)):0));
    }
}
