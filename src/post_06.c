/* ============================================================================
 * Post 6: "When Things Go Wrong: Error Handling Strategies in C"
 * ============================================================================
 *
 * This file is entirely self-contained. Compile with:
 *   gcc -Wall -Wextra -std=c11 -o build/post_06 src_posts/post_06.c
 *
 * Run:
 *   ./build/post_06                       (ASCII visualization to stdout)
 *   ./build/post_06 > output/post_06.txt (save ASCII output)
 *
 * The program also writes output/post_06_error_flow.dot (Graphviz).
 * Render with:  dot -Tsvg output/post_06_error_flow.dot \
 *                        -o output/post_06_error_flow.svg
 *
 * Learning outcome: implement push/insert that never corrupt array state
 * on allocation failure (OOM). Choose between error propagation strategies:
 * return codes, callbacks, and abort.
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ===========================================================================
 * 1. Error code definitions
 * ===========================================================================
 * Every fallible operation returns one of these. The caller can inspect the
 * value to decide how to react. Negative values = error, zero = success.
 *
 * This enum replaces the ad-hoc -1 returns from Posts 1-5. Having named
 * codes instead of magic numbers makes error handling self-documenting.
 * =========================================================================== */

typedef enum {
    ARRAY_OK          =  0,   /* Operation succeeded                          */
    ARRAY_ERR_NULL    = -1,   /* NULL pointer argument                        */
    ARRAY_ERR_ALLOC   = -2,   /* malloc or realloc failed                     */
    ARRAY_ERR_BOUNDS  = -3,   /* Index out of range                           */
    ARRAY_ERR_OVERFLOW = -4,  /* Capacity calculation would overflow size_t   */
    ARRAY_ERR_FULL    = -5,   /* Array at max capacity, cannot grow           */
    ARRAY_ERR_TYPE    = -6    /* Type size mismatch (from macro layer)        */
} ArrayError;

/* Human-readable error descriptions.
 * Maps each error code to a string. Useful for logging and diagnostics. */
static const char *array_error_str(ArrayError err)
{
    switch (err) {
        case ARRAY_OK:           return "success";
        case ARRAY_ERR_NULL:     return "NULL pointer argument";
        case ARRAY_ERR_ALLOC:    return "memory allocation failed";
        case ARRAY_ERR_BOUNDS:   return "index out of bounds";
        case ARRAY_ERR_OVERFLOW: return "capacity overflow";
        case ARRAY_ERR_FULL:     return "array at maximum capacity";
        case ARRAY_ERR_TYPE:     return "type size mismatch";
    }
    return "unknown error";
}


/* ===========================================================================
 * 2. Error callback type
 * ===========================================================================
 * An optional handler that the user can register. The library calls it
 * whenever an error occurs, passing the error code, a descriptive message,
 * the file name, and the line number where the error was detected.
 *
 * The callback receives the error but does NOT control the outcome — the
 * function still returns the error code to the caller. The callback is
 * for logging, metrics, or alerting — not for recovery.
 * =========================================================================== */

typedef void (*ArrayErrorCallback)(ArrayError err, const char *msg,
                                   const char *file, int line);

/* Global error callback — NULL means no callback is registered.
 * In a multithreaded program, this should be thread-local or protected
 * by a mutex. For this single-threaded post, a global is fine. */
static ArrayErrorCallback g_error_callback = NULL;

/* Register or clear the error callback. Pass NULL to disable. */
void array_set_error_callback(ArrayErrorCallback cb)
{
    g_error_callback = cb;
}

/* Internal macro: report an error through the callback (if registered)
 * and return the error code. This keeps error reporting consistent
 * across all functions without duplicating the callback logic. */
#define ARRAY_REPORT_ERROR(err, msg) \
    do { \
        if (g_error_callback) { \
            g_error_callback((err), (msg), __FILE__, __LINE__); \
        } \
    } while (0)


/* ===========================================================================
 * 3. The Array struct (carried forward from Post 5)
 * ===========================================================================
 * The struct is unchanged from Post 5. The error handling improvements
 * live in the functions, not in the data structure itself.
 * =========================================================================== */

typedef struct {
    void   *data;           /* Opaque heap buffer                            */
    size_t  size;           /* Elements currently stored                      */
    size_t  capacity;       /* Slots allocated                                */
    size_t  element_size;   /* Bytes per element                              */
    size_t  realloc_count;  /* Diagnostic: growth events                      */
} Array;

/* Callback type for printing elements (used by visualization) */
typedef void (*PrintElementFn)(char *buf, size_t bufsize, const void *elem);


/* ===========================================================================
 * 4. Lifecycle — Create and Destroy
 * ===========================================================================
 * These now return proper error codes and report through the callback.
 * array_create returns NULL on failure (the caller checks the pointer).
 * array_destroy is idempotent — calling it twice is safe.
 * =========================================================================== */

