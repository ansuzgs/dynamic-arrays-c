/* ============================================================================
 * Post 1: "Hello, Array: malloc, free, and Manual Bookkeeping"
 * ============================================================================
 *
 * This file is entirely self-contained. Compile with:
 *   gcc -Wall -Wextra -Wpedantic -std=c11 -o build/post_01 src_posts/post_01.c
 *
 * Run:
 *   ./build/post_01                       (ASCII visualization to stdout)
 *   ./build/post_01 > output/post_01.txt (save ASCII output)
 *
 * The program also writes output/post_01_state.dot (Graphviz).
 * Render with:  dot -Tsvg output/post_01_state.dot -o output/post_01_state.svg
 *
 * Learning outcome: understand malloc/free lifecycle, pointer ownership,
 * and the struct-based bookkeeping that every dynamic array needs.
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * 1. The struct: what the array *knows* about itself
 * ---------------------------------------------------------------------------
 * Three fields is the minimum viable bookkeeping:
 *   - data      → pointer to the heap allocation that holds elements
 *   - size      → how many elements the user has actually stored
 *   - capacity  → how many elements the allocation can hold
 *
 * We store the struct itself on the heap too (via malloc), so the caller
 * only needs to carry around a single pointer.
 * --------------------------------------------------------------------------- */

typedef struct {
    int    *data;       /* Heap buffer that holds the elements            */
    size_t  size;       /* Number of elements currently stored            */
    size_t  capacity;   /* Number of elements the buffer can hold         */
} IntArray;

/* ---------------------------------------------------------------------------
 * 2. Lifecycle: create and destroy
 * --------------------------------------------------------------------------- */

/*
 * array_create — allocate metadata struct + element buffer.
 *
 * Two allocations happen here:
 *   1) malloc(sizeof(IntArray))                → the bookkeeping struct
 *   2) malloc(capacity * sizeof(int))          → the element buffer
 *
 * If either fails, we clean up and return NULL.
 * The caller owns the returned pointer and must call array_destroy().
 */
IntArray *array_create(size_t capacity)
{
    if (capacity == 0) {
        fprintf(stderr, "array_create: capacity must be > 0\n");
        return NULL;
    }

    IntArray *arr = malloc(sizeof(IntArray));
    if (!arr) {
        fprintf(stderr, "array_create: failed to allocate struct (%zu bytes)\n",
                sizeof(IntArray));
        return NULL;
    }

    arr->data = malloc(capacity * sizeof(int));
    if (!arr->data) {
        fprintf(stderr, "array_create: failed to allocate buffer (%zu bytes)\n",
                capacity * sizeof(int));
        free(arr);          /* Don't leak the struct if the buffer fails */
        return NULL;
    }

    arr->size     = 0;
    arr->capacity = capacity;

    return arr;
}

/*
 * array_destroy — free element buffer, then free struct.
 *
 * Order matters: free the *inner* allocation first, then the *outer* one.
 * Reversing the order would dereference freed memory (arr->data after
 * free(arr)).
 */
void array_destroy(IntArray *arr)
{
    if (!arr) return;

    free(arr->data);    /* 1. free the element buffer                    */
    arr->data = NULL;   /*    (defensive: prevent use-after-free)        */
    free(arr);          /* 2. free the struct itself                     */
}

/* ---------------------------------------------------------------------------
 * 3. Operations: push, get, size
 *
 * No growth here — if the buffer is full, push fails.  Post 2 adds realloc.
 * --------------------------------------------------------------------------- */

/*
 * array_push — append a value after the last element.
 *
 * Returns  0 on success, -1 on failure (full or NULL).
 */
int array_push(IntArray *arr, int value)
{
    if (!arr) {
        fprintf(stderr, "array_push: NULL array\n");
        return -1;
    }
    if (arr->size >= arr->capacity) {
        fprintf(stderr,
                "array_push: full (size=%zu, capacity=%zu). "
                "Cannot add %d.\n",
                arr->size, arr->capacity, value);
        return -1;
    }

    arr->data[arr->size] = value;
    arr->size++;
    return 0;
}

/*
 * array_get — read the element at `index`.
 *
 * Returns the value via `out`.  Returns -1 if out of bounds.
 */
int array_get(const IntArray *arr, size_t index, int *out)
{
    if (!arr || index >= arr->size) return -1;
    *out = arr->data[index];
    return 0;
}

