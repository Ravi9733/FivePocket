package android.view;
import java.util.*;
public final class InputDevice {
    public static final class MotionRange {
        final float min,max,flat;
        public MotionRange(float min,float max,float flat){this.min=min;this.max=max;this.flat=flat;}
        public float getMin(){return min;} public float getMax(){return max;} public float getFlat(){return flat;}
    }
    public static final Map<Integer,InputDevice> DEVICES=new LinkedHashMap<>();
    public int id,vendor,product,sources; public String name="pad"; public boolean virtual;
    public final Map<Integer,MotionRange> axes=new HashMap<>();
    public final Set<Integer> keys=new HashSet<>();
    public static InputDevice getDevice(int id){return DEVICES.get(id);}
    public static int[] getDeviceIds(){int[] ids=new int[DEVICES.size()];int i=0;for(int id:DEVICES.keySet())ids[i++]=id;return ids;}
    public boolean isVirtual(){return virtual;}
    public boolean supportsSource(int source){return (sources&source)==source;}
    public int getVendorId(){return vendor;} public int getProductId(){return product;}
    public MotionRange getMotionRange(int axis,int source){return axes.get(axis);}
    public boolean[] hasKeys(int... codes){boolean[] out=new boolean[codes.length];for(int i=0;i<codes.length;++i)out[i]=keys.contains(codes[i]);return out;}
    public String getName(){return name;}
}
