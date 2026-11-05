package android.os;
public final class SystemClock {
    public static long uptimeMillis(){return checksim.Sim.now;}
    public static long elapsedRealtime(){return checksim.Sim.now;}
}
