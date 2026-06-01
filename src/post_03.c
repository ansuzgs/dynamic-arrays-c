/* ============================================================================
 * Post 3: "The Growth Factor Debate: 1.5x, 2x, or Something Else?"
 * ============================================================================
 *
 * This file is entirely self-contained. Compile with:
 *   gcc -Wall -Wextra -Wpedantic -std=c11 -o build/post_03 src_posts/post_03.c
 *
 * Run:
 *   ./build/post_03                       (ASCII visualization to stdout)
 *   ./build/post_03 > output/post_03.txt (save ASCII output)
 *
 * The program also writes output/post_03_timeline.dot (Graphviz).
 * Render with:  dot -Tsvg output/post_03_timeline.dot -o output/post_03_timeline.svg
 *
 * Learning outcome: understand amortized analysis of push, calculate memory
 * waste under different growth factors, explain why 1.5x allows memory reuse
 * and 2x doesn't, and choose a factor for a given use case.
 * ============================================================================
 */

#define _POSIX_C_SOURCE 199309L   /* Required for clock_gettime / CLOCK_MONOTONIC */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------------------------------------------------------------------
 * 1. The struct
 * ---------------------------------------------------------------------------
 * Same as Post 2, but we add a `growth_factor` field so each array instance
 * can grow at its own rate. This lets us run side-by-side comparisons.
 *
 * We also track `total_bytes_copied` — the cumulative number of bytes moved
 * across all reallocations. This is the real cost of growth.
 * --------------------------------------------------------------------------- */

typedef struct {
    int    *data;               /* Heap buffer holding the elements          */
    size_t  size;               /* Elements currently stored                 */
    size_t  capacity;           /* Slots allocated                           */
    size_t  realloc_count;      /* How many reallocations have occurred      */
    size_t  total_bytes_copied; /* Cumulative bytes moved by realloc/memcpy  */
    double  growth_factor;      /* Multiplicative factor (e.g. 1.5, 2.0)    */
} IntArray;

/* ---------------------------------------------------------------------------
 * 2. Allocation history tracking
 * ---------------------------------------------------------------------------
 * For the Graphviz timeline, we record every allocation and deallocation.
 * Each entry stores the capacity at that moment and whether it was a
 * "birth" (new allocation) or "death" (freed by realloc).
 * --------------------------------------------------------------------------- */

#define MAX_HISTORY 128

typedef struct {
    size_t capacity;       /* Capacity at this event                       */
    size_t cumulative_sum; /* Sum of all freed capacities so far           */
    int    is_freed;       /* 1 = this block was freed by a later realloc  */
} AllocEvent;

typedef struct {
    AllocEvent events[MAX_HISTORY];
    size_t     count;
} AllocHistory;

static void history_init(AllocHistory *h) { h->count = 0; }

static void history_record(AllocHistory *h, size_t cap, int freed)
{
    if (h->count >= MAX_HISTORY) return;

    /* Cumulative sum of freed capacities (for the memory reuse argument) */
    size_t prev_sum = 0;
    if (h->count > 0) {
        prev_sum = h->events[h->count - 1].cumulative_sum;
    }

    h->events[h->count].capacity       = cap;
    h->events[h->count].cumulative_sum = prev_sum + (freed ? cap : 0);
    h->events[h->count].is_freed       = freed;
    h->count++;
}

/* ---------------------------------------------------------------------------
 * 3. Lifecycle: create and destroy
 * --------------------------------------------------------------------------- */

