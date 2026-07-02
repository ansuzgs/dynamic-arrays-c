/* ============================================================================
 * Post 8: "Bounds Checking and Defensive APIs"
 * ============================================================================
 *
 * This file is entirely self-contained. Compile with:
 *   gcc -Wall -Wextra -std=c11 -o build/post_08 src_posts/post_08.c
 *
 * Run:
 *   ./build/post_08                       (ASCII visualization to stdout)
 *   ./build/post_08 > output/post_08.txt (save ASCII output)
 *
 * The program also writes output/post_08_api_layers.dot (Graphviz).
 * Render with:  dot -Tsvg output/post_08_api_layers.dot \
 *                        -o output/post_08_api_layers.svg
 *
 * Learning outcome: implement configurable bounds checking (assert in debug,
 * return error in release) and document API contracts. Understand the
 * tradeoff between safety and performance across build modes, and the
 * #ifdef pattern for dual-mode checking.
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

/* ===========================================================================
 * 1. Build mode detection — THIS POST'S FOUNDATION
 * ===========================================================================
 *
 * The C preprocessor gives us two build modes:
 *
 *   DEBUG:   gcc -DDEBUG -g -O0 ...     → asserts enabled, no optimization
 *   RELEASE: gcc -DNDEBUG -O2 ...       → asserts disabled, full optimization
 *
 * NDEBUG is the standard C macro that controls assert(). When defined,
 * assert() expands to nothing — zero overhead, zero safety. We use this
 * same mechanism to control our own bounds checking.
 *
 * Our three-level strategy:
 *
 *   Level 0 (NDEBUG defined):    No checks. Zero overhead. Caller responsible.
 *   Level 1 (default):           Runtime checks return error codes.
 *   Level 2 (ARRAY_DEBUG_CHECKS): Asserts + runtime checks. Crash on bugs.
 *
 * The user controls the level via compiler flags:
 *   -DNDEBUG                  → Level 0 (release, max performance)
 *   (nothing)                 → Level 1 (default, safe everywhere)
 *   -DARRAY_DEBUG_CHECKS      → Level 2 (debug, crash early on violations)
 * =========================================================================== */

/* If NDEBUG is defined, assert() is a no-op. We mirror that for our macros. */

#ifdef NDEBUG
  /* Level 0: trust the caller. No debug checks at all. */
  #define ARRAY_ASSERT(cond, msg) ((void)0)
  #define ARRAY_BOUNDS_CHECK_ENABLED 0
#elif defined(ARRAY_DEBUG_CHECKS)
  /* Level 2: crash immediately on contract violation. */
  #define ARRAY_ASSERT(cond, msg) \
      do { \
          if (!(cond)) { \
              fprintf(stderr, "ARRAY ASSERTION FAILED: %s\n  %s:%d\n", \
                      (msg), __FILE__, __LINE__); \
              abort(); \
          } \
      } while (0)
  #define ARRAY_BOUNDS_CHECK_ENABLED 1
#else
  /* Level 1: default. Runtime checks return errors. No asserts. */
  #define ARRAY_ASSERT(cond, msg) ((void)0)
  #define ARRAY_BOUNDS_CHECK_ENABLED 1
#endif


/* ===========================================================================
 * 2. Error codes (carried forward from Post 6)
 * =========================================================================== */

typedef enum {
    ARRAY_OK             =  0,
    ARRAY_ERR_NULL       = -1,
    ARRAY_ERR_ALLOC      = -2,
    ARRAY_ERR_BOUNDS     = -3,
    ARRAY_ERR_OVERFLOW   = -4,
    ARRAY_ERR_FULL       = -5,
    ARRAY_ERR_TYPE       = -6,
    ARRAY_ERR_CONTRACT   = -7   /* NEW: API contract violation */
} ArrayError;

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
        case ARRAY_ERR_CONTRACT: return "API contract violation";
    }
    return "unknown error";
}


/* ===========================================================================
 * 3. Error callback (carried forward from Post 6)
 * =========================================================================== */

typedef void (*ArrayErrorCallback)(ArrayError err, const char *msg,
                                   const char *file, int line);

static ArrayErrorCallback g_error_callback = NULL;

void array_set_error_callback(ArrayErrorCallback cb)
{
    g_error_callback = cb;
}

#define ARRAY_REPORT_ERROR(err, msg) \
    do { \
        if (g_error_callback) { \
            g_error_callback((err), (msg), __FILE__, __LINE__); \
        } \
    } while (0)


/* ===========================================================================
 * 4. Function pointer types (carried forward from Post 7)
 * =========================================================================== */

typedef int  (*ArrayCompareFn)(const void *a, const void *b);
typedef void (*ArrayDestroyFn)(void *element);
typedef void (*ArrayForeachFn)(void *element, void *context);
typedef int  (*ArrayPredicateFn)(const void *element, const void *context);
typedef void (*PrintElementFn)(char *buf, size_t bufsize, const void *elem);


/* ===========================================================================
 * 5. The Array struct (unchanged from Post 7)
 * =========================================================================== */

typedef struct {
    void          *data;
    size_t         size;
    size_t         capacity;
    size_t         element_size;
    size_t         realloc_count;
    ArrayDestroyFn destroy_fn;
} Array;