Array *array_create(size_t element_size, size_t initial_capacity)
{
    if (element_size == 0 || initial_capacity == 0) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_NULL,
                           "array_create: element_size and capacity must be > 0");
        return NULL;
    }

    /* Check for multiplication overflow before allocating.
     * If element_size * initial_capacity would overflow size_t, reject. */
    if (initial_capacity > SIZE_MAX / element_size) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_OVERFLOW,
                           "array_create: capacity * element_size overflows");
        return NULL;
    }

    Array *arr = malloc(sizeof(Array));
    if (!arr) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_ALLOC,
                           "array_create: malloc failed (struct)");
        return NULL;
    }

    arr->data = malloc(element_size * initial_capacity);
    if (!arr->data) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_ALLOC,
                           "array_create: malloc failed (buffer)");
        free(arr);  /* Clean up the struct — don't leak it */
        return NULL;
    }

    arr->size          = 0;
    arr->capacity      = initial_capacity;
    arr->element_size  = element_size;
    arr->realloc_count = 0;

    return arr;
}

void array_destroy(Array *arr)
{
    if (!arr) return;
    free(arr->data);
    arr->data     = NULL;  /* Defensive: prevent use-after-free             */
    arr->size     = 0;     /* Defensive: zero metadata so stale reads       */
    arr->capacity = 0;     /*   show clearly invalid state                  */
    free(arr);
}


/* ===========================================================================
 * 5. Core pointer arithmetic (unchanged)
 * =========================================================================== */

static void *element_at(const Array *arr, size_t index)
{
    return (char *)arr->data + index * arr->element_size;
}


/* ===========================================================================
 * 6. The safe growth function — the heart of this post
 * ===========================================================================
 *
 * array_ensure_capacity() is an INTERNAL function. It guarantees that the
 * array has room for at least `min_capacity` elements. If the current
 * capacity is already sufficient, it does nothing.
 *
 * CRITICAL INVARIANT: if this function fails (returns non-zero), the
 * array is UNCHANGED. Same data pointer, same size, same capacity.
 * The caller can continue using the array as if the call never happened.
 *
 * This is achieved by the temporary pointer pattern:
 *   void *tmp = realloc(arr->data, new_size);
 *   if (!tmp) return ARRAY_ERR_ALLOC;  // arr->data untouched
 *   arr->data = tmp;                    // only update on success
 *
 * The function also checks for overflow before computing the new size,
 * preventing a class of bugs where a very large array wraps around
 * size_t and allocates a tiny buffer.
 * =========================================================================== */

static ArrayError array_ensure_capacity(Array *arr, size_t min_capacity)
{
    if (arr->capacity >= min_capacity) {
        return ARRAY_OK;  /* Already have enough room */
    }

    /* Grow by 2x, but at least to min_capacity */
    size_t new_cap = arr->capacity * 2;
    if (new_cap < min_capacity) {
        new_cap = min_capacity;
    }

    /* Overflow check: can new_cap * element_size fit in size_t? */
    if (new_cap > SIZE_MAX / arr->element_size) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_OVERFLOW,
                           "array_ensure_capacity: new size overflows size_t");
        return ARRAY_ERR_OVERFLOW;
    }

    /* ----------------------------------------------------------------
     * THE TEMPORARY POINTER PATTERN
     * ----------------------------------------------------------------
     * This is the single most important pattern in this post.
     *
     * WRONG:
     *   arr->data = realloc(arr->data, new_size);
     *   // If realloc fails, arr->data is now NULL.
     *   // The old buffer is leaked. The data is lost.
     *
     * CORRECT:
     *   void *tmp = realloc(arr->data, new_size);
     *   if (!tmp) return error;  // arr->data unchanged!
     *   arr->data = tmp;          // update only on success
     *
     * On failure: arr->data still points to the original buffer.
     *             All existing elements are intact.
     *             The caller can retry, log, or gracefully degrade.
     *
     * On success: tmp holds the new (possibly moved) pointer.
     *             We assign it to arr->data.
     *             The old buffer was freed by realloc internally.
     * ---------------------------------------------------------------- */

    size_t new_size = new_cap * arr->element_size;
    void *tmp = realloc(arr->data, new_size);
    if (!tmp) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_ALLOC,
                           "array_ensure_capacity: realloc failed");
        return ARRAY_ERR_ALLOC;
    }

    arr->data     = tmp;
    arr->capacity = new_cap;
    arr->realloc_count++;

    return ARRAY_OK;
}


