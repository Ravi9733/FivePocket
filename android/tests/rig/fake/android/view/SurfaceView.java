package android.view;
public class SurfaceView extends View {
    public Surface surface=new Surface(); public android.graphics.Rect frame=new android.graphics.Rect(1920,1080);
    public SurfaceView(android.content.Context c){}
    public SurfaceHolder getHolder(){return new SurfaceHolder(){
        public Surface getSurface(){return surface;} public android.graphics.Rect getSurfaceFrame(){return frame;}};}
}
