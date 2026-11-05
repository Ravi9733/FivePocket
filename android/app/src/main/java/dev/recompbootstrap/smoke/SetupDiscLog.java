package dev.recompbootstrap.smoke;

import java.util.List;

/**
 * The lines the two-disc setup writes to the log under one tag, RecompSetup. Pure Java, so that the exact text is
 * host tested; the proof tool prints the same FILE lines. The ship build is not debuggable: these lines are how each
 * state of the page is read from outside (DEVICE-STEPS.md). They name disc paths, sizes and SHA-256 values only:
 * never content, never a document address.
 */
final class SetupDiscLog {
    static final String TAG = "RecompSetup";
    private static final String[] DISC = {"play", "install"};

    private SetupDiscLog() {}

    static String disc(int disc) {
        return disc >= 0 && disc < DISC.length ? DISC[disc] : "none";
    }

    /** "PAGE shown play=a/b install=c/d free=(bytes) ready=yes|no code=ok|problem remembered_play=yes|no remembered_install=yes|no" */
    static String page(SetupDiscModel.Disk disk) {
        return "PAGE shown play=" + disk.have(SetupDiscModel.PLAY) + "/" + disk.total(SetupDiscModel.PLAY) + " install="
                + disk.have(SetupDiscModel.INSTALL) + "/" + disk.total(SetupDiscModel.INSTALL) + " other_missing="
                + SetupDiscCopy.notIn(disk.pins.other, disk.inPlace).size() + " free=" + disk.freeBytes + " ready=" + yes(disk.ready)
                + " code=" + (disk.codeProblem == null ? "ok" : "problem") + " remembered_play=" + yes(disk.remembered[0] != null)
                + " remembered_install=" + yes(disk.remembered[1] != null)
                // Only when the marker says something (setup-fixes, C1-1): the line is unchanged otherwise.
                + last("play", disk.lastCopy[0]) + last("install", disk.lastCopy[1]);
    }

    /** " last_copy_play=interrupted|stopped|left", or "" for LAST_NONE. */
    static String last(String disc, int state) {
        String[] words = {null, "interrupted", "stopped", "left"};
        return state <= 0 || state >= words.length ? "" : " last_copy_" + disc + "=" + words[state];
    }

    static String chooser(int row, boolean opened) {
        return "CHOOSER row=" + disc(row) + (opened ? " opened" : " none");
    }

    /** The read permission of a chosen document that was refused was given back (setup-fixes, C4-4). */
    static String released(int row, String why) {
        return "CHOSEN row=" + disc(row) + " permission=released why=" + why;
    }

    /** The marker of a running copy could not be written: a cut-off copy will not be reported by the page. */
    static String markerNotWritten(int disc) {
        return "COPY marker disc=" + disc(disc) + " not-written";
    }

    static String nothingChosen(int row) {
        return "CHOSEN row=" + disc(row) + " nothing";
    }

    /** permission: whether the persistable read permission was taken. */
    static String chosen(int row, long bytes, boolean permission) {
        return "CHOSEN row=" + disc(row) + " bytes=" + bytes + " permission=" + (permission ? "kept" : "not-kept");
    }

    /**
     * "DISC row=play chosen kind=INSTALL serves=install lacking=0 candidates=8 partition=34078720 bytes=..."; a
     * refused image whose sentence on the page is about one file, package or the game program ends with that name,
     * which the page does not show: "... bytes=... named=content/0000000000000000/545408A7/00000002/545408A700000001".
     */
    static String recognised(int row, Disc2Discs.Disc found, SetupDiscModel.Recognition r, boolean fromMemory) {
        return "DISC row=" + disc(row) + (fromMemory ? " remembered" : " chosen") + " kind=" + found.kind + " serves=" + disc(r.disc)
                + " lacking=" + r.lacking + " candidates=" + found.candidates.size() + " partition=" + found.partitionOffset
                + " bytes=" + r.imageBytes + (found.named == null ? "" : " named=" + oneLine(found.named));
    }