/* ===========================================================================
 * 7. Push — with proper error handling
 * ===========================================================================
 *
 * The contract:
 *   - On success: element is appended, size incremented, returns ARRAY_OK.
 *   - On failure: array is UNCHANGED. No element added, no size change,
 *                 no data pointer change. Returns an error code.
 *
 * The key insight: we call array_ensure_capacity BEFORE touching the
 * array's data. If ensure_capacity fails, we haven't modified anything.
 * If it succeeds, we know the memcpy and size increment will work
 * (they can't fail — memcpy doesn't allocate, and incrementing size
 * is just an arithmetic operation).
 *
 * This "check-then-act" pattern is what guarantees state preservation.
 * =========================================================================== */

ArrayError array_push(Array *arr, const void *element)
{
    if (!arr || !element) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_NULL, "array_push: NULL argument");
        return ARRAY_ERR_NULL;
    }

    /* Step 1: ensure room — if this fails, arr is unchanged */
    ArrayError err = array_ensure_capacity(arr, arr->size + 1);
    if (err != ARRAY_OK) {
        return err;  /* Propagate. Array untouched. */
    }

    /* Step 2: copy element — cannot fail */
    memcpy(element_at(arr, arr->size), element, arr->element_size);

    /* Step 3: update bookkeeping — cannot fail */
    arr->size++;

    return ARRAY_OK;
}


/* ===========================================================================
 * 8. Insert — the harder case
 * ===========================================================================
 *
 * Insert at an arbitrary position requires TWO operations that must both
 * succeed for the overall operation to be valid:
 *   1. Ensure capacity (may realloc)
 *   2. Shift existing elements to make room (memmove)
 *   3. Copy the new element into the gap
 *
 * Since memmove and memcpy don't allocate, only step 1 can fail.
 * If it fails, we haven't shifted anything yet, so the array is intact.
 *
 * The ordering matters: ensure capacity FIRST, shift SECOND.
 * If we shifted first and then realloc failed, we'd have moved the
 * elements but have no room for the new one — the array would be in
 * a corrupted state with a gap in the middle.
 * =========================================================================== */

ArrayError array_insert(Array *arr, size_t index, const void *element)
{
    if (!arr || !element) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_NULL, "array_insert: NULL argument");
        return ARRAY_ERR_NULL;
    }

    /* Allow inserting at the end (index == size), but not beyond */
    if (index > arr->size) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_BOUNDS, "array_insert: index out of range");
        return ARRAY_ERR_BOUNDS;
    }

    /* Step 1: ensure capacity — if this fails, nothing has changed */
    ArrayError err = array_ensure_capacity(arr, arr->size + 1);
    if (err != ARRAY_OK) {
        return err;
    }

    /* Step 2: shift elements [index..size-1] one position right.
     * memmove handles overlapping regions correctly (memcpy doesn't). */
    if (index < arr->size) {
        void *dst = element_at(arr, index + 1);
        void *src = element_at(arr, index);
        size_t bytes = (arr->size - index) * arr->element_size;
        memmove(dst, src, bytes);
    }

    /* Step 3: copy the new element into the gap */
    memcpy(element_at(arr, index), element, arr->element_size);
    arr->size++;

    return ARRAY_OK;
}


/* ===========================================================================
 * 9. Get, Set, and utility functions
 * =========================================================================== */

void *array_get(const Array *arr, size_t index)
{
    if (!arr || index >= arr->size) return NULL;
    return element_at(arr, index);
}

ArrayError array_set(Array *arr, size_t index, const void *element)
{
    if (!arr || !element) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_NULL, "array_set: NULL argument");
        return ARRAY_ERR_NULL;
    }
    if (index >= arr->size) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_BOUNDS, "array_set: index out of range");
        return ARRAY_ERR_BOUNDS;
    }
    memcpy(element_at(arr, index), element, arr->element_size);
    return ARRAY_OK;
}

size_t array_size(const Array *arr)         { return arr ? arr->size          : 0; }
size_t array_capacity(const Array *arr)     { return arr ? arr->capacity      : 0; }
size_t array_realloc_count(const Array *arr){ return arr ? arr->realloc_count : 0; }


/* ===========================================================================
 * 10. Type-safe macros (carried forward from Post 5, updated for ArrayError)
 * =========================================================================== */

#define ARRAY_CREATE(type, capacity) \
    array_create(sizeof(type), (capacity))

#define ARRAY_PUSH(arr, type, value)                                       \
    do {                                                                   \
        if (sizeof(type) != (arr)->element_size) {                         \
            fprintf(stderr,                                                \
                "ARRAY_PUSH: type size mismatch (sizeof(%s)=%zu, "         \
                "element_size=%zu) at %s:%d\n",                            \
                #type, sizeof(type), (arr)->element_size,                  \
                __FILE__, __LINE__);                                       \
        } else {                                                           \
            type _push_tmp = (value);                                      \
            ArrayError _push_err = array_push((arr), &_push_tmp);          \
            (void)_push_err; /* caller can't capture from statement macro */\
        }                                                                  \
    } while (0)

