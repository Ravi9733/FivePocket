package android.content;
public class Context {
    public android.hardware.input.InputManager inputManager=new android.hardware.input.InputManager();
    public Object getSystemService(String name){return "input".equals(name)?inputManager:null;}
}
