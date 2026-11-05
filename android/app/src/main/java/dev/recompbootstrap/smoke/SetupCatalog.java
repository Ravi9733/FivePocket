package dev.recompbootstrap.smoke;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * What the first-run setup expects on the player's discs: names, sizes and SHA-256 values only - no file content
 * and no key. Pure Java.
 *
 * Source of the table: the repository's local records local-analysis/app-game-cache-replay-bx9xcvpn/verified-inputs.json
 * and local-analysis/repro-tag-leaf/additional-read-only-files.json (the entries whose source lies in the play disc
 * folder). SetupCatalogTest compares this table with those records when they are present.
 *
 * The setup page of a developer build does not cover the install disc: installDiscAvailable() is false and an
 * install made from items() is never recorded as complete (see InstallGuard.refreshRecord). The two-disc setup of a
 * build that carries its game code uses installItems() as well: the install disc's eight files, from
 * local-analysis/app-game-cache-replay-om2astl7/verified-inputs.json (roles installed-file-0 to -7) and the
 * real-disc check of lane mini-disc-on-device (verify/expected-disc-inputs.txt).
 */
final class SetupCatalog {
    static final String EXECUTABLE_NAME = "default.xex";
    static final String EXECUTABLE_SHA256 = "f0ca93fbe6b3dfbf50c026e0d77a71e8294b13a5a775af1b6a64f07b0fc10a3d";
    static final String LABEL = "GTA V for Xbox 360 (disc release)";
    static final String PLAY = "play", INSTALL = "install";

    static final class Item {
        /** "play" or "install". */
        final String disc;
        /** Path on that disc, parts joined by '/'. */
        final String path;
        final long bytes;
        /** 64 lower-case hex digits. */
        final String sha256;

        Item(String disc, String path, long bytes, String sha256) {
            if (!PLAY.equals(disc) && !INSTALL.equals(disc)) throw new IllegalArgumentException("unknown disc: " + disc);
            if (path == null || path.isEmpty()) throw new IllegalArgumentException("empty path");
            if (bytes < 0) throw new IllegalArgumentException("negative size: " + path);
            if (!isSha256(sha256)) throw new IllegalArgumentException("not a SHA-256 value: " + path);
            this.disc = disc;
            this.path = path;
            this.bytes = bytes;
            this.sha256 = sha256;
        }
    }

    /** Everything one setup run is checked against. The app uses spec(); tests build small synthetic ones. */
    static final class Spec {
        final String label;
        final String executableName;
        final String executableSha256;
        /** The whole catalog (every disc that is covered). */
        final List<Item> items;
        /** False while a disc the game needs is missing from items: such an install is never recorded as complete. */
        final boolean coversAllDiscs;

        Spec(String label, String executableName, String executableSha256, List<Item> items, boolean coversAllDiscs) {
            if (!isSha256(executableSha256)) throw new IllegalArgumentException("not a SHA-256 value");
            this.label = label;
            this.executableName = executableName;
            this.executableSha256 = executableSha256;
            this.items = Collections.unmodifiableList(new ArrayList<Item>(items));
            this.coversAllDiscs = coversAllDiscs;
        }
    }

    private static final List<Item> ITEMS = build();
    private static final Spec SPEC = new Spec(LABEL, EXECUTABLE_NAME, EXECUTABLE_SHA256, ITEMS, installDiscAvailable());

    private SetupCatalog() {}

    /** The play disc's files (the executable first). Unmodifiable. */
    static List<Item> items() {
        return ITEMS;
    }

    static long totalBytes() {
        long total = 0;
        for (Item item : ITEMS) total += item.bytes;
        return total;
    }

    /** False in this version: the install disc cannot be read yet. */
    static boolean installDiscAvailable() {
        return false;
    }

    static Spec spec() {
        return SPEC;
    }

    // ---- the two-disc setup of a build that carries its game code (SetupDiscModel, SetupDiscCopy) ----------------

    /** The title the install disc's content packages name in their headers (read from the real install disc). */
    static final long TITLE_ID = 0x545408A7L;

    private static final List<Item> INSTALL_ITEMS = buildInstall();
    private static final Disc2Discs.Game GAME = new Disc2Discs.Game(LABEL, EXECUTABLE_NAME, EXECUTABLE_SHA256, TITLE_ID);

