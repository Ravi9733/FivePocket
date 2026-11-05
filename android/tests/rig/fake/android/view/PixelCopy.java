package android.view;
public final class PixelCopy {
    public interface OnPixelCopyFinishedListener { void onPixelCopyFinished(int result); }
    public static int result=0; public static long copyMs=0; public static int requests; public static boolean neverAnswers;
    /** As Android: refuses an invalid Surface with an exception; the answer arrives on the given handler. */
    public static void request(Surface source,android.graphics.Bitmap dest,OnPixelCopyFinishedListener listener,android.os.Handler handler){
        if(!source.isValid())throw new IllegalArgumentException("Surface isn't valid, source.isValid() == false");
        ++requests;
        if(neverAnswers)return;
        final int r=result;
        handler.postDelayed(()->listener.onPixelCopyFinished(r),copyMs);
    }
    public static void resetForTest(){result=0;copyMs=0;requests=0;neverAnswers=false;}
}
