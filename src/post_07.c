/* ============================================================================
 * Post 7: "Function Pointers and Callbacks: Sort, Search, Destroy"
 * ============================================================================
 *
 * This file is entirely self-contained. Compile with:
 *   gcc -Wall -Wextra -std=c11 -o build/post_07 src_posts/post_07.c
 *
 * Run:
 *   ./build/post_07                       (ASCII visualization to stdout)
 *   ./build/post_07 > output/post_07.txt (save ASCII output)
 *
 * The program also writes output/post_07_callback_dispatch.dot (Graphviz).
 * Render with:  dot -Tsvg output/post_07_callback_dispatch.dot \
 *                        -o output/post_07_callback_dispatch.svg
 *
 * Learning outcome: add array_sort(), array_find(), array_foreach(), and
 * element destructor callbacks to the generic array. Understand function
 * pointer syntax, qsort-compatible comparators, and the ownership model
 * when the array stores heap-allocated data.
 * ============================================================================
 */

/* Feature test macro: strdup is POSIX, not standard C11.
 * This must come BEFORE any includes. */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ===========================================================================
 * 1. Error codes (carried forward from Post 6)
 * =========================================================================== */

typedef enum {
    ARRAY_OK          =  0,
    ARRAY_ERR_NULL    = -1,
    ARRAY_ERR_ALLOC   = -2,
    ARRAY_ERR_BOUNDS  = -3,
    ARRAY_ERR_OVERFLOW = -4,
    ARRAY_ERR_FULL    = -5,
    ARRAY_ERR_TYPE    = -6
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
    }
    return "unknown error";
}


/* ===========================================================================
 * 2. Error callback (carried forward from Post 6)
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
 * 3. Function pointer types — THIS POST'S NEW CONTRIBUTION
 * ===========================================================================
 *
 * Function pointers let you pass behavior as data. A comparator tells the
 * array HOW to order elements. A destructor tells it HOW to clean up.
 * A foreach callback tells it WHAT to do with each element.
 *
 * The syntax is notoriously ugly:
 *
 *   int (*compare)(const void *, const void *)
 *       ↑       ↑                ↑
 *   return   name of         parameter types
 *   type     the pointer
 *
 * The parentheses around (*compare) are mandatory. Without them,
 * `int *compare(...)` declares a function returning int*, not a pointer
 * to a function returning int.
 *
 * We typedef each callback type to keep signatures readable.
 * =========================================================================== */

/* Comparator: qsort-compatible. Returns <0, 0, or >0.
 *
 * CRITICAL SUBTLETY: qsort passes pointers TO the elements in the array.
 * If the array stores int values, qsort passes int* (as const void*).
 * If the array stores char* pointers, qsort passes char** (as const void*).
 *
 * This is because qsort doesn't know your type — it gives you a pointer
 * to each element's location in the buffer. For value types, that's a
 * pointer to the value. For pointer types, that's a pointer to the pointer.
 */
typedef int (*ArrayCompareFn)(const void *a, const void *b);

/* Destructor: called on each element when the array is destroyed.
 * Receives a pointer to the element's location in the buffer.
 * For value types (int, struct), this is typically NULL — nothing to free.
 * For pointer types (char*, malloc'd structs), this frees the pointed-to
 * memory that the array doesn't own. */
typedef void (*ArrayDestroyFn)(void *element);

/* Foreach callback: called once per element during iteration.
 * Receives a pointer to the element and an optional user context.
 * The context pointer allows passing state without globals. */
typedef void (*ArrayForeachFn)(void *element, void *context);

/* Predicate: returns non-zero if the element matches some criterion.
 * Used by array_find(). Receives a pointer to the element and a context. */
typedef int (*ArrayPredicateFn)(const void *element, const void *context);

/* Print callback for ASCII visualization (from Post 6) */
typedef void (*PrintElementFn)(char *buf, size_t bufsize, const void *elem);


/* ===========================================================================
 * 4. The Array struct — now with a destructor callback
 * ===========================================================================
 *
 * The only structural change from Post 6: we add a `destroy_fn` field.
 * This callback, if set, is called on each element when the array is
 * destroyed or when an element is removed. It solves the ownership
 * problem from Post 4: if the array stores pointers to heap-allocated
 * data, who frees that data?
 *
 * Without a destructor: the array frees its buffer (the pointer copies),
 * but the pointed-to memory leaks.
 *
 * With a destructor: the array calls destroy_fn on each element before
 * freeing the buffer. The destructor does whatever cleanup is needed:
 * free(*(char **)element) for strings, or a custom function for structs
 * with multiple allocations.
 * =========================================================================== */