/*
 * array_size / array_capacity — simple accessors.
 */
size_t array_size(const IntArray *arr)     { return arr ? arr->size     : 0; }
size_t array_capacity(const IntArray *arr) { return arr ? arr->capacity : 0; }

/* ---------------------------------------------------------------------------
 * 4. Visualization: ASCII
 *
 * Prints a box-drawing diagram of the heap buffer.
 *   - Occupied slots show their integer value.
 *   - Free slots show a centered dot.
 * --------------------------------------------------------------------------- */

void array_visualize_ascii(const IntArray *arr, const char *label)
{
    if (!arr) {
        printf("(NULL array)\n");
        return;
    }

    size_t cap = arr->capacity;
    size_t sz  = arr->size;

    /* ── Header ────────────────────────────────────────────────── */
    printf("\n");
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║  %-50s  ║\n", label ? label : "ARRAY STATE");
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║  size = %-5zu  capacity = %-5zu  elem = %zu bytes    ║\n",
           sz, cap, sizeof(int));
    printf("║  data = %-14p  (heap)                   ║\n",
           (void *)arr->data);
    printf("╠══════════════════════════════════════════════════════╣\n");

    /* ── Memory boxes (capped at 16 columns for readability) ─── */
    size_t show = cap <= 16 ? cap : 16;

    /* Top border */
    printf("║  ");
    for (size_t i = 0; i < show; i++) printf("┌──────");
    printf("┐  ║\n");

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
    printf("  ║\n");

    /* Bottom border */
    printf("║  ");
    for (size_t i = 0; i < show; i++) printf("└──────");
    printf("┘  ║\n");

    /* Index labels */
    printf("║  ");
    for (size_t i = 0; i < show; i++) printf(" %3zu   ", i);
    printf("   ║\n");

    /* ── Markers ───────────────────────────────────────────────── */
    if (sz > 0 && sz <= show) {
        printf("║  ");
        for (size_t i = 0; i < sz - 1; i++) printf("       ");
        printf("   ▲ size=%zu", sz);
        size_t pad = show > sz ? (show - sz) * 7 : 0;
        for (size_t i = 0; i < pad; i++) printf(" ");
        printf("         ║\n");
    }

    /* ── Stats ─────────────────────────────────────────────────── */
    size_t used_bytes  = sz  * sizeof(int);
    size_t alloc_bytes = cap * sizeof(int);
    size_t waste_bytes = alloc_bytes - used_bytes;
    double utilization = cap > 0 ? 100.0 * (double)sz / (double)cap : 0.0;
    double waste_pct   = 100.0 - utilization;

    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║  %zuB used / %zuB allocated = %.1f%% utilization      ",
           used_bytes, alloc_bytes, utilization);
    printf("║\n");
    printf("║  %zuB wasted (%.1f%%)                                 ",
           waste_bytes, waste_pct);
    printf("║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");
}

/* ---------------------------------------------------------------------------
 * 5. Visualization: Graphviz DOT
 *
 * Writes a .dot file that shows:
 *   - The IntArray struct (metadata)
 *   - The heap buffer (element slots)
 *   - An edge from metadata.data to the buffer
 * --------------------------------------------------------------------------- */

void array_generate_dot(const IntArray *arr, const char *filename)
{
    if (!arr || !filename) return;

    FILE *f = fopen(filename, "w");
    if (!f) {
        fprintf(stderr, "array_generate_dot: cannot open %s\n", filename);
        return;
    }

    fprintf(f, "digraph IntArray {\n");
    fprintf(f, "  rankdir=LR;\n");
    fprintf(f, "  graph [fontname=\"Helvetica\", fontsize=12];\n");
    fprintf(f, "  node  [fontname=\"Helvetica\", fontsize=11];\n");
    fprintf(f, "  edge  [fontname=\"Helvetica\", fontsize=10];\n\n");

    /* Metadata node */
    fprintf(f, "  metadata [shape=record, style=filled, "
               "fillcolor=\"#FFF3CD\",\n");
    fprintf(f, "    label=\"{IntArray (struct)|"
               "data: %p|size: %zu|capacity: %zu}\"];\n\n",
            (void *)arr->data, arr->size, arr->capacity);

    /* Buffer node — show each slot */
    fprintf(f, "  buffer [shape=record, style=filled, "
               "fillcolor=\"#D1ECF1\",\n");
    fprintf(f, "    label=\"{Heap Buffer (%zu bytes)|{",
            arr->capacity * sizeof(int));

    for (size_t i = 0; i < arr->capacity; i++) {
        if (i > 0) fprintf(f, "|");
        if (i < arr->size)
            fprintf(f, "<e%zu> [%zu]=%d", i, i, arr->data[i]);
        else
            fprintf(f, "<e%zu> [%zu]=·", i, i);
    }
    fprintf(f, "}}\"];\n\n");

    /* Edge: metadata.data → buffer */
    fprintf(f, "  metadata -> buffer "
               "[label=\"  owns (heap)\", style=bold, color=\"#0C5460\"];\n");

    fprintf(f, "}\n");
    fclose(f);

    printf("DOT file written to %s\n", filename);
}