/* ===========================================================================
 * 6. Lifecycle (unchanged from Post 7)
 * =========================================================================== */

Array *array_create(size_t element_size, size_t initial_capacity)
{
    /* Contract: both must be positive. */
    ARRAY_ASSERT(element_size > 0, "element_size must be > 0");
    ARRAY_ASSERT(initial_capacity > 0, "initial_capacity must be > 0");

    if (element_size == 0 || initial_capacity == 0) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_CONTRACT,
                           "array_create: element_size and capacity must be > 0");
        return NULL;
    }

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
        free(arr);
        return NULL;
    }

    arr->size          = 0;
    arr->capacity      = initial_capacity;
    arr->element_size  = element_size;
    arr->realloc_count = 0;
    arr->destroy_fn    = NULL;

    return arr;
}

void array_set_destroy_fn(Array *arr, ArrayDestroyFn fn)
{
    if (arr) arr->destroy_fn = fn;
}

void array_destroy(Array *arr)
{
    if (!arr) return;

    if (arr->destroy_fn) {
        for (size_t i = 0; i < arr->size; i++) {
            void *elem = (char *)arr->data + i * arr->element_size;
            arr->destroy_fn(elem);
        }
    }

    free(arr->data);
    arr->data     = NULL;
    arr->size     = 0;
    arr->capacity = 0;
    free(arr);
}


/* ===========================================================================
 * 7. Internal helpers — THE UNCHECKED LAYER
 * ===========================================================================
 *
 * These functions are the private, unchecked core. They do NOT validate
 * arguments — they trust that the caller (the public API) has already
 * verified everything. This is the "fast path" that never redundantly
 * checks what's already been checked.
 *
 * API CONTRACT (preconditions):
 *   element_at_unchecked: arr != NULL, arr->data != NULL, index < capacity
 *   ensure_capacity_unchecked: arr != NULL, min_capacity > 0
 *
 * Naming convention: the _unchecked suffix is a SIGNAL. It tells
 * maintainers "this function trusts its inputs — the caller is responsible
 * for validation." In a code review, seeing a call to _unchecked without
 * preceding validation is a red flag.
 * =========================================================================== */

/* Returns a pointer to the element at `index`. No bounds check. */
static void *element_at_unchecked(const Array *arr, size_t index)
{
    return (char *)arr->data + index * arr->element_size;
}

static ArrayError ensure_capacity_unchecked(Array *arr, size_t min_capacity)
{
    if (arr->capacity >= min_capacity) {
        return ARRAY_OK;
    }

    size_t new_cap = arr->capacity * 2;
    if (new_cap < min_capacity) {
        new_cap = min_capacity;
    }

    if (new_cap > SIZE_MAX / arr->element_size) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_OVERFLOW,
                           "ensure_capacity: new size overflows size_t");
        return ARRAY_ERR_OVERFLOW;
    }

    void *tmp = realloc(arr->data, new_cap * arr->element_size);
    if (!tmp) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_ALLOC,
                           "ensure_capacity: realloc failed");
        return ARRAY_ERR_ALLOC;
    }

    arr->data     = tmp;
    arr->capacity = new_cap;
    arr->realloc_count++;

    return ARRAY_OK;
}


/* ===========================================================================
 * 8. The Checked Public API — THIS POST'S CORE CONTRIBUTION
 * ===========================================================================
 *
 * Every public function follows the same structure:
 *
 *   1. ASSERT preconditions (debug builds: crash immediately on violation)
 *   2. VALIDATE preconditions (all builds: return error code on violation)
 *   3. Call the unchecked internal function to do the actual work
 *
 * In debug builds (ARRAY_DEBUG_CHECKS), step 1 catches bugs before step 2
 * even runs — the assert fires, the program aborts with a clear message,
 * and the developer sees the exact line of the violation.
 *
 * In release builds (NDEBUG), step 1 is compiled away to nothing. Step 2
 * still runs, returning an error code that the caller can handle. The
 * penalty is one or two comparisons per call — measurable in tight loops,
 * irrelevant in most programs.
 *
 * In default builds (neither flag), step 1 is a no-op and step 2 provides
 * safety. This is the "safe by default" configuration.
 * =========================================================================== */


/* --- array_push: append an element --- */

ArrayError array_push(Array *arr, const void *element)
{
    /* Contract: arr and element must not be NULL */
    ARRAY_ASSERT(arr != NULL, "array_push: arr must not be NULL");
    ARRAY_ASSERT(element != NULL, "array_push: element must not be NULL");

    /* Runtime validation (survives all build modes except NDEBUG-only API) */
    if (!arr || !element) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_NULL, "array_push: NULL argument");
        return ARRAY_ERR_NULL;
    }

    ArrayError err = ensure_capacity_unchecked(arr, arr->size + 1);
    if (err != ARRAY_OK) return err;

    memcpy(element_at_unchecked(arr, arr->size), element, arr->element_size);
    arr->size++;

    return ARRAY_OK;
}


/* --- array_get: safe read access (checked) --- */

void *array_get(const Array *arr, size_t index)
{
    /* Contract: arr not NULL, index in bounds */
    ARRAY_ASSERT(arr != NULL, "array_get: arr must not be NULL");
    ARRAY_ASSERT(index < arr->size, "array_get: index out of bounds");

    if (!arr || index >= arr->size) {
        return NULL;
    }

    return element_at_unchecked(arr, index);
}