IntArray *array_create(size_t capacity, double growth_factor)
{
    if (capacity == 0) {
        fprintf(stderr, "array_create: capacity must be > 0\n");
        return NULL;
    }
    if (growth_factor <= 1.0) {
        fprintf(stderr, "array_create: growth_factor must be > 1.0\n");
        return NULL;
    }

    IntArray *arr = malloc(sizeof(IntArray));
    if (!arr) return NULL;

    arr->data = malloc(capacity * sizeof(int));
    if (!arr->data) {
        free(arr);
        return NULL;
    }

    arr->size               = 0;
    arr->capacity           = capacity;
    arr->realloc_count      = 0;
    arr->total_bytes_copied = 0;
    arr->growth_factor      = growth_factor;

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
 * 4. Push with configurable growth factor
 * ---------------------------------------------------------------------------
 * The growth calculation uses:
 *   new_cap = (size_t)(old_cap * growth_factor)
 *   if (new_cap == old_cap) new_cap = old_cap + 1;  // avoid stall
 *
 * For factor=2.0: 1 → 2 → 4 → 8 → 16 → 32 → ...
 * For factor=1.5: 1 → 2 → 3 → 5 → 8 → 12 → 18 → 27 → ...
 *   (note: integer truncation means the exact sequence depends on rounding)
 *
 * We optionally record allocation events for the timeline visualization.
 * --------------------------------------------------------------------------- */

int array_push(IntArray *arr, int value, AllocHistory *history)
{
    if (!arr) return -1;

    if (arr->size >= arr->capacity) {
        size_t old_cap = arr->capacity;
        size_t new_cap = (size_t)(old_cap * arr->growth_factor);

        /* Guard against stalling: if truncation produces the same capacity,
         * bump by at least 1. This matters for small capacities with
         * growth factors close to 1.0. */
        if (new_cap <= old_cap) new_cap = old_cap + 1;

        int *tmp = realloc(arr->data, new_cap * sizeof(int));
        if (!tmp) return -1;

        /* Record the freed block (old capacity) and the new block */
        if (history) {
            history_record(history, old_cap, 1);  /* old block freed */
            history_record(history, new_cap, 0);  /* new block alive */
        }

        arr->total_bytes_copied += arr->size * sizeof(int);
        arr->data     = tmp;
        arr->capacity = new_cap;
        arr->realloc_count++;
    }

    arr->data[arr->size] = value;
    arr->size++;
    return 0;
}

/* ---------------------------------------------------------------------------
 * 5. Accessors
 * --------------------------------------------------------------------------- */

size_t array_size(const IntArray *arr)     { return arr ? arr->size     : 0; }
size_t array_capacity(const IntArray *arr) { return arr ? arr->capacity : 0; }

/* ---------------------------------------------------------------------------
 * 6. Push with ADDITIVE growth (for comparison)
 * ---------------------------------------------------------------------------
 * Instead of multiplying, we add a fixed number of slots each time.
 * This destroys the amortized O(1) property — total copy cost becomes O(n²).
 * --------------------------------------------------------------------------- */

int array_push_additive(IntArray *arr, int value, size_t increment,
                        AllocHistory *history)
{
    if (!arr) return -1;

    if (arr->size >= arr->capacity) {
        size_t old_cap = arr->capacity;
        size_t new_cap = old_cap + increment;

        int *tmp = realloc(arr->data, new_cap * sizeof(int));
        if (!tmp) return -1;

        if (history) {
            history_record(history, old_cap, 1);
            history_record(history, new_cap, 0);
        }

        arr->total_bytes_copied += arr->size * sizeof(int);
        arr->data     = tmp;
        arr->capacity = new_cap;
        arr->realloc_count++;
    }

    arr->data[arr->size] = value;
    arr->size++;
    return 0;
}

/* ---------------------------------------------------------------------------
 * 7. ASCII visualization: comparison table
 * ---------------------------------------------------------------------------
 * Instead of showing individual array states (Posts 1-2 covered that),
 * this post's visualization compares strategies side by side.
 *
 * The table shows, for each strategy after N pushes:
 *   - Final capacity
 *   - Number of reallocations
 *   - Total bytes copied
 *   - Peak memory waste (bytes and percentage)
 *   - Whether freed memory can be reused
 * --------------------------------------------------------------------------- */

static void print_comparison_header(size_t n_pushes, size_t init_cap)
{
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║  GROWTH STRATEGY COMPARISON: %zu pushes, initial capacity = %zu             ║\n",
           n_pushes, init_cap);
    printf("╠═══════════════╦══════════╦══════════╦════════════╦══════════╦══════════════╣\n");
    printf("║  Strategy     ║ Final    ║ Reallocs ║ Bytes      ║ Peak     ║ Memory       ║\n");
    printf("║               ║ Capacity ║          ║ Copied     ║ Waste    ║ Reuse?       ║\n");
    printf("╠═══════════════╬══════════╬══════════╬════════════╬══════════╬══════════════╣\n");
}

