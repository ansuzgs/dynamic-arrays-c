/* ============================================================================
 * Post 9: "Insert, Remove, and the Cost of Shifting"
 * ============================================================================
 *
 * This file is entirely self-contained. Compile with:
 *   gcc -Wall -Wextra -std=c11 -o build/post_09 src_posts/post_09.c
 *
 * Run:
 *   ./build/post_09                       (ASCII visualization to stdout)
 *   ./build/post_09 > outputs/post_09.txt (save ASCII output)
 *
 * The program also writes outputs/post_09_shift_layout.dot (Graphviz).
 * Render with:  dot -Tsvg outputs/post_09_shift_layout.dot \
 *                        -o outputs/post_09_shift_layout.svg
 *
 * Learning outcome: understand exactly what memmove does during insert/remove,
 * measure the O(n) shifting cost in bytes moved, and implement swap-remove
 * as an O(1) alternative when order doesn't matter.
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ===========================================================================
 * 1. Build mode detection (carried forward from Post 8)
 * =========================================================================== */

#ifdef NDEBUG
  #define ARRAY_ASSERT(cond, msg) ((void)0)
  #define ARRAY_BOUNDS_CHECK_ENABLED 0
#elif defined(ARRAY_DEBUG_CHECKS)
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
    ARRAY_ERR_CONTRACT   = -7,
    ARRAY_ERR_EMPTY      = -8   /* NEW: operation on empty array */
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
        case ARRAY_ERR_EMPTY:    return "array is empty";
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
 * 5. The Array struct
 *
 * Same as Post 8, with one addition: bytes_shifted tracks the total number
 * of bytes moved by memmove across all insert/remove operations. This lets
 * us measure the real cost of shifting in demos.
 * =========================================================================== */

typedef struct {
    void          *data;
    size_t         size;
    size_t         capacity;
    size_t         element_size;
    size_t         realloc_count;
    size_t         bytes_shifted;     /* NEW: cumulative memmove cost */
    ArrayDestroyFn destroy_fn;
} Array;


/* ===========================================================================
 * 6. Lifecycle
 * =========================================================================== */

Array *array_create(size_t element_size, size_t initial_capacity)
{
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
    arr->bytes_shifted = 0;
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
 * 7. Internal helpers — unchecked layer
 * =========================================================================== */

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
 * 8. Checked public API — core operations
 * =========================================================================== */

ArrayError array_push(Array *arr, const void *element)
{
    ARRAY_ASSERT(arr != NULL, "array_push: arr must not be NULL");
    ARRAY_ASSERT(element != NULL, "array_push: element must not be NULL");

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

void *array_get(const Array *arr, size_t index)
{
    ARRAY_ASSERT(arr != NULL, "array_get: arr must not be NULL");
    ARRAY_ASSERT(index < arr->size, "array_get: index out of bounds");

    if (!arr || index >= arr->size) return NULL;

    return element_at_unchecked(arr, index);
}

void *array_get_unchecked(const Array *arr, size_t index)
{
    return element_at_unchecked(arr, index);
}


/* ===========================================================================
 * 9. Insert — THIS POST'S FIRST FOCUS
 * ===========================================================================
 *
 * Inserting at position `index` requires three steps:
 *
 *   1. Ensure capacity (may realloc — this is the only step that can fail)
 *   2. Shift elements [index..size-1] one position RIGHT using memmove
 *   3. Copy the new element into the gap at position `index`
 *
 * The shift is an O(n) operation: inserting at index 0 moves ALL n elements.
 * Inserting at index n (i.e., push) moves zero elements.
 *
 * Why memmove, not memcpy?
 *
 * When shifting right, the source region [index..size-1] and the destination
 * region [index+1..size] OVERLAP. memcpy's behavior with overlapping regions
 * is undefined — it might copy left-to-right and overwrite source data before
 * reading it. memmove guarantees correct copying regardless of overlap by
 * detecting the direction and copying in the safe order.
 *
 * The cost is measured in bytes_shifted: (size - index) * element_size.
 * We track this cumulatively so demos can show the total shifting cost
 * across many operations.
 * =========================================================================== */

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

    /* Step 1: ensure capacity — BEFORE any shifting */
    ArrayError err = ensure_capacity_unchecked(arr, arr->size + 1);
    if (err != ARRAY_OK) return err;

    /* Step 2: shift elements right to open a gap */
    if (index < arr->size) {
        void *dst = element_at_unchecked(arr, index + 1);
        void *src = element_at_unchecked(arr, index);
        size_t bytes = (arr->size - index) * arr->element_size;
        memmove(dst, src, bytes);
        arr->bytes_shifted += bytes;     /* Track the cost */
    }

    /* Step 3: copy the new element into the gap */
    memcpy(element_at_unchecked(arr, index), element, arr->element_size);
    arr->size++;

    return ARRAY_OK;
}


/* ===========================================================================
 * 10. Remove (stable) — THIS POST'S SECOND FOCUS
 * ===========================================================================
 *
 * Removing at position `index` shifts elements [index+1..size-1] one
 * position LEFT to fill the gap. This preserves the relative order of
 * all remaining elements — that's what "stable" means.
 *
 * The cost is (size - index - 1) * element_size bytes shifted.
 * Removing from the end (pop) shifts zero bytes.
 * Removing from the front shifts (size - 1) * element_size bytes.
 *
 * The destructor is called BEFORE the shift. If we shifted first, the
 * element's memory would be overwritten by its right neighbor before
 * we could clean it up.
 * =========================================================================== */

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

    /* Step 1: destroy the element being removed */
    if (arr->destroy_fn) {
        arr->destroy_fn(element_at_unchecked(arr, index));
    }

    /* Step 2: shift elements left to fill the gap */
    if (index < arr->size - 1) {
        void *dst = element_at_unchecked(arr, index);
        void *src = element_at_unchecked(arr, index + 1);
        size_t bytes = (arr->size - index - 1) * arr->element_size;
        memmove(dst, src, bytes);
        arr->bytes_shifted += bytes;     /* Track the cost */
    }

    /* Step 3: decrement size */
    arr->size--;
    return ARRAY_OK;
}


