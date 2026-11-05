package dev.recompbootstrap.smoke;

import java.io.File;
import java.nio.file.Files;
import java.util.List;

/**
 * A staging proof, not a unit test: reads a folder made by tools/ship_stage.py with the app's own classes and says
 * whether the app would accept its record and its start request on the device, without installing anything.
 *
 *   java -cp ... dev.recompbootstrap.smoke.ShipRecordCheck STAGE_DIR
 *
 * It asks exactly what ShipFirstRun asks before it writes: ShipConfig.parse of ship/config.json, the start
 * request's size and SHA-256 against the record, LaunchRequest.checkArguments for the record's run id under the
 * record's files root, and that every path the start request names is listed in the record. It reads every staged
 * game data piece (ship/pieces/...) and compares its size and SHA-256 with the record. It does not read the two
 * large pieces (ship_stage.py hashed them when it wrote the record). It prints the pinned files that are NOT
 * bundled: the list a disc import works from. Exit 0 only when everything is accepted.
 */
public final class ShipRecordCheck {
    public static void main(String[] args) throws Exception {
        if (args.length != 1) {
            System.out.println("usage: ShipRecordCheck STAGE_DIR");
            System.exit(2);
        }
        File ship = new File(args[0], "ship");
        ShipConfig config = ShipConfig.parse(Files.readAllBytes(new File(ship, "config.json").toPath()));
        System.out.println("record accepted: " + config.sha256 + "  build " + config.build + "  files root " + config.filesRoot);
        System.out.println("  game code " + config.library.bytes + " bytes " + config.library.sha256);
        System.out.println("  shaders   " + config.shaderPack.bytes + " bytes " + config.shaderPack.sha256 + " -> " + config.shaderPackName);
        System.out.println("  switches " + config.switches.size() + ", directories " + config.directories.size() + ", required inputs "
                + config.requiredInputs.size() + ", pinned data files " + config.pinnedData.size());
        byte[] request = Files.readAllBytes(new File(ship, "start-request.bin").toPath());
        boolean ok = request.length == config.startRequest.bytes && ShipFiles.sha256Hex(request).equals(config.startRequest.sha256);
        System.out.println("start request: " + request.length + " bytes, SHA-256 " + (ok ? "as recorded" : "NOT as recorded"));
        String root = config.filesRoot + "/private-trace";
        String refusal = LaunchRequest.checkArguments(request, root, config.runId);
        System.out.println("the game's own rule for run " + config.runId + ": " + (refusal == null ? "accepted" : "REFUSED: " + refusal));
        ok &= refusal == null;
        List<String> named = LaunchRequest.referencedInputs(request, config.filesRoot);
        int unlisted = 0;
        for (String path : named) {
            String relative = path.substring(root.length() + 1);
            if (!config.requiredInputs.contains(relative) && !config.directories.contains(relative)) {
                ++unlisted;
                System.out.println("  NOT LISTED in the record: " + relative);
            }
        }
        System.out.println("paths the start request names: " + named.size() + ", not listed in the record: " + unlisted);
        ok &= unlisted == 0;
        long bundledBytes = 0;
        int wrong = 0;
        for (ShipConfig.BundledData piece : config.bundledData) {
            bundledBytes += piece.bytes;
            File staged = new File(new File(args[0]), piece.asset);
            boolean same = staged.isFile() && staged.length() == piece.bytes && ShipFiles.sha256Hex(Files.readAllBytes(staged.toPath())).equals(piece.sha256);
            if (!same) {
                ++wrong;
                System.out.println("  NOT AS RECORDED: the staged piece " + piece.asset);
            }
        }
        System.out.println("bundled game data pieces: " + config.bundledData.size() + " files, " + bundledBytes
                + " bytes; staged files that are not as recorded: " + wrong);
        ok &= wrong == 0;
        long pinnedBytes = 0;
        for (ShipConfig.DataFile file : config.pinnedData) pinnedBytes += file.bytes;
        System.out.println("pinned game data: " + config.pinnedData.size() + " files, " + pinnedBytes + " bytes");
        System.out.println("pinned and NOT bundled (must come from the player's own discs): " + config.notBundled.size()
                + " files, " + config.notBundledBytes() + " bytes; free space a copy of them will ask for: "
                + ShipFiles.amount(config.notBundledBytes() + ShipDataImport.RESERVE_BYTES, true));
        for (ShipConfig.DataFile file : config.notBundled) {
            System.out.println("  " + file.sha256 + "  " + file.bytes + "  " + file.path);
        }
        System.out.println(ok ? "SHIP RECORD CHECK PASSED" : "SHIP RECORD CHECK FAILED");
        System.exit(ok ? 0 : 1);
    }
}