/* --- array_get_unchecked: zero-overhead read (no validation at all) ---
 *
 * This is the escape hatch for performance-critical code. The caller
 * is 100% responsible for ensuring arr != NULL and index < arr->size.
 * If those preconditions are violated, behavior is undefined.
 *
 * Use case: inner loops where you've already validated the range.
 *
 *   for (size_t i = 0; i < array_size(arr); i++) {
 *       int *p = array_get_unchecked(arr, i);  // safe: i < size
 *       total += *p;
 *   }
 *
 * The loop bounds guarantee the index is valid. Checking inside
 * array_get would be redundant — paying the branch cost N times
 * for a condition that's provably true.
 */
void *array_get_unchecked(const Array *arr, size_t index)
{
    return element_at_unchecked(arr, index);
}


/* --- array_set: safe write access (checked) --- */

ArrayError array_set(Array *arr, size_t index, const void *element)
{
    ARRAY_ASSERT(arr != NULL, "array_set: arr must not be NULL");
    ARRAY_ASSERT(element != NULL, "array_set: element must not be NULL");
    ARRAY_ASSERT(index < arr->size, "array_set: index out of bounds");

    if (!arr || !element) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_NULL, "array_set: NULL argument");
        return ARRAY_ERR_NULL;
    }
    if (index >= arr->size) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_BOUNDS, "array_set: index out of range");
        return ARRAY_ERR_BOUNDS;
    }

    /* Destroy old element if destructor is set */
    if (arr->destroy_fn) {
        arr->destroy_fn(element_at_unchecked(arr, index));
    }

    memcpy(element_at_unchecked(arr, index), element, arr->element_size);
    return ARRAY_OK;
}


/* --- array_insert: insert at index, shifting right --- */

ArrayError array_insert(Array *arr, size_t index, const void *element)
{
    ARRAY_ASSERT(arr != NULL, "array_insert: arr must not be NULL");
    ARRAY_ASSERT(element != NULL, "array_insert: element must not be NULL");
    ARRAY_ASSERT(index <= arr->size, "array_insert: index out of bounds");

    if (!arr || !element) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_NULL, "array_insert: NULL argument");
        return ARRAY_ERR_NULL;
    }
    if (index > arr->size) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_BOUNDS,
                           "array_insert: index out of range");
        return ARRAY_ERR_BOUNDS;
    }

    ArrayError err = ensure_capacity_unchecked(arr, arr->size + 1);
    if (err != ARRAY_OK) return err;

    /* Shift elements right to make room */
    if (index < arr->size) {
        void *dst = element_at_unchecked(arr, index + 1);
        void *src = element_at_unchecked(arr, index);
        size_t bytes = (arr->size - index) * arr->element_size;
        memmove(dst, src, bytes);
    }

    memcpy(element_at_unchecked(arr, index), element, arr->element_size);
    arr->size++;

    return ARRAY_OK;
}


/* --- array_remove: remove at index, shifting left --- */

ArrayError array_remove(Array *arr, size_t index)
{
    ARRAY_ASSERT(arr != NULL, "array_remove: arr must not be NULL");
    ARRAY_ASSERT(index < arr->size, "array_remove: index out of bounds");

    if (!arr) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_NULL, "array_remove: NULL array");
        return ARRAY_ERR_NULL;
    }
    if (index >= arr->size) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_BOUNDS,
                           "array_remove: index out of range");
        return ARRAY_ERR_BOUNDS;
    }

    if (arr->destroy_fn) {
        arr->destroy_fn(element_at_unchecked(arr, index));
    }

    if (index < arr->size - 1) {
        void *dst = element_at_unchecked(arr, index);
        void *src = element_at_unchecked(arr, index + 1);
        size_t bytes = (arr->size - index - 1) * arr->element_size;
        memmove(dst, src, bytes);
    }

    arr->size--;
    return ARRAY_OK;
}


/* --- Query functions --- */

size_t array_size(const Array *arr)          { return arr ? arr->size          : 0; }
size_t array_capacity(const Array *arr)      { return arr ? arr->capacity      : 0; }
size_t array_realloc_count(const Array *arr) { return arr ? arr->realloc_count : 0; }


/* --- Callback operations (from Post 7) --- */

ArrayError array_sort(Array *arr, ArrayCompareFn compare)
{
    ARRAY_ASSERT(arr != NULL, "array_sort: arr must not be NULL");
    ARRAY_ASSERT(compare != NULL, "array_sort: comparator must not be NULL");

    if (!arr) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_NULL, "array_sort: NULL array");
        return ARRAY_ERR_NULL;
    }
    if (!compare) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_NULL, "array_sort: NULL comparator");
        return ARRAY_ERR_NULL;
    }

    if (arr->size > 1) {
        qsort(arr->data, arr->size, arr->element_size, compare);
    }

    return ARRAY_OK;
}

