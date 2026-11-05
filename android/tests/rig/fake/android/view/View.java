package android.view;
public class View {
    public int visibility; public final ViewTreeObserver observer=new ViewTreeObserver();
    public ViewTreeObserver getViewTreeObserver(){return observer;}
    public void setVisibility(int v){visibility=v;}
    public void setBackgroundColor(int c){}
    public void setPadding(int a,int b,int c,int d){}
}
