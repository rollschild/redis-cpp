#ifndef HASHTABLE_H
#define HASHTABLE_H

#include "constants.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

struct HNode {
    HNode *next = NULL;
    uint64_t hcode = 0;
};

struct HTable {
    HNode **table = NULL;
    size_t mask = 0;
    size_t size = 0;
};

struct HMap {
    HTable ht_to;
    HTable ht_from;
    size_t resizing_pos = 0;
};

static void h_init(HTable *htable, size_t n) {
    assert(n > 0 && ((n - 1) & n) == 0); // make sure n is power of 2
    htable->table =
        (HNode **)calloc(sizeof(HNode *), n); // array of pointers to HNode
    htable->mask = n - 1;
    htable->size = 0;
}

/**
 * Insertion
 */
static void h_insert(HTable *htable, HNode *node) {
    size_t pos = node->hcode & htable->mask;
    HNode *next = htable->table[pos];
    node->next = next;
    htable->table[pos] = node;
    htable->size++;
}

static HNode **h_lookup(HTable *htable, HNode *key,
                        bool (*cmp)(HNode *, HNode *)) {
    if (!htable->table) {
        return NULL;
    }

    size_t pos = key->hcode & htable->mask;
    HNode **from = &htable->table[pos]; // list of the Nodes on this linked list
    while (*from) {
        if (cmp(*from, key)) {
            return from;
        }
        from = &(*from)->next;
    }

    return NULL;
}

static HNode *h_detach(HTable *htable, HNode **from) {
    HNode *node = *from;
    *from = (*from)->next;
    htable->size--;
    return node;
}

static void hm_help_resizing(HMap *hmap) {
    if (hmap->ht_from.table == NULL) {
        return;
    }

    size_t work_done = 0;
    while (work_done < K_RESIZING_WORK && hmap->ht_from.size > 0) {
        HNode **from = &hmap->ht_from.table[hmap->resizing_pos];
        if (!*from) {
            // bucket empty
            continue;
        }

        h_insert(&hmap->ht_to, h_detach(&hmap->ht_from, from));
        work_done++;
    }

    if (hmap->ht_from.size == 0) {
        // resizing finished
        free(hmap->ht_from.table);
        hmap->ht_from = HTable{};
    }
}

static HNode *hm_lookup(HMap *hmap, HNode *key, bool (*cmp)(HNode *, HNode *)) {
    hm_help_resizing(hmap);
    HNode **from = h_lookup(&hmap->ht_to, key, cmp);
    if (!from) {
        from = h_lookup(&hmap->ht_from, key, cmp);
    }
    return from ? *from : NULL;
}

static void hm_start_resizing(HMap *hmap) {
    assert(hmap->ht_from.table == NULL);
    // create a bigger hashtable and swap them
    hmap->ht_from = hmap->ht_to;
    h_init(&hmap->ht_to, (hmap->ht_to.mask + 1) * 2);
    hmap->resizing_pos = 0;
}

static void hm_insert(HMap *hmap, HNode *node) {
    if (!hmap->ht_to.table) {
        h_init(&hmap->ht_to, 4);
    }
    h_insert(&hmap->ht_to, node);

    if (!hmap->ht_from.table) {
        // why resizing here?
        size_t load_factor = hmap->ht_to.size / (hmap->ht_to.mask + 1);
        if (load_factor >= K_MAX_LOAD_FACTOR) {
            hm_start_resizing(hmap);
        }
    }
    hm_help_resizing(hmap);
}

static HNode *hm_pop(HMap *hmap, HNode *key, bool (*cmp)(HNode *, HNode *)) {
    hm_help_resizing(hmap);

    HNode **from = h_lookup(&hmap->ht_to, key, cmp);
    if (from) {
        return h_detach(&hmap->ht_to, from);
    }

    from = h_lookup(&hmap->ht_from, key, cmp);
    if (from) {
        return h_detach(&hmap->ht_from, from);
    }
    return NULL;
}

#endif /* HASHTABLE_H */
