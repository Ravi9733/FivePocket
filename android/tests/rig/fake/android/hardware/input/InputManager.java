package android.hardware.input;
import java.util.*;
public final class InputManager {
    public interface InputDeviceListener { void onInputDeviceAdded(int id); void onInputDeviceRemoved(int id); void onInputDeviceChanged(int id); }
    public final List<InputDeviceListener> listeners=new ArrayList<>();
    public void registerInputDeviceListener(InputDeviceListener l,android.os.Handler h){listeners.add(l);}
    public void unregisterInputDeviceListener(InputDeviceListener l){listeners.remove(l);}
    /** As Android: every listener is told, in the order they registered (all on the main thread here). */
    public void added(int id){for(InputDeviceListener l:new ArrayList<>(listeners))l.onInputDeviceAdded(id);}
    public void removed(int id){for(InputDeviceListener l:new ArrayList<>(listeners))l.onInputDeviceRemoved(id);}
    public void changed(int id){for(InputDeviceListener l:new ArrayList<>(listeners))l.onInputDeviceChanged(id);}
}