/* ===========================================================================
 * 11. Swap-remove — THIS POST'S KEY NEW FUNCTION
 * ===========================================================================
 *
 * Swap-remove is the O(1) alternative to stable removal. Instead of shifting
 * all elements left, it copies the LAST element into the gap and decrements
 * size. The result:
 *
 *   - The removed element is gone.
 *   - The last element is now at position `index`.
 *   - All other elements are unchanged.
 *   - The relative ORDER is NOT preserved.
 *
 * This is exactly one memcpy (element_size bytes) regardless of array size
 * or removal position. For a 1,000,000-element array, removing element 0
 * costs 4 bytes (one int) instead of 3,999,996 bytes (999,999 ints).
 *
 * The tradeoff is index stability: after a swap-remove, the element that
 * was at position (size - 1) is now at position `index`. Any external code
 * that stored indices into the array (iterators, cross-references, secondary
 * indices) may now point to the wrong element. This is why swap-remove is
 * inappropriate for ordered data or when indices are used as stable handles.
 *
 * When to use swap-remove:
 *   - Entity systems (game objects, particles, ECS components)
 *   - Unordered collections (sets, bags)
 *   - Batch processing where order doesn't matter
 *   - Any hot loop where remove performance matters more than order
 *
 * When NOT to use swap-remove:
 *   - Sorted arrays (order is the whole point)
 *   - UI lists where the user sees element positions
 *   - Any context where indices are stored externally as references
 *   - When you're iterating forward and removing (the swapped element
 *     would be skipped — iterate backward instead, or use stable remove)
 * =========================================================================== */

ArrayError array_swap_remove(Array *arr, size_t index)
{
    ARRAY_ASSERT(arr != NULL, "array_swap_remove: arr must not be NULL");
    ARRAY_ASSERT(index < arr->size, "array_swap_remove: index out of bounds");

    if (!arr) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_NULL, "array_swap_remove: NULL array");
        return ARRAY_ERR_NULL;
    }
    if (arr->size == 0) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_EMPTY, "array_swap_remove: empty array");
        return ARRAY_ERR_EMPTY;
    }
    if (index >= arr->size) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_BOUNDS,
                           "array_swap_remove: index out of range");
        return ARRAY_ERR_BOUNDS;
    }

    /* Step 1: destroy the element being removed */
    if (arr->destroy_fn) {
        arr->destroy_fn(element_at_unchecked(arr, index));
    }

    /* Step 2: if not removing the last element, overwrite with last */
    if (index < arr->size - 1) {
        void *dst = element_at_unchecked(arr, index);
        void *src = element_at_unchecked(arr, arr->size - 1);
        memcpy(dst, src, arr->element_size);
        /* No bytes_shifted increment — this is a fixed-cost copy,
         * not a proportional shift. We want the counter to reflect
         * only the O(n) memmove cost for comparison purposes. */
    }

    /* Step 3: decrement size */
    arr->size--;
    return ARRAY_OK;
}


/* ===========================================================================
 * 12. Remaining API (carried forward)
 * =========================================================================== */

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

    if (arr->destroy_fn) {
        arr->destroy_fn(element_at_unchecked(arr, index));
    }

    memcpy(element_at_unchecked(arr, index), element, arr->element_size);
    return ARRAY_OK;
}

