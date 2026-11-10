package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.ShipTestKit.*;
import java.io.*;
import java.nio.file.*;
import java.util.*;

public final class PublicSetupTest {
    public static void main(String[] args) throws Exception {
        check("U01 setup action opens the code picker, discs, and menu in order", () -> {
            PublicSetupModel m=new PublicSetupModel();expectEquals(PublicSetupModel.PICK,m.tap(0,false),"one tap opens picker");
            expectEquals(0,m.focus,"picker row focused");
            expectEquals(PublicSetupModel.DISCS,m.tap(1,false),"one tap opens disc setup");m.move(1,false);
            expectEquals(PublicSetupModel.LEAVE,m.confirm(false),"menu");
            expectEquals("Install Game Code",m.page().rows.get(0).label,"label");
        });
        check("U02 setup blocks writes during import and while game is running", () -> {
            PublicSetupModel m=new PublicSetupModel();expectEquals(PublicSetupModel.NONE,m.confirm(true),"game running");
            expectEquals(PublicCodeWords.RUNNING,m.notice,"running sentence");m.started();
            expectEquals(PublicSetupModel.NONE,m.confirm(false),"busy");expectEquals(PublicSetupModel.STOP,m.back(),"B stops");
            m.move(1,false);expectEquals(0,m.focus,"focus stable during work");
        });
        check("U03 cancelled picker and interrupted import retain useful instructions", () -> {
            PublicSetupModel m=new PublicSetupModel();m.started();m.finished(VerifiedExtractor.Outcome.stopped(),null);
            expect(!m.busy,"no stuck work");expectEquals(PublicCodeWords.STOPPED,m.notice,"stopped sentence");
            expectEquals(PublicSetupModel.PICK,m.confirm(false),"can choose again");
            expectContains(PublicCodeWords.NOT_CHOSEN,"No file was chosen","picker cancellation");
        });
        check("U04 public launcher routes missing code to setup without an unqualified-code override", () -> {
            LauncherMenu m=new LauncherMenu(true,true,true);m.setInstall(InstallState.detect(newFilesDir("public-menu")));
            expectEquals("Set Up Game",m.label(LauncherMenu.PRIMARY),"label");
            expectEquals(LauncherMenu.Action.OPEN_GAME_FILES_SETUP,m.confirm(false),"setup route");
            expectEquals(PublicCodeWords.HELP,m.detail(),"public instructions");
        });
        check("U05 public APK build isolates assets and picker wiring remains connected", () -> {
            Path src=Paths.get(System.getProperty("app.main"));
            String gradle=Files.readString(Paths.get(System.getProperty("public.tree"),"app/build.gradle.kts"));
            expect(gradle.contains("create(\"public\")") && gradle.contains("prePublicBuild")
                    && (gradle.contains("signingConfig = null")
                        // (since RC1, 2026-10-24: -PpublicTestSigned signs a test copy with the debug key; without it, unsigned)
                        || gradle.contains("signingConfig = if (project.hasProperty(\"publicTestSigned\")) signingConfigs.getByName(\"debug\") else null")),
                    "isolated unsigned public variant");
            String setup=Files.readString(src.resolve("SetupScreen.java"));
            String screen=Files.readString(src.resolve("PublicSetupScreen.java"));
            String launch=Files.readString(src.resolve("LauncherScreen.java"));
            String start=Files.readString(src.resolve("ShipStartActivity.java"));
            String about=Files.readString(src.resolve("AboutScreen.java"));
            expect(setup.contains("ShipApplication.publicApp() ? new PublicSetupScreen()"),"setup delegates public flow");
            expect(screen.contains("Intent.ACTION_OPEN_DOCUMENT") && screen.contains("CodeBundleImporter.importPublic"),"picker/import wire");
            expect(screen.contains("code.activate(") && screen.contains("new SetupScreen(true)"),"import/activation/discs wire");
            expect(launch.contains("ShipApplication.playerApp(), ShipApplication.publicApp()"),"public menu");
            String pages=Files.readString(src.resolve("ShellPages.java"));
            expect(pages.contains("\"Setup Game\"") && pages.contains("nav.setColumnCount(ShellNav.PLAY, 2)")
                    && !pages.contains("if (menu.publicApp())"),"Setup Game entry is present in asset-free and public APKs");
            expect(launch.contains("nav.column(ShellNav.PLAY) == 1") && launch.contains("new SetupScreen()"),"Setup Game menu route");
            expect(screen.contains("model.tap(row, host.gameRunning())"),"one-tap file row activation");
            expect(start.contains("if (PublicGameCode.isPublic(apkPath)) { done(null); return; }"),"update does not replace import");
            expect(about.contains("PublicGameCode.about(filesDir)"),"About seal wire");
        });
        check("U06 every public refusal category gives distinct player-facing advice", () -> {
            Set<String> seen=new HashSet<>();
            for (VerifiedExtractor.Problem p : new VerifiedExtractor.Problem[]{VerifiedExtractor.Problem.BUSY,
                    VerifiedExtractor.Problem.SOURCE_UNREADABLE,VerifiedExtractor.Problem.BUNDLE_DAMAGED,
                    VerifiedExtractor.Problem.BUNDLE_OTHER,VerifiedExtractor.Problem.NOT_A_BUNDLE,
                    VerifiedExtractor.Problem.EXISTING_INSTALL,VerifiedExtractor.Problem.NO_SPACE,VerifiedExtractor.Problem.STORAGE}) {
                String text=PublicCodeWords.outcome(VerifiedExtractor.Outcome.failed(p,"/private/raw-detail"));
                expect(seen.add(text),"distinct refusal: "+p);expect(!text.contains("/private/") && text.endsWith("."),"plain sentence: "+p);
            }
        });
        check("U07 progress is bounded and a failed activation is never called installed", () -> {
            PublicSetupModel m=new PublicSetupModel();m.started();m.total=10;m.done=15;
            expectEquals(1f,m.page().infoBar,"bounded progress");
            m.finished(VerifiedExtractor.Outcome.done(),PublicCodeWords.DIFFERENT);
            expect(m.notice.contains("could not finish") && !m.notice.equals(PublicCodeWords.INSTALLED),"activation failure");
        });
        check("U08 recreated setup routes only disc picker results to the restored disc page", () -> {
            expect(SetupPickerRoute.restoreDiscs(SetupPickerRoute.PLAY,true),"play result restored");
            expect(SetupPickerRoute.restoreDiscs(SetupPickerRoute.INSTALL,true),"install result restored");
            expect(!SetupPickerRoute.restoreDiscs(SetupPickerRoute.CODE,true),"code stays on import page");
            expect(!SetupPickerRoute.restoreDiscs(-1,true),"unknown result ignored");
            expect(!SetupPickerRoute.restoreDiscs(SetupPickerRoute.PLAY,false),"live disc page is not recreated");
            Path src=Paths.get(System.getProperty("app.main"));
            String screen=Files.readString(src.resolve("SetupScreen.java"));
            expect(screen.contains("SetupPickerRoute.restoreDiscs(requestCode, twoDisc instanceof PublicSetupScreen)")
                    && screen.contains("publicContainer.addView(twoDisc.createView(host))")
                    && screen.contains("twoDisc.onActivityResult(requestCode, resultCode, data)"),"restore and result delivery connected");
            expect(Files.readString(src.resolve("SetupDiscScreen.java")).contains("{SetupPickerRoute.PLAY, SetupPickerRoute.INSTALL}"),"picker codes shared");
        });
        check("U09 public activation and launch failures never expose raw storage paths", () -> {
            String raw="Could not put the game code in place: /private/device/file";
            expect(!PublicCodeWords.activation(raw).contains("/private/"),"activation has its own storage sentence");
            expect(!PublicCodeWords.launch(raw).contains("/private/"),"launch has its own sentence");
            expectContains(PublicCodeWords.activation("This app only works for the device's main user: /private/path"),
                    "main user profile", "main-user refusal");
            expectContains(PublicCodeWords.activation("Not enough free space: /private/path"), "Free space", "space refusal");
            expectEquals(PublicCodeWords.MISSING,PublicCodeWords.launch(PublicCodeWords.MISSING),"specific import refusal preserved");
            Path src=Paths.get(System.getProperty("app.main"));
            expect(Files.readString(src.resolve("PublicGameCode.java")).contains("PublicCodeWords.activation(result.error)"),"activation words connected");
            expect(Files.readString(src.resolve("LauncherMenu.java")).contains("publicApp ? PublicCodeWords.launch(reason)"),"launch words connected");
        });
        ShipTestKit.finish("PUBLIC SETUP");
    }
}
