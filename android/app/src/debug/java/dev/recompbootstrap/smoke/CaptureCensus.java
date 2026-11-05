package dev.recompbootstrap.smoke;

import java.io.File;
import java.io.IOException;

/**
 * Asks the renderer for its one-frame census on a capture. Plain Java (java.io only).
 *
 * What the game library (v464, generic_renderer.cpp) does, read from its code: every 30 presents it checks whether
 * files/private-trace/diag-frame-census exists (lines 8672 and 8813-8815); when it exists and did not exist at the
 * previous check, the census is armed, and the frame that starts after the next present is recorded draw by draw
 * (line 9514, censusBegin 3146-3160): RENDER_CENSUS lines under the tag RecompRenderer and 60 KB chunk files
 * files/private-trace/census/census-NN-MMM.txt (3091-3095), ending with "RENDER_CENSUS END n=NN" (3199). The file's
 * content is not read. So the census lands one to two check intervals after the press, not on the pictured frame.
 *
 * This class creates the file only when it does not exist (an existing file is someone else's and is left alone), keeps
 * it HOLD_MS so that at least one check sees it (30 presents: one second at 30 frames per second; HOLD_MS covers
 * down to 7.5 presents per second), then deletes it so the next census can arm again. A marker
 * files/captures/.census-owned says the file is ours, so a process that was ended while holding it removes it at the
 * next start (a standing file would take a census at start-up). At most one census per COOLDOWN_MS.
 */
final class CaptureCensus {
    static final long HOLD_MS=4000, COOLDOWN_MS=60000;
    static final String TRIGGER="private-trace/diag-frame-census", MARKER=CaptureStore.FOLDER+"/.census-owned";

    private final File trigger, marker, privateTrace;
    private final boolean enabled;
    private long lastArm; private boolean armedOnce, holding;

    /** `enabled`: the app is debuggable (only then can a census be pulled; in the ship build it would only cost time). */
    CaptureCensus(File filesDir,boolean enabled){
        trigger=new File(filesDir,TRIGGER);marker=new File(filesDir,MARKER);privateTrace=new File(filesDir,"private-trace");
        this.enabled=enabled;
    }

    /** At the window's start: removes a trigger file this class left behind in an earlier process. */
    void cleanUpEarlierRun(){
        if(marker.exists()){trigger.delete();marker.delete();}
    }

    /** One capture asks for a census. Returns the word logged as census=: armed, off, cooldown, busy, present, no-folder, error. */
    String arm(long now){
        if(!enabled)return "off";
        if(holding)return "busy";
        if(armedOnce && now-lastArm<COOLDOWN_MS)return "cooldown";
        if(!privateTrace.isDirectory())return "no-folder";
        if(trigger.exists())return "present";
        try{
            File parent=marker.getParentFile();
            if(parent!=null && !parent.isDirectory() && !parent.mkdirs())return "error";
            if(!marker.exists() && !marker.createNewFile())return "error";
            if(!trigger.createNewFile()){marker.delete();return "present";}
        }catch(IOException|SecurityException e){marker.delete();return "error";}
        holding=true;armedOnce=true;lastArm=now;
        return "armed";
    }

    /** HOLD_MS after a successful arm: removes the trigger file (only one this class made) and the marker. */
    void disarm(){
        if(!holding)return;
        holding=false;
        if(marker.exists()){trigger.delete();marker.delete();}
    }
}