    static String rememberedGone(int row, String reason) {
        return "DISC row=" + disc(row) + " remembered unreadable reason=" + oneLine(reason);
    }

    static String choice(int disc, boolean remembered, String why) {
        return "CHOICE disc=" + disc(disc) + (remembered ? " remembered" : " forgotten") + (why == null ? "" : " why=" + why);
    }

    /** "COPY start disc=play in_place=3/10 need=... free=...": in_place above 0 is a copy that goes on. */
    static String copyStart(int disc, SetupDiscModel.Disk disk, long need) {
        return "COPY start disc=" + disc(disc) + " in_place=" + disk.have(disc) + "/" + disk.total(disc) + " need=" + need + " free=" + disk.freeBytes;
    }

    /** "FILE 3/10 WANTED kept bytes=1431193600 sha256=(64) name=xbox360b.rpf" */
    static String file(SetupDiscCopy.FileResult f) {
        return "FILE " + (f.index + 1) + "/" + f.files + " " + f.verdict + (f.kept ? " kept" : " discarded") + " bytes=" + f.bytes
                + " sha256=" + f.sha256 + " name=" + f.name;
    }

    /** "COPY stop-asked disc=play why=app-left": the app left the front while that disc was being copied. */
    static String stopAsked(int disc, String why) {
        return "COPY stop-asked disc=" + disc(disc) + " why=" + why;
    }

    /** The menu's window was made anew while the system's file chooser was open; the setup page is shown again for its answer. */
    static String restored() {
        return "PAGE restored for the file chooser's answer";
    }

    /**
     * "COPY done|stopped|failed disc=play read=10 kept=10 left_overs=0 missing=0 [problem=NO_SPACE error=...]". The
     * error is the log's sentence (Outcome.error): it names the disc's file and the reason, which the page leaves out.
     */
    static String copyEnd(int disc, SetupDiscCopy.Outcome o) {
        String word = o.complete ? "done" : o.stopped ? "stopped" : "failed";
        return "COPY " + word + " disc=" + disc(disc) + " read=" + o.files.size() + "/" + o.filesToRead + " kept=" + o.kept() + " left_overs="
                + o.leftoversRemoved + " missing=" + o.stillMissing.size()
                + (o.error == null ? "" : " problem=" + o.problem + " error=" + oneLine(o.error));
    }

    /** "FINISH ready|not-ready play=10/10 install=8/8 missing_bytes=0" */
    static String finish(SetupDiscModel.Disk disk) {
        long missing = disk.missingBytes(SetupDiscModel.PLAY) + disk.missingBytes(SetupDiscModel.INSTALL)
                + SetupDiscCopy.bytes(SetupDiscCopy.notIn(disk.pins.other, disk.inPlace));
        return "FINISH " + (disk.ready && disk.codeProblem == null ? "ready" : "not-ready") + " play=" + disk.have(SetupDiscModel.PLAY) + "/"
                + disk.total(SetupDiscModel.PLAY) + " install=" + disk.have(SetupDiscModel.INSTALL) + "/" + disk.total(SetupDiscModel.INSTALL)
                + " missing_bytes=" + missing;
    }

    /** The missing entries, one line each: "MISSING disc=install bytes=14 sha256=(64)". */
    static void missing(SetupDiscModel.Disk disk, List<String> into) {
        for (int d = 0; d < SetupDiscModel.DISCS; ++d) {
            for (Disc2Discs.Pin p : disk.missing(d)) into.add("MISSING disc=" + disc(d) + " bytes=" + p.bytes + " sha256=" + p.sha256);
        }
    }

    private static String yes(boolean b) {
        return b ? "yes" : "no";
    }

    private static String oneLine(String text) {
        return text == null ? "" : text.replace('\n', ' ').trim();
    }
}
