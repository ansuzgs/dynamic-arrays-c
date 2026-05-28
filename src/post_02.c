/* ============================================================================
 * Post 2: "Growing Pains: realloc and Automatic Capacity Management"
 * ============================================================================
 *
 * This file is entirely self-contained. Compile with:
 *   gcc -Wall -Wextra -Wpedantic -std=c11 -o build/post_02 src_posts/post_02.c
 *
 * Run:
 *   ./build/post_02                       (ASCII visualization to stdout)
 *   ./build/post_02 > output/post_02.txt (save ASCII output)
 *
 * The program also writes output/post_02_realloc.dot (Graphviz).
 * Render with:  dot -Tsvg output/post_02_realloc.dot -o output/post_02_realloc.svg
 *
 * Learning outcome: understand realloc semantics, automatic capacity growth,
 * pointer invalidation after reallocation, and the "temporary pointer" pattern.
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * 1. The struct
 * ---------------------------------------------------------------------------
 * Same three fields as Post 1, plus a realloc counter so we can track how
 * many times the buffer was moved during a run.
 * --------------------------------------------------------------------------- */

typedef struct {
    int    *data;           /* Heap buffer holding the elements              */
    size_t  size;           /* Elements currently stored                     */
    size_t  capacity;       /* Slots allocated                               */
    size_t  realloc_count;  /* How many times we've reallocated (diagnostic) */
} IntArray;

/* ---------------------------------------------------------------------------
 * 2. Lifecycle: create and destroy
 * --------------------------------------------------------------------------- */

IntArray *array_create(size_t capacity)
{
    if (capacity == 0) {
        fprintf(stderr, "array_create: capacity must be > 0\n");
        return NULL;
    }

    IntArray *arr = malloc(sizeof(IntArray));
    if (!arr) {
        fprintf(stderr, "array_create: struct allocation failed\n");
        return NULL;
    }

    arr->data = malloc(capacity * sizeof(int));
    if (!arr->data) {
        fprintf(stderr, "array_create: buffer allocation failed\n");
        free(arr);
        return NULL;
    }

    arr->size          = 0;
    arr->capacity      = capacity;
    arr->realloc_count = 0;

    return arr;
}

void array_destroy(IntArray *arr)
{
    if (!arr) return;
    free(arr->data);
    arr->data = NULL;
    free(arr);
}

/* ---------------------------------------------------------------------------
 * 3. The star of this post: array_push with automatic growth
 * ---------------------------------------------------------------------------
 *
 * When size == capacity, we grow the buffer before inserting.
 *
 * Growth strategy: new_capacity = old_capacity * 2
 *   (Post 3 will debate 1.5x vs 2x vs additive — for now we keep it simple.)
 *
 * CRITICAL: we use a temporary pointer for realloc.
 *   int *tmp = realloc(arr->data, new_bytes);
 *   if (!tmp) return -1;     ← arr->data is STILL VALID here
 *   arr->data = tmp;         ← only update after success
 *
 * If we wrote arr->data = realloc(arr->data, ...) and realloc failed,
 * arr->data would be NULL, and the old buffer would be leaked.
 * --------------------------------------------------------------------------- */

int array_push(IntArray *arr, int value)
{
    if (!arr) {
        fprintf(stderr, "array_push: NULL array\n");
        return -1;
    }

    /* ── Do we need to grow? ──────────────────────────────────── */
    if (arr->size >= arr->capacity) {
        size_t old_cap = arr->capacity;
        size_t new_cap = old_cap * 2;

        printf("  [REALLOC] size=%zu hit capacity=%zu → growing to %zu\n",
               arr->size, old_cap, new_cap);
        printf("            old data pointer: %p\n", (void *)arr->data);

        /*
         * realloc() does one of two things:
         *   1. Extends the block in-place (returns same pointer, fast).
         *   2. Allocates a new block, copies the data, frees the old one
         *      (returns new pointer — old pointer is now INVALID).
         *
         * We cannot predict which will happen. We MUST use a temporary.
         */
        int *tmp = realloc(arr->data, new_cap * sizeof(int));
        if (!tmp) {
            fprintf(stderr, "  [REALLOC] FAILED — array unchanged\n");
            return -1;  /* arr->data still points to the original buffer */
        }

        int moved = (tmp != arr->data);
        arr->data     = tmp;
        arr->capacity = new_cap;
        arr->realloc_count++;

        printf("            new data pointer: %p (%s)\n",
               (void *)arr->data,
               moved ? "MOVED — old pointer is now INVALID"
                     : "extended in-place — same address");
    }

    /* ── Normal push (guaranteed to have room now) ────────────── */
    arr->data[arr->size] = value;
    arr->size++;
    return 0;
}

