// Less trivial implementation of crit-bit trees.
// Uses tagged pointers, and bit twiddling tricks.
// See http://www.imperialviolet.org/binary/critbit.pdf
//
// Differences:
//   - We fold strcmp into the crit-bit finder. If there is no crit-bit
//     then we have a match.
//   - During lookup, if the key is shorter than the position of the
//     crit bit in the current node, the path we take is irrelevant.
//     Ideally, we'd take one of the shortest paths to a leaf (the only
//     purpose is to get at a string so we can find the true crit bit),
//     but for simplicity we always follow the left child.
//     Our code skips a tiny bit of computation by assigning
//     direction = 0 rather than c = 0 plus some bit twiddling.
//   - Insertion: while walking down the tree (after we've figured out the crit
//     bit), we're guaranteed that the byte number of the current node is less
//     than the key length, so there's no need for special-case code to handle
//     keys shorter than the crit bit.
//   - We combine a couple of comparisons. Instead of byte0 < byte1 and then
//     mask0 < mask1 if they are equal, we simplify to:
//       (byte0 << 8) + mask0 < (byte1 << 8) + mask1
//   - Deletion: we can return early if the key length is shorter than
//     the current node's critical bit, as this implies the key is absent.
//   - When following child pointers, rather than the more elegant
//     kid[predicate()] where predicate() returns 0 or 1, we sometimes prefer
//     predicate() ? kid[1] : *kid (much like my old library's explicit left
//     and right child pointers), as this seems to produce slightly faster
//     code.

#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "blt.h"

static inline int has_tag(void *p) { return 1 & (intptr_t)p; }
static inline void *untag(void *p) { return ((char *)p) - 1; }
// Returns the byte where each bit is 1 except for the bit corresponding to
// the leading bit of x.
// Storing the crit bit in this mask form simplifies decide().
static inline uint8_t to_mask(uint8_t x) {
  // SWAR trick that sets every bit after the leading bit to 1.
  x |= x >> 1;
  x |= x >> 2;
  x |= x >> 4;
  // Zero all the bits after the leading bit then invert.
  return (x & ~(x >> 1)) ^ 255;
  if (0) {
    // Alternative that performs better when there are few set bits.
    // Zero all bits except leading bit with a bit-twiddling trick.
    while (x&(x-1)) x &= x-1;
    // Invert.
    return 255 - x;
  }
}
static inline int decide(uint8_t c, uint8_t m) { return (1 + (m | c)) >> 8; }

// An internal node. Leaf nodes are described by BLT_IT.
struct blt_node_s {
  // Tagged pointer:
  //   LSB = 0 for leaf node,
  //   LSB = 1 for internal node. 
  void *kid[2];
  unsigned int mask:8;   // ~mask = the crit bit within the byte.
  unsigned int byte:24;  // Byte # of difference.
};
typedef struct blt_node_s *blt_node_ptr;
// The blt_node_ptr pointers are tagged: LSB = 1 means internal node,
// LSB = 0 means external.

struct BLT {
  void *root;
};

BLT *blt_new() {
  BLT *blt = malloc(sizeof(*blt));
  blt->root = 0;
  return blt;
}

void blt_clear(BLT *blt) {
  void free_node(void *p) {
    if (!has_tag(p)) {
      free(((BLT_IT *) p)->key);
      free(p);
      return;
    }
    void *q = untag(p);
    void **kid = ((blt_node_ptr)q)->kid;
    free_node(kid[0]);
    free_node(kid[1]);
    free(q);
  }
  if (blt->root) free_node(blt->root);
  free(blt);
}

size_t blt_overhead(BLT *blt) {
  size_t n = sizeof(BLT);
  if (!blt->root) return n;
  void add(void *p) {
    if (has_tag(p)) {
      n += sizeof(struct blt_node_s);
      void **kid = ((blt_node_ptr)untag(p))->kid;
      add(kid[0]);
      add(kid[1]);
    } else {
      n += sizeof(BLT_IT);
    }
  }
  add(blt->root);
  return n;
}

