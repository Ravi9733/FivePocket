package android.app;
public class Activity extends android.content.Context {
    public java.io.File filesDir; public final android.content.Intent intent=new android.content.Intent();
    public final android.content.pm.ApplicationInfo info=new android.content.pm.ApplicationInfo();
    public final android.view.Window window=new android.view.Window();
    public java.io.File getFilesDir(){return filesDir;}
    public android.content.Intent getIntent(){return intent;}
    public android.content.pm.ApplicationInfo getApplicationInfo(){return info;}
    public android.view.Window getWindow(){return window;}
}