/* ---------------------------------------------------------------------------
 * 4. Accessors
 * --------------------------------------------------------------------------- */

int array_get(const IntArray *arr, size_t index, int *out)
{
    if (!arr || index >= arr->size) return -1;
    *out = arr->data[index];
    return 0;
}

size_t array_size(const IntArray *arr)     { return arr ? arr->size     : 0; }
size_t array_capacity(const IntArray *arr) { return arr ? arr->capacity : 0; }

/* ---------------------------------------------------------------------------
 * 5. Visualization: ASCII
 * ---------------------------------------------------------------------------
 * Enhanced from Post 1:
 *   - Shows the realloc counter
 *   - Shows "next realloc at" prediction
 * --------------------------------------------------------------------------- */

void array_visualize_ascii(const IntArray *arr, const char *label)
{
    if (!arr) { printf("(NULL array)\n"); return; }

    size_t cap = arr->capacity;
    size_t sz  = arr->size;

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  %-54s  ║\n", label ? label : "ARRAY STATE");
    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║  size = %-5zu  capacity = %-5zu  elem = %zu bytes        ║\n",
           sz, cap, sizeof(int));
    printf("║  data = %-14p  (heap)                       ║\n",
           (void *)arr->data);
    printf("║  reallocations so far: %-3zu                              ║\n",
           arr->realloc_count);
    printf("╠══════════════════════════════════════════════════════════╣\n");

    size_t show = cap <= 16 ? cap : 16;

    /* Top border */
    printf("║  ");
    for (size_t i = 0; i < show; i++) printf("┌──────");
    printf("┐ ║\n");

    /* Values */
    printf("║  ");
    for (size_t i = 0; i < show; i++) {
        if (i < sz)
            printf("│%5d ", arr->data[i]);
        else
            printf("│  ·   ");
    }
    if (cap > 16) printf("│…");
    else          printf("│");
    printf(" ║\n");

    /* Bottom border */
    printf("║  ");
    for (size_t i = 0; i < show; i++) printf("└──────");
    printf("┘ ║\n");

    /* Index labels */
    printf("║  ");
    for (size_t i = 0; i < show; i++) printf(" %3zu   ", i);
    printf("  ║\n");

    /* Stats */
    size_t used_bytes  = sz  * sizeof(int);
    size_t alloc_bytes = cap * sizeof(int);
    size_t waste_bytes = alloc_bytes - used_bytes;
    double util = cap > 0 ? 100.0 * (double)sz / (double)cap : 0.0;

    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║  %3zuB used / %3zuB alloc = %5.1f%% utilization            ║\n",
           used_bytes, alloc_bytes, util);
    printf("║  %3zuB wasted (%5.1f%%)     next realloc at size=%zu      ║\n",
           waste_bytes, 100.0 - util, cap);
    printf("╚══════════════════════════════════════════════════════════╝\n\n");
}

/* ---------------------------------------------------------------------------
 * 6. Visualization: Graphviz DOT (before/after realloc comparison)
 *
 * Generates a diagram showing old memory (freed, red) and new memory
 * (current, green) side by side, with a "realloc" arrow connecting them.
 * --------------------------------------------------------------------------- */

