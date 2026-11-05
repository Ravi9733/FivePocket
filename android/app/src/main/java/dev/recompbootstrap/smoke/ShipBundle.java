package dev.recompbootstrap.smoke;

import java.io.Closeable;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * The pieces that travel inside the APK (its assets folder "ship"). Pure Java: the APK is read as the ZIP file it
 * is, by its path, so the same code runs on the device and in a host check against a built APK.
 */
interface ShipBundle extends Closeable {
    /**
     * The bytes of one asset, for example "ship/config.json". FileNotFoundException when the APK has no such
     * asset. The caller closes the stream.
     */
    InputStream open(String asset) throws IOException;

    /** The bundle of an APK file (on the device: the path Android gives as the app's source). */
    final class Apk implements ShipBundle {
        private final ZipFile zip;

        Apk(String apkPath) throws IOException {
            zip = new ZipFile(apkPath);
        }

        @Override public InputStream open(String asset) throws IOException {
            ZipEntry entry = zip.getEntry("assets/" + asset);
            if (entry == null || entry.isDirectory()) throw new FileNotFoundException(asset);
            return zip.getInputStream(entry);
        }

        @Override public void close() throws IOException {
            zip.close();
        }
    }
}
