package dev.recompbootstrap.smoke;

import android.content.ContentResolver;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import java.io.EOFException;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/**
 * A DiscSource over a document the player chose with the system file chooser, and (folder()) a whole chosen
 * folder. Read-only: the document is opened with mode "r" and nothing is ever written to it. Thin Android glue;
 * the logic that uses it (Xdvdfs, DiscIdentifier, VerifiedExtractor) is pure Java and host tested.
 */
final class UriDiscSource implements DiscSource {
    private final ParcelFileDescriptor descriptor;
    private final FileInputStream stream;
    private final FileChannel channel;

    private UriDiscSource(ParcelFileDescriptor descriptor) {
        this.descriptor = descriptor;
        this.stream = new FileInputStream(descriptor.getFileDescriptor());
        this.channel = stream.getChannel();
    }

    static UriDiscSource open(ContentResolver resolver, Uri uri) throws IOException {
        ParcelFileDescriptor descriptor;
        try {
            descriptor = resolver.openFileDescriptor(uri, "r");
        } catch (RuntimeException e) {   // SecurityException, IllegalArgumentException from the provider
            throw new IOException("the file cannot be opened (" + e.getClass().getSimpleName() + ")");
        }
        if (descriptor == null) throw new FileNotFoundException("the file cannot be opened");
        return new UriDiscSource(descriptor);
    }

    @Override
    public long size() throws IOException {
        long size = descriptor.getStatSize();
        return size >= 0 ? size : channel.size();
    }

    @Override
    public void readFully(long position, byte[] buffer, int offset, int length) throws IOException {
        ByteBuffer target = ByteBuffer.wrap(buffer, offset, length);
        long at = position;
        while (target.hasRemaining()) {
            int n = channel.read(target, at);
            if (n < 0) throw new EOFException("read past the end of the file");
            at += n;
        }
    }

    @Override
    public void close() throws IOException {
        try {
            stream.close();
        } finally {
            descriptor.close();
        }
    }

    /** An extracted disc: the folder the player chose (a tree Uri from ACTION_OPEN_DOCUMENT_TREE). */
    static DiscIdentifier.FolderSource folder(final ContentResolver resolver, final Uri tree) {
        return new DiscIdentifier.FolderSource() {
            /** Per folder document: lower-case child name -> {document id, MIME type}. */
            private final Map<String, Map<String, String[]>> listings = new HashMap<String, Map<String, String[]>>();

            @Override
            public DiscSource open(String relativePath) throws IOException {
                String document = DocumentsContract.getTreeDocumentId(tree);
                String[] parts = relativePath.split("/");
                for (int i = 0; i < parts.length; ++i) {
                    if (parts[i].isEmpty()) continue;
                    String[] child = children(document).get(parts[i].toLowerCase(Locale.ROOT));
                    if (child == null) return null;
                    boolean directory = DocumentsContract.Document.MIME_TYPE_DIR.equals(child[1]);
                    if (i == parts.length - 1) {
                        if (directory) return null;
                        return UriDiscSource.open(resolver, DocumentsContract.buildDocumentUriUsingTree(tree, child[0]));
                    }
                    if (!directory) return null;
                    document = child[0];
                }
                return null;
            }

            private Map<String, String[]> children(String document) throws IOException {
                Map<String, String[]> known = listings.get(document);
                if (known != null) return known;
                Map<String, String[]> found = new HashMap<String, String[]>();
                Cursor cursor;
                try {
                    cursor = resolver.query(DocumentsContract.buildChildDocumentsUriUsingTree(tree, document),
                            new String[] {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                                    DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                                    DocumentsContract.Document.COLUMN_MIME_TYPE}, null, null, null);
                } catch (RuntimeException e) {
                    throw new IOException("the folder cannot be listed (" + e.getClass().getSimpleName() + ")");
                }
                if (cursor == null) throw new IOException("the folder cannot be listed");
                try {
                    while (cursor.moveToNext()) {
                        String name = cursor.getString(1);
                        if (name != null) found.put(name.toLowerCase(Locale.ROOT), new String[] {cursor.getString(0), cursor.getString(2)});
                    }
                } finally {
                    cursor.close();
                }
                listings.put(document, found);
                return found;
            }
        };
    }
}
