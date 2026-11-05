package android.os;
import checksim.Sim;
public class HandlerThread {
    private final String name; private Looper looper;
    public static HandlerThread last;
    public HandlerThread(String name){this.name=name;last=this;}
    public void start(){looper=new Looper(name);}
    public Looper getLooper(){return looper;}
    /** As Android: messages already due are still delivered, later ones are dropped, new ones refused. */
    public boolean quitSafely(){Sim.dropAfter(looper,Sim.now);looper.quit=true;return true;}
}
