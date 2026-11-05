package dev.recompbootstrap.smoke;
/** Host stand-in for the activity's two native calls that feed the game (the real ones are JNI): records what the game is given. */
public final class PrivateTraceActivity {
    static void updateGamepad(boolean connected,int buttons,int leftTrigger,int rightTrigger,int leftX,int leftY,int rightX,int rightY){
        checksim.Sim.game(connected,buttons,leftTrigger,rightTrigger,leftX,leftY,rightX,rightY);
    }
    static void updateGamepadCapabilities(int buttons,int axes,int triggers){}
}
