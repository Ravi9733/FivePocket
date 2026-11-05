package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.ShipTestKit.*;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.zip.*;

/** Checks the actual unsigned APK against fresh native outputs; three independent APK mutations must be refused. */
public final class PublicPackageTest {
    static Map<String,byte[]> entries(File apk) throws Exception {
        Map<String,byte[]> files=new TreeMap<>();
        try(ZipFile z=new ZipFile(apk)) {
            Enumeration<? extends ZipEntry> all=z.entries();
            while(all.hasMoreElements()) {ZipEntry e=all.nextElement();if(e.isDirectory())continue;
                expect(!files.containsKey(e.getName()),"no duplicate ZIP name");
                try(InputStream in=z.getInputStream(e)){files.put(e.getName(),in.readAllBytes());}
            }
        }
        return files;
    }
    static void assets(Map<String,byte[]> files) {
        Set<String> names=new TreeSet<>();for(String n:files.keySet())if(n.startsWith("assets/"))names.add(n);
        expectEquals(new TreeSet<>(Arrays.asList("assets/public/config.json","assets/public/start-request.bin",
                "assets/licenses/AMD-FidelityFX-FSR1-MIT.txt","assets/fonts/NunitoSans-VF.ttf","assets/licenses/NunitoSans-OFL.txt",
                "assets/licenses/GPL-3.0.txt")),names,"exact public asset allowlist");
    }
    static void nativeCode(Map<String,byte[]> files,File build) throws Exception {
        Set<String> names=new TreeSet<>();for(String n:files.keySet())if(n.startsWith("lib/"))names.add(n);
        expectEquals(new TreeSet<>(Arrays.asList("lib/arm64-v8a/librecomp_bootstrap.so","lib/arm64-v8a/librecomp_private_debug.so",
                "lib/arm64-v8a/libc++_shared.so")),names,"only the freshly built loader, self-test and standard runtime");
        for(String n:names) {
            String base=n.substring(n.lastIndexOf('/')+1);
            // The reference is what this build compiled and packaged: native/<name> in the build folder. Gradle packages
            // a stripped copy of the compiler kit's libc++ and the build folder then holds that copy; the offline
            // packager stores the kit's file unchanged and keeps no copy, so there the kit's own file is the reference.
            File built=new File(build,"native/"+base);
            File original=base.equals("libc++_shared.so") && !built.isFile()
                    ?new File("/opt/homebrew/share/android-commandlinetools/ndk/27.2.12479018/toolchains/llvm/prebuilt/darwin-x86_64/sysroot/usr/lib/aarch64-linux-android/"+base)
                    :built;
            expectEquals(ShipFiles.sha256Of(original),sha256(files.get(n)),"compiled native identity");
        }
    }
    static void record(Map<String,byte[]> files) throws Exception {
        ShipConfig c=ShipConfig.parse(files.get("assets/public/config.json"));
        byte[] request=files.get("assets/public/start-request.bin");
        expectEquals(c.startRequest.bytes,(long)request.length,"request length");
        expectEquals(c.startRequest.sha256,sha256(request),"request identity");
        Set<String> absent=new HashSet<>();absent.add(c.library.sha256);absent.add(c.shaderPack.sha256);
        for(ShipConfig.DataFile p:c.pinnedData)absent.add(p.sha256);
        for(byte[] data:files.values())expect(!absent.contains(sha256(data)),"no pinned game or boot payload packaged");
        expect(files.containsKey("AndroidManifest.xml") && files.containsKey("classes.dex"),"APK manifest and dex");
    }
    static void portablePin(Map<String,byte[]> files) throws Exception {
        ShipConfig c=ShipConfig.parse(files.get("assets/public/config.json"));
        expectEquals(PublicGameCode.PORTABLE_LIBRARY_SHA256,c.library.sha256,"named portable library pin");
    }
    interface Audit {void run(Map<String,byte[]> files) throws Exception;}
    static void caught(String name,Map<String,byte[]> bad,Audit audit) throws Exception {
        boolean caught=false;try {audit.run(bad);}catch(AssertionError e){caught=true;}
        expect(caught,"APK mutant rejected: "+name);System.out.println("APK MUTANT "+name+" rejected");
    }
    public static void main(String[] args) throws Exception {
        File build=new File(System.getProperty("public.build"));
        Map<String,byte[]> apk=entries(new File(build,"app-public-unsigned.apk"));
        check("A01 actual APK includes only the three allowlisted public assets",()->{
            assets(apk);Map<String,byte[]> bad=new TreeMap<>(apk);bad.put("assets/ship/game-library.bin",new byte[]{1});
            caught("extra game asset",bad,PublicPackageTest::assets);
        });
        check("A02 actual APK native entries equal only the fresh runtime compilation",()->{
            nativeCode(apk,build);Map<String,byte[]> bad=new TreeMap<>(apk);bad.put("lib/arm64-v8a/librecomp_private_debug.so",new byte[]{1});
            caught("substituted native code",bad,f->nativeCode(f,build));
            Map<String,byte[]> other=new TreeMap<>(apk);byte[] runtime=apk.get("lib/arm64-v8a/libc++_shared.so").clone();runtime[runtime.length/2]^=1;
            other.put("lib/arm64-v8a/libc++_shared.so",runtime);caught("standard runtime altered by one bit",other,f->nativeCode(f,build));
        });
        check("A03 actual APK metadata/request match and all pinned payloads are absent",()->{
            record(apk);Map<String,byte[]> bad=new TreeMap<>(apk);byte[] changed=apk.get("assets/public/start-request.bin").clone();changed[0]^=1;
            bad.put("assets/public/start-request.bin",changed);caught("altered start request",bad,PublicPackageTest::record);
        });
        check("A04 actual APK uses the named portable pin and refuses another identity",()->{
            portablePin(apk);Map<String,byte[]> bad=new TreeMap<>(apk);
            String changed=new String(apk.get("assets/public/config.json"),java.nio.charset.StandardCharsets.UTF_8)
                    .replace(PublicGameCode.PORTABLE_LIBRARY_SHA256,"1".repeat(64));
            bad.put("assets/public/config.json",changed.getBytes(java.nio.charset.StandardCharsets.UTF_8));
            caught("different portable pin",bad,PublicPackageTest::portablePin);
        });
        ShipTestKit.finish("PUBLIC PACKAGE (5 APK mutants)");
    }
}