/* ARRAY_PUSH_ERR: like ARRAY_PUSH but stores the error code.
 * Usage: ArrayError err; ARRAY_PUSH_ERR(arr, int, 42, err);
 * The caller can then check: if (err != ARRAY_OK) { handle it } */
#define ARRAY_PUSH_ERR(arr, type, value, err_var)                          \
    do {                                                                   \
        if (sizeof(type) != (arr)->element_size) {                         \
            (err_var) = ARRAY_ERR_TYPE;                                    \
        } else {                                                           \
            type _push_tmp = (value);                                      \
            (err_var) = array_push((arr), &_push_tmp);                     \
        }                                                                  \
    } while (0)

#define ARRAY_INSERT(arr, type, index, value)                              \
    do {                                                                   \
        if (sizeof(type) != (arr)->element_size) {                         \
            fprintf(stderr,                                                \
                "ARRAY_INSERT: type size mismatch at %s:%d\n",             \
                __FILE__, __LINE__);                                       \
        } else {                                                           \
            type _ins_tmp = (value);                                       \
            (void)array_insert((arr), (index), &_ins_tmp);                 \
        }                                                                  \
    } while (0)

#define ARRAY_GET(arr, type, index)                                        \
    ( sizeof(type) != (arr)->element_size                                   \
      ? ( fprintf(stderr,                                                   \
              "ARRAY_GET: type size mismatch at %s:%d\n",                   \
              __FILE__, __LINE__),                                          \
          (type *)NULL )                                                    \
      : (type *)array_get((arr), (index)) )

#define ARRAY_FOREACH(arr, type, var_name)                                 \
    for (size_t _fe_i = 0; _fe_i < (arr)->size; _fe_i++)                   \
        for (type *var_name = (type *)element_at((arr), _fe_i);            \
             var_name; var_name = NULL)


/* ===========================================================================
 * 11. ASCII visualization
 * ===========================================================================
 * Shows the array state after an operation, with emphasis on error
 * information when relevant. Carried forward from Post 5, with a new
 * section that shows the error status of the last operation.
 * =========================================================================== */

static void print_int(char *buf, size_t bufsize, const void *elem)
{
    snprintf(buf, bufsize, "%d", *(const int *)elem);
}

void array_visualize_ascii(const Array *arr, const char *label,
                           const char *type_label, PrintElementFn printer,
                           ArrayError last_error)
{
    if (!arr) {
        printf("╔══════════════════════════════════════════════════════════╗\n");
        printf("║  %-54s  ║\n", label ? label : "(null array)");
        printf("║  Array: NULL — creation failed or was destroyed        ║\n");
        printf("╚══════════════════════════════════════════════════════════╝\n\n");
        return;
    }

    size_t sz  = arr->size;
    size_t cap = arr->capacity;
    size_t show = cap <= 12 ? cap : 12;

    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  %-54s  ║\n", label ? label : "Array state");

    /* Error status line */
    if (last_error != ARRAY_OK) {
        printf("║  ⚠ Last operation: %-37s  ║\n", array_error_str(last_error));
    } else {
        printf("║  ✓ Last operation: %-37s  ║\n", "success");
    }

    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║  type: %-8s  element_size: %-4zu  reallocs: %-4zu      ║\n",
           type_label ? type_label : "void",
           arr->element_size, arr->realloc_count);
    printf("║  size: %-8zu  capacity: %-8zu                        ║\n",
           sz, cap);
    printf("╠══════════════════════════════════════════════════════════╣\n");

    /* Memory layout: element-by-element view */
    if (printer) {
        for (size_t i = 0; i < show; i++) {
            char val_buf[32] = "";
            if (i < sz) {
                printer(val_buf, sizeof(val_buf), element_at(arr, i));
                printf("║  [%2zu] +%-4zu  │ %-38s  ║\n",
                       i, i * arr->element_size, val_buf);
            } else {
                printf("║  [%2zu] +%-4zu  │ %-38s  ║\n",
                       i, i * arr->element_size, "(unused)");
            }
        }
        if (cap > show) {
            printf("║  ... %zu more slots not shown %-24s  ║\n",
                   cap - show, "");
        }
    } else {
        /* Compact box-drawing layout for small arrays */
        printf("║  ");
        for (size_t i = 0; i < show; i++) {
            printf(i < sz ? "[###]" : "[ · ]");
        }
        if (cap > show) printf("...");
        printf("\n");
    }

    /* Stats */
    size_t used_bytes  = sz  * arr->element_size;
    size_t alloc_bytes = cap * arr->element_size;
    double util = cap > 0 ? 100.0 * (double)sz / (double)cap : 0.0;

    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║  %zuB used / %zuB allocated = %.1f%% utilization",
           used_bytes, alloc_bytes, util);
    /* Pad to right border */
    int printed = (int)(strlen("  B used / B allocated = % utilization") +
                        20); /* rough estimate for numbers */
    for (int i = printed; i < 56; i++) printf(" ");
    printf("║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");
}


