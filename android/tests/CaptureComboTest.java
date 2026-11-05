package dev.recompbootstrap.smoke;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Host test of CaptureCombo (the Select-held-then-A capture button of the game window). Needs only a JDK:
 *   javac -d out android/app/src/debug/java/dev/recompbootstrap/smoke/CaptureCombo.java android/tests/CaptureComboTest.java
 *   java -cp out dev.recompbootstrap.smoke.CaptureComboTest
 * One line per case (PASS / FAIL); exit code 1 if any failed. Each case states the whole timeline the game receives
 * ("S+@250" = Select pressed at 250 ms, "A-@100" = A released at 100 ms) and the captures; the driver plays the
 * events and calls advance() at every deadline the combo names, as CaptureController's main-thread timer does.
 */
public final class CaptureComboTest {
    private interface Case { void run(); }
    private static int passed, failed;
    private static final int S=CaptureCombo.SELECT, A=CaptureCombo.A, X=99 /* KEYCODE_BUTTON_X */, B=97, PAD=7, TEST=-1, OTHER=9;

    private static void test(String name, Case body) {
        try { body.run(); ++passed; System.out.println("PASS     " + name); }
        catch (Throwable error) { ++failed; System.out.println("FAIL     " + name + ": " + error.getMessage()); }
    }

    /**
     * One input event: a label like "S+" (down), "S-" (up), "S*" (repeat), its own time, the time it is handled (the
     * same unless the main thread was late) and its controller. An event of the second controller OTHER is labelled
     * "S+/2".
     */
    private static final class Ev {
        final String label; final int code; final boolean down; final int repeat; final long time, handled; final int device;
        Ev(String label,int code,boolean down,int repeat,long time,long handled,int device){
            this.label=label;this.code=code;this.down=down;this.repeat=repeat;this.time=time;this.handled=handled;this.device=device;}
    }
    private static Ev ev(String spec,long time){return ev(spec,time,PAD);}
    private static Ev ev(String spec,long time,int device){return late(spec,time,time,device);}
    /** An event with the time `time` that is handled at `handled`. */
    private static Ev late(String spec,long time,long handled,int device){
        char key=spec.charAt(0),kind=spec.charAt(1);
        int code=key=='S'?S:key=='A'?A:key=='X'?X:B;
        return new Ev(device==OTHER?spec+"/2":spec,code,kind!='-',kind=='*'?1:0,time,handled,device);
    }
    /** Not an event: the window loses focus at this time (CaptureController calls reset()). */
    private static Ev reset(long time){return new Ev("RESET",0,false,0,time,time,0);}
    /** Not an event: this controller disconnects at this time (CaptureController calls reset(device)). */
    private static Ev gone(long time,int device){return new Ev("GONE",0,false,0,time,time,device);}

    /** Plays the events; returns what the game received and the captures, as one string. */
    private static String play(boolean timer,Ev... events){
        List<String> game=new ArrayList<>();
        long[] clock={0};
        CaptureCombo<Ev> combo=new CaptureCombo<>(new CaptureCombo.Output<Ev>(){
            @Override public void forward(Ev e){game.add(e.label+"@"+clock[0]);}
            @Override public void capture(long at){game.add("CAPTURE@"+at);}
        });
        for(Ev e:events){
            if(timer)drain(combo,clock,e.handled);
            clock[0]=e.handled;
            if(e.label.equals("RESET")){combo.reset();continue;}
            if(e.label.equals("GONE")){combo.reset(e.device);continue;}
            boolean taken=e.handled==e.time?combo.key(e,e.code,e.down,e.repeat,e.time,e.device)
                    :combo.key(e,e.code,e.down,e.repeat,e.time,e.handled,e.device);
            if(!taken)game.add(e.label+"@"+e.handled);
        }
        drain(combo,clock,Long.MAX_VALUE);
        return String.join(" ",game);
    }
    private static void drain(CaptureCombo<Ev> combo,long[] clock,long until){
        int steps=0;
        for(long due;(due=combo.nextDeadline())!=Long.MIN_VALUE && due<=until;){
            if(++steps>1000)throw new AssertionError("advance() at the named deadline "+due+" does not move it (the timer would spin)");
            clock[0]=Math.max(clock[0],due);combo.advance(clock[0]);
        }
    }
    private static void expect(String got,String want){
        if(!got.equals(want))throw new AssertionError("game got ["+got+"], expected ["+want+"]");
    }