size_t array_size(const Array *arr)          { return arr ? arr->size          : 0; }
size_t array_capacity(const Array *arr)      { return arr ? arr->capacity      : 0; }
size_t array_realloc_count(const Array *arr) { return arr ? arr->realloc_count : 0; }
size_t array_bytes_shifted(const Array *arr) { return arr ? arr->bytes_shifted : 0; }

ArrayError array_sort(Array *arr, ArrayCompareFn compare)
{
    ARRAY_ASSERT(arr != NULL, "array_sort: arr must not be NULL");
    ARRAY_ASSERT(compare != NULL, "array_sort: comparator must not be NULL");

    if (!arr || !compare) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_NULL, "array_sort: NULL argument");
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

    if (!arr || !fn) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_NULL, "array_foreach: NULL argument");
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
 * 13. Type-safe macros (carried forward)
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

#define ARRAY_INSERT(arr, type, index, value) \
    do { \
        if (sizeof(type) != (arr)->element_size) { \
            fprintf(stderr, \
                "ARRAY_INSERT: type size mismatch at %s:%d\n", \
                __FILE__, __LINE__); \
        } else { \
            type _ins_tmp = (value); \
            (void)array_insert((arr), (index), &_ins_tmp); \
        } \
    } while (0)


/* ===========================================================================
 * 14. ASCII Visualization — STEP-BY-STEP SHIFTING
 * ===========================================================================
 *
 * This post's visualization shows the individual steps of a shift operation:
 * which elements move, how many bytes are copied, and what the array looks
 * like before and after.
 * =========================================================================== */

static void print_int(char *buf, size_t bufsize, const void *elem)
{
    snprintf(buf, bufsize, "%d", *(const int *)elem);
}

/* Print a compact one-line view of the array contents */
static void print_array_inline(const Array *arr, PrintElementFn printer)
{
    printf("[");
    for (size_t i = 0; i < arr->size; i++) {
        char buf[32];
        printer(buf, sizeof(buf), element_at_unchecked(arr, i));
        printf("%s%s", buf, i < arr->size - 1 ? ", " : "");
    }
    printf("]");
}

/* Full state visualization */
static void array_visualize_ascii(const Array *arr, const char *label,
                                  const char *type_label,
                                  PrintElementFn printer)
{
    if (!arr) {
        printf("╔══════════════════════════════════════════════════════════╗\n");
        printf("║  %-54s  ║\n", label ? label : "(null array)");
        printf("║  Array: NULL                                           ║\n");
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
    printf("║  bytes_shifted (cumulative): %-27zu  ║\n", arr->bytes_shifted);
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
            printf("║  · ... %zu more unused slots %-25s  ║\n",
                   cap - show, "");
        }
    }

    size_t used_bytes  = sz  * arr->element_size;
    size_t alloc_bytes = cap * arr->element_size;
    double util = cap > 0 ? 100.0 * (double)sz / (double)cap : 0.0;

    printf("╠══════════════════════════════════════════════════════════╣\n");

    /* Use a buffer for the utilization line to calculate padding */
    char util_line[128];
    int len = snprintf(util_line, sizeof(util_line),
        "  %zuB used / %zuB allocated = %.1f%% utilization",
        used_bytes, alloc_bytes, util);
    printf("║%-56s  ║\n", util_line);
    (void)len;  /* suppress unused warning */

    printf("╚══════════════════════════════════════════════════════════╝\n\n");
}


/* ===========================================================================
 * 15. ASCII step-by-step shift visualization — THE SIGNATURE VIZ
 * ===========================================================================
 *
 * Shows the element-by-element movement during insert or remove.
 * This is what makes the O(n) cost viscerally obvious.
 * =========================================================================== */

