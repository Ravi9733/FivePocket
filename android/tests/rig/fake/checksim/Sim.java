package checksim;
import java.util.*;
/** A virtual clock and message queues for the fake android.os classes: one "thread" per looper, time moves only here. */
public final class Sim {
    private Sim(){}
    public static long now;
    private static long seq;
    public static final List<String> log=new ArrayList<>();      // "<time> <tag>: <message>"
    public static final List<String> crashes=new ArrayList<>();  // an exception that left a message: on Android the process ends
    public static final List<long[]> game=new ArrayList<>();     // {time, connected, buttons, lt, rt, lx, ly, rx, ry}: every updateGamepad call
    private static final class Msg{final Object looper;final Runnable r;final long when,n;Msg(Object l,Runnable r,long w,long n){looper=l;this.r=r;when=w;this.n=n;}}
    private static final List<Msg> queue=new ArrayList<>();
    private static final Set<Object> running=Collections.newSetFromMap(new IdentityHashMap<>());
    private static final Map<Object,Long> busy=new IdentityHashMap<>();
    public static void reset(){now=0;seq=0;log.clear();crashes.clear();game.clear();queue.clear();running.clear();busy.clear();}
    public static void enqueue(Object looper,Runnable r,long when){queue.add(new Msg(looper,r,when,seq++));}
    public static void remove(Object looper,Runnable r){queue.removeIf(m->m.looper==looper && m.r==r);}
    public static void dropAfter(Object looper,long t){queue.removeIf(m->m.looper==looper && m.when>t);}
    public static int pending(Object looper){int n=0;for(Msg m:queue)if(m.looper==looper)++n;return n;}
    /** This looper's thread is busy until t: none of its messages runs before. */
    public static void busyUntil(Object looper,long t){busy.put(looper,t);}
    /** Runs every message due up to t, in time order (then posting time, then posting order); a looper runs one message at a time. */
    public static void runUntil(long t){
        for(;;){
            Msg best=null;long bestAt=0;
            for(Msg m:queue){
                if(running.contains(m.looper))continue;
                long at=Math.max(Math.max(m.when,busy.getOrDefault(m.looper,Long.MIN_VALUE)),now);
                if(at>t)continue;
                if(best==null || at<bestAt || (at==bestAt && (m.when<best.when || (m.when==best.when && m.n<best.n)))){best=m;bestAt=at;}
            }
            if(best==null)break;
            queue.remove(best);
            now=bestAt;
            running.add(best.looper);
            try{best.r.run();}
            catch(Throwable e){crashes.add(now+" "+best.looper+": "+e);}
            finally{running.remove(best.looper);}
        }
        if(t>now)now=t;
    }
    /** The message that runs now takes ms more: the other loopers' messages run meanwhile. */
    public static void spend(long ms){runUntil(now+ms);}
    public static void log(String tag,String message){log.add(now+" "+tag+": "+message);}
    public static void game(boolean connected,int buttons,int lt,int rt,int lx,int ly,int rx,int ry){
        game.add(new long[]{now,connected?1:0,buttons,lt,rt,lx,ly,rx,ry});
    }
}