static void print_comparison_row(const char *name, const IntArray *arr,
                                 size_t peak_waste_slots, int can_reuse)
{
    size_t waste_bytes = (arr->capacity - arr->size) * sizeof(int);
    double waste_pct   = 100.0 * (double)(arr->capacity - arr->size)
                               / (double)arr->capacity;

    printf("║  %-12s ║ %8zu ║ %8zu ║ %10zu ║ %5zuB   ║ %-12s ║\n",
           name,
           arr->capacity,
           arr->realloc_count,
           arr->total_bytes_copied,
           peak_waste_slots * sizeof(int),
           can_reuse ? "Yes (after ~4)" : "No");

    /* Suppress unused warning for waste_bytes / waste_pct in this row format */
    (void)waste_bytes;
    (void)waste_pct;
}

static void print_comparison_footer(void)
{
    printf("╚═══════════════╩══════════╩══════════╩════════════╩══════════╩══════════════╝\n");
}

/* ---------------------------------------------------------------------------
 * 8. Memory reuse analysis: the key insight of this post
 * ---------------------------------------------------------------------------
 * When we realloc from capacity C to capacity C*g, the old block of size C
 * is freed. If we keep growing, we accumulate freed blocks:
 *
 *   2x growth from 1:  freed blocks = 1, 2, 4, 8, 16, ...
 *     Sum of freed = 1 + 2 + 4 + ... + 2^(k-1) = 2^k - 1
 *     Next allocation = 2^k
 *     Can we reuse? Sum of freed = 2^k - 1 < 2^k. NEVER enough.
 *
 *   1.5x growth from 1: freed blocks = 1, 2, 3, 5, 8, 12, ...
 *     After a few rounds, sum of freed > next allocation. CAN reuse.
 *
 * This function prints the step-by-step analysis showing exactly when
 * (and whether) the cumulative freed memory exceeds the next allocation.
 * --------------------------------------------------------------------------- */

static void analyze_memory_reuse(double factor, size_t start_cap,
                                 size_t max_steps)
{
    printf("\n┌─────────────────────────────────────────────────────────────┐\n");
    printf("│  MEMORY REUSE ANALYSIS: factor = %.2fx, start = %zu         │\n",
           factor, start_cap);
    printf("├───────┬──────────┬──────────┬──────────────┬────────────────┤\n");
    printf("│ Step  │ Capacity │ Freed    │ Sum of freed │ Reuse?         │\n");
    printf("├───────┼──────────┼──────────┼──────────────┼────────────────┤\n");

    size_t cap     = start_cap;
    size_t sum_freed = 0;
    int    found_reuse = 0;

    for (size_t step = 0; step < max_steps; step++) {
        size_t old_cap = cap;
        size_t new_cap = (size_t)(cap * factor);
        if (new_cap <= cap) new_cap = cap + 1;

        sum_freed += old_cap;

        const char *reuse_str;
        if (sum_freed >= new_cap && !found_reuse) {
            reuse_str = "◀ YES, first!";
            found_reuse = 1;
        } else if (sum_freed >= new_cap) {
            reuse_str = "yes";
        } else {
            reuse_str = "no";
        }

        printf("│ %5zu │ %5zu→%-3zu │ %8zu │ %12zu │ %-14s │\n",
               step + 1, old_cap, new_cap, old_cap, sum_freed,
               reuse_str);

        cap = new_cap;
    }

    printf("└───────┴──────────┴──────────┴──────────────┴────────────────┘\n");

    if (!found_reuse) {
        printf("  ⚠ After %zu reallocations, freed memory NEVER exceeds\n",
               max_steps);
        printf("    the next allocation. This growth factor cannot reuse.\n");
    } else {
        printf("  ✓ Freed memory can be coalesced and reused by the allocator.\n");
    }
}