static void visualize_insert_steps(const Array *arr, size_t index,
                                   int new_value, PrintElementFn printer)
{
    printf("  ┌─────────────────────────────────────────────────┐\n");
    printf("  │  INSERT %d at index %zu                ", new_value, index);
    if (index < 10 && new_value < 100) printf("          ");
    else if (index < 10 || new_value < 100) printf("        ");
    else printf("      ");
    printf("│\n");
    printf("  └─────────────────────────────────────────────────┘\n\n");

    /* Show the BEFORE state */
    printf("  BEFORE: ");
    print_array_inline(arr, printer);
    printf("  (size=%zu)\n\n", arr->size);

    /* Show the shift animation */
    size_t elements_to_shift = arr->size - index;
    size_t bytes = elements_to_shift * arr->element_size;

    if (elements_to_shift > 0) {
        printf("  Step 1: Shift %zu element%s right (%zu bytes via memmove)\n\n",
               elements_to_shift, elements_to_shift == 1 ? "" : "s", bytes);

        /* Show which elements move */
        printf("          ");
        for (size_t i = 0; i < arr->size; i++) {
            char buf[16];
            printer(buf, sizeof(buf), element_at_unchecked(arr, i));
            if (i >= index) {
                printf("[%s]→ ", buf);
            } else {
                printf("[%s]  ", buf);
            }
        }
        printf("[  ]\n");

        printf("           ");
        for (size_t i = 0; i < arr->size; i++) {
            char buf[16];
            printer(buf, sizeof(buf), element_at_unchecked(arr, i));
            (void)buf;
            if (i == index) {
                printf(" ↑gap ");
            } else if (i < index) {
                printf("      ");
            } else {
                printf("      ");
            }
        }
        printf("\n\n");
    } else {
        printf("  Step 1: No shift needed (inserting at end)\n\n");
    }

    printf("  Step 2: Write %d into gap at index %zu\n\n", new_value, index);
}

static void visualize_remove_steps(const Array *arr, size_t index,
                                   PrintElementFn printer)
{
    char removed_buf[32];
    printer(removed_buf, sizeof(removed_buf),
            element_at_unchecked(arr, index));

    printf("  ┌─────────────────────────────────────────────────┐\n");
    printf("  │  REMOVE element %s at index %zu", removed_buf, index);
    /* Pad to fill the box */
    int pad = 49 - 24 - (int)strlen(removed_buf);
    if (index >= 10) pad--;
    for (int i = 0; i < pad; i++) printf(" ");
    printf("│\n");
    printf("  └─────────────────────────────────────────────────┘\n\n");

    printf("  BEFORE: ");
    print_array_inline(arr, printer);
    printf("  (size=%zu)\n\n", arr->size);

    size_t elements_to_shift = arr->size - index - 1;
    size_t bytes = elements_to_shift * arr->element_size;

    /* Show which element is removed */
    printf("  Step 1: Remove element at [%zu] = %s\n\n", index, removed_buf);

    if (elements_to_shift > 0) {
        printf("  Step 2: Shift %zu element%s left (%zu bytes via memmove)\n\n",
               elements_to_shift, elements_to_shift == 1 ? "" : "s", bytes);

        printf("          ");
        for (size_t i = 0; i < arr->size; i++) {
            char buf[16];
            printer(buf, sizeof(buf), element_at_unchecked(arr, i));
            if (i == index) {
                printf("[XX]  ");
            } else if (i > index) {
                printf(" ←[%s]", buf);
            } else {
                printf("[%s]  ", buf);
            }
        }
        printf("\n\n");
    } else {
        printf("  Step 2: No shift needed (removing last element)\n\n");
    }
}

static void visualize_swap_remove_steps(const Array *arr, size_t index,
                                        PrintElementFn printer)
{
    char removed_buf[32];
    printer(removed_buf, sizeof(removed_buf),
            element_at_unchecked(arr, index));

    char last_buf[32];
    printer(last_buf, sizeof(last_buf),
            element_at_unchecked(arr, arr->size - 1));

    printf("  ┌─────────────────────────────────────────────────┐\n");
    printf("  │  SWAP-REMOVE element %s at index %zu",
           removed_buf, index);
    int pad = 49 - 29 - (int)strlen(removed_buf);
    if (index >= 10) pad--;
    for (int i = 0; i < pad; i++) printf(" ");
    printf("│\n");
    printf("  └─────────────────────────────────────────────────┘\n\n");

    printf("  BEFORE: ");
    print_array_inline(arr, printer);
    printf("  (size=%zu)\n\n", arr->size);

    if (index < arr->size - 1) {
        printf("  Step 1: Copy last element (%s) into position [%zu]\n",
               last_buf, index);
        printf("          Cost: %zu bytes (ONE memcpy, always)\n\n",
               arr->element_size);

        printf("          ");
        for (size_t i = 0; i < arr->size; i++) {
            char buf[16];
            printer(buf, sizeof(buf), element_at_unchecked(arr, i));
            if (i == index) {
                printf("[%s←", last_buf);
            } else if (i == arr->size - 1) {
                printf(" %s]", buf);
            } else {
                printf("[%s]  ", buf);
            }
        }
        printf("\n\n");
    } else {
        printf("  Step 1: Removing last element — no copy needed\n\n");
    }

    printf("  Step 2: Decrement size (no shifting, order NOT preserved)\n\n");
}


