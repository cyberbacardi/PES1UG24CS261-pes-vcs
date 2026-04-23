// tree.c — Tree object serialization and construction

#include "tree.h"
#include "index.c"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);
int index_load(Index *index);

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ──────────────────────────────────────────────────────────

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1;

        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';

        ptr = null_byte + 1;

        if (ptr + HASH_SIZE > end) return -1; 
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296; 
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written;
        buffer[offset] = '\0';
        offset += 1;
        
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

static int build_tree_recursive(IndexEntry *entries, int count, const char *prefix, ObjectID *tree_id_out) {
    Tree tree;
    tree.count = 0;

    int i = 0;
    while (i < count && tree.count < MAX_TREE_ENTRIES) {
        const char *path = entries[i].path;
        
 
        size_t prefix_len = strlen(prefix);
        if (prefix_len > 0) {
            if (strncmp(path, prefix, prefix_len) != 0) {
                i++;
                continue;
            }
            path += prefix_len;
        }

 
        const char *slash = strchr(path, '/');
        
        if (slash == NULL) {
 
            TreeEntry *entry = &tree.entries[tree.count];
            entry->mode = entries[i].mode;
            entry->hash = entries[i].hash;
            
            size_t file_name_len = strlen(path);
            if (file_name_len >= sizeof(entry->name)) {
                return -1;
            }
            strncpy(entry->name, path, sizeof(entry->name) - 1);
            entry->name[sizeof(entry->name) - 1] = '\0';
            tree.count++;
            i++;
        } else {
 
            size_t dir_name_len = slash - path;
            char dir_name[256];
            if (dir_name_len >= sizeof(dir_name)) {
                return -1;
            }
            memcpy(dir_name, path, dir_name_len);
            dir_name[dir_name_len] = '\0';

             int already_added = 0;
            for (int j = 0; j < tree.count; j++) {
                if (strcmp(tree.entries[j].name, dir_name) == 0) {
                    already_added = 1;
                    break;
                }
            }

            if (already_added) {
                i++;
                continue;
            }

 
            char new_prefix[512];
            int prefix_result;
            if (prefix_len > 0) {
                prefix_result = snprintf(new_prefix, sizeof(new_prefix), "%s%s/", prefix, dir_name);
            } else {
                prefix_result = snprintf(new_prefix, sizeof(new_prefix), "%s/", dir_name);
            }
            
            if (prefix_result < 0 || (size_t)prefix_result >= sizeof(new_prefix)) {
                return -1;
            }

             ObjectID subtree_hash;
            if (build_tree_recursive(entries, count, new_prefix, &subtree_hash) != 0) {
                return -1;
            }

             TreeEntry *entry = &tree.entries[tree.count];
            entry->mode = MODE_DIR;
            entry->hash = subtree_hash;
            strncpy(entry->name, dir_name, dir_name_len);
            entry->name[dir_name_len] = '\0';
            tree.count++;
            i++;
        }
    }

 
    void *tree_data;
    size_t tree_len;
    if (tree_serialize(&tree, &tree_data, &tree_len) != 0) {
        return -1;
    }

    int result = object_write(OBJ_TREE, tree_data, tree_len, tree_id_out);
    free(tree_data);
    return result;
}

int tree_from_index(ObjectID *id_out) {
    Index index;
    if (index_load(&index) != 0) {
        return -1;
    }

    if (index.count == 0) {
        fprintf(stderr, "error: nothing to commit (index is empty)\n");
        return -1;
    }

    return build_tree_recursive(index.entries, index.count, "", id_out);
}