typedef struct {
    void          *data;           /* Opaque heap buffer                    */
    size_t         size;           /* Elements currently stored              */
    size_t         capacity;       /* Slots allocated                        */
    size_t         element_size;   /* Bytes per element                      */
    size_t         realloc_count;  /* Diagnostic: growth events              */
    ArrayDestroyFn destroy_fn;     /* NEW: per-element cleanup (or NULL)     */
} Array;


/* ===========================================================================
 * 5. Lifecycle — Create and Destroy (updated for destructor)
 * =========================================================================== */

Array *array_create(size_t element_size, size_t initial_capacity)
{
    if (element_size == 0 || initial_capacity == 0) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_NULL,
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
    arr->destroy_fn    = NULL;   /* No destructor by default */

    return arr;
}

/* Set the element destructor. Can be called any time after creation.
 * Pass NULL to disable. */
void array_set_destroy_fn(Array *arr, ArrayDestroyFn fn)
{
    if (arr) arr->destroy_fn = fn;
}

/* Destroy: now calls the destructor on every element before freeing.
 *
 * The order matters: destroy elements FIRST (they may reference memory
 * that would become dangling if we freed the buffer first), THEN free
 * the buffer, THEN free the struct.
 *
 * If destroy_fn is NULL, the loop body is a no-op — we just free the
 * buffer as before. */
void array_destroy(Array *arr)
{
    if (!arr) return;

    /* Call destructor on each live element */
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
 * 6. Core pointer arithmetic and growth (unchanged from Post 6)
 * =========================================================================== */

static void *element_at(const Array *arr, size_t index)
{
    return (char *)arr->data + index * arr->element_size;
}

static ArrayError array_ensure_capacity(Array *arr, size_t min_capacity)
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
                           "array_ensure_capacity: new size overflows size_t");
        return ARRAY_ERR_OVERFLOW;
    }

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
 * 7. Push, Get, Set (unchanged from Post 6)
 * =========================================================================== */

ArrayError array_push(Array *arr, const void *element)
{
    if (!arr || !element) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_NULL, "array_push: NULL argument");
        return ARRAY_ERR_NULL;
    }

    ArrayError err = array_ensure_capacity(arr, arr->size + 1);
    if (err != ARRAY_OK) return err;

    memcpy(element_at(arr, arr->size), element, arr->element_size);
    arr->size++;

    return ARRAY_OK;
}

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

size_t array_size(const Array *arr)          { return arr ? arr->size          : 0; }
size_t array_capacity(const Array *arr)      { return arr ? arr->capacity      : 0; }
size_t array_realloc_count(const Array *arr) { return arr ? arr->realloc_count : 0; }


/* ===========================================================================
 * 8. array_sort() — qsort-compatible sorting
 * ===========================================================================
 *
 * We delegate to the standard library's qsort(). The caller provides a
 * comparator that follows qsort's contract: receives two const void*
 * pointers TO the elements, returns <0 / 0 / >0.
 *
 * Why use qsort's signature instead of a cleaner custom one?
 *
 *   - Standard: every C programmer recognizes it.
 *   - Reusable: comparators written for qsort() work here unchanged.
 *   - Portable: no GCC extensions, no non-standard behavior.
 *   - The ugliness (double dereference for pointer types) is the price
 *     of a universal interface. It's ugly, but it's the SAME ugly that
 *     every C codebase uses.
 *
 * The alternative — a custom signature like
 *   int (*compare)(const void *elem_a, const void *elem_b, void *ctx)
 * — is cleaner (context parameter, no double dereference) but
 * non-standard. Comparators written for it don't work with qsort().
 * We discuss this tradeoff in the article.
 * =========================================================================== */

ArrayError array_sort(Array *arr, ArrayCompareFn compare)
{
    if (!arr) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_NULL, "array_sort: NULL array");
        return ARRAY_ERR_NULL;
    }
    if (!compare) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_NULL, "array_sort: NULL comparator");
        return ARRAY_ERR_NULL;
    }

    /* qsort on an empty or single-element array is a no-op */
    if (arr->size > 1) {
        qsort(arr->data, arr->size, arr->element_size, compare);
    }

    return ARRAY_OK;
}