/* ===========================================================================
 * 16. DOT Generation — Before/After memory layout
 * ===========================================================================
 *
 * Generates a side-by-side view of the array before and after an operation,
 * showing which elements moved and how the layout changed.
 * This function is a tool for the blog author to generate the diagram
 * image — not explained in the article text.
 * =========================================================================== */

static void generate_shift_layout_dot(const char *filename)
{
    FILE *f = fopen(filename, "w");
    if (!f) {
        fprintf(stderr, "generate_shift_layout_dot: cannot open %s\n", filename);
        return;
    }

    fprintf(f, "digraph ShiftLayout {\n");
    fprintf(f, "  rankdir=TB;\n");
    fprintf(f, "  graph [fontname=\"Helvetica\", fontsize=12, "
               "bgcolor=\"#fafafa\",\n");
    fprintf(f, "         label=\"Insert & Remove: Memory Movement\\n"
               "Post 9: The Cost of Shifting\", labelloc=t,\n");
    fprintf(f, "         nodesep=0.4, ranksep=0.6];\n");
    fprintf(f, "  node [fontname=\"Helvetica\", fontsize=10, style=filled];\n");
    fprintf(f, "  edge [fontname=\"Helvetica\", fontsize=9];\n\n");

    /* --- Insert scenario: insert 25 at index 2 in [10, 20, 30, 40, 50] --- */
    fprintf(f, "  subgraph cluster_insert {\n");
    fprintf(f, "    label=\"array_insert(arr, 2, &val_25)\";\n");
    fprintf(f, "    style=rounded; color=\"#1565c0\"; penwidth=2;\n");
    fprintf(f, "    fontname=\"Helvetica\"; fontsize=11;\n\n");

    /* Before state */
    fprintf(f, "    ins_before [shape=record, fillcolor=\"#e3f2fd\",\n");
    fprintf(f, "      label=\"{BEFORE (size=5)|"
               "{[0] 10|[1] 20|[2] 30|[3] 40|[4] 50|[5] ·}}\"];\n\n");

    /* Shift annotation */
    fprintf(f, "    ins_shift [shape=note, fillcolor=\"#fff9c4\",\n");
    fprintf(f, "      label=\"memmove: 3 elements right\\l"
               "= 12 bytes shifted\\l"
               "O(n) where n = size - index\\l\"];\n\n");

    /* After state */
    fprintf(f, "    ins_after [shape=record, fillcolor=\"#c8e6c9\",\n");
    fprintf(f, "      label=\"{AFTER (size=6)|"
               "{[0] 10|[1] 20|[2] \\<b\\>25\\</b\\>|"
               "[3] 30|[4] 40|[5] 50}}\"];\n\n");

    fprintf(f, "    ins_before -> ins_shift "
               "[label=\"shift [2..4] right\"];\n");
    fprintf(f, "    ins_shift -> ins_after "
               "[label=\"write 25 at [2]\"];\n");
    fprintf(f, "  }\n\n");

    /* --- Remove scenario: remove index 1 from [10, 20, 30, 40, 50] --- */
    fprintf(f, "  subgraph cluster_remove {\n");
    fprintf(f, "    label=\"array_remove(arr, 1)  vs  "
               "array_swap_remove(arr, 1)\";\n");
    fprintf(f, "    style=rounded; color=\"#c62828\"; penwidth=2;\n");
    fprintf(f, "    fontname=\"Helvetica\"; fontsize=11;\n\n");

    /* Before state */
    fprintf(f, "    rem_before [shape=record, fillcolor=\"#ffebee\",\n");
    fprintf(f, "      label=\"{BEFORE (size=5)|"
               "{[0] 10|[1] \\<b\\>20\\</b\\>|[2] 30|[3] 40|[4] 50}}\"];\n\n");

    /* Stable remove path */
    fprintf(f, "    rem_stable [shape=note, fillcolor=\"#fff9c4\",\n");
    fprintf(f, "      label=\"STABLE remove:\\l"
               "memmove 3 elements left\\l"
               "= 12 bytes shifted\\l"
               "Order preserved ✓\\l\"];\n\n");

    fprintf(f, "    rem_stable_after [shape=record, fillcolor=\"#c8e6c9\",\n");
    fprintf(f, "      label=\"{AFTER stable (size=4)|"
               "{[0] 10|[1] 30|[2] 40|[3] 50}}\"];\n\n");

    /* Swap-remove path */
    fprintf(f, "    rem_swap [shape=note, fillcolor=\"#e1bee7\",\n");
    fprintf(f, "      label=\"SWAP remove:\\l"
               "copy last element to [1]\\l"
               "= 4 bytes copied\\l"
               "Order BROKEN ✗\\l\"];\n\n");

    fprintf(f, "    rem_swap_after [shape=record, fillcolor=\"#f3e5f5\",\n");
    fprintf(f, "      label=\"{AFTER swap (size=4)|"
               "{[0] 10|[1] \\<b\\>50\\</b\\>|[2] 30|[3] 40}}\"];\n\n");

    fprintf(f, "    rem_before -> rem_stable "
               "[label=\"preserve order\"];\n");
    fprintf(f, "    rem_before -> rem_swap "
               "[label=\"O(1) fast path\", style=dashed];\n");
    fprintf(f, "    rem_stable -> rem_stable_after;\n");
    fprintf(f, "    rem_swap -> rem_swap_after;\n");
    fprintf(f, "  }\n\n");

    /* Cost comparison box */
    fprintf(f, "  subgraph cluster_cost {\n");
    fprintf(f, "    label=\"Cost Comparison (4-byte elements)\";\n");
    fprintf(f, "    style=rounded; color=\"#616161\"; penwidth=1;\n");
    fprintf(f, "    fontname=\"Helvetica\"; fontsize=11;\n\n");
    fprintf(f, "    cost_table [shape=plaintext, fillcolor=\"#ffffff\",\n");
    fprintf(f, "      label=<\n");
    fprintf(f, "        <table border=\"0\" cellborder=\"1\" cellspacing=\"0\">\n");
    fprintf(f, "          <tr><td><b>Operation</b></td>"
               "<td><b>Bytes moved</b></td>"
               "<td><b>Complexity</b></td></tr>\n");
    fprintf(f, "          <tr><td>insert at front (n=1000)</td>"
               "<td>4000</td><td>O(n)</td></tr>\n");
    fprintf(f, "          <tr><td>insert at back (push)</td>"
               "<td>0</td><td>O(1)</td></tr>\n");
    fprintf(f, "          <tr><td>remove from front (n=1000)</td>"
               "<td>3996</td><td>O(n)</td></tr>\n");
    fprintf(f, "          <tr><td>remove from back (pop)</td>"
               "<td>0</td><td>O(1)</td></tr>\n");
    fprintf(f, "          <tr><td>swap_remove anywhere</td>"
               "<td>4</td><td>O(1)</td></tr>\n");
    fprintf(f, "        </table>\n");
    fprintf(f, "      >];\n");
    fprintf(f, "  }\n");

    fprintf(f, "}\n");
    fclose(f);

    printf("  DOT file written to %s\n", filename);
    printf("  Render: dot -Tsvg %s -o output/post_09_shift_layout.svg\n\n",
           filename);
}