ArrayError array_foreach(Array *arr, ArrayForeachFn fn, void *context)
{
    ARRAY_ASSERT(arr != NULL, "array_foreach: arr must not be NULL");
    ARRAY_ASSERT(fn != NULL, "array_foreach: callback must not be NULL");

    if (!arr) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_NULL, "array_foreach: NULL array");
        return ARRAY_ERR_NULL;
    }
    if (!fn) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_NULL, "array_foreach: NULL callback");
        return ARRAY_ERR_NULL;
    }

    for (size_t i = 0; i < arr->size; i++) {
        fn(element_at_unchecked(arr, i), context);
    }

    return ARRAY_OK;
}

void *array_find(const Array *arr, ArrayPredicateFn predicate,
                 const void *context, size_t *out_index)
{
    if (!arr || !predicate) return NULL;

    for (size_t i = 0; i < arr->size; i++) {
        void *elem = element_at_unchecked(arr, i);
        if (predicate(elem, context)) {
            if (out_index) *out_index = i;
            return elem;
        }
    }

    return NULL;
}


/* ===========================================================================
 * 9. Type-safe macros (carried forward)
 * =========================================================================== */

#define ARRAY_CREATE(type, capacity) \
    array_create(sizeof(type), (capacity))

#define ARRAY_PUSH(arr, type, value) \
    do { \
        if (sizeof(type) != (arr)->element_size) { \
            fprintf(stderr, \
                "ARRAY_PUSH: type size mismatch (sizeof(%s)=%zu, " \
                "element_size=%zu) at %s:%d\n", \
                #type, sizeof(type), (arr)->element_size, \
                __FILE__, __LINE__); \
        } else { \
            type _push_tmp = (value); \
            (void)array_push((arr), &_push_tmp); \
        } \
    } while (0)

#define ARRAY_GET(arr, type, index) \
    ( sizeof(type) != (arr)->element_size \
      ? ( fprintf(stderr, \
              "ARRAY_GET: type size mismatch at %s:%d\n", \
              __FILE__, __LINE__), \
          (type *)NULL ) \
      : (type *)array_get((arr), (index)) )

#define ARRAY_GET_UNCHECKED(arr, type, index) \
    ( (type *)array_get_unchecked((arr), (index)) )

#define ARRAY_SET(arr, type, index, value) \
    do { \
        if (sizeof(type) != (arr)->element_size) { \
            fprintf(stderr, \
                "ARRAY_SET: type size mismatch at %s:%d\n", \
                __FILE__, __LINE__); \
        } else { \
            type _set_tmp = (value); \
            (void)array_set((arr), (index), &_set_tmp); \
        } \
    } while (0)

#define ARRAY_FOREACH_TYPED(arr, type, var_name) \
    for (size_t _fe_i = 0; _fe_i < (arr)->size; _fe_i++) \
        for (type *var_name = (type *)element_at_unchecked((arr), _fe_i); \
             var_name; var_name = NULL)


/* ===========================================================================
 * 10. ASCII visualization — ENHANCED for bounds checking
 * ===========================================================================
 *
 * This version shows the valid vs invalid access regions of the array.
 * Valid region:   indices 0..size-1 (filled elements)
 * Allocated but unused: indices size..capacity-1 (accessible but uninitialized)
 * Out of bounds:  indices >= capacity (undefined behavior territory)
 *
 * The visualization marks each region with a different indicator so
 * the reader can see exactly where checked vs unchecked access diverges.
 * =========================================================================== */

static void print_int(char *buf, size_t bufsize, const void *elem)
{
    snprintf(buf, bufsize, "%d", *(const int *)elem);
}

static void array_visualize_ascii(const Array *arr, const char *label,
                                  const char *type_label,
                                  PrintElementFn printer)
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
    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║  type: %-8s  element_size: %-4zu  reallocs: %-4zu      ║\n",
           type_label ? type_label : "void",
           arr->element_size, arr->realloc_count);
    printf("║  size: %-8zu  capacity: %-8zu                        ║\n",
           sz, cap);

    /* Build mode info */
    const char *mode =
    #ifdef NDEBUG
        "RELEASE (no checks)"
    #elif defined(ARRAY_DEBUG_CHECKS)
        "DEBUG (asserts + checks)"
    #else
        "DEFAULT (runtime checks)"
    #endif
    ;
    printf("║  build mode: %-43s  ║\n", mode);

    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║  Legend: ✓ = valid  · = allocated/unused  ✗ = OOB      ║\n");
    printf("╠══════════════════════════════════════════════════════════╣\n");

    if (printer) {
        for (size_t i = 0; i < show; i++) {
            char val_buf[32] = "";
            const char *marker;

            if (i < sz) {
                printer(val_buf, sizeof(val_buf),
                        element_at_unchecked(arr, i));
                marker = "✓";
            } else {
                snprintf(val_buf, sizeof(val_buf), "(unused)");
                marker = "·";
            }
            printf("║  %s [%2zu] +%-4zu  │ %-36s  ║\n",
                   marker, i, i * arr->element_size, val_buf);
        }

        if (cap > show) {
            printf("║  · ... %zu more unused slots not shown %-16s  ║\n",
                   cap - show, "");
        }

        /* Show the out-of-bounds region */
        printf("║  ✗ [%2zu] +%-4zu  │ %-36s  ║\n",
               cap, cap * arr->element_size,
               "OUT OF BOUNDS — undefined behavior");
        printf("║  ✗ [%2zu] +%-4zu  │ %-36s  ║\n",
               cap + 1, (cap + 1) * arr->element_size,
               "OUT OF BOUNDS — undefined behavior");
    }

    size_t used_bytes  = sz  * arr->element_size;
    size_t alloc_bytes = cap * arr->element_size;
    double util = cap > 0 ? 100.0 * (double)sz / (double)cap : 0.0;

    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║  %zuB used / %zuB allocated = %.1f%% utilization",
           used_bytes, alloc_bytes, util);
    int padding = 56 - snprintf(NULL, 0,
        "  %zuB used / %zuB allocated = %.1f%% utilization",
        used_bytes, alloc_bytes, util);
    for (int i = 0; i < padding; i++) printf(" ");
    printf("║\n");

    /* Access summary */
    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║  array_get(arr, %zu)       → ", sz > 0 ? sz - 1 : (size_t)0);
    if (sz > 0)
        printf("%-28s", "valid (last element)");
    else
        printf("%-28s", "NULL (empty array)");
    printf("  ║\n");
    printf("║  array_get(arr, %zu)       → %-28s  ║\n",
           sz, "NULL (out of bounds)");
    printf("║  array_get_unchecked(arr, %zu) → %-24s  ║\n",
           sz, "UNDEFINED BEHAVIOR!");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");
}


