package dev.recompbootstrap.smoke;

import android.view.KeyEvent;

/** Run with java -ea; KeyEvent constants are inlined, no Android runtime needed. */
public final class XboxGamepadMapperTest {
    public static void main(String[] args) {
        boolean assertions=false;assert assertions=true;
        if(!assertions)throw new IllegalStateException("Run with -ea");
        XboxGamepadMapper m=new XboxGamepadMapper();m.configure(true,true);
        int[] codes={KeyEvent.KEYCODE_DPAD_UP,KeyEvent.KEYCODE_DPAD_DOWN,KeyEvent.KEYCODE_DPAD_LEFT,KeyEvent.KEYCODE_DPAD_RIGHT,
                KeyEvent.KEYCODE_BUTTON_START,KeyEvent.KEYCODE_BUTTON_SELECT,KeyEvent.KEYCODE_BUTTON_THUMBL,KeyEvent.KEYCODE_BUTTON_THUMBR,
                KeyEvent.KEYCODE_BUTTON_L1,KeyEvent.KEYCODE_BUTTON_R1,KeyEvent.KEYCODE_BUTTON_MODE,
                KeyEvent.KEYCODE_BUTTON_A,KeyEvent.KEYCODE_BUTTON_B,KeyEvent.KEYCODE_BUTTON_X,KeyEvent.KEYCODE_BUTTON_Y};
        int[] bits={1,2,4,8,16,32,64,128,256,512,1024,4096,8192,16384,32768};
        for(int i=0;i<codes.length;++i) {
            assert m.key(codes[i],true,false)&&m.buttons()==bits[i];
            assert m.key(codes[i],true,false)&&m.buttons()==bits[i]; // repeat is idempotent
            assert m.key(codes[i],false,false)&&m.buttons()==0;
        }
        assert !m.key(KeyEvent.KEYCODE_BACK,true,false); // OS navigation isn't Xbox Back
        assert !m.key(KeyEvent.KEYCODE_HOME,true,false);
        assert !m.key(KeyEvent.KEYCODE_BUTTON_C,true,false);
        m.key(KeyEvent.KEYCODE_BUTTON_A,true,false);m.key(KeyEvent.KEYCODE_BUTTON_B,true,false);
        m.key(KeyEvent.KEYCODE_BUTTON_A,true,true);m.key(KeyEvent.KEYCODE_BUTTON_A,false,true);
        assert m.buttons()==0x3000; // adb release cannot release physical A
        m.key(KeyEvent.KEYCODE_BUTTON_Y,true,true);
        assert m.buttons()==0xB000&&m.physicalButtons()==0x3000;
        m.clear();assert m.buttons()==0;
        m.motion(-1,-1,1,1,-1,-1,.25f,.5f);
        assert m.leftX==-32768&&m.leftY==32767&&m.rightX==32767&&m.rightY==-32768;
        assert m.buttons()==5&&m.leftTrigger()==64&&m.rightTrigger()==128;
        m.key(KeyEvent.KEYCODE_BUTTON_L2,true,false);m.key(KeyEvent.KEYCODE_BUTTON_R2,true,false);
        assert m.leftTrigger()==64&&m.rightTrigger()==128; // analogue wins over duplicate key events
        m.key(KeyEvent.KEYCODE_BUTTON_L2,true,true);
        assert m.leftTrigger()==255&&m.physicalLeftTrigger()==64;
        m.key(KeyEvent.KEYCODE_BUTTON_L2,false,true);assert m.leftTrigger()==64;
        m.key(KeyEvent.KEYCODE_DPAD_UP,true,false);m.motion(0,0,0,0,0,0,0,1);
        assert m.buttons()==1&&m.leftTrigger()==0&&m.rightTrigger()==255;
        m.key(KeyEvent.KEYCODE_DPAD_UP,false,false);assert m.buttons()==0;
        m.configure(false,false);assert m.rightTrigger()==0;
        m.key(KeyEvent.KEYCODE_BUTTON_L2,true,false);m.key(KeyEvent.KEYCODE_BUTTON_R2,true,false);
        assert m.leftTrigger()==255&&m.rightTrigger()==255;
        m.key(KeyEvent.KEYCODE_BUTTON_L2,false,false);assert m.leftTrigger()==0;
        m.clear();assert m.rightTrigger()==0&&m.leftX==0&&m.buttons()==0;
        assert XboxGamepadMapper.centered(.01f,-1,1,.02f)==0;
        assert XboxGamepadMapper.centered(-2,-2,4,0)==-1;
        assert XboxGamepadMapper.centered(2,-2,4,0)==.5f;
        assert XboxGamepadMapper.centered(Float.NaN,-1,1,0)==0;
        assert XboxGamepadMapper.centered(1,0,1,0)==0;
        assert XboxGamepadMapper.centered(1,-1,1,Float.NaN)==0;
        assert XboxGamepadMapper.trigger(-1,-1,1,0)==0;
        assert XboxGamepadMapper.trigger(0,-1,1,0)==.5f;
        assert XboxGamepadMapper.trigger(2,0,1,0)==1;
        assert XboxGamepadMapper.trigger(.01f,0,1,.02f)==0;
        assert XboxGamepadMapper.trigger(Float.POSITIVE_INFINITY,0,1,0)==0;
        assert XboxGamepadMapper.trigger(1,1,1,0)==0;
        m.configure(true,true);m.motion(Float.NaN,Float.NaN,Float.NaN,Float.NaN,0,0,Float.NaN,Float.NaN);
        assert m.leftX==0&&m.leftY==0&&m.rightX==0&&m.rightY==0&&m.leftTrigger()==0&&m.rightTrigger()==0;
        int previous=-1;
        for(int i=0;i<=100;++i) {
            m.motion(0,0,0,0,0,0,i/100f,1-i/100f);
            assert m.leftTrigger()>=previous;previous=m.leftTrigger();
        }
        assert previous==255;
        System.out.println("XBOX_GAMEPAD_MAPPING_PASS: 15 buttons, combinations, both sticks, D-pad, analogue/digital triggers, source isolation, reset, calibration");
    }
}