/* ===========================================================================
 * 9. array_find() — linear search with predicate
 * ===========================================================================
 *
 * Searches for the first element matching a predicate. Returns a pointer
 * to the element, or NULL if not found. Also optionally returns the index.
 *
 * Why a predicate instead of a simple value comparison?
 *   - More flexible: you can search by any criterion, not just equality.
 *   - The context parameter lets you pass the search key without globals.
 *   - For simple equality, the predicate is a 3-line function.
 *
 * For sorted arrays, binary search (bsearch()) would be faster. We don't
 * wrap bsearch here because it requires a comparator, not a predicate,
 * and the array must be sorted first. A future post could add it.
 * =========================================================================== */

void *array_find(const Array *arr, ArrayPredicateFn predicate,
                 const void *context, size_t *out_index)
{
    if (!arr || !predicate) return NULL;

    for (size_t i = 0; i < arr->size; i++) {
        void *elem = element_at(arr, i);
        if (predicate(elem, context)) {
            if (out_index) *out_index = i;
            return elem;
        }
    }

    return NULL;  /* Not found */
}


/* ===========================================================================
 * 10. array_foreach() — iterate with a callback
 * ===========================================================================
 *
 * Calls `fn` on every element, passing an optional context pointer.
 *
 * The context pointer is C's answer to closures. In a language with
 * closures (JavaScript, Python, Rust), you'd capture variables from the
 * enclosing scope. In C, you pack them into a struct and pass a pointer
 * to it as the context. It's manual, but it works without heap allocation
 * and is completely transparent to the caller.
 *
 * The callback receives a non-const pointer — it CAN modify elements.
 * This is intentional: foreach is used for both read-only operations
 * (printing, summing) and mutation (scaling values, updating fields).
 * If you want a const iteration, make your callback accept const void*
 * and cast internally.
 * =========================================================================== */

ArrayError array_foreach(Array *arr, ArrayForeachFn fn, void *context)
{
    if (!arr) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_NULL, "array_foreach: NULL array");
        return ARRAY_ERR_NULL;
    }
    if (!fn) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_NULL, "array_foreach: NULL callback");
        return ARRAY_ERR_NULL;
    }

    for (size_t i = 0; i < arr->size; i++) {
        fn(element_at(arr, i), context);
    }

    return ARRAY_OK;
}


/* ===========================================================================
 * 11. array_remove() — remove with destructor call
 * ===========================================================================
 *
 * Removes the element at `index` by shifting subsequent elements left.
 * If a destructor is set, calls it on the removed element BEFORE the
 * shift overwrites it.
 *
 * The order is: destroy → shift → decrement size.
 * We can't shift first because that would overwrite the element we need
 * to destroy. We can't destroy after the shift because the element's
 * memory has been overwritten by the next element.
 * =========================================================================== */

ArrayError array_remove(Array *arr, size_t index)
{
    if (!arr) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_NULL, "array_remove: NULL array");
        return ARRAY_ERR_NULL;
    }
    if (index >= arr->size) {
        ARRAY_REPORT_ERROR(ARRAY_ERR_BOUNDS, "array_remove: index out of range");
        return ARRAY_ERR_BOUNDS;
    }

    /* Step 1: destroy the element being removed */
    if (arr->destroy_fn) {
        arr->destroy_fn(element_at(arr, index));
    }

    /* Step 2: shift elements left to fill the gap */
    if (index < arr->size - 1) {
        void *dst = element_at(arr, index);
        void *src = element_at(arr, index + 1);
        size_t bytes = (arr->size - index - 1) * arr->element_size;
        memmove(dst, src, bytes);
    }

    /* Step 3: decrement size */
    arr->size--;

    return ARRAY_OK;
}