BLT_IT *blt_firstlast(void *p, int dir) {
  if (!p) return NULL;
  while (has_tag(p)) p = ((blt_node_ptr)untag(p))->kid[dir];
  return p;
}

BLT_IT *blt_first(BLT *blt) { return blt_firstlast(blt->root, 0); }
BLT_IT *blt_last (BLT *blt) { return blt_firstlast(blt->root, 1); }

// 'it' must be an element of 'blt'.
BLT_IT *blt_nextprev(BLT *blt, BLT_IT *it, int way) {
  char *p = blt->root;
  void *other = 0;
  while (has_tag(p)) {
    blt_node_ptr q = untag(p);
    int dir = decide(q->mask, it->key[q->byte]);
    if (dir == way) other = q->kid[1 - way];
    p = q->kid[dir];
  }
  assert(!strcmp(((BLT_IT *)p)->key, it->key));
  return blt_firstlast(other, way);
}

BLT_IT *blt_next(BLT *blt, BLT_IT *it) { return blt_nextprev(blt, it, 0); }
BLT_IT *blt_prev(BLT *blt, BLT_IT *it) { return blt_nextprev(blt, it, 1); }

// Walk down the tree as if the key is there.
static inline BLT_IT *confident_get(BLT *blt, char *key) {
  char *p = blt->root;
  if (!p) return NULL;
  int keylen = strlen(key);
  while (has_tag(p)) {
    blt_node_ptr q = untag(p);
    // When q->byte >= keylen, key is absent, but we must return something.
    // Either kid works; we pick 0 each time.
    p = q->byte < keylen && decide(key[q->byte], q->mask) ? q->kid[1] : *q->kid;
  }
  return (void *)p;
}

BLT_IT *blt_ceilfloor(BLT *blt, char *key, int way) {
  BLT_IT *p = confident_get(blt, key);
  if (!p) return 0;
  // Compare keys.
  for(char *c = key, *pc = p->key;; c++, pc++) {
    // XOR the current bytes being compared.
    uint8_t x = *c ^ *pc;
    if (x) {
      int byte = c - key;
      x = to_mask(x);
      int ndir = decide(x, key[byte]);
      void *other = 0;
      // Walk down the tree until we hit an external node or a node
      // whose crit bit is higher.
      void **p0 = &blt->root;
      for (;;) {
        char *p = *p0;
        if (!has_tag(p)) break;
        blt_node_ptr q = untag(p);
        if ((byte << 8) + x < (q->byte << 8) + q->mask) break;
        int dir = decide(q->mask, key[q->byte]);
        if (dir == way) other = q->kid[1 - way];
        p0 = q->kid + dir;
      }
      if (ndir == way) other = *p0;
      return blt_firstlast(other, way);
    }
    if (!*c) return (BLT_IT *)p;
  }
}

BLT_IT *blt_ceil (BLT *blt, char *key) { return blt_ceilfloor(blt, key, 0); }
BLT_IT *blt_floor(BLT *blt, char *key) { return blt_ceilfloor(blt, key, 1); }