/* ===========================================================================
 * 12. State snapshot for before/after comparison
 * ===========================================================================
 * To demonstrate that the array is preserved after a failed operation,
 * we take a snapshot of the relevant state before the operation and
 * compare it afterwards.
 * =========================================================================== */

typedef struct {
    void   *data_ptr;     /* The pointer value (not the data itself)        */
    size_t  size;
    size_t  capacity;
    int     first_elem;   /* Value of first element, for integrity check    */
    int     last_elem;    /* Value of last element                          */
} ArraySnapshot;

static ArraySnapshot take_snapshot(const Array *arr)
{
    ArraySnapshot snap = {0};
    if (!arr) return snap;

    snap.data_ptr = arr->data;
    snap.size     = arr->size;
    snap.capacity = arr->capacity;

    if (arr->size > 0) {
        snap.first_elem = *(int *)element_at(arr, 0);
        snap.last_elem  = *(int *)element_at(arr, arr->size - 1);
    }
    return snap;
}

static void compare_snapshots(const ArraySnapshot *before,
                              const ArraySnapshot *after,
                              const char *context)
{
    printf("  ┌─ State comparison: %s\n", context);
    printf("  │  data pointer:  %s (before=%p, after=%p)\n",
           before->data_ptr == after->data_ptr ? "SAME ✓" : "CHANGED",
           before->data_ptr, after->data_ptr);
    printf("  │  size:          %s (before=%zu, after=%zu)\n",
           before->size == after->size ? "SAME ✓" : "CHANGED",
           before->size, after->size);
    printf("  │  capacity:      %s (before=%zu, after=%zu)\n",
           before->capacity == after->capacity ? "SAME ✓" : "CHANGED",
           before->capacity, after->capacity);

    if (before->size > 0) {
        printf("  │  first element: %s (before=%d, after=%d)\n",
               before->first_elem == after->first_elem ? "SAME ✓" : "CHANGED",
               before->first_elem, after->first_elem);
        printf("  │  last element:  %s (before=%d, after=%d)\n",
               before->last_elem == after->last_elem ? "SAME ✓" : "CHANGED",
               before->last_elem, after->last_elem);
    }
    printf("  └─\n\n");
}


/* ===========================================================================
 * 13. Graphviz DOT: Error flow diagram
 * ===========================================================================
 * Generates a diagram showing the success path vs failure path through
 * array_push. This is the conceptual diagram for the blog post — it
 * shows the decision tree, not the array state.
 * =========================================================================== */