/* ===========================================================================
 * 12. Type-safe macros (carried forward, minimal additions)
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
            (void)array_push((arr), &_push_tmp);                           \
        }                                                                  \
    } while (0)

#define ARRAY_GET(arr, type, index)                                        \
    ( sizeof(type) != (arr)->element_size                                   \
      ? ( fprintf(stderr,                                                   \
              "ARRAY_GET: type size mismatch at %s:%d\n",                   \
              __FILE__, __LINE__),                                          \
          (type *)NULL )                                                    \
      : (type *)array_get((arr), (index)) )

#define ARRAY_FOREACH_TYPED(arr, type, var_name)                           \
    for (size_t _fe_i = 0; _fe_i < (arr)->size; _fe_i++)                   \
        for (type *var_name = (type *)element_at((arr), _fe_i);            \
             var_name; var_name = NULL)


/* ===========================================================================
 * 13. ASCII visualization (carried forward from Post 6)
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

    /* Show destructor status */
    printf("║  destroy_fn: %-43s  ║\n",
           arr->destroy_fn ? "SET (elements will be cleaned up)" : "NULL");

    printf("╠══════════════════════════════════════════════════════════╣\n");

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
    }

    size_t used_bytes  = sz  * arr->element_size;
    size_t alloc_bytes = cap * arr->element_size;
    double util = cap > 0 ? 100.0 * (double)sz / (double)cap : 0.0;

    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║  %zuB used / %zuB allocated = %.1f%% utilization",
           used_bytes, alloc_bytes, util);
    /* Pad to right border */
    int padding = 56 - snprintf(NULL, 0,
        "  %zuB used / %zuB allocated = %.1f%% utilization",
        used_bytes, alloc_bytes, util);
    for (int i = 0; i < padding; i++) printf(" ");
    printf("║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");
}


/* ===========================================================================
 * 14. Graphviz DOT: Callback dispatch flow
 * ===========================================================================
 * Generates a diagram showing how the Array struct connects to its
 * callback functions and how each operation dispatches through them.
 * This is the conceptual diagram for the blog post.
 * =========================================================================== */