void array_generate_realloc_dot(
    const int *old_data_snapshot, size_t old_size, size_t old_cap,
    const IntArray *current,
    const char *filename)
{
    if (!current || !filename) return;

    FILE *f = fopen(filename, "w");
    if (!f) { fprintf(stderr, "Cannot open %s\n", filename); return; }

    fprintf(f, "digraph Realloc {\n");
    fprintf(f, "  rankdir=LR;\n");
    fprintf(f, "  graph [fontname=\"Helvetica\", fontsize=12, "
               "label=\"realloc: capacity %zu → %zu\", labelloc=t];\n",
            old_cap, current->capacity);
    fprintf(f, "  node  [fontname=\"Helvetica\", fontsize=11];\n");
    fprintf(f, "  edge  [fontname=\"Helvetica\", fontsize=10];\n\n");

    /* ── Old state (freed) ──────────────────────────────────────── */
    fprintf(f, "  subgraph cluster_old {\n");
    fprintf(f, "    label=\"Before (FREED)\"; style=dashed; "
               "color=\"#DC3545\"; fontcolor=\"#DC3545\";\n");
    fprintf(f, "    old_meta [shape=record, style=filled, "
               "fillcolor=\"#F8D7DA\",\n");
    fprintf(f, "      label=\"{IntArray|size: %zu|capacity: %zu}\"];\n",
            old_size, old_cap);
    fprintf(f, "    old_buf [shape=record, style=filled, "
               "fillcolor=\"#F8D7DA\",\n");
    fprintf(f, "      label=\"{");
    for (size_t i = 0; i < old_cap; i++) {
        if (i > 0) fprintf(f, "|");
        if (i < old_size)
            fprintf(f, "%d", old_data_snapshot[i]);
        else
            fprintf(f, "·");
    }
    fprintf(f, "}\"];\n");
    fprintf(f, "    old_meta -> old_buf;\n");
    fprintf(f, "  }\n\n");

    /* ── New state (current) ────────────────────────────────────── */
    fprintf(f, "  subgraph cluster_new {\n");
    fprintf(f, "    label=\"After (CURRENT)\"; style=solid; "
               "color=\"#28A745\"; fontcolor=\"#28A745\";\n");
    fprintf(f, "    new_meta [shape=record, style=filled, "
               "fillcolor=\"#D4EDDA\",\n");
    fprintf(f, "      label=\"{IntArray|size: %zu|capacity: %zu}\"];\n",
            current->size, current->capacity);
    fprintf(f, "    new_buf [shape=record, style=filled, "
               "fillcolor=\"#D4EDDA\",\n");
    fprintf(f, "      label=\"{");
    size_t show = current->capacity <= 16 ? current->capacity : 16;
    for (size_t i = 0; i < show; i++) {
        if (i > 0) fprintf(f, "|");
        if (i < current->size)
            fprintf(f, "%d", current->data[i]);
        else
            fprintf(f, "·");
    }
    if (current->capacity > 16) fprintf(f, "|...");
    fprintf(f, "}\"];\n");
    fprintf(f, "    new_meta -> new_buf;\n");
    fprintf(f, "  }\n\n");

    /* ── Realloc arrow ──────────────────────────────────────────── */
    fprintf(f, "  old_buf -> new_buf [style=dashed, color=\"#0D6EFD\", "
               "label=\"  memcpy %zuB\\n  + free old\", fontcolor=\"#0D6EFD\"];\n",
            old_size * sizeof(int));

    fprintf(f, "}\n");
    fclose(f);

    printf("  DOT file written to %s\n", filename);
}

/* ---------------------------------------------------------------------------
 * 7. Main: demonstrate growth in action
 * --------------------------------------------------------------------------- */

