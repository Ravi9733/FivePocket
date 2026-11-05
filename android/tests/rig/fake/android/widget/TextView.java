package android.widget;
public class TextView extends android.view.View {
    public static TextView last; public CharSequence text;
    public final java.util.List<String> shown=new java.util.ArrayList<>(); // "<time> <text>" at every time it became visible with a text
    public TextView(android.content.Context c){last=this;}
    public void setTextSize(float s){} public void setTextColor(int c){}
    public void setText(CharSequence t){text=t;}
    @Override public void setVisibility(int v){super.setVisibility(v);shown.add(checksim.Sim.now+(v==0?" SHOW ":" HIDE ")+(v==0?text:""));}
}