static void generate_error_flow_dot(const char *filename)
{
    FILE *f = fopen(filename, "w");
    if (!f) {
        fprintf(stderr, "generate_error_flow_dot: cannot open %s\n", filename);
        return;
    }

    fprintf(f, "digraph ErrorFlow {\n");
    fprintf(f, "  rankdir=TB;\n");
    fprintf(f, "  graph [fontname=\"Helvetica\", fontsize=12, ");
    fprintf(f, "bgcolor=\"#fafafa\", label=\"array_push() Error Flow\\n"
               "Post 6: When Things Go Wrong\", labelloc=t];\n");
    fprintf(f, "  node [fontname=\"Helvetica\", fontsize=10, "
               "style=filled];\n");
    fprintf(f, "  edge [fontname=\"Helvetica\", fontsize=9];\n\n");

    /* Entry point */
    fprintf(f, "  entry [label=\"array_push(arr, elem)\", "
               "shape=oval, fillcolor=\"#e3f2fd\"];\n\n");

    /* Decision nodes */
    fprintf(f, "  null_check [label=\"arr == NULL\\nor elem == NULL?\", "
               "shape=diamond, fillcolor=\"#fff9c4\"];\n");
    fprintf(f, "  need_grow [label=\"size >= capacity?\", "
               "shape=diamond, fillcolor=\"#fff9c4\"];\n");
    fprintf(f, "  overflow [label=\"new_cap * elem_size\\noverflows?\", "
               "shape=diamond, fillcolor=\"#fff9c4\"];\n");
    fprintf(f, "  realloc_call [label=\"tmp = realloc(\\n"
               "  arr->data,\\n  new_size)\", "
               "shape=box, fillcolor=\"#e8f5e9\"];\n");
    fprintf(f, "  realloc_ok [label=\"tmp != NULL?\", "
               "shape=diamond, fillcolor=\"#fff9c4\"];\n\n");

    /* Success actions */
    fprintf(f, "  update_ptr [label=\"arr->data = tmp\\n"
               "arr->capacity = new_cap\", "
               "shape=box, fillcolor=\"#c8e6c9\"];\n");
    fprintf(f, "  do_copy [label=\"memcpy(slot, elem,\\n"
               "  element_size)\\narr->size++\", "
               "shape=box, fillcolor=\"#c8e6c9\"];\n");
    fprintf(f, "  success [label=\"return ARRAY_OK\", "
               "shape=oval, fillcolor=\"#a5d6a7\"];\n\n");

    /* Failure nodes */
    fprintf(f, "  err_null [label=\"return\\nARRAY_ERR_NULL\\n\\n"
               "Array: UNCHANGED\", "
               "shape=box, fillcolor=\"#ffcdd2\"];\n");
    fprintf(f, "  err_overflow [label=\"return\\nARRAY_ERR_OVERFLOW\\n\\n"
               "Array: UNCHANGED\", "
               "shape=box, fillcolor=\"#ffcdd2\"];\n");
    fprintf(f, "  err_alloc [label=\"return\\nARRAY_ERR_ALLOC\\n\\n"
               "arr->data: VALID\\n"
               "Old elements: INTACT\", "
               "shape=box, fillcolor=\"#ffcdd2\"];\n\n");

    /* Edges — success path */
    fprintf(f, "  entry -> null_check;\n");
    fprintf(f, "  null_check -> need_grow [label=\"no\", color=\"#2e7d32\"];\n");
    fprintf(f, "  need_grow -> do_copy [label=\"no\\n(room exists)\", "
               "color=\"#2e7d32\"];\n");
    fprintf(f, "  need_grow -> overflow [label=\"yes\", color=\"#f57c00\"];\n");
    fprintf(f, "  overflow -> realloc_call [label=\"no\", "
               "color=\"#2e7d32\"];\n");
    fprintf(f, "  realloc_call -> realloc_ok;\n");
    fprintf(f, "  realloc_ok -> update_ptr [label=\"yes\", "
               "color=\"#2e7d32\"];\n");
    fprintf(f, "  update_ptr -> do_copy;\n");
    fprintf(f, "  do_copy -> success;\n\n");

    /* Edges — failure path */
    fprintf(f, "  null_check -> err_null [label=\"yes\", "
               "color=\"#c62828\", style=dashed];\n");
    fprintf(f, "  overflow -> err_overflow [label=\"yes\", "
               "color=\"#c62828\", style=dashed];\n");
    fprintf(f, "  realloc_ok -> err_alloc [label=\"no (NULL)\", "
               "color=\"#c62828\", style=dashed];\n");

    /* Legend */
    fprintf(f, "\n  subgraph cluster_legend {\n");
    fprintf(f, "    label=\"Legend\";\n");
    fprintf(f, "    style=rounded; color=\"#bdbdbd\";\n");
    fprintf(f, "    fontname=\"Helvetica\"; fontsize=10;\n");
    fprintf(f, "    leg_ok [label=\"Success\", shape=box, "
               "fillcolor=\"#c8e6c9\"];\n");
    fprintf(f, "    leg_fail [label=\"Failure\\n(state preserved)\", "
               "shape=box, fillcolor=\"#ffcdd2\"];\n");
    fprintf(f, "    leg_decide [label=\"Decision\", shape=diamond, "
               "fillcolor=\"#fff9c4\"];\n");
    fprintf(f, "    leg_ok -> leg_fail [style=invis];\n");
    fprintf(f, "    leg_fail -> leg_decide [style=invis];\n");
    fprintf(f, "  }\n");

    fprintf(f, "}\n");
    fclose(f);

    printf("DOT file written to %s\n", filename);
    printf("Render with: dot -Tsvg %s -o output/post_06_error_flow.svg\n\n",
           filename);
}


/* ===========================================================================
 * 14. Demonstrations
 * =========================================================================== */

/* --- Demo 1: Error codes in action --- */

static void demo_error_codes(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Demo 1: Error codes — checking every return value\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    Array *arr = ARRAY_CREATE(int, 4);
    if (!arr) {
        printf("  Failed to create array!\n");
        return;
    }

    printf("  Created array with capacity 4.\n");
    printf("  Pushing 6 values (triggers growth at push #5):\n\n");

    for (int i = 1; i <= 6; i++) {
        int val = i * 100;
        ArrayError err = array_push(arr, &val);
        printf("    push(%d) → %s", val, array_error_str(err));
        if (i == 5) printf("  ← triggered realloc (cap 4→8)");
        printf("\n");
    }

    array_visualize_ascii(arr, "After 6 successful pushes", "int",
                          print_int, ARRAY_OK);

    /* Demonstrate NULL argument error */
    printf("  Pushing to NULL array:\n");
    ArrayError err = array_push(NULL, &(int){42});
    printf("    push(42) → %s\n\n", array_error_str(err));

    /* Demonstrate bounds error on insert */
    printf("  Inserting at index 100 (out of bounds):\n");
    int val = 999;
    err = array_insert(arr, 100, &val);
    printf("    insert(100, 999) → %s\n\n", array_error_str(err));

    /* Demonstrate valid insert */
    printf("  Inserting 50 at index 0 (shifts all elements right):\n");
    val = 50;
    err = array_insert(arr, 0, &val);
    printf("    insert(0, 50) → %s\n", array_error_str(err));

    array_visualize_ascii(arr, "After insert(0, 50)", "int",
                          print_int, err);

    array_destroy(arr);
}