int main(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Post 2: Growing Pains — realloc and Capacity Management\n");
    printf("═══════════════════════════════════════════════════════════\n");

    /* ── Start small: capacity = 2 ───────────────────────────────
     * This guarantees we'll hit multiple reallocations quickly,
     * so the reader can see the growth pattern in action.
     * ──────────────────────────────────────────────────────────── */
    printf("\n▸ Creating array with capacity = 2 (intentionally small)\n");
    IntArray *arr = array_create(2);
    if (!arr) return 1;

    array_visualize_ascii(arr, "After create (empty, cap=2)");

    /* ── Push 12 elements, triggering multiple reallocations ───── */
    int old_data_snapshot[16];
    size_t old_size = 0, old_cap = 0;
    int captured_realloc = 0;

    for (int i = 1; i <= 12; i++) {
        int val = i * 10;
        printf("▸ Pushing %d\n", val);

        /* Snapshot state before push (to capture the realloc moment) */
        size_t pre_cap = arr->capacity;
        int   *pre_ptr = arr->data;

        /* Save a copy of data before first realloc for DOT diagram */
        if (!captured_realloc && arr->size >= arr->capacity) {
            old_size = arr->size;
            old_cap  = arr->capacity;
            size_t to_copy = old_size < 16 ? old_size : 16;
            memcpy(old_data_snapshot, arr->data, to_copy * sizeof(int));
        }

        array_push(arr, val);

        /* Detect if realloc happened */
        if (arr->capacity != pre_cap) {
            if (!captured_realloc) captured_realloc = 1;
            char label[80];
            snprintf(label, sizeof(label),
                     "After push(%d) — REALLOC: %zu → %zu",
                     val, pre_cap, arr->capacity);
            array_visualize_ascii(arr, label);

            /* Demonstrate pointer invalidation */
            if (arr->data != pre_ptr) {
                printf("  ⚠ POINTER INVALIDATED: %p → %p\n",
                       (void *)pre_ptr, (void *)arr->data);
                printf("    Any pointer/reference to the old buffer is now\n");
                printf("    a dangling pointer — using it is undefined behavior.\n\n");
            }
        } else {
            char label[64];
            snprintf(label, sizeof(label),
                     "After push(%d) — no realloc needed", val);
            array_visualize_ascii(arr, label);
        }
    }

    /* ── Summary: growth history ─────────────────────────────────
     * Starting from capacity=2, after 12 pushes with 2x growth:
     *   cap=2 → push 3 triggers → cap=4
     *   cap=4 → push 5 triggers → cap=8
     *   cap=8 → push 9 triggers → cap=16
     * That's 3 reallocations for 12 elements. Not bad.
     * ──────────────────────────────────────────────────────────── */
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Growth Summary\n");
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Final:  size=%zu, capacity=%zu\n",
           array_size(arr), array_capacity(arr));
    printf("  Reallocations: %zu\n", arr->realloc_count);
    printf("  Utilization:   %.1f%%\n",
           100.0 * (double)array_size(arr) / (double)array_capacity(arr));
    printf("═══════════════════════════════════════════════════════════\n");

    /* ── Generate DOT for the first realloc ──────────────────── */
    printf("\n▸ Generating Graphviz DOT (before/after first realloc)\n");
    array_generate_realloc_dot(old_data_snapshot, old_size, old_cap,
                               arr, "output/post_02_realloc.dot");

    /* ── The Dangerous Pattern ───────────────────────────────────
     * Demonstrate WHY you must use a temporary pointer.
     * This section is instructional — we never actually do the
     * dangerous thing, but we show what the wrong code looks like.
     * ──────────────────────────────────────────────────────────── */
    printf("\n───────────────────────────────────────────────────────\n");
    printf("  THE DANGEROUS PATTERN (do NOT do this)\n");
    printf("───────────────────────────────────────────────────────\n");
    printf("\n  WRONG:\n");
    printf("    arr->data = realloc(arr->data, new_size);\n");
    printf("    // If realloc fails, arr->data is now NULL.\n");
    printf("    // The old buffer is leaked — no pointer to it exists.\n");
    printf("\n  RIGHT:\n");
    printf("    int *tmp = realloc(arr->data, new_size);\n");
    printf("    if (!tmp) return -1;  // arr->data is still valid!\n");
    printf("    arr->data = tmp;      // only update on success\n");
    printf("───────────────────────────────────────────────────────\n");

    /* ── Pointer Invalidation Demo ──────────────────────────── */
    printf("\n───────────────────────────────────────────────────────\n");
    printf("  POINTER INVALIDATION DEMO\n");
    printf("───────────────────────────────────────────────────────\n");

    IntArray *demo = array_create(2);
    if (demo) {
        int x = 100, y = 200;
        array_push(demo, x);
        array_push(demo, y);

        /* Take a pointer into the buffer */
        int *dangerous_ptr = &demo->data[0];
        printf("\n  Before realloc:\n");
        printf("    demo->data     = %p\n", (void *)demo->data);
        printf("    dangerous_ptr  = %p  (points to data[0] = %d)\n",
               (void *)dangerous_ptr, *dangerous_ptr);

        /* This push triggers realloc (capacity 2 → 4) */
        int z = 300;
        array_push(demo, z);

        printf("\n  After realloc (push triggered growth 2 → %zu):\n",
               demo->capacity);
        printf("    demo->data     = %p  (%s)\n",
               (void *)demo->data,
               (demo->data != dangerous_ptr) ? "CHANGED" : "same");
        printf("    dangerous_ptr  = %p  (STALE — may point to freed memory)\n",
               (void *)dangerous_ptr);

        if (demo->data != dangerous_ptr) {
            printf("\n  ⚠ dangerous_ptr still holds the OLD address.\n");
            printf("    Dereferencing it is UNDEFINED BEHAVIOR.\n");
            printf("    The old buffer was freed by realloc.\n");
        }

        array_destroy(demo);
    }

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("  End of Post 2.  Next: the growth factor debate (1.5x vs 2x).\n");
    printf("═══════════════════════════════════════════════════════════\n");

    /* ── Cleanup ─────────────────────────────────────────────── */
    array_destroy(arr);

    return 0;
}