static void generate_callback_dispatch_dot(const char *filename)
{
    FILE *f = fopen(filename, "w");
    if (!f) {
        fprintf(stderr, "generate_callback_dispatch_dot: cannot open %s\n",
                filename);
        return;
    }

    fprintf(f, "digraph CallbackDispatch {\n");
    fprintf(f, "  rankdir=TB;\n");
    fprintf(f, "  graph [fontname=\"Helvetica\", fontsize=12, "
               "bgcolor=\"#fafafa\",\n");
    fprintf(f, "         label=\"Callback Dispatch Flow\\n"
               "Post 7: Function Pointers and Callbacks\", labelloc=t];\n");
    fprintf(f, "  node [fontname=\"Helvetica\", fontsize=10, "
               "style=filled];\n");
    fprintf(f, "  edge [fontname=\"Helvetica\", fontsize=9];\n\n");

    /* The Array struct with function pointer fields */
    fprintf(f, "  array_struct [shape=record, fillcolor=\"#e3f2fd\",\n");
    fprintf(f, "    label=\"{ Array |"
               " data: void* | size: %zu | capacity: %zu |"
               " element_size: %zu |"
               " { destroy_fn | (function pointer) }}\"];\n\n",
            (size_t)0, (size_t)0, (size_t)0);

    /* Operations that accept callbacks */
    fprintf(f, "  sort_op [label=\"array_sort()\\n"
               "accepts: ArrayCompareFn\", "
               "shape=box, fillcolor=\"#c8e6c9\"];\n");
    fprintf(f, "  find_op [label=\"array_find()\\n"
               "accepts: ArrayPredicateFn\", "
               "shape=box, fillcolor=\"#c8e6c9\"];\n");
    fprintf(f, "  foreach_op [label=\"array_foreach()\\n"
               "accepts: ArrayForeachFn\", "
               "shape=box, fillcolor=\"#c8e6c9\"];\n");
    fprintf(f, "  destroy_op [label=\"array_destroy()\\n"
               "uses: arr->destroy_fn\", "
               "shape=box, fillcolor=\"#ffcdd2\"];\n");
    fprintf(f, "  remove_op [label=\"array_remove()\\n"
               "uses: arr->destroy_fn\", "
               "shape=box, fillcolor=\"#ffcdd2\"];\n\n");

    /* User-provided callback functions */
    fprintf(f, "  compare_fn [label=\"compare_ints()\\n"
               "compare_by_name()\\n...\", "
               "shape=ellipse, fillcolor=\"#fff9c4\"];\n");
    fprintf(f, "  predicate_fn [label=\"match_by_id()\\n"
               "match_threshold()\\n...\", "
               "shape=ellipse, fillcolor=\"#fff9c4\"];\n");
    fprintf(f, "  foreach_fn [label=\"print_element()\\n"
               "scale_values()\\n...\", "
               "shape=ellipse, fillcolor=\"#fff9c4\"];\n");
    fprintf(f, "  destroy_fn_node [label=\"free_string()\\n"
               "free_person()\\n...\", "
               "shape=ellipse, fillcolor=\"#fff9c4\"];\n\n");

    /* qsort delegation */
    fprintf(f, "  qsort_fn [label=\"qsort()\\n(standard library)\", "
               "shape=box, fillcolor=\"#e0e0e0\"];\n\n");

    /* Edges: operations → callbacks */
    fprintf(f, "  sort_op -> qsort_fn "
               "[label=\"delegates to\"];\n");
    fprintf(f, "  qsort_fn -> compare_fn "
               "[label=\"calls per\\ncomparison\", style=dashed];\n");
    fprintf(f, "  find_op -> predicate_fn "
               "[label=\"calls per\\nelement\", style=dashed];\n");
    fprintf(f, "  foreach_op -> foreach_fn "
               "[label=\"calls per\\nelement\", style=dashed];\n");
    fprintf(f, "  destroy_op -> destroy_fn_node "
               "[label=\"calls per\\nelement\", style=dashed, "
               "color=\"#c62828\"];\n");
    fprintf(f, "  remove_op -> destroy_fn_node "
               "[label=\"calls on\\nremoved element\", style=dashed, "
               "color=\"#c62828\"];\n\n");

    /* Array struct → stored destroy_fn */
    fprintf(f, "  array_struct -> destroy_op "
               "[label=\"destroy_fn\\nstored in struct\", "
               "color=\"#1565c0\"];\n");
    fprintf(f, "  array_struct -> remove_op "
               "[label=\"destroy_fn\\nstored in struct\", "
               "color=\"#1565c0\"];\n\n");

    /* Caller → operations */
    fprintf(f, "  caller [label=\"Caller Code\", shape=oval, "
               "fillcolor=\"#f3e5f5\"];\n");
    fprintf(f, "  caller -> sort_op [label=\"passes comparator\"];\n");
    fprintf(f, "  caller -> find_op [label=\"passes predicate\"];\n");
    fprintf(f, "  caller -> foreach_op [label=\"passes callback\"];\n");
    fprintf(f, "  caller -> destroy_op [label=\"calls\"];\n");
    fprintf(f, "  caller -> remove_op [label=\"calls\"];\n");

    /* Legend */
    fprintf(f, "\n  subgraph cluster_legend {\n");
    fprintf(f, "    label=\"Legend\";\n");
    fprintf(f, "    style=rounded; color=\"#bdbdbd\";\n");
    fprintf(f, "    fontname=\"Helvetica\"; fontsize=10;\n");
    fprintf(f, "    leg_op [label=\"Array operation\", shape=box, "
               "fillcolor=\"#c8e6c9\"];\n");
    fprintf(f, "    leg_cb [label=\"User callback\", shape=ellipse, "
               "fillcolor=\"#fff9c4\"];\n");
    fprintf(f, "    leg_destr [label=\"Destructive op\", shape=box, "
               "fillcolor=\"#ffcdd2\"];\n");
    fprintf(f, "    leg_op -> leg_cb [style=invis];\n");
    fprintf(f, "    leg_cb -> leg_destr [style=invis];\n");
    fprintf(f, "  }\n");

    fprintf(f, "}\n");
    fclose(f);

    printf("DOT file written to %s\n", filename);
    printf("Render: dot -Tsvg %s -o output/post_07_callback_dispatch.svg\n\n",
           filename);
}


/* ===========================================================================
 * 15. Demonstrations
 * =========================================================================== */

/* --- Comparators --- */

/* Compare two ints. qsort passes pointers to the elements in the buffer.
 * Since the buffer stores int values, each argument is an int*. */
static int compare_ints_asc(const void *a, const void *b)
{
    int va = *(const int *)a;
    int vb = *(const int *)b;
    /* Subtraction trick (va - vb) can overflow for extreme values.
     * The safe pattern uses explicit comparison: */
    return (va > vb) - (va < vb);
}

static int compare_ints_desc(const void *a, const void *b)
{
    int va = *(const int *)a;
    int vb = *(const int *)b;
    return (vb > va) - (vb < va);
}


/* --- Demo 1: Sorting integers --- */