/* ===========================================================================
 * 17. Demonstrations
 * =========================================================================== */


/* --- Demo 1: Insert at various positions --- */

static void demo_insert_positions(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Demo 1: Inserting at Different Positions\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    Array *arr = ARRAY_CREATE(int, 16);
    if (!arr) return;

    /* Build initial array: [10, 20, 30, 40, 50] */
    for (int i = 1; i <= 5; i++) {
        int val = i * 10;
        array_push(arr, &val);
    }

    arr->bytes_shifted = 0;   /* Reset counter for this demo */

    array_visualize_ascii(arr, "Initial state", "int", print_int);

    /* Insert at the front — worst case: shifts ALL elements */
    visualize_insert_steps(arr, 0, 5, print_int);
    ARRAY_INSERT(arr, int, 0, 5);

    printf("  AFTER:  ");
    print_array_inline(arr, print_int);
    printf("  (size=%zu, bytes shifted so far: %zu)\n\n",
           arr->size, arr->bytes_shifted);

    /* Insert in the middle */
    visualize_insert_steps(arr, 3, 25, print_int);
    ARRAY_INSERT(arr, int, 3, 25);

    printf("  AFTER:  ");
    print_array_inline(arr, print_int);
    printf("  (size=%zu, bytes shifted so far: %zu)\n\n",
           arr->size, arr->bytes_shifted);

    /* Insert at the end — best case: shifts ZERO elements */
    visualize_insert_steps(arr, arr->size, 99, print_int);
    ARRAY_INSERT(arr, int, (int)arr->size, 99);

    printf("  AFTER:  ");
    print_array_inline(arr, print_int);
    printf("  (size=%zu, bytes shifted so far: %zu)\n\n",
           arr->size, arr->bytes_shifted);

    array_visualize_ascii(arr, "After three inserts", "int", print_int);

    printf("  Summary: 3 inserts, total bytes shifted by memmove: %zu\n",
           arr->bytes_shifted);
    printf("  Insert at front shifted %zu bytes (worst case).\n",
           5 * sizeof(int));
    printf("  Insert at end shifted 0 bytes (best case = push).\n\n");

    array_destroy(arr);
}


