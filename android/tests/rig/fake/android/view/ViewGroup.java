package android.view;
public class ViewGroup extends View {
    public static class LayoutParams { public LayoutParams(int w,int h){} }
    public final java.util.List<View> children=new java.util.ArrayList<>();
    public void addView(View child,LayoutParams params){children.add(child);}
}