static void demo_sort_ints(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Demo 1: Sorting an array of integers\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    Array *arr = ARRAY_CREATE(int, 8);
    if (!arr) return;

    int values[] = {42, 17, 8, 99, 23, 71, 3, 55};
    for (int i = 0; i < 8; i++) {
        array_push(arr, &values[i]);
    }

    array_visualize_ascii(arr, "Before sort", "int", print_int);

    printf("  Sorting ascending with compare_ints_asc...\n\n");
    array_sort(arr, compare_ints_asc);
    array_visualize_ascii(arr, "After sort (ascending)", "int", print_int);

    printf("  Sorting descending with compare_ints_desc...\n\n");
    array_sort(arr, compare_ints_desc);
    array_visualize_ascii(arr, "After sort (descending)", "int", print_int);

    array_destroy(arr);
}


/* --- A struct for demos 2-4 --- */

typedef struct {
    int   id;
    char  name[32];
    float score;
} Person;

static void print_person(char *buf, size_t bufsize, const void *elem)
{
    const Person *p = (const Person *)elem;
    snprintf(buf, bufsize, "{id=%d, \"%s\", %.1f}", p->id, p->name, p->score);
}

/* Compare Person by name (alphabetical). Since the buffer stores Person
 * values (not pointers), each argument is a Person*. */
static int compare_by_name(const void *a, const void *b)
{
    const Person *pa = (const Person *)a;
    const Person *pb = (const Person *)b;
    return strcmp(pa->name, pb->name);
}

/* Compare Person by score (descending — highest first) */
static int compare_by_score_desc(const void *a, const void *b)
{
    const Person *pa = (const Person *)a;
    const Person *pb = (const Person *)b;
    /* Float comparison: avoid subtraction (returns int, truncates) */
    if (pa->score > pb->score) return -1;
    if (pa->score < pb->score) return  1;
    return 0;
}


/* --- Demo 2: Sorting structs by different fields --- */

static void demo_sort_structs(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Demo 2: Sorting structs with different comparators\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    Array *arr = ARRAY_CREATE(Person, 4);
    if (!arr) return;

    Person people[] = {
        {3, "Charlie", 87.5f},
        {1, "Alice",   95.2f},
        {4, "Diana",   72.0f},
        {2, "Bob",     91.8f},
    };
    for (int i = 0; i < 4; i++) {
        array_push(arr, &people[i]);
    }

    array_visualize_ascii(arr, "Unsorted people", "Person", print_person);

    printf("  Sorting by name (alphabetical)...\n\n");
    array_sort(arr, compare_by_name);
    array_visualize_ascii(arr, "Sorted by name", "Person", print_person);

    printf("  Sorting by score (highest first)...\n\n");
    array_sort(arr, compare_by_score_desc);
    array_visualize_ascii(arr, "Sorted by score (desc)", "Person", print_person);

    array_destroy(arr);
}


/* --- Demo 3: Find with predicate --- */

static int match_by_id(const void *element, const void *context)
{
    const Person *p = (const Person *)element;
    int target_id = *(const int *)context;
    return p->id == target_id;
}

static int match_score_above(const void *element, const void *context)
{
    const Person *p = (const Person *)element;
    float threshold = *(const float *)context;
    return p->score > threshold;
}

static void demo_find(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Demo 3: Finding elements with predicates\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    Array *arr = ARRAY_CREATE(Person, 4);
    if (!arr) return;

    Person people[] = {
        {3, "Charlie", 87.5f},
        {1, "Alice",   95.2f},
        {4, "Diana",   72.0f},
        {2, "Bob",     91.8f},
    };
    for (int i = 0; i < 4; i++) {
        array_push(arr, &people[i]);
    }

    /* Find by ID */
    int search_id = 2;
    size_t found_index;
    Person *found = (Person *)array_find(arr, match_by_id,
                                          &search_id, &found_index);
    if (found) {
        printf("  Found person with id=%d: \"%s\" (score=%.1f) at index %zu\n\n",
               search_id, found->name, found->score, found_index);
    } else {
        printf("  Person with id=%d not found\n\n", search_id);
    }

    /* Find by score threshold */
    float threshold = 90.0f;
    found = (Person *)array_find(arr, match_score_above,
                                  &threshold, &found_index);
    if (found) {
        printf("  First person with score > %.1f: \"%s\" (score=%.1f) "
               "at index %zu\n\n",
               threshold, found->name, found->score, found_index);
    }

    /* Search for a non-existent ID */
    search_id = 99;
    found = (Person *)array_find(arr, match_by_id,
                                  &search_id, &found_index);
    printf("  Person with id=%d: %s\n\n",
           search_id, found ? found->name : "NOT FOUND");

    array_destroy(arr);
}