/* --- Demo 2: State preservation on failure --- */

static void demo_state_preservation(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Demo 2: Array state preserved after failed operation\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    printf("  We cannot easily force a real realloc failure in a demo,\n");
    printf("  so we demonstrate the principle with overflow detection.\n\n");

    Array *arr = ARRAY_CREATE(int, 4);
    if (!arr) return;

    /* Fill with known data */
    for (int i = 1; i <= 4; i++) {
        int val = i * 10;
        array_push(arr, &val);
    }

    printf("  Array filled with [10, 20, 30, 40]:\n");
    array_visualize_ascii(arr, "Before failed operation", "int",
                          print_int, ARRAY_OK);

    /* Take snapshot BEFORE the failing operation */
    ArraySnapshot before = take_snapshot(arr);

    /* Try to insert at an invalid index */
    int val = 999;
    ArrayError err = array_insert(arr, 100, &val);
    printf("  Attempted insert at index 100: %s\n\n", array_error_str(err));

    /* Take snapshot AFTER the failing operation */
    ArraySnapshot after = take_snapshot(arr);

    /* Compare: they should be identical */
    compare_snapshots(&before, &after, "after failed insert");

    array_visualize_ascii(arr, "After failed operation — unchanged", "int",
                          print_int, err);

    /* Demonstrate that the array still works perfectly */
    printf("  Array still works — pushing 50:\n");
    val = 50;
    err = array_push(arr, &val);
    printf("    push(50) → %s\n", array_error_str(err));
    array_visualize_ascii(arr, "Array recovered, push succeeded", "int",
                          print_int, err);

    array_destroy(arr);
}


/* --- Demo 3: Error callback mechanism --- */

static int g_callback_count = 0;

static void my_error_handler(ArrayError err, const char *msg,
                             const char *file, int line)
{
    g_callback_count++;
    printf("    [CALLBACK #%d] error=%d (%s)\n",
           g_callback_count, err, array_error_str(err));
    printf("                  msg: %s\n", msg);
    printf("                  at:  %s:%d\n", file, line);
}

static void demo_error_callback(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Demo 3: Error callbacks — centralized error handling\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    /* Register our handler */
    array_set_error_callback(my_error_handler);
    g_callback_count = 0;

    printf("  Registered error callback. Now triggering some errors:\n\n");

    /* Trigger NULL error */
    printf("  1. Pushing to NULL array:\n");
    array_push(NULL, &(int){42});
    printf("\n");

    /* Trigger bounds error */
    Array *arr = ARRAY_CREATE(int, 4);
    int val = 10;
    array_push(arr, &val);

    printf("  2. Inserting at invalid index:\n");
    val = 999;
    array_insert(arr, 100, &val);
    printf("\n");

    /* Trigger overflow error */
    printf("  3. Creating array with overflowing size:\n");
    Array *bad = array_create(SIZE_MAX, 2);
    (void)bad;
    printf("\n");

    printf("  Total callbacks fired: %d\n\n", g_callback_count);

    printf("  Notice: the callback logged the errors, but the functions\n");
    printf("  still returned error codes. The callback is for monitoring;\n");
    printf("  error codes are for control flow.\n\n");

    /* Clean up and disable callback */
    array_destroy(arr);
    array_set_error_callback(NULL);
}


/* --- Demo 4: Strategy comparison --- */

