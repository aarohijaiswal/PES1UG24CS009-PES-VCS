// index.c — Staging area implementation

#include "index.h"
#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

#ifndef HASH_HEX_LEN
#define HASH_HEX_LEN (HASH_SIZE * 2)
#endif

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

// ─── IMPLEMENTATION ──────────────────────────────────────────────────────────

// Load index from file
int index_load(Index *index) {
    index->count = 0;

    FILE *f = fopen(".pes/index", "r");
    if (!f) return 0;  // no index yet → empty index

    char hash_hex[HASH_HEX_LEN + 1];

    while (index->count < MAX_INDEX_ENTRIES) {
        IndexEntry *e = &index->entries[index->count];

        if (fscanf(f, "%o %64s %ld %u %[^\n]\n",
                   &e->mode, hash_hex, &e->mtime_sec,
                   &e->size, e->path) != 5)
            break;

        hex_to_hash(hash_hex, &e->hash);
        index->count++;
    }

    fclose(f);
    return 0;
}

// Compare function for sorting
static int compare_paths(const void *a, const void *b) {
    return strcmp(((IndexEntry *)a)->path,
                  ((IndexEntry *)b)->path);
}

// Save index atomically
int index_save(const Index *index) {
    char temp_path[] = ".pes/index.tmpXXXXXX";
    int fd = mkstemp(temp_path);
    if (fd < 0) return -1;

    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        return -1;
    }

    // Sort entries
    Index sorted = *index;
    qsort(sorted.entries, sorted.count,
          sizeof(IndexEntry), compare_paths);

    // Write entries
    for (int i = 0; i < sorted.count; i++) {
        const IndexEntry *e = &sorted.entries[i];
        char hash_hex[HASH_HEX_LEN + 1];
        hash_to_hex(&e->hash, hash_hex);

        fprintf(f, "%o %s %ld %u %s\n",
                e->mode, hash_hex,
                (long)e->mtime_sec,
                e->size, e->path);
    }

    fflush(f);
    fsync(fd);
    fclose(f);

    // Atomic replace
    if (rename(temp_path, ".pes/index") != 0) {
        unlink(temp_path);
        return -1;
    }

    return 0;
}

// Add file to index
int index_add(Index *index, const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    void *data = NULL;
    if (st.st_size > 0) {
        data = malloc(st.st_size);
        fread(data, 1, st.st_size, f);
    }
    fclose(f);

    ObjectID id;
    if (object_write(OBJ_BLOB, data, st.st_size, &id) != 0) {
        free(data);
        return -1;
    }

    free(data);

    // Check if already exists
    IndexEntry *e = index_find(index, path);
    if (!e) {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        e = &index->entries[index->count++];
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';
    }

    // Update metadata
    e->mode = st.st_mode;
    e->mtime_sec = (int64_t)st.st_mtime;
    e->size = (uint32_t)st.st_size;
    memcpy(e->hash.hash, id.hash, HASH_SIZE);

    return index_save(index);
}