/* ---------------------------------------------------------------------------
 * 9. Amortized cost analysis: the banker's method explained in code
 * ---------------------------------------------------------------------------
 * For each push, we track whether it triggered a realloc (expensive) or not
 * (cheap). Then we compute the amortized cost: total work / total pushes.
 *
 * With geometric growth, the total copy work across all reallocations is:
 *   C + C*g + C*g² + ... + C*g^k ≈ C * g^(k+1) / (g-1)
 * And the number of pushes to fill to that point is roughly C * g^k.
 * So amortized cost per push = g / (g-1), which is O(1).
 *
 * With additive growth (increment K), the total copy work is:
 *   K + 2K + 3K + ... + nK/K² ≈ n²/(2K)
 * So amortized cost per push = n/(2K), which is O(n). Not constant.
 * --------------------------------------------------------------------------- */

static void analyze_amortized_cost(size_t n_pushes, size_t init_cap)
{
    printf("\n╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║  AMORTIZED COST ANALYSIS: %zu pushes, start capacity = %zu    ║\n",
           n_pushes, init_cap);
    printf("╠═══════════════╦═════════════════╦═════════════════════════════╣\n");
    printf("║  Strategy     ║ Total copies    ║ Amortized cost/push         ║\n");
    printf("╠═══════════════╬═════════════════╬═════════════════════════════╣\n");

    /* 2x growth */
    {
        IntArray *a = array_create(init_cap, 2.0);
        for (size_t i = 0; i < n_pushes; i++) array_push(a, (int)i, NULL);
        double amortized = (double)a->total_bytes_copied
                         / ((double)n_pushes * sizeof(int));
        printf("║  2.0x         ║ %15zu ║ %.4f elements/push          ║\n",
               a->total_bytes_copied / sizeof(int), amortized);
        array_destroy(a);
    }

    /* 1.5x growth */
    {
        IntArray *a = array_create(init_cap, 1.5);
        for (size_t i = 0; i < n_pushes; i++) array_push(a, (int)i, NULL);
        double amortized = (double)a->total_bytes_copied
                         / ((double)n_pushes * sizeof(int));
        printf("║  1.5x         ║ %15zu ║ %.4f elements/push          ║\n",
               a->total_bytes_copied / sizeof(int), amortized);
        array_destroy(a);
    }

    /* Additive (K=10) */
    {
        IntArray *a = array_create(init_cap, 2.0); /* factor unused */
        for (size_t i = 0; i < n_pushes; i++)
            array_push_additive(a, (int)i, 10, NULL);
        double amortized = (double)a->total_bytes_copied
                         / ((double)n_pushes * sizeof(int));
        printf("║  Additive +10 ║ %15zu ║ %.4f elements/push          ║\n",
               a->total_bytes_copied / sizeof(int), amortized);
        array_destroy(a);
    }

    /* Additive (K=100) */
    {
        IntArray *a = array_create(init_cap, 2.0);
        for (size_t i = 0; i < n_pushes; i++)
            array_push_additive(a, (int)i, 100, NULL);
        double amortized = (double)a->total_bytes_copied
                         / ((double)n_pushes * sizeof(int));
        printf("║  Additive+100 ║ %15zu ║ %.4f elements/push          ║\n",
               a->total_bytes_copied / sizeof(int), amortized);
        array_destroy(a);
    }

    printf("╚═══════════════╩═════════════════╩═════════════════════════════╝\n");
}