static void demo_strategy_comparison(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Demo 4: Three error handling strategies compared\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    printf("  Strategy A: Return Codes (what we use)\n");
    printf("  ─────────────────────────────────────────\n");
    printf("  ArrayError err = array_push(arr, &val);\n");
    printf("  if (err != ARRAY_OK) {\n");
    printf("      fprintf(stderr, \"push failed: %%s\\n\",\n");
    printf("              array_error_str(err));\n");
    printf("      // handle error: retry, degrade, abort\n");
    printf("  }\n\n");

    printf("  Pros: explicit, caller decides how to react,\n");
    printf("        no hidden control flow, debugger-friendly.\n");
    printf("  Cons: verbose (every call needs an if-check),\n");
    printf("        easy to forget checking (ignoring the return).\n\n");

    printf("  ───────────────────────────────────────────\n");
    printf("  Strategy B: Callback (our optional addition)\n");
    printf("  ─────────────────────────────────────────────\n");
    printf("  array_set_error_callback(my_handler);\n");
    printf("  array_push(arr, &val);  // handler called on error\n\n");

    printf("  Pros: centralized logging, no code at each call site,\n");
    printf("        good for metrics and monitoring.\n");
    printf("  Cons: doesn't replace return-code checking,\n");
    printf("        global state (callback pointer),\n");
    printf("        complex if handler needs context.\n\n");

    printf("  ───────────────────────────────────────────\n");
    printf("  Strategy C: Abort (not recommended)\n");
    printf("  ─────────────────────────────────────────\n");
    printf("  void array_push_or_die(Array *arr, const void *elem) {\n");
    printf("      if (array_push(arr, elem) != ARRAY_OK) {\n");
    printf("          fprintf(stderr, \"FATAL: push failed\\n\");\n");
    printf("          abort();\n");
    printf("      }\n");
    printf("  }\n\n");

    printf("  Pros: dead simple, no error paths to test,\n");
    printf("        fine for quick prototypes and scripts.\n");
    printf("  Cons: no recovery possible, unsuitable for libraries,\n");
    printf("        crashes the entire program on ANY allocation failure,\n");
    printf("        makes the library hostile to embedders.\n\n");

    printf("  ═══════════════════════════════════════════\n");
    printf("  Our choice: A (return codes) as primary mechanism,\n");
    printf("  with B (callback) as optional monitoring layer.\n");
    printf("  Never C for library code — let the caller decide.\n");
    printf("  ═══════════════════════════════════════════\n\n");
}


/* --- Demo 5: The ARRAY_PUSH_ERR macro --- */

static void demo_push_err_macro(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Demo 5: ARRAY_PUSH_ERR — error codes from macro layer\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    Array *arr = ARRAY_CREATE(int, 4);
    if (!arr) return;

    printf("  Using ARRAY_PUSH_ERR to capture error codes:\n\n");

    ArrayError err;
    for (int i = 1; i <= 5; i++) {
        ARRAY_PUSH_ERR(arr, int, i * 10, err);
        printf("    ARRAY_PUSH_ERR(arr, int, %d, err) → %s\n",
               i * 10, array_error_str(err));
    }

    printf("\n  Attempting type mismatch:\n");
    ARRAY_PUSH_ERR(arr, double, 3.14, err);
    printf("    ARRAY_PUSH_ERR(arr, double, 3.14, err) → %s\n",
           array_error_str(err));

    printf("\n  Result: type mismatch detected BEFORE calling array_push.\n");
    printf("  The macro layer catches it; the void* layer never sees it.\n\n");

    array_visualize_ascii(arr, "Final state (only ints pushed successfully)",
                          "int", print_int, ARRAY_OK);

    array_destroy(arr);
}


/* --- Knowledge test --- */

static void knowledge_test(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Knowledge Test\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    printf("  Q: If realloc fails, what value does arr->data still\n");
    printf("     hold? Why must you use a temporary pointer?\n\n");

    printf("  A: arr->data still holds its ORIGINAL value — the\n");
    printf("     pointer to the existing, valid buffer with all\n");
    printf("     elements intact.\n\n");

    printf("     When realloc fails, it returns NULL but does NOT\n");
    printf("     free the original block. The old memory is still\n");
    printf("     allocated and usable.\n\n");

    printf("     You must use a temporary pointer because:\n\n");

    printf("       void *tmp = realloc(arr->data, new_size);\n");
    printf("       if (!tmp) return ERR;  // arr->data unchanged!\n");
    printf("       arr->data = tmp;        // update only on success\n\n");

    printf("     If you wrote:\n");
    printf("       arr->data = realloc(arr->data, new_size); // WRONG\n\n");

    printf("     ...and realloc returned NULL, then:\n");
    printf("       - arr->data is now NULL\n");
    printf("       - The old buffer is still allocated (leaked)\n");
    printf("       - All your data is lost AND memory is leaked\n");
    printf("       - Double failure: data loss + resource leak\n\n");

    printf("     The temporary pointer costs one extra local variable.\n");
    printf("     It prevents data loss. Always use it.\n");
}


/* ===========================================================================
 * main
 * =========================================================================== */

int main(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Post 6: When Things Go Wrong —\n");
    printf("          Error Handling Strategies in C\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    demo_error_codes();
    demo_state_preservation();
    demo_error_callback();
    demo_strategy_comparison();
    demo_push_err_macro();

    /* Generate the error flow diagram */
    generate_error_flow_dot("output/post_06_error_flow.dot");

    knowledge_test();

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("  End of Post 6. Next: function pointers and callbacks.\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    return 0;
}
