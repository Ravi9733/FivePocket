package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.ShipTestKit.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.*;
import java.util.zip.*;

/** Synthetic bytes only. Connected public ZIP -> imported files -> installer -> disc copier -> launch request. */
public final class PublicGameCodeTest {
    static final class Fixture {
        final File files = newFilesDir("public");
        final ShipTestKit.Version version = new ShipTestKit.Version(files);
        final SetupCatalog.Spec spec;
        final SetupTestKit.Bundle zip;
        final PublicGameCode source;
        final byte[] archive;
        final String apk;
        Fixture() throws Exception {
            version.bundledPaths.addAll(Arrays.asList(ShipTestKit.GENERATED)); version.build();
            byte[] title = version.inputs.get(0);
            spec = SetupTestKit.spec(title, Arrays.asList(SetupTestKit.item("default.xex", title)), false);
            zip = new SetupTestKit.Bundle(sha256(title)); zip.runtimeAbi = CodeBundle.PUBLIC_ABI; zip.schema = "gta5-code-bundle/2";
            zip.add("game.so", "game-library", version.library).add("title.xex", "title-xex", title)
                    .add("image.bin", "decoded-image", dataOf(version).get("decoded.image.bin"))
                    .add("pack.bin", "shader-pack", version.pack);
            for (String path : version.bundledPaths) if (!path.equals("decoded.image.bin"))
                zip.add("runtime/" + path, "runtime-data", dataOf(version).get(path));
            archive = zip.build();
            File a = new File(files.getParentFile(), "public.apk");
            try (ZipOutputStream out = new ZipOutputStream(new FileOutputStream(a))) {
                out.putNextEntry(new ZipEntry("assets/" + PublicGameCode.CONFIG)); out.write(version.record); out.closeEntry();
                out.putNextEntry(new ZipEntry("assets/" + PublicGameCode.REQUEST)); out.write(version.request); out.closeEntry();
            }
            apk = a.getAbsolutePath(); source = (PublicGameCode)PublicGameCode.open(apk, files);
        }
        VerifiedExtractor.Outcome install(byte[] bytes) {
            return CodeBundleImporter.importPublic(new ByteArrayInputStream(bytes), InstallGuard.root(files),
                    new SetupTestKit.Recorder(), spec, SetupTestKit.PLENTY, source.config);
        }
        void install() { VerifiedExtractor.Outcome o = install(archive); expect(o.complete, "import: " + o.error); }
        void activate() { expectEquals(null, source.activate(ShipTestKit.QUIET), "activation"); }
        void discs() throws Exception {
            // Actual existing two-disc copy consumer, using synthetic in-memory candidates and the same pinned identities.
            List<Disc2Discs.Pin> pins = new ArrayList<>();
            List<Disc2Discs.Candidate> candidates = new ArrayList<>();
            for (byte[] bytes : version.inputs) {
                pins.add(new Disc2Discs.Pin(sha256(bytes), bytes.length));
                candidates.add(new Disc2Discs.Candidate("disc-file-" + candidates.size(), bytes.length, false,
                        () -> new ByteArrayInputStream(bytes)));
            }
            Disc2Discs.Disc disc = new Disc2Discs.Disc(Disc2Discs.Kind.PLAY, "", "", 0, candidates, Collections.emptyList(), Collections.emptyList());
            SetupDiscCopy.Outcome o = SetupDiscCopy.run(disc, pins, pins, Collections.emptySet(),
                    new File(files, "private-trace"), new SetupDiscCopy.DiskStore(new File(files, "private-trace")), new SetupDiscCopy.Progress() {
                        public void update(long a,long b,int c,int d,String e) {}
                        public boolean cancelled() { return false; }
                    }, SetupTestKit.PLENTY, r -> {});
            expect(o.complete, "disc copy: " + o.error);
        }
    }
    static void overwrite(File f, byte[] bytes) throws Exception {
        Files.setPosixFilePermissions(f.toPath(), PosixFilePermissions.fromString("rw-------"));
        Files.write(f.toPath(), bytes);
        Files.setPosixFilePermissions(f.toPath(), PosixFilePermissions.fromString("r--------"));
    }
    static void refused(Fixture f, SetupTestKit.Bundle b, String reason) {
        VerifiedExtractor.Outcome o = f.install(b.build()); expect(!o.complete, "refused"); expectEquals(reason, PublicCodeWords.outcome(o), "own sentence");
        expect(!new File(InstallGuard.root(f.files), "bundle").exists(), "no committed bundle");
    }
    public static void main(String[] args) throws Exception {
        check("P01 empty public APK refuses Play even with an unqualified developer install", () -> {
            Fixture f = new Fixture();
            ShipFirstRun.run(f.version.bundle, f.files, ShipTestKit.QUIET, ShipTestKit.PLENTY);
            f.discs();
            expect(InstallState.detect(f.files).ready(), "legacy layout would be ready");
            ShipPlay.Result p = ShipPlay.prepare(f.apk, f.files, true);
            expectEquals(PublicCodeWords.MISSING, p.error, "public gate"); expect(p.request == null, "no request");
        });
        check("P02 import before discs stores verified seal but cannot offer Play", () -> {
            Fixture f = new Fixture(); f.install();
            String seal = new String(f.archive, f.archive.length - 64, 64, StandardCharsets.US_ASCII);
            expectEquals(seal, PublicGameCode.seal(InstallGuard.root(f.files)), "seal preserved");
            expectContains(PublicGameCode.about(f.files), seal, "About identifies bundle");
            expect(!ShipReady.of(f.apk,f.files).ready(), "import alone is not readiness");
        });
        check("P03 sealed import activates, disc copy connects, Play binds the exact library and arguments", () -> {
            Fixture f = new Fixture(); f.install(); f.activate();
            expect(!ShipReady.of(f.apk,f.files).ready(), "disc data still required"); f.discs();
            expect(ShipReady.of(f.apk,f.files).ready(), "ready only after disc copy");
            ShipPlay.Result p = ShipPlay.prepare(f.apk,f.files,false); expectEquals(null,p.error,"Play");
            expectEquals(sha256(f.version.library),p.request.expectedSha256,"library identity");
            byte[] request = read(new File(f.files,"private-trace/run-"+p.request.requestId+"/arguments.bin"));
            expect(Arrays.equals(f.version.requestFor(p.request.requestId),request),"exact consumer arguments");
        });
        check("P04 public app reopen and repeated import preserve every imported byte and file identity", () -> {
            Fixture f = new Fixture();f.install();f.activate();f.discs();
            Map<String,String> before = snapshot(f.files);
            try (ShipBundle reopen = PublicGameCode.open(f.apk,f.files)) {
                expect(ShipReady.of(reopen,f.files).ready(),"update same contract stays ready");
                expectEquals(null,((PublicGameCode)reopen).activate(ShipTestKit.QUIET),"idempotent activation");
            }
            expect(f.install(new byte[]{1}).complete,"existing good import kept without reading chosen replacement");
            expectEquals(before,snapshot(f.files),"entire imported layout retained");
        });
        check("P05 wrong runtime version has its own refusal", () -> {
            Fixture f = new Fixture();f.zip.runtimeAbi="private-trace-fd-dlopen/77";
            // Remove extended roles so manifest shape is valid for the old parser; ABI must still refuse.
            f.zip.components.keySet().removeIf(n -> n.startsWith("runtime/"));
            refused(f,f.zip,PublicCodeWords.VERSION);
        });
        check("P06 another supported-looking game identity is refused", () -> {
            Fixture f = new Fixture(); f.zip.executableSha256=sha256(bytes(70,99));f.zip.components.remove("title.xex");
            refused(f,f.zip,PublicCodeWords.GAME);
        });
        check("P07 missing runtime piece refuses before committing", () -> {
            Fixture f = new Fixture(); f.zip.components.remove("runtime/modules/boot/depth/vertex/guest-code.bin");
            refused(f,f.zip,PublicCodeWords.COMPONENTS);
        });
        check("P08 duplicate role cannot replace a required part", () -> {
            Fixture f = new Fixture(); f.zip.add("second.so","game-library",f.version.library);
            refused(f,f.zip,PublicCodeWords.EXTRA);
        });
        check("P09 correctly sealed but wrong identity cannot reach the consumer", () -> {
            Fixture f = new Fixture(); f.zip.components.put("pack.bin", bytes(f.version.pack.length,88));
            refused(f,f.zip,PublicCodeWords.DIFFERENT);
        });
        check("P10 tampered seal refuses and leaves no import", () -> {
            Fixture f = new Fixture(); byte[] bad=f.archive.clone();bad[bad.length-1]^=1;
            VerifiedExtractor.Outcome o=f.install(bad);expectEquals(PublicCodeWords.DAMAGED,PublicCodeWords.outcome(o),"seal refusal");
            expect(!new File(InstallGuard.root(f.files),"bundle").exists(),"no commit");
        });
        check("P11 missing installed seal is a distinct refusal", () -> {
            Fixture f=new Fixture();f.install();Files.delete(new File(InstallGuard.root(f.files),"bundle/seal.txt").toPath());
            expectEquals(PublicCodeWords.SEAL,PublicGameCode.installedProblem(InstallGuard.root(f.files),f.source.config),"missing seal");
        });
        check("P12 changed private import refuses even when active copy is intact", () -> {
            Fixture f=new Fixture();f.install();f.activate();f.discs();
            overwrite(new File(InstallGuard.root(f.files),"bundle/pack.bin"),bytes(f.version.pack.length,75));
            expect(!ShipReady.of(f.apk,f.files).ready(),"menu refuses the same corrupted import before Play");
            expectEquals(PublicCodeWords.CHANGED,ShipPlay.prepare(f.apk,f.files,true).error,"private hash gate");
        });
        check("P13 changed active shader fails the existing connected launch gate", () -> {
            Fixture f=new Fixture();f.install();f.activate();f.discs();
            overwrite(new File(f.files,"private-trace/"+InstallState.SHADER_FILE),bytes(f.version.pack.length,76));
            expect(ShipPlay.prepare(f.apk,f.files,true).error!=null,"refused changed active shader");
        });
        check("P14 adapter never opens a bundle-selected launch request or arbitrary path", () -> {
            Fixture f=new Fixture();f.install();
            try(InputStream in=f.source.open(f.source.config.startRequest.asset)) {expect(Arrays.equals(f.version.request,in.readAllBytes()),"app owns request");}
            boolean refused=false;try {f.source.open("../../settings/settings.json");}catch(IOException e){refused=true;}
            expect(refused,"unmapped asset refused");
        });
        check("P15 cancellation cleans quarantine and keeps existing disc data", () -> {
            Fixture f=new Fixture();f.discs();Map<String,String> before=snapshot(new File(f.files,"private-trace"));
            VerifiedExtractor.Outcome o=CodeBundleImporter.importPublic(new ByteArrayInputStream(f.archive),InstallGuard.root(f.files),
                    new VerifiedExtractor.Progress(){public void update(long a,long b,int c,int d,String e){}public boolean cancelled(){return true;}},
                    f.spec,SetupTestKit.PLENTY,f.source.config);
            expect(o.cancelled,"cancelled");expectEquals(before,snapshot(new File(f.files,"private-trace")),"disc data kept");
            expect(!new File(InstallGuard.root(f.files),"bundle").exists(),"no bundle");
        });
        check("P16 low space and concurrent import each have their own refusal", () -> {
            Fixture f=new Fixture();
            VerifiedExtractor.Outcome o=CodeBundleImporter.importPublic(new ByteArrayInputStream(f.archive),InstallGuard.root(f.files),new SetupTestKit.Recorder(),f.spec,SetupTestKit.spaceOf(0),f.source.config);
            expectContains(PublicCodeWords.outcome(o),"not enough free space","space");
            expect(VerifiedExtractor.begin(),"lock taken");try {
                expectEquals(PublicCodeWords.BUSY,PublicCodeWords.outcome(f.install(f.archive)),"busy");
            }finally {VerifiedExtractor.end();}
        });
        check("P17 app with another pin preserves import and refuses compatibility", () -> {
            Fixture f=new Fixture();f.install();Map<String,String> before=snapshot(f.files);
            f.version.library=library(f.version.library.length,123);f.version.build();
            PublicGameCode newer=new PublicGameCode(f.source,f.files,f.version.record);
            expectEquals(PublicCodeWords.DIFFERENT,newer.activate(ShipTestKit.QUIET),"updated contract refused");
            expectEquals(before,snapshot(f.files),"update kept import");
        });
        check("P18 runtime-data is unavailable under the legacy ABI", () -> {
            Fixture f=new Fixture();f.zip.runtimeAbi=CodeBundle.RUNTIME_ABI;boolean refused=false;
            try {CodeBundle.parseManifest(f.zip.manifest());}catch(IllegalArgumentException e){refused=true;}
            expect(refused,"legacy contract unchanged");
        });
        check("P19 another library identity is refused before import", () -> {
            Fixture f=new Fixture();f.zip.components.put("game.so",library(f.version.library.length,91));
            refused(f,f.zip,PublicCodeWords.DIFFERENT);
        });
        check("P20 legacy format has its own public refusal", () -> {
            Fixture f=new Fixture();f.zip.schema="gta5-code-bundle/1";f.zip.runtimeAbi=CodeBundle.RUNTIME_ABI;
            f.zip.components.keySet().removeIf(n -> n.startsWith("runtime/"));
            refused(f,f.zip,PublicCodeWords.VERSION);
        });
        check("P21 real host ZIP writer feeds the public importer and Play consumer", () -> {
            Fixture f=new Fixture();Path root=Files.createTempDirectory("host-writer-");
            Files.writeString(root.resolve("input-manifest.json"),f.zip.manifest());
            for(Map.Entry<String,byte[]> e:f.zip.components.entrySet()) {
                Path target=root.resolve("components").resolve(e.getKey());Files.createDirectories(target.getParent());Files.write(target,e.getValue());
            }
            File helper=new File(System.getProperty("repo.root"),"local-analysis/agents/codex-mb/builder-app/make_host_fixture.py");
            Process process=new ProcessBuilder("python3",helper.getAbsolutePath(),root.toString()).redirectErrorStream(true).start();
            String result=new String(process.getInputStream().readAllBytes(),StandardCharsets.UTF_8);
            expectEquals(0,process.waitFor(),"real host writer: "+result);
            expectContains(result,"HOST_WRITER_FIXTURE_PASS","producer marker");
            byte[] bytes=Files.readAllBytes(root.resolve("host-written.bundle"));
            VerifiedExtractor.Outcome imported=f.install(bytes);expect(imported.complete,"real importer: "+imported.error);
            expectEquals(new String(bytes,bytes.length-64,64,StandardCharsets.US_ASCII),PublicGameCode.seal(InstallGuard.root(f.files)),"same seal");
            f.activate();f.discs();expectEquals(null,ShipPlay.prepare(f.apk,f.files,false).error,"connected launch consumer");
            // Preserve this receipt in the selected log folder before the runner deletes its temporary work directory.
            String keep=System.getProperty("public.evidence");
            if(keep!=null) {Path out=Path.of(keep);Files.createDirectories(out);
                Files.copy(root.resolve("writer-receipt.json"),out.resolve("host-writer-receipt.json"));
                Files.copy(root.resolve("host-written.bundle"),out.resolve("host-written.bundle"));}
        });
        ShipTestKit.finish("PUBLIC GAME CODE");
    }
}