/* ---------------------------------------------------------------------------
 * 10. Benchmark harness: wall-clock timing
 * ---------------------------------------------------------------------------
 * Measures real time for N pushes under each strategy. Uses clock_gettime
 * with CLOCK_MONOTONIC for stable measurements. Runs multiple iterations
 * and reports the median to reduce noise.
 *
 * NOTE: this is a simple harness for illustration. Post 12 builds a proper
 * benchmark framework with warm-up runs, outlier filtering, and statistical
 * analysis. Don't draw strong conclusions from these numbers — they're here
 * to show the *relative* difference between strategies.
 * --------------------------------------------------------------------------- */

#define BENCH_ITERATIONS 5

typedef struct {
    const char *name;
    double      times_ms[BENCH_ITERATIONS];
    double      median_ms;
    size_t      reallocs;
    size_t      final_cap;
} BenchResult;

/* Simple insertion sort for the tiny times array */
static void sort_doubles(double *arr, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        double key = arr[i];
        size_t j = i;
        while (j > 0 && arr[j - 1] > key) {
            arr[j] = arr[j - 1];
            j--;
        }
        arr[j] = key;
    }
}

static struct timespec ts_diff(struct timespec start, struct timespec end)
{
    struct timespec d;
    d.tv_sec  = end.tv_sec  - start.tv_sec;
    d.tv_nsec = end.tv_nsec - start.tv_nsec;
    if (d.tv_nsec < 0) {
        d.tv_sec--;
        d.tv_nsec += 1000000000L;
    }
    return d;
}