/* --- Demo 2: Stable remove vs swap-remove --- */

static void demo_remove_comparison(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Demo 2: Stable Remove vs Swap-Remove\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    /* --- Stable remove --- */
    printf("  ── Part A: Stable remove (memmove, preserves order) ──\n\n");

    Array *arr_stable = ARRAY_CREATE(int, 16);
    if (!arr_stable) return;

    for (int i = 1; i <= 8; i++) {
        int val = i * 10;
        array_push(arr_stable, &val);
    }
    arr_stable->bytes_shifted = 0;

    /* Remove from the middle */
    visualize_remove_steps(arr_stable, 2, print_int);
    array_remove(arr_stable, 2);

    printf("  AFTER:  ");
    print_array_inline(arr_stable, print_int);
    printf("  (size=%zu, bytes shifted: %zu)\n\n",
           arr_stable->size, arr_stable->bytes_shifted);

    printf("  Order preserved: ✓ (all elements still in original order)\n\n");

    /* --- Swap remove --- */
    printf("  ── Part B: Swap-remove (memcpy, O(1), reorders) ──\n\n");

    Array *arr_swap = ARRAY_CREATE(int, 16);
    if (!arr_swap) return;

    for (int i = 1; i <= 8; i++) {
        int val = i * 10;
        array_push(arr_swap, &val);
    }
    arr_swap->bytes_shifted = 0;

    /* Swap-remove from the middle */
    visualize_swap_remove_steps(arr_swap, 2, print_int);
    array_swap_remove(arr_swap, 2);

    printf("  AFTER:  ");
    print_array_inline(arr_swap, print_int);
    printf("  (size=%zu, bytes shifted: %zu)\n\n",
           arr_swap->size, arr_swap->bytes_shifted);

    printf("  Order preserved: ✗ (element 80 moved from [7] to [2])\n\n");

    /* --- Side-by-side cost --- */
    printf("  ┌──────────────────────────────────────────────────────┐\n");
    printf("  │  Cost Comparison: remove index 2 from 8 elements    │\n");
    printf("  ├──────────────────────────────────────────────────────┤\n");
    printf("  │  Stable (memmove):  %3zu bytes shifted   O(n)        │\n",
           arr_stable->bytes_shifted);
    printf("  │  Swap   (memcpy):   %3zu bytes copied    O(1)        │\n",
           arr_swap->element_size);
    printf("  │  Ratio:             %zu× more work for stable       │\n",
           arr_stable->bytes_shifted / arr_swap->element_size);
    printf("  └──────────────────────────────────────────────────────┘\n\n");

    array_destroy(arr_stable);
    array_destroy(arr_swap);
}


/* --- Demo 3: Scaling cost — remove from arrays of different sizes --- */

static void demo_scaling_cost(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Demo 3: How Shifting Cost Scales with Array Size\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    printf("  Removing element at index 0 (worst case) from arrays\n");
    printf("  of increasing size:\n\n");

    printf("  ┌──────────┬──────────────────┬──────────────────┐\n");
    printf("  │  Size    │  Stable remove   │  Swap-remove     │\n");
    printf("  │          │  (bytes shifted)  │  (bytes copied)  │\n");
    printf("  ├──────────┼──────────────────┼──────────────────┤\n");

    size_t sizes[] = {10, 100, 1000, 10000};
    size_t num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    for (size_t s = 0; s < num_sizes; s++) {
        size_t n = sizes[s];

        Array *arr = ARRAY_CREATE(int, n);
        if (!arr) continue;

        for (size_t i = 0; i < n; i++) {
            int val = (int)i;
            array_push(arr, &val);
        }

        /* Stable remove cost */
        size_t stable_bytes = (n - 1) * arr->element_size;
        /* Swap-remove cost */
        size_t swap_bytes = arr->element_size;

        printf("  │  %-6zu  │  %-14zu  │  %-14zu  │\n",
               n, stable_bytes, swap_bytes);

        array_destroy(arr);
    }

    printf("  └──────────┴──────────────────┴──────────────────┘\n\n");

    printf("  Stable remove scales linearly: O(n × element_size).\n");
    printf("  Swap-remove is constant: O(element_size), always.\n\n");
    printf("  For 10,000 int elements, stable remove at index 0 moves\n");
    printf("  %zu bytes. Swap-remove moves %zu bytes. That's a %zu× difference.\n\n",
           (size_t)9999 * sizeof(int), sizeof(int),
           (size_t)9999);
}


/* --- Demo 4: Index stability after removal --- */

