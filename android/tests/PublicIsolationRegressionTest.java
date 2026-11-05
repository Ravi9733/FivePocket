package dev.recompbootstrap.smoke;
import static dev.recompbootstrap.smoke.ShipTestKit.*;
import java.io.*;
import java.util.zip.*;

/** Uses only pre-M11 APIs. Fails at runtime on the original tree: a public marker fell through to developer Play. */
public final class PublicIsolationRegressionTest {
    public static void main(String[] args) {
        check("R01 public marker must not launch an unrelated developer installation", () -> {
            ShipTestKit.Version v=new ShipTestKit.Version(newFilesDir("public-regression")).build();
            expect(ShipFirstRun.run(v.bundle,v.files,ShipTestKit.QUIET,ShipTestKit.PLENTY).ok(),"fixture first run");
            expect(ShipDataImport.run(v.config(),new ShipTestKit.MemoryFolder().with(dataOf(v)),v.files,
                    new ShipTestKit.Recorder(),ShipTestKit.PLENTY).complete,"fixture data");
            expect(InstallState.detect(v.files).ready(),"baseline layout ready");
            File apk=new File(v.files.getParentFile(),"public.apk");
            try(ZipOutputStream z=new ZipOutputStream(new FileOutputStream(apk))) {
                z.putNextEntry(new ZipEntry("assets/public/config.json"));z.write(v.record);z.closeEntry();
                z.putNextEntry(new ZipEntry("assets/public/start-request.bin"));z.write(v.request);z.closeEntry();
            }
            ShipPlay.Result p=ShipPlay.prepare(apk.getAbsolutePath(),v.files,true);
            expect(p.error!=null && p.request==null,"public app cannot use developer fallback");
        });
        ShipTestKit.finish("PUBLIC ISOLATION REGRESSION");
    }
}
