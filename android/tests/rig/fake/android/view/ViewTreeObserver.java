package android.view;
public final class ViewTreeObserver {
    public interface OnWindowFocusChangeListener { void onWindowFocusChanged(boolean hasFocus); }
    public final java.util.List<OnWindowFocusChangeListener> listeners=new java.util.ArrayList<>();
    public void addOnWindowFocusChangeListener(OnWindowFocusChangeListener l){listeners.add(l);}
    public void removeOnWindowFocusChangeListener(OnWindowFocusChangeListener l){listeners.remove(l);}
    public boolean isAlive(){return true;}
    public void dispatch(boolean focus){for(OnWindowFocusChangeListener l:new java.util.ArrayList<>(listeners))l.onWindowFocusChanged(focus);}
}