static void demo_index_stability(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Demo 4: Index Stability After Removal\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    printf("  Suppose we store indices as external references:\n\n");

    Array *arr = ARRAY_CREATE(int, 16);
    if (!arr) return;

    int vals[] = {10, 20, 30, 40, 50};
    for (int i = 0; i < 5; i++) {
        array_push(arr, &vals[i]);
    }

    printf("  Starting array: ");
    print_array_inline(arr, print_int);
    printf("\n");
    printf("  Stored reference: 'element 40 is at index 3'\n\n");

    size_t reference_index = 3;
    int *ref_before = (int *)array_get(arr, reference_index);
    printf("  Before removal: arr[%zu] = %d ✓\n\n", reference_index,
           ref_before ? *ref_before : -1);

    /* Stable remove at index 1 */
    printf("  → array_remove(arr, 1)  (stable, preserves order)\n\n");
    array_remove(arr, 1);

    printf("  After stable remove: ");
    print_array_inline(arr, print_int);
    printf("\n");

    /* The reference index 3 now points to 50, not 40! */
    int *ref_after_stable = (int *)array_get(arr, reference_index);
    printf("  arr[%zu] = %d — WRONG! Element 40 is now at index 2.\n",
           reference_index,
           ref_after_stable ? *ref_after_stable : -1);
    printf("  Stable remove preserves order but shifts indices.\n\n");

    array_destroy(arr);

    /* Now demonstrate with swap-remove */
    arr = ARRAY_CREATE(int, 16);
    if (!arr) return;

    for (int i = 0; i < 5; i++) {
        array_push(arr, &vals[i]);
    }

    printf("  Reset: ");
    print_array_inline(arr, print_int);
    printf("\n");
    printf("  Stored reference: 'element 40 is at index 3'\n\n");

    printf("  → array_swap_remove(arr, 1)  (unstable, O(1))\n\n");
    array_swap_remove(arr, 1);

    printf("  After swap-remove: ");
    print_array_inline(arr, print_int);
    printf("\n");

    int *ref_after_swap = (int *)array_get(arr, reference_index);
    printf("  arr[%zu] = %d", reference_index,
           ref_after_swap ? *ref_after_swap : -1);
    if (ref_after_swap && *ref_after_swap == 40) {
        printf(" — CORRECT! Index 3 still holds 40.\n");
    } else {
        printf(" — this also shifted (depends on which index was removed).\n");
    }
    printf("\n  Key insight: NEITHER removal strategy preserves index\n");
    printf("  stability unless the removal is at or after the reference.\n");
    printf("  If you need stable handles, use a different data structure\n");
    printf("  (slot map, generational index, linked list).\n\n");

    array_destroy(arr);
}


/* --- Knowledge test --- */

static void knowledge_test(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Knowledge Test\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    printf("  Q: You have an array of 1000 int elements and need to\n");
    printf("     remove element at index 500. Compare the cost of\n");
    printf("     memmove vs swap-remove.\n\n");

    printf("  A:\n\n");

    printf("  Stable remove (memmove):\n");
    printf("    Elements to shift: 1000 - 500 - 1 = 499\n");
    printf("    Bytes moved: 499 × 4 = 1,996 bytes\n");
    printf("    Complexity: O(n) — proportional to elements after index\n\n");

    printf("  Swap-remove (memcpy):\n");
    printf("    Copy last element (index 999) to index 500\n");
    printf("    Bytes moved: 1 × 4 = 4 bytes\n");
    printf("    Complexity: O(1) — constant regardless of array size\n\n");

    printf("  The stable remove does 499× more work.\n");
    printf("  For element 0, it would be 999× more work.\n");
    printf("  For element 999 (last), both do zero work (just decrement).\n\n");

    printf("  The tradeoff: swap-remove breaks element order.\n");
    printf("  After swap-removing index 500, the element that was at\n");
    printf("  index 999 is now at index 500. If the array is sorted,\n");
    printf("  it isn't anymore. If external code stored index 999 as a\n");
    printf("  reference, that reference now points past the array.\n\n");

    /* Verify error handling still works */
    printf("  Bonus — error handling sanity check:\n");
    ArrayError err = array_swap_remove(NULL, 0);
    printf("  array_swap_remove(NULL, 0) → %s ✓\n", array_error_str(err));
}


/* ===========================================================================
 * main
 * =========================================================================== */

int main(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Post 9: Insert, Remove, and the Cost of Shifting\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    demo_insert_positions();
    demo_remove_comparison();
    demo_scaling_cost();
    demo_index_stability();

    /* Generate Graphviz diagram for blog post image */
    generate_shift_layout_dot("output/post_09_shift_layout.dot");

    knowledge_test();

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("  End of Post 9. Next: iterators and traversal patterns.\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    return 0;
}