/* ===========================================================================
 * 11. Graphviz DOT: API layers (checked → unchecked)
 * ===========================================================================
 *
 * This generates a diagram showing the two-layer architecture:
 *   - Public API (checked): array_get, array_set, array_push, etc.
 *   - Private core (unchecked): element_at_unchecked, ensure_capacity_unchecked
 *
 * The diagram shows how the public layer validates, then delegates
 * to the private layer. The escape hatch (array_get_unchecked) bypasses
 * the public checks entirely.
 * =========================================================================== */

static void generate_api_layers_dot(const char *filename)
{
    FILE *f = fopen(filename, "w");
    if (!f) {
        fprintf(stderr, "generate_api_layers_dot: cannot open %s\n", filename);
        return;
    }

    fprintf(f, "digraph APILayers {\n");
    fprintf(f, "  rankdir=TB;\n");
    fprintf(f, "  graph [fontname=\"Helvetica\", fontsize=12, "
               "bgcolor=\"#fafafa\",\n");
    fprintf(f, "         label=\"API Layers: Checked → Unchecked\\n"
               "Post 8: Bounds Checking and Defensive APIs\", labelloc=t];\n");
    fprintf(f, "  node [fontname=\"Helvetica\", fontsize=10, style=filled];\n");
    fprintf(f, "  edge [fontname=\"Helvetica\", fontsize=9];\n\n");

    /* Caller */
    fprintf(f, "  caller [label=\"Caller Code\", shape=oval, "
               "fillcolor=\"#f3e5f5\"];\n\n");

    /* Public checked layer */
    fprintf(f, "  subgraph cluster_public {\n");
    fprintf(f, "    label=\"Public API (checked)\";\n");
    fprintf(f, "    style=rounded; color=\"#2e7d32\"; penwidth=2;\n");
    fprintf(f, "    fontname=\"Helvetica\"; fontsize=11;\n\n");
    fprintf(f, "    get [label=\"array_get()\\n"
               "NULL check + bounds check\\n"
               "returns NULL on failure\", "
               "shape=box, fillcolor=\"#c8e6c9\"];\n");
    fprintf(f, "    set [label=\"array_set()\\n"
               "NULL check + bounds check\\n"
               "returns error code\", "
               "shape=box, fillcolor=\"#c8e6c9\"];\n");
    fprintf(f, "    push [label=\"array_push()\\n"
               "NULL check + capacity check\\n"
               "returns error code\", "
               "shape=box, fillcolor=\"#c8e6c9\"];\n");
    fprintf(f, "    insert [label=\"array_insert()\\n"
               "NULL check + bounds check\\n"
               "returns error code\", "
               "shape=box, fillcolor=\"#c8e6c9\"];\n");
    fprintf(f, "    remove [label=\"array_remove()\\n"
               "NULL check + bounds check\\n"
               "returns error code\", "
               "shape=box, fillcolor=\"#c8e6c9\"];\n");
    fprintf(f, "  }\n\n");

    /* Escape hatch */
    fprintf(f, "  get_unchecked [label=\"array_get_unchecked()\\n"
               "NO validation\\n"
               "caller responsible\", "
               "shape=box, fillcolor=\"#ffcdd2\"];\n\n");

    /* Private unchecked layer */
    fprintf(f, "  subgraph cluster_private {\n");
    fprintf(f, "    label=\"Private Core (unchecked)\";\n");
    fprintf(f, "    style=rounded; color=\"#c62828\"; penwidth=2;\n");
    fprintf(f, "    fontname=\"Helvetica\"; fontsize=11;\n\n");
    fprintf(f, "    elem_at [label=\"element_at_unchecked()\\n"
               "raw pointer arithmetic\\n"
               "trusts all inputs\", "
               "shape=box, fillcolor=\"#ffcdd2\"];\n");
    fprintf(f, "    ensure_cap [label=\"ensure_capacity_unchecked()\\n"
               "growth + realloc\\n"
               "trusts arr != NULL\", "
               "shape=box, fillcolor=\"#ffcdd2\"];\n");
    fprintf(f, "  }\n\n");

    /* Debug assert layer (conditional) */
    fprintf(f, "  subgraph cluster_debug {\n");
    fprintf(f, "    label=\"Debug Layer (compile-time conditional)\";\n");
    fprintf(f, "    style=dashed; color=\"#ff8f00\";\n");
    fprintf(f, "    fontname=\"Helvetica\"; fontsize=11;\n\n");
    fprintf(f, "    assert_layer [label=\"ARRAY_ASSERT()\\n"
               "Level 2: abort() on violation\\n"
               "Level 0-1: compiled away\", "
               "shape=diamond, fillcolor=\"#fff9c4\"];\n");
    fprintf(f, "  }\n\n");

    /* Edges: caller → public */
    fprintf(f, "  caller -> get [label=\"safe path\"];\n");
    fprintf(f, "  caller -> set [label=\"safe path\"];\n");
    fprintf(f, "  caller -> push [label=\"safe path\"];\n");
    fprintf(f, "  caller -> insert [label=\"safe path\"];\n");
    fprintf(f, "  caller -> remove [label=\"safe path\"];\n");
    fprintf(f, "  caller -> get_unchecked "
               "[label=\"perf escape hatch\", style=dashed, "
               "color=\"#c62828\"];\n\n");

    /* Public → assert → private */
    fprintf(f, "  get -> assert_layer [style=dotted, "
               "color=\"#ff8f00\"];\n");
    fprintf(f, "  set -> assert_layer [style=dotted, "
               "color=\"#ff8f00\"];\n");
    fprintf(f, "  push -> assert_layer [style=dotted, "
               "color=\"#ff8f00\"];\n");
    fprintf(f, "  assert_layer -> elem_at "
               "[style=dotted, color=\"#ff8f00\"];\n\n");

    fprintf(f, "  get -> elem_at;\n");
    fprintf(f, "  set -> elem_at;\n");
    fprintf(f, "  push -> elem_at;\n");
    fprintf(f, "  push -> ensure_cap;\n");
    fprintf(f, "  insert -> elem_at;\n");
    fprintf(f, "  insert -> ensure_cap;\n");
    fprintf(f, "  remove -> elem_at;\n\n");

    /* Unchecked → private directly */
    fprintf(f, "  get_unchecked -> elem_at "
               "[color=\"#c62828\", label=\"direct\\n(no checks)\"];\n\n");

    /* Legend */
    fprintf(f, "  subgraph cluster_legend {\n");
    fprintf(f, "    label=\"Legend\";\n");
    fprintf(f, "    style=rounded; color=\"#bdbdbd\";\n");
    fprintf(f, "    fontname=\"Helvetica\"; fontsize=10;\n");
    fprintf(f, "    leg_safe [label=\"Checked (safe)\", shape=box, "
               "fillcolor=\"#c8e6c9\"];\n");
    fprintf(f, "    leg_unsafe [label=\"Unchecked (fast)\", shape=box, "
               "fillcolor=\"#ffcdd2\"];\n");
    fprintf(f, "    leg_debug [label=\"Debug-only\", shape=diamond, "
               "fillcolor=\"#fff9c4\"];\n");
    fprintf(f, "    leg_safe -> leg_unsafe [style=invis];\n");
    fprintf(f, "    leg_unsafe -> leg_debug [style=invis];\n");
    fprintf(f, "  }\n");

    fprintf(f, "}\n");
    fclose(f);

    printf("DOT file written to %s\n", filename);
    printf("Render: dot -Tsvg %s -o output/post_08_api_layers.svg\n\n",
           filename);
}