    public static void main(String[] args) {
        test("Select tap alone: pressed late by its length, released after its real length", () ->
            expect(play(true,ev("S+",0),ev("S-",80)),"S+@80 S-@160"));
        test("Select held alone: pressed at 250, released 250 after the real release; repeats kept back", () ->
            expect(play(true,ev("S+",0),ev("S*",400),ev("S*",450),ev("S-",1000)),"S+@250 S-@1250"));
        test("Select held, timer late (main thread busy): the next event gives it, duration kept", () ->
            expect(play(false,ev("S+",0),ev("S-",400)),"S+@400 S-@800"));
        test("Timer late, A after the window (by the events' own times): no combo", () ->
            expect(play(false,ev("S+",0),ev("A+",300),ev("A-",350),ev("S-",400)),"S+@300 A+@300 A-@350 S-@700"));
        test("A alone: never delayed, repeats pass", () ->
            expect(play(true,ev("A+",0),ev("A*",500),ev("A-",600)),"A+@0 A*@500 A-@600"));
        test("Select then A inside the window: capture, the game gets nothing", () ->
            expect(play(true,ev("S+",0),ev("A+",100),ev("A-",200),ev("S-",300)),"CAPTURE@100"));
        test("Select then A, A released after Select: still nothing to the game", () ->
            expect(play(true,ev("S+",0),ev("A+",249),ev("S-",260),ev("A-",900)),"CAPTURE@249"));
        test("A then Select: A at once; Select late as a lone press", () ->
            expect(play(true,ev("A+",0),ev("S+",50),ev("A-",100),ev("S-",500)),"A+@0 A-@100 S+@300 S-@750"));
        test("Select, a long pause, then A: no combo, both reach the game", () ->
            expect(play(true,ev("S+",0),ev("A+",600),ev("A-",700),ev("S-",900)),"S+@250 A+@600 A-@700 S-@1150"));
        test("A exactly at the end of the window is too late", () ->
            expect(play(true,ev("S+",0),ev("A+",250),ev("A-",300),ev("S-",400)),"S+@250 A+@250 A-@300 S-@650"));
        test("Select+A twice in a row: two captures, nothing to the game", () ->
            expect(play(true,ev("S+",0),ev("A+",100),ev("A-",150),ev("S-",200),
                    ev("S+",2000),ev("A+",2100),ev("A-",2150),ev("S-",2200)),"CAPTURE@100 CAPTURE@2100"));
        test("Select held through two A presses: two captures", () ->
            expect(play(true,ev("S+",0),ev("A+",100),ev("A-",150),ev("A+",1300),ev("A-",1350),ev("S-",1400)),
                    "CAPTURE@100 CAPTURE@1300"));
        test("Combo while other buttons are held: they pass untouched", () ->
            expect(play(true,ev("B+",0),ev("X+",5),ev("S+",10),ev("A+",50),ev("A-",90),ev("S-",95),ev("X-",400),ev("B-",500)),
                    "B+@0 X+@5 CAPTURE@50 X-@400 B-@500"));
        test("Another button during the window passes at once; Select still late", () ->
            expect(play(true,ev("S+",0),ev("X+",100),ev("X-",120),ev("S-",600)),"X+@100 X-@120 S+@250 S-@850"));
        test("Repeats during the combo: none reaches the game", () ->
            expect(play(true,ev("S+",0),ev("A+",100),ev("S*",500),ev("A*",550),ev("A*",600),ev("A-",700),ev("S-",800)),
                    "CAPTURE@100"));
        test("Controller disconnects while Select is held back: the game never gets it", () ->
            expect(play(true,ev("S+",0),reset(100),ev("S-",300)),"S-@300"));
        test("Disconnect while a delayed release waits: the release is given at once", () ->
            expect(play(true,ev("S+",0),ev("S-",50),reset(70)),"S+@50 S-@70"));
        test("Disconnect after a combo: the later releases pass (harmless), no stray capture", () ->
            expect(play(true,ev("S+",0),ev("A+",100),reset(150),ev("A-",200),ev("S-",250)),"CAPTURE@100 A-@200 S-@250"));
        test("Disconnect after a late Select press: its release passes at once", () ->
            expect(play(true,ev("S+",0),reset(400),ev("S-",500)),"S+@250 S-@500"));
        test("Synthetic (adb) Select then A: a capture", () ->
            expect(play(true,ev("S+",0,TEST),ev("A+",1,TEST),ev("S-",2,TEST),ev("A-",3,TEST)),"CAPTURE@1"));
        test("adb keycombination (all at one time): a capture", () ->
            expect(play(true,ev("S+",0,TEST),ev("A+",0,TEST),ev("A-",0,TEST),ev("S-",0,TEST)),"CAPTURE@0"));
        test("adb longpress Select alone (down, long-press repeat, up): press 250 late, held 500", () ->
            expect(play(true,ev("S+",0,TEST),ev("S*",500,TEST),ev("S-",500,TEST)),"S+@250 S-@750"));
        test("Select from one controller, A from another: no combo", () ->
            expect(play(true,ev("S+",0,PAD),ev("A+",10,TEST),ev("A-",20,TEST),ev("S-",100,PAD)),"A+@10 A-@20 S+@100 S-@200"));
        test("Double tap faster than the delay: the first release is given before the second press", () ->
            expect(play(true,ev("S+",0),ev("S-",50),ev("S+",70),ev("S-",120)),"S+@50 S-@70 S+@120 S-@170"));
        test("Repeat of A held from before the combo is not swallowed", () ->
            expect(play(true,ev("A+",0),ev("S+",100),ev("A*",150),ev("A-",200),ev("S-",300)),"A+@0 A*@150 A-@200 S+@300 S-@500"));
        // ---- an event's own time and the time it is handled (the main thread was busy, or the input was injected) ----
        test("adb longpress as Android sends it (the release carries the press's own time): the game has Select as long as it was held", () ->
            expect(play(true,ev("S+",1000,TEST),ev("S*",1400,TEST),late("S-",1000,1401,TEST)),"S+@1250 S-@1651"));
        test("Main thread late (Select handled 300 late): A pressed inside the window by its own time is still the combo", () ->
            expect(play(true,late("S+",0,300,PAD),late("A+",100,301,PAD),late("A-",200,302,PAD),late("S-",280,303,PAD)),"CAPTURE@100"));
        test("Main thread late, no A: Select waits a whole window from when it was handled, then keeps its real length", () ->
            expect(play(true,late("S+",0,300,PAD),ev("S-",900)),"S+@550 S-@1450"));
        test("Main thread late, A pressed after the window by its own time: no combo, Select reaches the game before A", () ->
            expect(play(true,late("S+",0,300,PAD),late("A+",260,301,PAD),late("A-",290,302,PAD),late("S-",295,303,PAD)),
                    "S+@301 A+@301 A-@302 S-@596"));
        test("A tap whose release is handled late, inside the window: the game still has it for the tap's length", () ->
            expect(play(true,ev("S+",0),late("S-",80,200,PAD)),"S+@200 S-@280"));
        test("A late timer does not undo a combo: A handled after the window's deadline but pressed inside the window", () ->
            expect(play(false,ev("S+",0),late("A+",249,252,PAD),late("A-",300,301,PAD),ev("S-",400)),"CAPTURE@249"));
        // ---- two controllers: releases, repeats and second presses belong to the controller they came from ----
        test("A combo held on one controller does not keep another controller's A from the game", () ->
            expect(play(true,ev("S+",0,OTHER),ev("A+",100,OTHER),ev("A+",300),ev("A*",320),ev("A-",350),ev("A-",1000,OTHER),ev("S-",1100,OTHER)),
                    "CAPTURE@100 A+@300 A*@320 A-@350"));
        test("Select held back for one controller does not take another controller's Select press", () ->
            expect(play(true,ev("S+",0,OTHER),ev("S+",100),ev("S-",400),ev("S-",500,OTHER)),"S+/2@250 S+@350 S-@650 S-/2@750"));
        test("A further A on another controller while a combo is held is no capture", () ->
            expect(play(true,ev("S+",0),ev("A+",100),ev("A+",200,OTHER),ev("A-",250,OTHER),ev("A-",300),ev("S-",350)),
                    "CAPTURE@100 A+/2@200 A-/2@250"));
        test("One controller goes away: only its own held-back Select is dropped", () ->
            expect(play(true,ev("S+",0),ev("S+",10,OTHER),gone(100,OTHER),ev("S-",600),ev("S-",700,OTHER)),"S+@250 S-/2@700 S-@850"));
        test("Known and left as it is: a tap keeps its length and A is never delayed, so A right after a tap overlaps it", () ->
            expect(play(true,ev("S+",0),ev("S-",80),ev("A+",100),ev("A-",300)),"S+@80 A+@100 S-@160 A-@300"));
        test("Constants are the Android key codes 109 / 96, window 250 ms", () -> {
            if(S!=109 || A!=96 || CaptureCombo.WINDOW_MS!=250)throw new AssertionError(Arrays.toString(new long[]{S,A,CaptureCombo.WINDOW_MS}));
        });
        System.out.println("CaptureComboTest: " + passed + " passed, " + failed + " failed");
        if (failed > 0) System.exit(1);
    }
}
