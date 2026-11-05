/* windows: find byte runs shared between two sets of files.

   windows WIDTH MIN_DISTINCT INDEX_LIST STREAM_LIST [PRIMARY]

   Every WIDTH-byte window of the files named in INDEX_LIST (one path per line) is indexed, except windows with fewer
   than MIN_DISTINCT different byte values (padding, runs of spaces). Every offset of the files in STREAM_LIST is then
   hashed with the same rolling hash; a hash hit is compared byte for byte, extended forward to its full length and
   printed as

       RUN <stream file #> <stream offset> <index file #> <index offset> <length>

   Scanning resumes after the run, and an indexed window that already started a printed run is not printed again
   (text repeated thousands of times in the streamed files prints once). At most 100000 runs are printed; the last line is
   always "DONE runs=<n> truncated=<0|1> indexed=<windows> streamed=<bytes>".

   PRIMARY (default: all) is how many of the INDEX_LIST files are the files being checked; the rest are controls.
   A control match is printed the same way but never moves the scan past its run, so a control can never hide a
   match of a checked file that starts a byte later. Exit 0 when the scan finished, 2 on a
   usage or I/O error. Used by taint_scan.py and scan_secrets.py; it reads files and never writes any. */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define BASE 0x100000001b3ULL
#define MAX_RUNS 100000

typedef struct { const uint8_t *data; size_t size; } mapped;
typedef struct { uint64_t hash; uint32_t file; uint32_t pad; uint64_t offset; } slot;

static int map_file(const char *path, mapped *out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "windows: cannot open %s\n", path); return -1; }
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return -1; }
    out->size = (size_t)st.st_size;
    out->data = NULL;
    if (out->size) {
        void *p = mmap(NULL, out->size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) { close(fd); fprintf(stderr, "windows: cannot map %s\n", path); return -1; }
        out->data = p;
    }
    close(fd);
    return 0;
}

static int read_list(const char *path, mapped **files, size_t *count) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "windows: cannot open list %s\n", path); return -1; }
    char line[8192];
    size_t cap = 64;
    *files = malloc(cap * sizeof(mapped));
    *count = 0;
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n) continue;
        if (*count == cap) { cap *= 2; *files = realloc(*files, cap * sizeof(mapped)); }
        if (map_file(line, &(*files)[*count]) != 0) { fclose(f); return -1; }
        (*count)++;
    }
    fclose(f);
    return 0;
}

static int distinct_enough(const uint8_t *w, int width, int min_distinct) {
    if (min_distinct <= 1) return 1;
    uint8_t seen[256] = {0};
    int distinct = 0;
    for (int i = 0; i < width; i++)
        if (!seen[w[i]]++ && ++distinct >= min_distinct) return 1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 5 && argc != 6) { fprintf(stderr, "usage: windows WIDTH MIN_DISTINCT INDEX_LIST STREAM_LIST [PRIMARY]\n"); return 2; }
    int width = atoi(argv[1]), min_distinct = atoi(argv[2]);
    if (width < 8 || width > 4096) { fprintf(stderr, "windows: WIDTH must be 8..4096\n"); return 2; }
    mapped *index, *stream;
    size_t index_count, stream_count;
    if (read_list(argv[3], &index, &index_count) || read_list(argv[4], &stream, &stream_count)) return 2;
    size_t primary = argc == 6 ? (size_t)strtoull(argv[5], NULL, 10) : index_count;

    uint64_t power = 1; /* BASE^(width-1), for removing the byte leaving the window */
    for (int i = 1; i < width; i++) power *= BASE;

    size_t windows = 0;
    for (size_t f = 0; f < index_count; f++)
        if (index[f].size >= (size_t)width) windows += index[f].size - width + 1;
    size_t capacity = 1024;
    while (capacity < windows * 2 + 1) capacity <<= 1;
    slot *table = calloc(capacity, sizeof(slot));
    uint8_t *used = calloc(capacity, 1); /* 1: holds a window, 2: that window has started a printed run */
    /* A 2^28-bit filter (32 MB) answers most stream offsets without touching the table. */
    uint64_t *filter = calloc((1ULL << 28) / 64, sizeof(uint64_t));
    if (!table || !used || !filter) { fprintf(stderr, "windows: out of memory for %zu windows\n", windows); return 2; }

    size_t indexed = 0;
    for (size_t f = 0; f < index_count; f++) {
        const uint8_t *d = index[f].data;
        size_t n = index[f].size;
        if (n < (size_t)width) continue;
        uint64_t h = 0;
        for (int i = 0; i < width; i++) h = h * BASE + d[i];
        for (size_t off = 0;; off++) {
            if (distinct_enough(d + off, width, min_distinct)) {
                size_t s = (size_t)(h * 0x9E3779B97F4A7C15ULL >> 1) & (capacity - 1);
                while (used[s] && table[s].hash != h) s = (s + 1) & (capacity - 1);
                if (!used[s]) { used[s] = 1; table[s].hash = h; table[s].file = (uint32_t)f; table[s].offset = off; indexed++; }
                uint64_t bit = (h * 0xC2B2AE3D27D4EB4FULL) >> 36;
                filter[bit >> 6] |= 1ULL << (bit & 63);
            }
            if (off + width >= n) break;
            h = (h - d[off] * power) * BASE + d[off + width];
        }
    }

    size_t runs = 0, streamed = 0;
    int truncated = 0;
    for (size_t f = 0; f < stream_count && !truncated; f++) {
        const uint8_t *d = stream[f].data;
        size_t n = stream[f].size;
        streamed += n;
        if (n < (size_t)width) continue;
        uint64_t h = 0;
        for (int i = 0; i < width; i++) h = h * BASE + d[i];
        size_t off = 0;
        while (1) {
            size_t s = (size_t)(h * 0x9E3779B97F4A7C15ULL >> 1) & (capacity - 1);
            size_t skip_to = 0;
            uint64_t bit = (h * 0xC2B2AE3D27D4EB4FULL) >> 36;
            while ((filter[bit >> 6] >> (bit & 63) & 1) && used[s]) {
                if (table[s].hash == h) {
                    const mapped *m = &index[table[s].file];
                    size_t io = table[s].offset;
                    if (memcmp(m->data + io, d + off, width) == 0) {
                        size_t len = width;
                        while (off + len < n && io + len < m->size && d[off + len] == m->data[io + len]) len++;
                        if (used[s] == 1) {
                            used[s] = 2;
                            printf("RUN %zu %zu %u %zu %zu\n", f, off, table[s].file, io, len);
                            if (++runs >= MAX_RUNS) truncated = 1;
                        }
                        if (table[s].file < primary) skip_to = off + len;
                    }
                    break;
                }
                s = (s + 1) & (capacity - 1);
            }
            if (truncated) break;
            if (skip_to) {
                if (skip_to + width > n) break;
                off = skip_to;
                h = 0;
                for (int i = 0; i < width; i++) h = h * BASE + d[off + i];
                continue;
            }
            if (off + width >= n) break;
            h = (h - d[off] * power) * BASE + d[off + width];
            off++;
        }
    }
    printf("DONE runs=%zu truncated=%d indexed=%zu streamed=%zu\n", runs, truncated, indexed, streamed);
    return 0;
}
