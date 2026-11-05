package dev.recompbootstrap.smoke;

import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.provider.MediaStore;
import java.io.File;
import java.io.IOException;
import java.io.OutputStream;
import java.util.HashSet;
import java.util.Set;

/**
 * The phone side of CaptureExport: each capture becomes an image in Pictures/FivePocket Captures through MediaStore
 * (Android 10 / API 29 and later; an app needs no permission to add its own images there). Not on the main thread:
 * copyToPictures reads and writes files.
 */
final class CaptureMediaStore implements CaptureExport.Sink {
    static final String RELATIVE_PATH="Pictures/FivePocket Captures";
    private final ContentResolver resolver;
    private CaptureMediaStore(ContentResolver resolver){this.resolver=resolver;}

    /** Copies every capture not copied before. Below API 29 nothing is copied and the result says why. */
    static CaptureExport.Result copyToPictures(Context context){
        File root=new File(context.getFilesDir(),CaptureStore.FOLDER);
        if(Build.VERSION.SDK_INT<29)return new CaptureExport.Result(0,0,CaptureStore.list(root).size(),"needs Android 10 or later");
        return CaptureExport.copyAll(root,new CaptureMediaStore(context.getContentResolver()));
    }

    /**
     * What the About tab counts: {pictures, bytes, not in Pictures}. On Android 10 and later the shared folder is asked
     * which of this app's pictures it still holds, so one the player removed there is offered again. Not on the main thread.
     */
    static long[] summary(Context context){
        File root=new File(context.getFilesDir(),CaptureStore.FOLDER);
        if(Build.VERSION.SDK_INT<29)return CaptureExport.summary(root);
        return CaptureExport.summary(root,CaptureExport.shared(root,new CaptureMediaStore(context.getContentResolver())));
    }

    /**
     * The names of the images in Pictures/FivePocket Captures that this app may see: without a permission those are the
     * ones it added itself, which are the only ones asked about. null when Android gives no answer.
     */
    @android.annotation.TargetApi(29)
    @Override public Set<String> names() throws IOException {
        Set<String> found=new HashSet<>();
        try(Cursor rows=resolver.query(MediaStore.Images.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY),
                new String[]{MediaStore.MediaColumns.DISPLAY_NAME},
                MediaStore.MediaColumns.RELATIVE_PATH+" LIKE ?",new String[]{RELATIVE_PATH+"%"},null)){
            if(rows==null)return null;
            while(rows.moveToNext()){String name=rows.getString(0);if(name!=null)found.add(name);}
        }
        return found;
    }

    @android.annotation.TargetApi(29)
    @Override public CaptureExport.Target open(String displayName,String mime) throws IOException {
        ContentValues values=new ContentValues();
        values.put(MediaStore.MediaColumns.DISPLAY_NAME,displayName);
        values.put(MediaStore.MediaColumns.MIME_TYPE,mime);
        values.put(MediaStore.MediaColumns.RELATIVE_PATH,RELATIVE_PATH);
        values.put(MediaStore.MediaColumns.IS_PENDING,1);
        Uri uri=resolver.insert(MediaStore.Images.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY),values);
        if(uri==null)return null;
        return new CaptureExport.Target(){
            @Override public OutputStream stream() throws IOException {
                OutputStream out=resolver.openOutputStream(uri,"w");
                if(out==null)throw new IOException("no output stream for "+uri);
                return out;
            }
            @Override public void publish() throws IOException {
                ContentValues done=new ContentValues();
                done.put(MediaStore.MediaColumns.IS_PENDING,0);
                if(resolver.update(uri,done,null,null)!=1)throw new IOException("could not publish "+uri);
            }
            @Override public void abandon(){
                try{resolver.delete(uri,null,null);}catch(RuntimeException ignored){}
            }
        };
    }
}
