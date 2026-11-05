package dev.recompbootstrap.smoke;

import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.regex.Pattern;

/**
 * Where the game window's captures live and how many are kept. Plain Java (java.io only).
 *
 * Layout: files/captures/<run>/cap-NNN.png, <run> = "run-" + the start request's 32-digit id (the same id as
 * files/private-trace/run-<id>/), or "boot-" + the boot time in ms when a window has no request id. A picture is
 * written as cap-NNN.png.part and renamed when complete, so a reader never sees half a file.
 *
 * Bounds: at most one capture per MIN_INTERVAL_MS (Gate), and the newest KEEP_COUNT pictures within KEEP_BYTES over
 * all runs (prune). prune deletes only files named cap-<digits>.png directly inside a folder named like a run folder
 * directly inside files/captures; it never deletes a folder, a .part file, the export ledger or anything else.
 *
 * A picture that was being written when the game's process ended stays behind as cap-NNN.png.part: it is no picture,
 * prune does not count it, and nobody finishes it. removeUnfinished deletes those when the next game window starts
 * (one game process at a time, so none is being written then).
 */
final class CaptureStore {
    static final String FOLDER="captures";
    static final int KEEP_COUNT=200;
    static final long KEEP_BYTES=800L*1024*1024;
    static final long MIN_INTERVAL_MS=1000;
    static final Pattern RUN=Pattern.compile("run-[0-9a-f]{32}|boot-[0-9]{1,19}");
    static final Pattern PICTURE=Pattern.compile("cap-([0-9]{3,9})\\.png");

    private CaptureStore(){}

    /** The run folder's name for a start request id (32 lower-case hex digits) or, failing that, the boot time. */
    static String runName(String requestId,long bootMs){
        return requestId!=null && requestId.matches("[0-9a-f]{32}")?"run-"+requestId:"boot-"+Math.max(0,bootMs);
    }

    static String pictureName(int n){return String.format(java.util.Locale.ROOT,"cap-%03d.png",n);}

    /** The next number in a run folder: one above the highest cap-NNN.png or .part there, 1 in a new folder. */
    static int nextNumber(File runDir){
        int high=0;
        String[] names=runDir.list();
        if(names!=null)for(String name:names){
            String base=name.endsWith(".part")?name.substring(0,name.length()-5):name;
            java.util.regex.Matcher m=PICTURE.matcher(base);
            if(m.matches())high=Math.max(high,Integer.parseInt(m.group(1)));
        }
        return high+1;
    }

    /**
     * The number of a window's next capture: one above the highest in its run folder and above the last number this
     * window used, so that a capture which left no file does not hand its number to the next one (the log's
     * CAPTURE lines and the names of the pictures then count alike, each number once).
     */
    static int nextNumber(File runDir,int lastUsed){
        return Math.max(nextNumber(runDir),lastUsed+1);
    }

    /**
     * Deletes every unfinished picture (cap-NNN.png.part, a regular file) directly inside a run folder directly
     * inside `root`; returns how many were deleted. Nothing else is touched. Call it only while no capture is being
     * written (at a game window's start).
     */
    static int removeUnfinished(File root){
        int removed=0;
        File[] runs=root.listFiles();
        if(runs==null)return 0;
        for(File run:runs){
            if(!run.isDirectory() || !RUN.matcher(run.getName()).matches())continue;
            File[] files=run.listFiles();
            if(files==null)continue;
            for(File f:files){
                String name=f.getName();
                if(!name.endsWith(".part") || !PICTURE.matcher(name.substring(0,name.length()-5)).matches())continue;
                if(java.nio.file.Files.isRegularFile(f.toPath(),java.nio.file.LinkOption.NOFOLLOW_LINKS) && f.delete())++removed;
            }
        }
        return removed;
    }

    /** One kept picture. */
    static final class Picture {
        final File file; final String run; final long bytes, modified;
        Picture(File file,String run,long bytes,long modified){this.file=file;this.run=run;this.bytes=bytes;this.modified=modified;}
        String relative(){return run+"/"+file.getName();}
    }

    /** Every complete picture under `root` (files/captures), oldest first (modification time, then path). */
    static List<Picture> list(File root){
        List<Picture> found=new ArrayList<>();
        File[] runs=root.listFiles();
        if(runs==null)return found;
        for(File run:runs){
            if(!run.isDirectory() || !RUN.matcher(run.getName()).matches())continue;
            File[] files=run.listFiles();
            if(files==null)continue;
            for(File f:files)if(f.isFile() && PICTURE.matcher(f.getName()).matches())
                found.add(new Picture(f,run.getName(),f.length(),f.lastModified()));
        }
        Collections.sort(found,(a,b)->a.modified!=b.modified?Long.compare(a.modified,b.modified):
                a.relative().compareTo(b.relative()));
        return found;
    }

    /**
     * Keeps the newest `keepCount` pictures whose sizes add up to at most `keepBytes` (the newest one is always kept);
     * deletes the older ones. Returns the pictures it deleted (a picture it could not delete is not in the list).
     */
    static List<Picture> prune(File root,int keepCount,long keepBytes){
        List<Picture> all=list(root);
        List<Picture> deleted=new ArrayList<>();
        int kept=0;long bytes=0;boolean full=false; // once one is dropped, every older one is dropped too
        for(int i=all.size()-1;i>=0;--i){
            Picture p=all.get(i);
            if(!full && (kept==0 || (kept<keepCount && bytes+p.bytes<=keepBytes))){++kept;bytes+=p.bytes;continue;}
            full=true;
            if(p.file.delete())deleted.add(p);
        }
        Collections.reverse(deleted);
        return deleted;
    }

    /** At most one capture per interval; a refused press does not move the interval. */
    static final class Gate {
        private final long interval;private long last;private boolean any;
        Gate(long intervalMs){interval=intervalMs;}
        boolean allow(long now){
            if(any && now-last<interval)return false;
            any=true;last=now;return true;
        }
    }
}
