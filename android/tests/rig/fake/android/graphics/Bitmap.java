package android.graphics;
public final class Bitmap {
    public enum Config { ARGB_8888 } public enum CompressFormat { PNG }
    /** What the fake encoder does: bytes written, how long it takes, and how it fails. */
    public static int encodeBytes=1000; public static long encodeMs=0; public static String fail="";   // "", "false", "throw", "oom-create"
    public static int live;   // bitmaps created and not recycled
    private final int w,h; private boolean recycled;
    private Bitmap(int w,int h){this.w=w;this.h=h;}
    public static Bitmap createBitmap(int w,int h,Config c){if("oom-create".equals(fail))throw new OutOfMemoryError("fake");++live;return new Bitmap(w,h);}
    public int getWidth(){return w;} public int getHeight(){return h;}
    public void recycle(){if(!recycled){recycled=true;--live;}}
    public boolean compress(CompressFormat f,int quality,java.io.OutputStream out){
        if(recycled)throw new IllegalStateException("Can't compress a recycled bitmap");
        if("throw".equals(fail))throw new RuntimeException("fake encoder failure");
        if(encodeMs>0)checksim.Sim.spend(encodeMs);
        if("false".equals(fail))return false;
        try{out.write(new byte[encodeBytes]);}catch(java.io.IOException e){return false;}   // as Android: a failed write gives false
        return true;
    }
    public static void resetForTest(){encodeBytes=1000;encodeMs=0;fail="";live=0;}
}