/* --- Demo 4: foreach with context --- */

/* Foreach callback: accumulate total score.
 * The context points to a double that we add each score to. */
static void sum_scores(void *element, void *context)
{
    const Person *p = (const Person *)element;
    double *total = (double *)context;
    *total += (double)p->score;
}

/* Foreach callback: apply a curve (add 5 points) to each score */
static void apply_curve(void *element, void *context)
{
    Person *p = (Person *)element;
    float bonus = *(const float *)context;
    p->score += bonus;
}

static void demo_foreach(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Demo 4: Iterating with foreach and context\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    Array *arr = ARRAY_CREATE(Person, 4);
    if (!arr) return;

    Person people[] = {
        {1, "Alice",   85.0f},
        {2, "Bob",     90.0f},
        {3, "Charlie", 78.0f},
        {4, "Diana",   92.0f},
    };
    for (int i = 0; i < 4; i++) {
        array_push(arr, &people[i]);
    }

    /* Sum all scores */
    double total = 0.0;
    array_foreach(arr, sum_scores, &total);
    printf("  Total of all scores: %.1f\n", total);
    printf("  Average: %.1f\n\n", total / (double)arr->size);

    /* Apply a 5-point curve to every score */
    float curve = 5.0f;
    printf("  Applying +%.1f curve to all scores...\n\n", curve);
    array_foreach(arr, apply_curve, &curve);

    array_visualize_ascii(arr, "After applying curve", "Person", print_person);

    /* Sum again to verify */
    total = 0.0;
    array_foreach(arr, sum_scores, &total);
    printf("  New total: %.1f (was %.1f before curve)\n",
           total, total - (double)(curve * (float)arr->size));
    printf("  New average: %.1f\n\n", total / (double)arr->size);

    array_destroy(arr);
}


/* --- Demo 5: Destructors for owned resources --- */

/* Destructor for an array of heap-allocated strings (char*).
 *
 * The array stores char* values — each element is a pointer. The array
 * owns copies of the pointers, but NOT the strings they point to. Without
 * a destructor, destroying the array frees the pointer copies but leaks
 * the strings.
 *
 * The destructor receives a pointer TO the element in the buffer.
 * For char* elements, that's a char** — a pointer to the pointer. */
static void destroy_string(void *element)
{
    char **str_ptr = (char **)element;
    free(*str_ptr);
    *str_ptr = NULL;  /* Defensive: prevent use-after-free */
}

static void print_string(char *buf, size_t bufsize, const void *elem)
{
    const char *const *str_ptr = (const char *const *)elem;
    snprintf(buf, bufsize, "\"%s\"", *str_ptr ? *str_ptr : "(null)");
}

static void demo_destructor(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Demo 5: Destructors — cleaning up owned resources\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    /* Create an array of char* (pointers to strings) */
    Array *arr = ARRAY_CREATE(char *, 4);
    if (!arr) return;

    /* Register the destructor */
    array_set_destroy_fn(arr, destroy_string);

    /* Push heap-allocated strings. strdup() calls malloc internally. */
    char *s1 = strdup("Hello");
    char *s2 = strdup("World");
    char *s3 = strdup("Callbacks");
    char *s4 = strdup("Rock");

    array_push(arr, &s1);
    array_push(arr, &s2);
    array_push(arr, &s3);
    array_push(arr, &s4);

    array_visualize_ascii(arr, "Heap-allocated strings (char*)",
                          "char*", print_string);

    printf("  Removing element at index 1 (\"%s\")...\n", s2);
    printf("  The destructor will free the string before shifting.\n\n");
    ArrayError err = array_remove(arr, 1);
    printf("  array_remove(arr, 1) → %s\n\n", array_error_str(err));

    array_visualize_ascii(arr, "After removing index 1", "char*", print_string);

    printf("  Now calling array_destroy()...\n");
    printf("  The destructor will free each remaining string.\n");
    printf("  Without the destructor, these strings would leak.\n\n");

    array_destroy(arr);
    /* s1, s3, s4 have been freed by the destructor.
     * s2 was freed by array_remove. No leaks. */

    printf("  Array destroyed. All strings freed. No memory leaks.\n");
    printf("  (Verify with: valgrind ./build/post_07)\n\n");
}