    /**
     * The install disc's files the game needs: four archives and four files of 14 bytes inside its four content
     * packages, named "PACKAGE/path inside the package" as Disc2Discs names them. Unmodifiable. They are not part of
     * items() or spec(): the setup page of a developer build does not read the install disc and stays as it was.
     */
    static List<Item> installItems() {
        return INSTALL_ITEMS;
    }

    /** What tells this release's two discs from others, for Disc2Discs.open. */
    static Disc2Discs.Game game() {
        return GAME;
    }

    static boolean isSha256(String text) {
        if (text == null || text.length() != 64) return false;
        for (int i = 0; i < 64; ++i) {
            char c = text.charAt(i);
            if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f')) return false;
        }
        return true;
    }

    private static void add(List<Item> list, String disc, String path, long bytes, String sha256) {
        list.add(new Item(disc, path, bytes, sha256));
    }

    private static List<Item> build() {
        List<Item> list = new ArrayList<Item>();
        add(list, PLAY, "default.xex", 26628096L, "f0ca93fbe6b3dfbf50c026e0d77a71e8294b13a5a775af1b6a64f07b0fc10a3d");
        add(list, PLAY, "common.rpf", 12916736L, "08e44da0c420f1c6686ea09faed83d085351cd56b973e1945554012521bcdeea");
        add(list, PLAY, "xbox360a.rpf", 711852032L, "5c7a633870485a4e79738fa6169e5a7937b39ba4474fb6f51a994c8dc6b8e525");
        add(list, PLAY, "xbox360b.rpf", 1431193600L, "9d15357ffb79a815020f2bdb66b0faee8ec0873a6499c927569f236bdda41cb1");
        add(list, PLAY, "common/data/installpartitions.meta", 1028L, "159902888e5406ce4fff59831a9ecf7b4d0816080f65ddd7789d2038091ffd16");
        add(list, PLAY, "audio_rel.rpf", 3108864L, "04f4fd93791723c5205b814290003c86fd23a99910edaeb58a338fff9af3cce9");
        add(list, PLAY, "sfx/audio_cs.rpf", 666355712L, "14558590d94ce5af23b416a79e298d47e335878a3d932639a9f4ff3d9175f7c7");
        add(list, PLAY, "sfx/audio_misc.rpf", 2762117120L, "9d8b825c57029213340730514f92355f71208c8451fb669e1e4a702bc27e36a3");
        add(list, PLAY, "sfx/audio_music.rpf", 1141999616L, "c541e611f63a34d71ea409c9882770901161462985c30acebaa828ea15626739");
        add(list, PLAY, "sfx/audio_radio.rpf", 1390331904L, "55e9a283545f813a1ec0896d757c716c524e0261aa72da27ad368c2b0726b8a9");
        return Collections.unmodifiableList(list);
    }

    private static List<Item> buildInstall() {
        List<Item> list = new ArrayList<Item>();
        add(list, INSTALL, "545408A700000000/part0.rpf", 2133954560L, "ae1c120717f93e397f7561798bc9e8e42ef9fa367e250bf12c774cd86ddb7d88");
        add(list, INSTALL, "545408A700000000/part0.timestamp", 14L, "96bc0b2719da62e933476c095a16ad54157f38753da465209c58a3802bfe52bd");
        add(list, INSTALL, "545408A700000001/part1.rpf", 2134566912L, "78c21d9a783186e0ae4a4828d9b72fa27f5f7938177079e73d1f039e4f6dbfd8");
        add(list, INSTALL, "545408A700000001/part1.timestamp", 14L, "20b768dd4d6a085fdc8002ebab2467df7f7ac6c6c4b5e16b0fa3337a9e381db1");
        add(list, INSTALL, "545408A700000002/part2.rpf", 2134116352L, "ad9b4679a03508dfeaf729478fb43de867d9f8756f4d4b56e100f20b4af9926e");
        add(list, INSTALL, "545408A700000002/part2.timestamp", 14L, "a27533bb1a9f15d9e273f24cef04b68484da96f9bf9d5e4b0b0ddd6f1989761c");
        add(list, INSTALL, "545408A700000003/part3.rpf", 1806764032L, "1566241deb22e940e0cf7a0408af64f725f9ab82ea008137587f31403bbafb38");
        add(list, INSTALL, "545408A700000003/part3.timestamp", 14L, "ab18d5eaa3bd6d85fd563cabda550f11c5c6b59842ec67336a9b6ecf4fffe874");
        return Collections.unmodifiableList(list);
    }
}
