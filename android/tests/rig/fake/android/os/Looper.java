package android.os;
public final class Looper {
    private static Looper main=new Looper("main");
    public final String name; public boolean quit;
    public Looper(String name){this.name=name;}
    public static Looper getMainLooper(){return main;}
    public static void resetForTest(){main=new Looper("main");}
    @Override public String toString(){return name;}
}
