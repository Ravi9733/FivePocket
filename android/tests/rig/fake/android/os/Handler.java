package android.os;
import checksim.Sim;
public class Handler {
    private final Looper looper;
    public Handler(Looper looper){this.looper=looper;}
    public final boolean post(Runnable r){return postAtTime(r,Sim.now);}
    public final boolean postDelayed(Runnable r,long delay){return postAtTime(r,Sim.now+delay);}
    public boolean postAtTime(Runnable r,long when){if(looper.quit)return false;Sim.enqueue(looper,r,when);return true;}
    public final void removeCallbacks(Runnable r){Sim.remove(looper,r);}
}