/* ===========================================================================
 * 12. Demonstrations
 * =========================================================================== */

/* --- Demo 1: Checked vs unchecked access --- */

static void demo_checked_vs_unchecked(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Demo 1: Checked vs Unchecked Access\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    Array *arr = ARRAY_CREATE(int, 8);
    if (!arr) return;

    /* Fill with known values */
    for (int i = 0; i < 5; i++) {
        int val = (i + 1) * 100;
        array_push(arr, &val);
    }

    array_visualize_ascii(arr, "5 elements in capacity-8 array", "int",
                          print_int);

    /* Safe access: within bounds */
    printf("  Safe access (checked):\n");
    int *p = ARRAY_GET(arr, int, 2);
    printf("    array_get(arr, 2) → %d ✓\n", p ? *p : 0);

    /* Safe access: out of bounds → returns NULL */
    printf("\n  Out-of-bounds access (checked):\n");
    p = ARRAY_GET(arr, int, 10);
    printf("    array_get(arr, 10) → %s ✓ (caught by bounds check)\n",
           p ? "non-NULL?!" : "NULL");

    /* Unchecked access: within bounds — works fine */
    printf("\n  Unchecked access (within bounds):\n");
    p = ARRAY_GET_UNCHECKED(arr, int, 2);
    printf("    array_get_unchecked(arr, 2) → %d ✓\n", *p);

    /* Unchecked access: out of bounds — reads garbage! */
    printf("\n  Unchecked access (out of bounds — DANGEROUS):\n");
    printf("    array_get_unchecked(arr, 10) → would read uninitialized\n");
    printf("    memory beyond the buffer. This is undefined behavior.\n");
    printf("    We don't execute it because the result is unpredictable.\n\n");

    array_destroy(arr);
}


