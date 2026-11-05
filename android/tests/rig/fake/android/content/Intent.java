package android.content;
public class Intent {
    public final java.util.Map<String,String> extras=new java.util.HashMap<>();
    public String getStringExtra(String name){return extras.get(name);}
}