/* ---------------------------------------------------------------------------
 * 6. Main: demonstrate the full lifecycle
 * --------------------------------------------------------------------------- */

int main(void)
{
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Post 1: Hello, Array — malloc, free, and Bookkeeping\n");
    printf("═══════════════════════════════════════════════════════\n");

    /* ── Step 1: Create ──────────────────────────────────────── */
    printf("\n▸ Creating array with capacity = 5\n");
    IntArray *arr = array_create(5);
    if (!arr) {
        fprintf(stderr, "FATAL: could not create array\n");
        return 1;
    }
    array_visualize_ascii(arr, "After create (empty)");

    /* ── Step 2: Push elements one by one ────────────────────── */
    int values[] = {10, 20, 30, 40};
    for (int i = 0; i < 4; i++) {
        printf("▸ Pushing %d\n", values[i]);
        array_push(arr, values[i]);
        char label[64];
        snprintf(label, sizeof(label), "After push(%d)", values[i]);
        array_visualize_ascii(arr, label);
    }

    /* ── Step 3: Read back ───────────────────────────────────── */
    printf("▸ Reading elements back:\n  ");
    for (size_t i = 0; i < array_size(arr); i++) {
        int val;
        if (array_get(arr, i, &val) == 0)
            printf("arr[%zu] = %d  ", i, val);
    }
    printf("\n");

    /* ── Step 4: Fill to capacity ────────────────────────────── */
    printf("\n▸ Pushing 50 (last slot)\n");
    array_push(arr, 50);
    array_visualize_ascii(arr, "Full array (5/5)");

    /* ── Step 5: Attempt to exceed capacity ──────────────────── */
    printf("▸ Attempting to push 60 into a full array...\n");
    int rc = array_push(arr, 60);
    if (rc != 0) {
        printf("  ↳ Push rejected (returned %d). Array unchanged.\n", rc);
    }
    array_visualize_ascii(arr, "Still 5/5 — push was rejected");

    /* ── Step 6: Generate Graphviz ───────────────────────────── */
    printf("▸ Writing Graphviz DOT file\n");
    array_generate_dot(arr, "output/post_01_state.dot");

    /* ── Step 7: Destroy ─────────────────────────────────────── */
    printf("\n▸ Destroying array (free data, then free struct)\n");
    array_destroy(arr);
    printf("  ↳ Done. No leaks.\n");

    /* ── Bonus: demonstrate the leak scenario ────────────────── */
    printf("\n───────────────────────────────────────────────────\n");
    printf("  DANGER ZONE: what a leak looks like\n");
    printf("───────────────────────────────────────────────────\n");
    IntArray *leaky = array_create(3);
    if (leaky) {
        array_push(leaky, 99);
        printf("  leaky->data = %p  (4 bytes on heap)\n",
               (void *)leaky->data);
        printf("  If we call free(leaky) WITHOUT free(leaky->data):\n");
        printf("    → The 12-byte buffer at %p is leaked.\n",
               (void *)leaky->data);
        printf("    → valgrind would report 'definitely lost: 12 bytes'.\n");
        printf("  Correct order: free(data) first, free(struct) second.\n");

        /* Actually clean up properly so valgrind is happy */
        array_destroy(leaky);
    }

    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  End of Post 1.  Next: automatic growth with realloc.\n");
    printf("═══════════════════════════════════════════════════════\n");

    return 0;
}