/* --- Demo 2: The #ifdef pattern in action --- */

static void demo_ifdef_pattern(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Demo 2: The #ifdef Pattern — Build Mode Behavior\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    printf("  Current build mode:\n");
    #ifdef NDEBUG
        printf("    NDEBUG defined → Level 0 (release)\n");
        printf("    ARRAY_ASSERT → compiled away (no overhead)\n");
        printf("    Runtime checks → disabled in NDEBUG-stripped API\n");
    #elif defined(ARRAY_DEBUG_CHECKS)
        printf("    ARRAY_DEBUG_CHECKS defined → Level 2 (debug)\n");
        printf("    ARRAY_ASSERT → active (abort on violation)\n");
        printf("    Runtime checks → also active (double protection)\n");
    #else
        printf("    Default → Level 1 (safe)\n");
        printf("    ARRAY_ASSERT → compiled away\n");
        printf("    Runtime checks → active (return error codes)\n");
    #endif

    printf("\n  The same source code, three different behaviors:\n\n");

    printf("    gcc -o prog post_08.c                   # Level 1 (safe)\n");
    printf("    gcc -DARRAY_DEBUG_CHECKS -o prog post_08.c  # Level 2 (debug)\n");
    printf("    gcc -DNDEBUG -O2 -o prog post_08.c      # Level 0 (release)\n\n");

    printf("  What happens on array_get(arr, 999) for size=5 array:\n");
    printf("    Level 0: returns NULL (runtime check still present)\n");
    printf("    Level 1: returns NULL (runtime check + error callback)\n");
    printf("    Level 2: ABORTS with message before reaching NULL return\n\n");

    /* Demonstrate with a real call */
    Array *arr = ARRAY_CREATE(int, 4);
    if (!arr) return;

    ARRAY_PUSH(arr, int, 42);
    ARRAY_PUSH(arr, int, 43);

    printf("  Testing: array_get(arr, 999) on a 2-element array...\n");
    void *result = array_get(arr, 999);
    printf("  Result: %s\n", result ? "non-NULL (bug!)" : "NULL (correct)");
    printf("  Array is still intact after the failed access.\n\n");

    array_destroy(arr);
}


/* --- Demo 3: Error callback catches violations --- */

static int g_error_count = 0;

static void counting_error_handler(ArrayError err, const char *msg,
                                   const char *file, int line)
{
    g_error_count++;
    printf("    [error #%d] %s: %s (at %s:%d)\n",
           g_error_count, array_error_str(err), msg, file, line);
}

static void demo_error_callback(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Demo 3: Error Callback — Monitoring Contract Violations\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    /* Install a counting error handler */
    g_error_count = 0;
    array_set_error_callback(counting_error_handler);

    Array *arr = ARRAY_CREATE(int, 4);
    if (!arr) return;

    ARRAY_PUSH(arr, int, 10);
    ARRAY_PUSH(arr, int, 20);
    ARRAY_PUSH(arr, int, 30);

    printf("  Triggering various contract violations:\n\n");

    /* Out-of-bounds set */
    int val = 999;
    array_set(arr, 100, &val);

    /* NULL array push */
    array_push(NULL, &val);

    /* Out-of-bounds remove */
    array_remove(arr, 50);

    /* Out-of-bounds insert */
    array_insert(arr, 100, &val);

    printf("\n  Total violations caught: %d\n", g_error_count);
    printf("  Array is still perfectly valid after all violations.\n\n");

    array_visualize_ascii(arr, "Intact after 4 violations", "int", print_int);

    /* Clean up */
    array_set_error_callback(NULL);
    array_destroy(arr);
}


/* --- Demo 4: Unchecked access in performance loops --- */

static void demo_performance_loop(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Demo 4: Unchecked Access in Performance-Critical Loops\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    Array *arr = ARRAY_CREATE(int, 16);
    if (!arr) return;

    /* Fill with values */
    for (int i = 0; i < 10; i++) {
        int val = i * i;
        array_push(arr, &val);
    }

    printf("  Pattern 1 — Safe loop (checked access each iteration):\n\n");
    printf("    long total = 0;\n");
    printf("    for (size_t i = 0; i < array_size(arr); i++) {\n");
    printf("        int *p = array_get(arr, i);   // bounds check per call\n");
    printf("        if (p) total += *p;\n");
    printf("    }\n\n");

    long total_safe = 0;
    for (size_t i = 0; i < array_size(arr); i++) {
        int *p = (int *)array_get(arr, i);
        if (p) total_safe += *p;
    }
    printf("    Result: %ld (with %zu bounds checks)\n\n", total_safe,
           array_size(arr));

    printf("  Pattern 2 — Fast loop (unchecked, bounds proven by loop):\n\n");
    printf("    long total = 0;\n");
    printf("    size_t n = array_size(arr);  // one check, up front\n");
    printf("    for (size_t i = 0; i < n; i++) {\n");
    printf("        int *p = array_get_unchecked(arr, i);  // zero overhead\n");
    printf("        total += *p;\n");
    printf("    }\n\n");

    long total_fast = 0;
    size_t n = array_size(arr);
    for (size_t i = 0; i < n; i++) {
        int *p = (int *)array_get_unchecked(arr, i);
        total_fast += *p;
    }
    printf("    Result: %ld (with 0 bounds checks)\n\n", total_fast);

    printf("  Both produce %ld. The difference is %zu avoided branches.\n",
           total_safe, n);
    printf("  In a loop processing millions of elements per second,\n");
    printf("  those branches add up to measurable overhead.\n\n");

    array_destroy(arr);
}