/* --- Demo 6: The qsort comparator subtlety --- */

/* Compare heap-allocated strings (char*).
 *
 * THIS IS THE TRICKY PART. The array stores char* values. Each element
 * in the buffer is a char*. qsort passes a pointer to each element.
 * So each argument is a pointer to a char* — that is, a char**.
 *
 * If the array stored Person values (not pointers), each argument would
 * be a Person*. The rule: qsort always adds one level of indirection.
 *
 * For value types:  element is T     → comparator receives T*
 * For pointer types: element is T*   → comparator receives T**
 */
static int compare_strings(const void *a, const void *b)
{
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;
    return strcmp(*sa, *sb);  /* Dereference once to get the char* */
}

static void demo_qsort_subtlety(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Demo 6: The qsort double-dereference for pointer types\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    Array *arr = ARRAY_CREATE(char *, 4);
    if (!arr) return;
    array_set_destroy_fn(arr, destroy_string);

    char *strings[] = {
        strdup("delta"),
        strdup("alpha"),
        strdup("charlie"),
        strdup("bravo"),
    };
    for (int i = 0; i < 4; i++) {
        array_push(arr, &strings[i]);
    }

    array_visualize_ascii(arr, "Unsorted strings", "char*", print_string);

    printf("  The comparator receives char** (not char*):\n\n");
    printf("    int compare_strings(const void *a, const void *b) {\n");
    printf("        const char *const *sa = (const char *const *)a;\n");
    printf("        const char *const *sb = (const char *const *)b;\n");
    printf("        return strcmp(*sa, *sb);  // dereference once\n");
    printf("    }\n\n");

    array_sort(arr, compare_strings);
    array_visualize_ascii(arr, "Sorted strings (alphabetical)", "char*",
                          print_string);

    printf("  Why char** ? Because:\n");
    printf("    Buffer: [ char* | char* | char* | char* ]\n");
    printf("    qsort gives you a pointer TO each char* in the buffer.\n");
    printf("    A pointer to a char* is a char**.\n\n");

    printf("  For value types (int, Person), you get one level:\n");
    printf("    Buffer: [ int  | int  | int  | int  ]\n");
    printf("    qsort gives you int* for each element.\n\n");

    array_destroy(arr);
}


/* --- Knowledge test --- */

static void knowledge_test(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Knowledge Test\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    printf("  Q: Write a comparator function that sorts Person structs\n");
    printf("     by name. Why does qsort pass void** not void* when\n");
    printf("     the array stores pointer types?\n\n");

    printf("  A: The comparator for Person structs sorted by name:\n\n");
    printf("     int compare_by_name(const void *a, const void *b) {\n");
    printf("         const Person *pa = (const Person *)a;\n");
    printf("         const Person *pb = (const Person *)b;\n");
    printf("         return strcmp(pa->name, pb->name);\n");
    printf("     }\n\n");
    printf("     This works because Person is a value type — the array\n");
    printf("     stores Person values directly in the buffer. qsort\n");
    printf("     passes a pointer to each Person, so each argument\n");
    printf("     is a Person* (cast to const void*).\n\n");

    printf("     For pointer types (char*, Person*), qsort passes a\n");
    printf("     pointer to the pointer. If the buffer holds char*\n");
    printf("     values, each argument is a char** (cast to const\n");
    printf("     void*). You must dereference once to get the char*.\n\n");

    printf("     The rule: qsort always gives you &buffer[i], which\n");
    printf("     is a pointer to whatever the buffer stores. For T,\n");
    printf("     you get T*. For T*, you get T**.\n");
}


/* ===========================================================================
 * main
 * =========================================================================== */

int main(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Post 7: Function Pointers and Callbacks —\n");
    printf("          Sort, Search, Destroy\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    demo_sort_ints();
    demo_sort_structs();
    demo_find();
    demo_foreach();
    demo_destructor();
    demo_qsort_subtlety();

    /* Generate the callback dispatch diagram */
    generate_callback_dispatch_dot("output/post_07_callback_dispatch.dot");

    knowledge_test();

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("  End of Post 7. Next: bounds checking and defensive APIs.\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    return 0;
}
