package dev.recompbootstrap.smoke;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * Copies the game window's captures (files/captures, see CaptureStore) to a place a file manager shows. Plain Java;
 * the destination is a Sink (CaptureMediaStore on the phone: Pictures/FivePocket Captures through MediaStore).
 *
 * A picture is copied once: files/captures/exported.txt lists the ones already copied (one "run/cap-NNN.png" per
 * line), written after the destination confirmed the copy. Nothing in files/captures is changed or deleted, apart
 * from that list. The copy stops at the first refused or failed picture; a half-written destination is withdrawn
 * (Target.abandon). The result tells a destination that did not accept a picture (Result.refused: no permission,
 * no shared storage) from one that could not be written (most likely the shared storage is full).
 *
 * A picture the player has since removed from the shared folder is not "in Pictures" any more, and the app's own
 * list cannot know that. So the destination is asked which names it holds now (Sink.names): a listed picture whose
 * copy is gone counts as not copied and is copied again by the next request. A destination that cannot say (null)
 * leaves the list as the only knowledge. And a destination that does not show a picture right after it accepted
 * it is not asked again on this device (the file NO_LISTING in files/captures says so): otherwise every request
 * would copy every picture once more.
 */
final class CaptureExport {
    static final String LEDGER="exported.txt";
    static final String MIME="image/png";

    interface Target {
        OutputStream stream() throws IOException;
        /** The copy is complete: make it visible. */
        void publish() throws IOException;
        /** The copy failed: remove what was written. */
        void abandon();
    }
    interface Sink {
        Target open(String displayName,String mime) throws IOException;
        /** The names of the pictures the destination holds now; null when it cannot say. */
        default Set<String> names() throws IOException {return null;}
    }
    /** Exists once the destination failed to show a picture it had just accepted: its listing is not used again. */
    static final String NO_LISTING=".shared-listing-not-used";

    static final class Result {
        final int copied, already, total; final String error;
        /** With an error: the destination did not accept the picture (as opposed to: it could not be written). */
        final boolean refused;
        Result(int copied,int already,int total,String error){this(copied,already,total,error,false);}
        Result(int copied,int already,int total,String error,boolean refused){
            this.copied=copied;this.already=already;this.total=total;this.error=error;this.refused=refused;}
        /** A sentence for the About tab. */
        String sentence(){
            if(total==0)return "No captures yet.";
            String done=copied+" copied to Pictures/FivePocket Captures"+(already>0?", "+already+" were already there":"")+".";
            return error==null?done:done+" Stopped: "+error;
        }
    }

    /** The count and size the About tab shows, by the app's own list alone. {pictures, bytes, not copied}. */
    static long[] summary(File root){return summary(root,(Set<String>)null);}

    /**
     * {pictures, bytes, not in the shared folder}. shared: the names the shared folder holds now (shared()), or null
     * when only the app's own list is known. A picture counts as copied when the list has it and, where the shared
     * folder could be asked, its copy is still there.
     */
    static long[] summary(File root,Set<String> shared){
        List<CaptureStore.Picture> all=CaptureStore.list(root);
        Set<String> done=ledger(root);
        long bytes=0,fresh=0;
        for(CaptureStore.Picture p:all){bytes+=p.bytes;if(!inShared(p,done,shared))++fresh;}
        return new long[]{all.size(),bytes,fresh};
    }

    private static boolean inShared(CaptureStore.Picture p,Set<String> done,Set<String> shared){
        return done.contains(p.relative()) && (shared==null || shared.contains(displayName(p)));
    }

    /** What the destination holds now, as far as it is to be believed; null: only the app's own list is known. */
    static Set<String> shared(File root,Sink sink){
        if(new File(root,NO_LISTING).exists())return null;
        try{return sink.names();}
        catch(IOException|RuntimeException unknown){return null;}
    }

    /** The name a copy gets in the shared folder: unique per run and number. */
    static String displayName(CaptureStore.Picture p){return "gta5-"+p.run+"-"+p.file.getName();}

    static Result copyAll(File root,Sink sink){
        List<CaptureStore.Picture> all=CaptureStore.list(root);
        Set<String> done=ledger(root);
        Set<String> shared=shared(root,sink);
        List<String> published=new java.util.ArrayList<>();
        Result result=copy(root,sink,all,done,shared,published);
        // The destination must show what it has just accepted. One that does not is not asked again: its answer
        // would make every picture look removed, and every request would copy all of them once more.
        if(shared!=null && !published.isEmpty()){
            Set<String> now=shared(root,sink);
            if(now==null || !now.containsAll(published)){
                try{new File(root,NO_LISTING).createNewFile();}catch(IOException|RuntimeException ignored){}
            }
        }
        return result;
    }

    private static Result copy(File root,Sink sink,List<CaptureStore.Picture> all,Set<String> done,Set<String> shared,List<String> published){
        int copied=0,already=0;
        byte[] buffer=new byte[65536];
        for(CaptureStore.Picture p:all){
            if(inShared(p,done,shared)){++already;continue;}
            Target target=null;
            boolean accepted=false; // the destination has taken the picture's name: what fails from here on is the writing
            try{
                target=sink.open(displayName(p),MIME);
                if(target==null)return new Result(copied,already,all.size(),"the shared Pictures folder refused "+p.relative(),true);
                accepted=true;
                long written=0;
                try(InputStream in=new FileInputStream(p.file);OutputStream out=target.stream()){
                    for(int n;(n=in.read(buffer))!=-1;){out.write(buffer,0,n);written+=n;}
                }
                if(written!=p.bytes)throw new IOException(p.relative()+" changed while it was copied");
                target.publish();
                target=null; // published: a later failure (the list) must not withdraw it
                published.add(displayName(p));
                if(!done.contains(p.relative()))appendLedger(root,p.relative());
                ++copied;
            }catch(IOException|RuntimeException e){
                if(target!=null)target.abandon();
                // Refused: the destination would not take the picture at all (Android answers that with a
                // SecurityException, an IllegalArgumentException or an IllegalStateException, not with an IOException).
                return new Result(copied,already,all.size(),p.relative()+": "+e.getMessage(),!accepted && e instanceof RuntimeException);
            }
        }
        return new Result(copied,already,all.size(),null);
    }

    static Set<String> ledger(File root){
        Set<String> done=new HashSet<>();
        File f=new File(root,LEDGER);
        if(!f.isFile())return done;
        try{for(String line:Files.readAllLines(f.toPath(),StandardCharsets.UTF_8))if(!line.isEmpty())done.add(line.trim());}
        catch(IOException ignored){}
        return done;
    }

    private static void appendLedger(File root,String relative) throws IOException {
        try(FileOutputStream out=new FileOutputStream(new File(root,LEDGER),true)){
            out.write((relative+"\n").getBytes(StandardCharsets.UTF_8));
        }
    }
}