/* --- Demo 5: API contract documentation style --- */

static void demo_api_contracts(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Demo 5: API Contract Documentation\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    printf("  Every function in the API has a documented contract:\n\n");

    printf("  ┌──────────────────────────────────────────────────────┐\n");
    printf("  │  array_get(arr, index)                               │\n");
    printf("  │                                                      │\n");
    printf("  │  PRECONDITIONS:                                      │\n");
    printf("  │    - arr must not be NULL                             │\n");
    printf("  │    - index must be < arr->size                       │\n");
    printf("  │                                                      │\n");
    printf("  │  POSTCONDITIONS:                                     │\n");
    printf("  │    - Returns pointer to element at index              │\n");
    printf("  │    - Returns NULL if preconditions violated           │\n");
    printf("  │    - Array state is never modified                    │\n");
    printf("  │                                                      │\n");
    printf("  │  CHECKED:   validates preconditions, returns NULL     │\n");
    printf("  │  UNCHECKED: no validation, undefined behavior on     │\n");
    printf("  │             precondition violation                    │\n");
    printf("  └──────────────────────────────────────────────────────┘\n\n");

    printf("  ┌──────────────────────────────────────────────────────┐\n");
    printf("  │  array_push(arr, element)                            │\n");
    printf("  │                                                      │\n");
    printf("  │  PRECONDITIONS:                                      │\n");
    printf("  │    - arr must not be NULL                             │\n");
    printf("  │    - element must not be NULL                         │\n");
    printf("  │    - element must point to element_size bytes         │\n");
    printf("  │                                                      │\n");
    printf("  │  POSTCONDITIONS:                                     │\n");
    printf("  │    - On success: element appended, size incremented   │\n");
    printf("  │    - On failure: array unchanged, error code returned │\n");
    printf("  │                                                      │\n");
    printf("  │  ERRORS:                                             │\n");
    printf("  │    - ARRAY_ERR_NULL if arr or element is NULL         │\n");
    printf("  │    - ARRAY_ERR_ALLOC if growth realloc fails          │\n");
    printf("  │    - ARRAY_ERR_OVERFLOW if capacity overflows size_t  │\n");
    printf("  └──────────────────────────────────────────────────────┘\n\n");

    printf("  The contract tells the caller three things:\n");
    printf("    1. What they must guarantee (preconditions)\n");
    printf("    2. What the function guarantees in return (postconditions)\n");
    printf("    3. What can go wrong and how it's reported (errors)\n\n");

    printf("  This is Design by Contract (Bertrand Meyer, 1986).\n");
    printf("  The function promises to fulfill postconditions IF the\n");
    printf("  caller fulfills preconditions. If the caller violates a\n");
    printf("  precondition, the function's response depends on the\n");
    printf("  build mode (see Demo 2).\n\n");
}


/* --- Knowledge test --- */

static void knowledge_test(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Knowledge Test\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    printf("  Q: Design an array_get() that is safe in debug builds\n");
    printf("     and zero-overhead in release. Show the #ifdef pattern.\n\n");

    printf("  A: The pattern uses ARRAY_ASSERT + conditional compilation:\n\n");

    printf("     void *array_get(const Array *arr, size_t index)\n");
    printf("     {\n");
    printf("         /* Debug: crash immediately on violation */\n");
    printf("         ARRAY_ASSERT(arr != NULL, \"NULL array\");\n");
    printf("         ARRAY_ASSERT(index < arr->size, \"OOB\");\n");
    printf("     \n");
    printf("         /* Release: this whole block compiles away */\n");
    printf("         #if ARRAY_BOUNDS_CHECK_ENABLED\n");
    printf("             if (!arr || index >= arr->size)\n");
    printf("                 return NULL;\n");
    printf("         #endif\n");
    printf("     \n");
    printf("         return element_at_unchecked(arr, index);\n");
    printf("     }\n\n");

    printf("     In debug (ARRAY_DEBUG_CHECKS): ARRAY_ASSERT fires → abort()\n");
    printf("     In default: ARRAY_ASSERT is no-op, if-check returns NULL\n");
    printf("     In release (NDEBUG): both compiled away → zero overhead,\n");
    printf("     but caller is 100%% responsible for valid arguments.\n\n");

    printf("     The tradeoff: release mode is as fast as raw pointer\n");
    printf("     arithmetic, but a bug that passes an invalid index\n");
    printf("     causes silent corruption instead of a clean error.\n");
}


/* ===========================================================================
 * main
 * =========================================================================== */

int main(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Post 8: Bounds Checking and Defensive APIs\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    demo_checked_vs_unchecked();
    demo_ifdef_pattern();
    demo_error_callback();
    demo_performance_loop();
    demo_api_contracts();

    /* Generate the API layers diagram */
    generate_api_layers_dot("output/post_08_api_layers.dot");

    knowledge_test();

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("  End of Post 8. Next: iterators and element access\n");
    printf("  patterns.\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    return 0;
}
