package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SettingsCoreTestKit.OLD_TIME;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.age;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.ascii;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.bytes;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.check;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.differences;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.put;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.same;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.sameBytes;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.snapshot;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.tempDir;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.test;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.throwsA;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Map;

/** Host test of AtomicFiles: whole-file writes through a temporary file and a rename, and removal. Run with java -ea. */
public final class AtomicFilesTest {
    public static void main(String[] args) {
        SettingsCoreTestKit.begin("AtomicFilesTest");

        test("write: the file holds exactly the content and no temporary file is left", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                File directory = tempDir();
                byte[] binary = new byte[70000];
                for (int i = 0; i < binary.length; i++) binary[i] = (byte) (i * 31);
                byte[][] contents = {ascii("{\"version\":1}\n"), new byte[0], binary};
                for (int i = 0; i < contents.length; i++) {
                    File target = new File(directory, "file" + i);
                    AtomicFiles.write(target, contents[i]);
                    sameBytes(contents[i], bytes(directory, "file" + i), "content " + i);
                }
                same(Arrays.asList("file0", "file1", "file2"), new ArrayList<String>(snapshot(directory).keySet()), "files in the directory");
            }
        });

        test("write: an existing file is replaced as a whole", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                File directory = tempDir();
                put(directory, "settings.json", "a long earlier content that must not shine through\n");
                AtomicFiles.write(new File(directory, "settings.json"), ascii("short\n"));
                sameBytes(ascii("short\n"), bytes(directory, "settings.json"), "content");
                same(Arrays.asList("settings.json"), new ArrayList<String>(snapshot(directory).keySet()), "files in the directory");
            }
        });

        test("a crash between writing the temporary file and the rename leaves the old file as it was", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                File directory = tempDir();
                put(directory, "settings.json", "old content\n");
                age(directory);
                File target = new File(directory, "settings.json");
                // the first half of a write, and then nothing more: what a crash or a power loss at this point leaves
                File temp = AtomicFiles.writeTemp(target, ascii("new content\n"));
                sameBytes(ascii("old content\n"), bytes(directory, "settings.json"), "the target after the interrupted write");
                same(OLD_TIME, target.lastModified(), "the target's modification time");
                same(new File(directory, "settings.json.tmp"), temp, "the temporary file's name");
                sameBytes(ascii("new content\n"), bytes(directory, "settings.json.tmp"), "the temporary file is complete");
                // the next write replaces the stray temporary file, also when the crash left only a part of it
                put(directory, "settings.json.tmp", "new con");
                AtomicFiles.write(target, ascii("newer content\n"));
                sameBytes(ascii("newer content\n"), bytes(directory, "settings.json"), "the target after the next write");
                same(Arrays.asList("settings.json"), new ArrayList<String>(snapshot(directory).keySet()), "no temporary file is left");
            }
        });

        test("write: creates no directory, and a failed write leaves nothing behind", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                final File directory = tempDir();
                throwsA(IOException.class, "write into a folder that does not exist", new SettingsCoreTestKit.Case() {
                    @Override public void run() throws IOException { AtomicFiles.write(new File(directory, "missing/file"), ascii("x")); }
                });
                put(directory, "folder/inside", "kept");
                put(directory, "file", "old");
                check(new File(directory, "file.tmp").mkdir(), "test setup: an empty folder stands under the temporary name");
                Map<String, String> before = snapshot(directory);
                throwsA(IOException.class, "write over a folder", new SettingsCoreTestKit.Case() {
                    @Override public void run() throws IOException { AtomicFiles.write(new File(directory, "folder"), ascii("x")); }
                });
                throwsA(IOException.class, "write while a folder stands under the temporary name", new SettingsCoreTestKit.Case() {
                    @Override public void run() throws IOException { AtomicFiles.write(new File(directory, "file"), ascii("new")); }
                });
                same(new ArrayList<String>(), differences(before, snapshot(directory)), "after the three failed writes (no folder removed, no file changed)");
            }
        });

        test("write: a symbolic link standing under the temporary name is not written through", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                File directory = tempDir();
                put(directory, "saves/slot0", "the user's save");
                put(directory, "private-trace/keep", "");
                age(directory);
                String saveBefore = snapshot(directory).get("saves/slot0");
                File target = new File(directory, "private-trace/diag-fps-overlay");
                Files.createSymbolicLink(AtomicFiles.tempFor(target).toPath(), new File(directory, "saves/slot0").toPath());
                AtomicFiles.write(target, ascii("switch"));
                same(saveBefore, snapshot(directory).get("saves/slot0"), "the save behind the link");
                sameBytes(ascii("switch"), bytes(directory, "private-trace/diag-fps-overlay"), "the target");
                check(!Files.exists(AtomicFiles.tempFor(target).toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS), "the link is gone, no temporary file is left");
            }
        });

        test("remove: says whether a file was removed, and refuses directories", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                final File directory = tempDir();
                put(directory, "switch", "");
                put(directory, "folder/inside", "kept");
                check(new File(directory, "empty-folder").mkdir(), "test setup");
                check(AtomicFiles.remove(new File(directory, "switch")), "an existing file: true");
                check(!new File(directory, "switch").exists(), "the file is gone");
                check(!AtomicFiles.remove(new File(directory, "switch")), "no file: false");
                Map<String, String> before = snapshot(directory);
                for (final String name : new String[] {"folder", "empty-folder"}) {
                    throwsA(IOException.class, "remove of the directory " + name, new SettingsCoreTestKit.Case() {
                        @Override public void run() throws IOException { AtomicFiles.remove(new File(directory, name)); }
                    });
                }
                same(new ArrayList<String>(), differences(before, snapshot(directory)), "the directories are untouched");
            }
        });

        test("remove: a symbolic link is unlinked, what it points to stays", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                File directory = tempDir();
                put(directory, "saves/slot0", "the user's save");
                File link = new File(directory, "link");
                Files.createSymbolicLink(link.toPath(), new File(directory, "saves/slot0").toPath());
                check(AtomicFiles.remove(link), "the link is removed");
                sameBytes(ascii("the user's save"), bytes(directory, "saves/slot0"), "the save");
                check(!Files.isSymbolicLink(link.toPath()), "the link is gone");
            }
        });

        SettingsCoreTestKit.end();
    }
}