void *blt_put_with(BLT *blt, char *key, void *data,
                   void *(*already_present_cb)(BLT_IT *)) {
  BLT_IT *p = confident_get(blt, key);
  if (!p) {  // Empty tree case.
    p = malloc(sizeof(*p));
    blt->root = p;
    p->key = strdup(key);
    p->data = data;
    return 0;
  }
  // Compare keys.
  for(char *c = key, *pc = p->key;; c++, pc++) {
    // XOR the current bytes being compared.
    uint8_t x = *c ^ *pc;
    if (x) {
      // Allocate a new node.
      // Find crit bit using bit twiddling tricks.
      blt_node_ptr n = malloc(sizeof(*n));
      n->byte = c - key;
      n->mask = to_mask(x);
      BLT_IT *leaf = malloc(sizeof(*leaf));
      int ndir = decide(key[n->byte], n->mask);
      n->kid[ndir] = leaf;
      leaf->key = strdup(key);
      leaf->data = data;

      // Insert the new node.
      // Walk down the tree until we hit an external node or a node
      // whose crit bit is higher.
      void **p0 = &blt->root;
      for (;;) {
        char *p = *p0;
        if (!has_tag(p)) break;
        blt_node_ptr q = untag(p);
        if ((n->byte << 8) + n->mask < (q->byte << 8) + q->mask) break;
        p0 = q->kid + decide(key[q->byte], q->mask);
      }
      n->kid[1 - ndir] = *p0;
      *p0 = 1 + (char *)n;
      return 0;
    }
    if (!*c) return already_present_cb(p);
  }
}

void *blt_put(BLT *blt, char *key, void *data) {
  void *f(BLT_IT *it) {
    void *orig = it->data;
    it->data = data;
    return orig;
  }
  return blt_put_with(blt, key, data, f);
}

int blt_put_if_absent(BLT *blt, char *key, void *data) {
  void *f(BLT_IT *it) { return (void *) 1; }
  return blt_put_with(blt, key, data, f) != 0;
}

int blt_delete(BLT *blt, char *key) {
  char *p = blt->root;
  if (!p) return 0;
  int keylen = strlen(key);
  void **p0 = &blt->root, **q0 = 0;
  blt_node_ptr q;
  int dir;
  while (has_tag(p)) {
    q = untag(p);
    if (q->byte > keylen) return 0;
    dir = decide(q->mask, key[q->byte]);
    q0 = p0;
    p0 = q->kid + dir;
    p = *p0;
  }
  BLT_IT *leaf = (BLT_IT *)p;
  if (strcmp(key, leaf->key)) return 0;
  free(leaf->key);
  free(leaf);
  if (!q0) {
    blt->root = 0;
    return 1;
  }
  *q0 = q->kid[1 - dir];
  free(q);
  return 1;
}

void blt_dump(BLT* blt, void *p) {
  if (!p) return;
  if (has_tag(p)) {
    blt_node_ptr q = untag(p);
    blt_dump(blt, q->kid[0]);
    blt_dump(blt, q->kid[1]);
    return;
  }
  printf("  %s\n", (char *) ((BLT_IT *) p)->key);
}

int blt_allprefixed(BLT *blt, char *key, int (*fun)(BLT_IT *)) {
  char *p = blt->root;
  if (!p) return 1;
  char *top = p;
  int keylen = strlen(key);
  while (has_tag(p)) {
    blt_node_ptr q = untag(p);
    if (q->byte >= keylen) {
      p = q->kid[0];
    } else {
      p = decide(q->mask, key[q->byte]) ? q->kid[1] : *q->kid;
      top = p;
    }
  }
  if (strncmp(key, ((BLT_IT *)p)->key, keylen)) return 1;
  int traverse(char *p) {
    if (has_tag(p)) {
      blt_node_ptr q = untag(p);
      for (int dir = 0; dir < 2; dir++) {
        int status = traverse(q->kid[dir]);
        switch(status) {
        case 1: break;
        case 0: return 0;
        default: return status;
        }
      }
      return 1;
    }
    return fun((BLT_IT *)p);
  }
  return traverse(top);
}

BLT_IT *blt_get(BLT *blt, char *key) {
  char *p = blt->root;
  if (!p) return NULL;
  int keylen = strlen(key);
  while (has_tag(p)) {
    blt_node_ptr q = untag(p);
    if (q->byte > keylen) return NULL;
    p = decide(key[q->byte], q->mask) ? q->kid[1] : *q->kid;
  }
  BLT_IT *r = (BLT_IT *)p;
  return strcmp(key, r->key) ? NULL : r;
}