static double ts_to_ms(struct timespec ts)
{
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

static void run_benchmark(size_t n_pushes, size_t init_cap)
{
    printf("\n╔════════════════════════════════════════════════════════════════╗\n");
    printf("║  BENCHMARK: %zu pushes, %d iterations, start capacity = %zu   ║\n",
           n_pushes, BENCH_ITERATIONS, init_cap);
    printf("╠════════════════╦══════════╦════════════╦═══════════════════════╣\n");
    printf("║  Strategy      ║ Reallocs ║ Final Cap  ║ Median Time (ms)      ║\n");
    printf("╠════════════════╬══════════╬════════════╬═══════════════════════╣\n");

    /* Strategies to benchmark */
    struct {
        const char *name;
        double      factor;
        size_t      additive;  /* 0 = use multiplicative */
    } strategies[] = {
        { "2.0x",          2.0,  0   },
        { "1.5x",          1.5,  0   },
        { "1.25x",         1.25, 0   },
        { "Additive +10",  2.0,  10  },
        { "Additive +100", 2.0,  100 },
    };
    size_t n_strats = sizeof(strategies) / sizeof(strategies[0]);

    for (size_t s = 0; s < n_strats; s++) {
        BenchResult result;
        result.name = strategies[s].name;

        for (int iter = 0; iter < BENCH_ITERATIONS; iter++) {
            IntArray *a = array_create(init_cap, strategies[s].factor);
            if (!a) continue;

            struct timespec t_start, t_end;
            clock_gettime(CLOCK_MONOTONIC, &t_start);

            if (strategies[s].additive > 0) {
                for (size_t i = 0; i < n_pushes; i++)
                    array_push_additive(a, (int)i, strategies[s].additive, NULL);
            } else {
                for (size_t i = 0; i < n_pushes; i++)
                    array_push(a, (int)i, NULL);
            }

            clock_gettime(CLOCK_MONOTONIC, &t_end);
            struct timespec diff = ts_diff(t_start, t_end);
            result.times_ms[iter] = ts_to_ms(diff);
            result.reallocs  = a->realloc_count;
            result.final_cap = a->capacity;

            array_destroy(a);
        }

        sort_doubles(result.times_ms, BENCH_ITERATIONS);
        result.median_ms = result.times_ms[BENCH_ITERATIONS / 2];

        printf("║  %-13s ║ %8zu ║ %10zu ║ %10.3f             ║\n",
               result.name, result.reallocs, result.final_cap,
               result.median_ms);
    }

    printf("╚════════════════╩══════════╩════════════╩═══════════════════════╝\n");
    printf("  (Times are wall-clock. See Post 12 for proper methodology.)\n");
}

/* ---------------------------------------------------------------------------
 * 11. Graphviz DOT: allocation timeline
 * ---------------------------------------------------------------------------
 * Generates a vertical timeline showing each allocation as a box.
 * Freed blocks are red/dashed; the current (live) block is green/solid.
 * The cumulative freed sum is annotated so the reader can see when
 * reuse becomes possible.
 * --------------------------------------------------------------------------- */

static void generate_timeline_dot(const AllocHistory *h1,
                                  const char *label1,
                                  const AllocHistory *h2,
                                  const char *label2,
                                  const char *filename)
{
    FILE *f = fopen(filename, "w");
    if (!f) {
        fprintf(stderr, "Cannot open %s for writing\n", filename);
        return;
    }

    /* The 1.5x strategy produces many more events than 2.0x.
     * To keep the diagram readable, we split the 1.5x column into two
     * visual columns: R0..R(split-1) and R(split)..R(end).
     * The split point aligns with h1->count - 3 so both halves of 1.5x
     * run alongside the 2.0x column visually. A dashed bridge edge
     * connects the two halves. */

    /* Split point: the row at which the second 1.5x column starts.
     * We split h2 roughly in half so both visual columns of 1.5x have
     * similar height. This keeps the diagram compact: the left column
     * (h1) plus the two 1.5x columns form a balanced 3-column layout. */
    size_t split = (h2->count + 1) / 2;
    /* Ensure split is at most h2->count - 1 so column 2 has content */
    if (split >= h2->count) split = h2->count - 1;

    fprintf(f, "digraph AllocationTimeline {\n");
    fprintf(f, "  newrank=true;\n");
    fprintf(f, "  rankdir=TB;\n");
    fprintf(f, "  // nodesep=0.6 for more horizontal spacing between blocks\n");
    fprintf(f, "  graph [fontname=\"Helvetica\", fontsize=13, "
               "label=\"Allocation Timeline: %s vs %s\", labelloc=t, "
               "nodesep=0.6, ranksep=0.4];\n", label1, label2);
    fprintf(f, "  node  [fontname=\"Helvetica\", fontsize=10, "
               "shape=box, width=1.8];\n");
    fprintf(f, "  edge  [fontname=\"Helvetica\", fontsize=9];\n\n");

    /* ── Left column: strategy 1 (2.0x) ────────────────────────── */
    fprintf(f, "  subgraph cluster_left {\n");
    fprintf(f, "    label=\"%s\"; style=rounded; color=\"#495057\";\n", label1);
    for (size_t i = 0; i < h1->count; i++) {
        const char *color = h1->events[i].is_freed ? "#F8D7DA" : "#D4EDDA";
        const char *border = h1->events[i].is_freed ? "dashed" : "solid";
        fprintf(f, "    L%zu [label=\"cap=%zu", i, h1->events[i].capacity);
        if (h1->events[i].is_freed)
            fprintf(f, "\\nfreed (sum=%zu)", h1->events[i].cumulative_sum);
        fprintf(f, "\", style=\"filled,%s\", fillcolor=\"%s\"];\n",
                border, color);
        if (i > 0) fprintf(f, "    L%zu -> L%zu;\n", i - 1, i);
    }
    fprintf(f, "  }\n\n");

    /* ── Right cluster: strategy 2 (1.5x), split into two columns ─ */
    fprintf(f, "  subgraph cluster_right {\n");
    fprintf(f, "    label=\"%s\"; style=rounded; color=\"#495057\";\n", label2);

    /* Column 1 of 1.5x: R0 .. R(split-1) */
    fprintf(f, "    \n    // --- Column 1 of 1.5x ---\n");
    for (size_t i = 0; i < split; i++) {
        const char *color = h2->events[i].is_freed ? "#F8D7DA" : "#D4EDDA";
        const char *border = h2->events[i].is_freed ? "dashed" : "solid";
        fprintf(f, "    R%zu [label=\"cap=%zu", i, h2->events[i].capacity);
        if (h2->events[i].is_freed)
            fprintf(f, "\\nfreed (sum=%zu)", h2->events[i].cumulative_sum);
        fprintf(f, "\", style=\"filled,%s\", fillcolor=\"%s\"];\n",
                border, color);
        if (i > 0) fprintf(f, "    R%zu -> R%zu;\n", i - 1, i);
    }

    /* Bridge edge: dashed, constraint=false so column 2 can start at top */
    fprintf(f, "\n    // Bridge edge to second column (no rank constraint)\n");
    fprintf(f, "    R%zu -> R%zu [constraint=false, style=dashed, "
               "color=\"#007bff\", label=\" \", fontcolor=\"#007bff\"];\n\n",
               split - 1, split);

    /* Column 2 of 1.5x: R(split) .. R(end) */
    fprintf(f, "    // --- Column 2 of 1.5x ---\n");
    for (size_t i = split; i < h2->count; i++) {
        const char *color = h2->events[i].is_freed ? "#F8D7DA" : "#D4EDDA";
        const char *border = h2->events[i].is_freed ? "dashed" : "solid";
        fprintf(f, "    R%zu [label=\"cap=%zu", i, h2->events[i].capacity);
        if (h2->events[i].is_freed)
            fprintf(f, "\\nfreed (sum=%zu)", h2->events[i].cumulative_sum);
        fprintf(f, "\", style=\"filled,%s\", fillcolor=\"%s\"];\n",
                border, color);
        if (i > split) fprintf(f, "    R%zu -> R%zu;\n", i - 1, i);
    }

    fprintf(f, "  }\n\n");

    /* ── Rank synchronization: align 3 visual columns ─────────── */
    /* L(i) aligns with R(i) (first 1.5x col) and R(split+i) (second
     * 1.5x col). This creates a neat 3-column layout. */
    fprintf(f, "  // Synchronize ranks across 3 visual columns\n");
    size_t col2_len = h2->count - split;
    for (size_t i = 0; i < h1->count || i < split || i < col2_len; i++) {
        int has_l  = (i < h1->count);
        int has_r1 = (i < split);
        int has_r2 = (i < col2_len);

        /* Only emit rank=same when at least two nodes align */
        if ((has_l + has_r1 + has_r2) >= 2) {
            fprintf(f, "  { rank=same;");
            if (has_l)  fprintf(f, " L%zu;", i);
            if (has_r1) fprintf(f, " R%zu;", i);
            if (has_r2) fprintf(f, " R%zu;", split + i);
            fprintf(f, " }\n");
        }
    }

    /* Any leftover rows in the first 1.5x column that don't align with
     * either L or R2 (e.g. the last row R17 that has no L18 or R35) */

    fprintf(f, "}\n");
    fclose(f);

    printf("  DOT file written to %s\n", filename);
}

/* ---------------------------------------------------------------------------
 * 12. Knowledge test: calculate waste after 20 pushes
 * ---------------------------------------------------------------------------
 * This function answers the post's Knowledge Test question by running
 * the actual simulation and printing the results.
 * --------------------------------------------------------------------------- */

static void knowledge_test(void)
{
    printf("\n╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║  KNOWLEDGE TEST                                                ║\n");
    printf("║  \"After 20 pushes from capacity=1, how much memory is wasted?\" ║\n");
    printf("╠═══════════════╦════════════╦══════════╦═════════════════════════╣\n");
    printf("║  Factor       ║ Final Cap  ║ Wasted   ║ Waste %%                 ║\n");
    printf("╠═══════════════╬════════════╬══════════╬═════════════════════════╣\n");

    double factors[] = { 2.0, 1.5 };
    const char *names[] = { "2.0x", "1.5x" };

    for (int f = 0; f < 2; f++) {
        IntArray *a = array_create(1, factors[f]);
        for (int i = 0; i < 20; i++) array_push(a, i * 10, NULL);

        size_t waste = a->capacity - a->size;
        double pct   = 100.0 * (double)waste / (double)a->capacity;
        printf("║  %-12s ║ %10zu ║ %5zu el ║ %6.1f%%                   ║\n",
               names[f], a->capacity, waste, pct);
        array_destroy(a);
    }

    printf("╚═══════════════╩════════════╩══════════╩═════════════════════════╝\n");
}

/* ---------------------------------------------------------------------------
 * 13. Main: run all analyses
 * --------------------------------------------------------------------------- */

int main(void)
{
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  Post 3: The Growth Factor Debate — 1.5x, 2x, or else?\n");
    printf("═══════════════════════════════════════════════════════════════\n");

    const size_t N          = 1000;   /* pushes for comparison         */
    const size_t INIT_CAP   = 1;      /* start from 1 for clear math   */

    /* ── Part A: side-by-side comparison ────────────────────────── */
    printf("\n▸ Running %zu pushes with each strategy...\n", N);

    AllocHistory hist_2x, hist_15x;
    history_init(&hist_2x);
    history_init(&hist_15x);

    /* Record initial allocations */
    history_record(&hist_2x, INIT_CAP, 0);
    history_record(&hist_15x, INIT_CAP, 0);

    IntArray *arr_2x  = array_create(INIT_CAP, 2.0);
    IntArray *arr_15x = array_create(INIT_CAP, 1.5);
    IntArray *arr_add = array_create(INIT_CAP, 2.0); /* factor unused */

    if (!arr_2x || !arr_15x || !arr_add) {
        fprintf(stderr, "FATAL: allocation failure\n");
        return 1;
    }

    size_t peak_waste_2x  = 0;
    size_t peak_waste_15x = 0;
    size_t peak_waste_add = 0;

    for (size_t i = 0; i < N; i++) {
        array_push(arr_2x,  (int)i, &hist_2x);
        array_push(arr_15x, (int)i, &hist_15x);
        array_push_additive(arr_add, (int)i, 10, NULL);

        /* Track peak waste */
        size_t w;
        w = arr_2x->capacity - arr_2x->size;
        if (w > peak_waste_2x) peak_waste_2x = w;
        w = arr_15x->capacity - arr_15x->size;
        if (w > peak_waste_15x) peak_waste_15x = w;
        w = arr_add->capacity - arr_add->size;
        if (w > peak_waste_add) peak_waste_add = w;
    }

    print_comparison_header(N, INIT_CAP);
    print_comparison_row("2.0x",         arr_2x,  peak_waste_2x,  0);
    print_comparison_row("1.5x",         arr_15x, peak_waste_15x, 1);
    print_comparison_row("Additive +10", arr_add, peak_waste_add, 1);
    print_comparison_footer();

    /* ── Part B: memory reuse analysis ─────────────────────────── */
    printf("\n▸ Memory reuse analysis\n");
    analyze_memory_reuse(2.0, 1, 10);
    analyze_memory_reuse(1.5, 1, 10);

    /* ── Part C: amortized cost ────────────────────────────────── */
    printf("\n▸ Amortized cost analysis\n");
    analyze_amortized_cost(N, INIT_CAP);

    /* ── Part D: benchmark ─────────────────────────────────────── */
    printf("\n▸ Benchmark (wall-clock timing)\n");
    run_benchmark(100000, 1);

    /* ── Part E: Graphviz timeline ─────────────────────────────── */
    printf("\n▸ Generating allocation timeline (Graphviz DOT)\n");
    generate_timeline_dot(&hist_2x,  "2.0x growth",
                          &hist_15x, "1.5x growth",
                          "output/post_03_timeline.dot");

    /* ── Part F: Knowledge test ────────────────────────────────── */
    knowledge_test();

    /* ── Cleanup ──────────────────────────────────────────────── */
    array_destroy(arr_2x);
    array_destroy(arr_15x);
    array_destroy(arr_add);

    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("  End of Post 3. Next: type erasure with void* and memcpy.\n");
    printf("═══════════════════════════════════════════════════════════════\n");

    return 0;
}
