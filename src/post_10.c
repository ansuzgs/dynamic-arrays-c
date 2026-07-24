/* ============================================================================
 * Post 10: "Iterators and Traversal Patterns in C"
 * ============================================================================
 *
 * This file is entirely self-contained. Compile with:
 *   gcc -Wall -Wextra -std=c11 -o build/post_10 src_posts/post_10.c
 *
 * Run:
 *   ./build/post_10                       (ASCII visualization to stdout)
 *   ./build/post_10 > outputs/post_10.txt (save ASCII output)
 *
 * The program also writes outputs/post_10_iterator_ownership.dot (Graphviz).
 * Render with:  dot -Tsvg outputs/post_10_iterator_ownership.dot \
 *                        -o outputs/post_10_iterator_ownership.svg
 *
 * Learning outcome: implement a lightweight iterator struct with begin/next/end
 * semantics, understand when and why iterators become invalid, and use filtered
 * iteration with predicate callbacks.
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
    ARRAY_ERR_EMPTY      = -8,
    ARRAY_ERR_ITER       = -9   /* NEW: iterator invalidated */
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
        case ARRAY_ERR_ITER:     return "iterator invalidated";
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
 * NEW in this post: mod_count. Every operation that changes the array's
 * structure — push, insert, remove, swap_remove, sort — increments this
 * counter. Iterators snapshot mod_count at creation. If the snapshot doesn't
 * match the current mod_count, the iterator knows the array was modified
 * and can report invalidation instead of silently reading wrong data.
 *
 * Note: array_set() does NOT increment mod_count. It replaces an element
 * in place without changing size, capacity, or element positions. An
 * iterator pointing at index 3 still points at index 3 after a set() on
 * index 7 — the traversal structure is unchanged.
 * =========================================================================== */

typedef struct {
    void          *data;
    size_t         size;
    size_t         capacity;
    size_t         element_size;
    size_t         realloc_count;
    size_t         bytes_shifted;
    size_t         mod_count;        /* NEW: mutation counter */
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
    arr->mod_count     = 0;
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
 *
 * Every mutating operation increments mod_count. This is the mechanism
 * that makes iterator invalidation detectable.
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
    arr->mod_count++;       /* Structural change */

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
    /* No mod_count increment — set() doesn't change structure */
    return ARRAY_OK;
}

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

    if (index < arr->size) {
        void *dst = element_at_unchecked(arr, index + 1);
        void *src = element_at_unchecked(arr, index);
        size_t bytes = (arr->size - index) * arr->element_size;
        memmove(dst, src, bytes);
        arr->bytes_shifted += bytes;
    }

    memcpy(element_at_unchecked(arr, index), element, arr->element_size);
    arr->size++;
    arr->mod_count++;       /* Structural change */

    return ARRAY_OK;
}

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
        arr->bytes_shifted += bytes;
    }

    arr->size--;
    arr->mod_count++;       /* Structural change */
    return ARRAY_OK;
}

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

    if (arr->destroy_fn) {
        arr->destroy_fn(element_at_unchecked(arr, index));
    }

    if (index < arr->size - 1) {
        void *dst = element_at_unchecked(arr, index);
        void *src = element_at_unchecked(arr, arr->size - 1);
        memcpy(dst, src, arr->element_size);
    }

    arr->size--;
    arr->mod_count++;       /* Structural change */
    return ARRAY_OK;
}

size_t array_size(const Array *arr)          { return arr ? arr->size          : 0; }
size_t array_capacity(const Array *arr)      { return arr ? arr->capacity      : 0; }
size_t array_realloc_count(const Array *arr) { return arr ? arr->realloc_count : 0; }
size_t array_bytes_shifted(const Array *arr) { return arr ? arr->bytes_shifted : 0; }
size_t array_mod_count(const Array *arr)     { return arr ? arr->mod_count     : 0; }

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

    arr->mod_count++;       /* Sort changes element positions */
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
 * 10. The Iterator — THIS POST'S CORE ADDITION
 * ===========================================================================
 *
 * An ArrayIterator is a lightweight value type (24 bytes on 64-bit) that
 * encapsulates the state of a traversal: which array, which position, and
 * a snapshot of the array's modification counter at the moment the iterator
 * was created.
 *
 * Design decisions:
 *
 *   1. VALUE TYPE, NOT HEAP-ALLOCATED. The iterator is a struct returned by
 *      value, not a pointer to a malloc'd object. This means no cleanup is
 *      needed — the iterator disappears when it goes out of scope, like an
 *      int. This is the idiomatic C pattern for lightweight cursors.
 *
 *   2. INDEX-BASED, NOT POINTER-BASED. The iterator stores an index into
 *      the array, not a raw pointer into the buffer. An index survives
 *      realloc — after realloc, index 3 is still index 3, even though the
 *      buffer's base address changed. A raw pointer would become a dangling
 *      pointer after realloc, pointing into freed memory. This is why C++
 *      vector iterators (which are often pointers) are invalidated by push.
 *
 *   3. MODIFICATION COUNTER FOR INVALIDATION DETECTION. The iterator
 *      snapshots mod_count at creation. When you call array_iter_get() or
 *      array_iter_next(), the iterator compares its snapshot against the
 *      array's current mod_count. If they differ, the array was modified
 *      since the iterator was created — indices may have shifted, and the
 *      traversal is no longer reliable.
 *
 *      This is a DETECTION mechanism, not a PREVENTION mechanism. The
 *      iterator can't stop you from calling array_push() during iteration.
 *      It can tell you that you did, so you can decide what to do about it.
 *      Java's ConcurrentModificationException works the same way.
 *
 *   4. NON-OWNING REFERENCE. The iterator does not own the array. It holds
 *      a pointer (not a copy) and assumes the array outlives the iterator.
 *      If you destroy the array while an iterator exists, the iterator's
 *      array pointer becomes dangling — the same as any other use-after-free
 *      in C. The iterator cannot detect this; it's a contract the caller
 *      must uphold.
 * =========================================================================== */

typedef struct {
    Array  *array;           /* Non-owning pointer to the array            */
    size_t  index;           /* Current position in the array              */
    size_t  _mod_snapshot;   /* mod_count at iterator creation             */
} ArrayIterator;


/* --- Create an iterator at the beginning of the array --- */

ArrayIterator array_iter_begin(Array *arr)
{
    ArrayIterator it;
    it.array         = arr;
    it.index         = 0;
    it._mod_snapshot = arr ? arr->mod_count : 0;
    return it;
}

/* --- Create an iterator at a specific index --- */

ArrayIterator array_iter_at(Array *arr, size_t index)
{
    ArrayIterator it;
    it.array         = arr;
    it.index         = index;
    it._mod_snapshot = arr ? arr->mod_count : 0;
    return it;
}

/* --- Check whether the iterator has been invalidated ---
 *
 * Returns 1 if the array was modified since the iterator was created.
 * This means element positions may have changed (insert/remove shifted
 * indices) or the buffer was reallocated (push triggered growth).
 *
 * Note: invalidation doesn't mean the iterator is *wrong* — a push that
 * doesn't trigger realloc might leave all existing indices valid. But we
 * can't know that cheaply, so we flag conservatively: any mutation counts.
 */

int array_iter_invalidated(const ArrayIterator *it)
{
    if (!it || !it->array) return 1;
    return it->_mod_snapshot != it->array->mod_count;
}

/* --- Check whether the iterator points to a valid element ---
 *
 * An iterator is "valid" (usable) when:
 *   1. It points to a real array (not NULL).
 *   2. Its index is within the array's current size.
 *   3. The array has not been modified since the iterator was created.
 *
 * This is the loop condition: for (it = begin; valid(&it); next(&it))
 */

int array_iter_valid(const ArrayIterator *it)
{
    if (!it || !it->array)                       return 0;
    if (it->_mod_snapshot != it->array->mod_count) return 0;
    if (it->index >= it->array->size)            return 0;
    return 1;
}

/* --- Get the element at the iterator's current position --- */

void *array_iter_get(const ArrayIterator *it)
{
    if (!array_iter_valid(it)) return NULL;
    return element_at_unchecked(it->array, it->index);
}

/* --- Advance the iterator to the next element --- */

void array_iter_next(ArrayIterator *it)
{
    if (it) it->index++;
}

/* --- Move the iterator to the previous element --- */

void array_iter_prev(ArrayIterator *it)
{
    if (it && it->index > 0) it->index--;
}

/* --- Return the iterator's current index --- */

size_t array_iter_index(const ArrayIterator *it)
{
    return it ? it->index : 0;
}

/* --- Advance to the next element matching a predicate ---
 *
 * Starts checking from the CURRENT position. If the current element
 * matches, the iterator stays put. Otherwise, it advances until it
 * finds a match or reaches the end (at which point array_iter_valid
 * will return 0).
 *
 * This is the building block for filtered iteration: create an
 * iterator, call find_next to jump to the first match, process it,
 * call next + find_next to jump to the second match, and so on.
 */

void array_iter_find_next(ArrayIterator *it, ArrayPredicateFn predicate,
                          const void *context)
{
    if (!it || !it->array || !predicate) return;

    while (it->index < it->array->size &&
           it->_mod_snapshot == it->array->mod_count) {
        void *elem = element_at_unchecked(it->array, it->index);
        if (predicate(elem, context)) return;   /* Found a match */
        it->index++;
    }
}

/* --- Reset an iterator to track the array's current mod_count ---
 *
 * After deliberately modifying the array during iteration (e.g., removing
 * elements in a known-safe pattern), the caller can "re-arm" the iterator
 * so subsequent valid/get calls succeed. Use with caution: this tells the
 * iterator "I know what I'm doing, the index is still correct."
 */

void array_iter_resync(ArrayIterator *it)
{
    if (it && it->array) {
        it->_mod_snapshot = it->array->mod_count;
    }
}


/* ===========================================================================
 * 11. Type-safe iterator macro
 * ===========================================================================
 *
 * ARRAY_FOREACH wraps the begin/valid/next/get pattern into a single
 * macro that declares a typed pointer variable for the loop body.
 *
 * Usage:
 *   ARRAY_FOREACH(arr, int, val) {
 *       printf("%d\n", *val);
 *   }
 *
 * Expands to a for-loop with an ArrayIterator and a typed pointer.
 * The cast is safe because we check element_size at the start.
 * =========================================================================== */

#define ARRAY_FOREACH(arr, type, var_name) \
    for (ArrayIterator _it_##var_name = array_iter_begin(arr); \
         array_iter_valid(&_it_##var_name) && \
         sizeof(type) == (arr)->element_size; \
         array_iter_next(&_it_##var_name)) \
        for (type *var_name = (type *)array_iter_get(&_it_##var_name); \
             var_name; var_name = NULL)


/* ===========================================================================
 * 12. ASCII Visualization — Iterator position
 * ===========================================================================
 *
 * Shows the array contents with an arrow indicating the iterator's current
 * position. This makes iterator movement tangible.
 * =========================================================================== */

static void print_int(char *buf, size_t bufsize, const void *elem)
{
    snprintf(buf, bufsize, "%d", *(const int *)elem);
}

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

static void visualize_iterator_position(const Array *arr,
                                        const ArrayIterator *it,
                                        const char *label,
                                        PrintElementFn printer)
{
    printf("  %s\n", label);
    printf("  ");

    /* Print element boxes */
    for (size_t i = 0; i < arr->size; i++) {
        char buf[8];
        printer(buf, sizeof(buf), element_at_unchecked(arr, i));
        printf("[%3s] ", buf);
    }
    printf("\n  ");

    /* Print the arrow under the iterator's position */
    for (size_t i = 0; i < arr->size; i++) {
        if (i == it->index) {
            printf("  ↑   ");
        } else {
            printf("      ");
        }
    }
    printf("\n  ");

    /* Print iterator label */
    for (size_t i = 0; i < arr->size; i++) {
        if (i == it->index) {
            printf(" it   ");
        } else {
            printf("      ");
        }
    }

    if (it->index >= arr->size) {
        printf("  → it (past end, index=%zu)", it->index);
    }
    printf("\n\n");
}


/* Full state visualization (carried forward, updated with mod_count) */
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
    printf("║  size: %-8zu  capacity: %-8zu  mod_count: %-5zu     ║\n",
           sz, cap, arr->mod_count);
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

    char util_line[128];
    snprintf(util_line, sizeof(util_line),
        "  %zuB used / %zuB allocated = %.1f%% utilization",
        used_bytes, alloc_bytes, util);
    printf("║%-56s  ║\n", util_line);

    printf("╚══════════════════════════════════════════════════════════╝\n\n");
}


/* ===========================================================================
 * 13. DOT Generation — Iterator → Array ownership diagram
 * ===========================================================================
 *
 * This function generates a Graphviz diagram showing the structural
 * relationship between an ArrayIterator and the Array it references.
 * The blog author uses this to generate the post's main illustration.
 * =========================================================================== */

static void generate_iterator_ownership_dot(const char *filename)
{
    FILE *f = fopen(filename, "w");
    if (!f) {
        fprintf(stderr, "generate_iterator_ownership_dot: cannot open %s\n",
                filename);
        return;
    }

    fprintf(f, "digraph IteratorOwnership {\n");
    fprintf(f, "  rankdir=LR;\n");
    fprintf(f, "  graph [fontname=\"Helvetica\", fontsize=12, "
               "bgcolor=\"#fafafa\",\n");
    fprintf(f, "         label=\"Iterator ↔ Array Relationship\\n"
               "Post 10: Iterators and Traversal Patterns\",\n");
    fprintf(f, "         labelloc=t, nodesep=0.5, ranksep=0.8];\n");
    fprintf(f, "  node [fontname=\"Helvetica\", fontsize=10, style=filled];\n");
    fprintf(f, "  edge [fontname=\"Helvetica\", fontsize=9];\n\n");

    /* Iterator struct */
    fprintf(f, "  subgraph cluster_iterator {\n");
    fprintf(f, "    label=\"ArrayIterator (24 bytes, stack)\";\n");
    fprintf(f, "    style=rounded; color=\"#1565c0\"; penwidth=2;\n");
    fprintf(f, "    fontname=\"Helvetica\"; fontsize=11;\n\n");
    fprintf(f, "    iter [shape=record, fillcolor=\"#e3f2fd\",\n");
    fprintf(f, "      label=\"{ArrayIterator|"
               "array: Array*\\l|"
               "index: size_t (= 3)\\l|"
               "_mod_snapshot: size_t (= 7)\\l}\"];\n");
    fprintf(f, "  }\n\n");

    /* Array struct */
    fprintf(f, "  subgraph cluster_array {\n");
    fprintf(f, "    label=\"Array (heap)\";\n");
    fprintf(f, "    style=rounded; color=\"#2e7d32\"; penwidth=2;\n");
    fprintf(f, "    fontname=\"Helvetica\"; fontsize=11;\n\n");
    fprintf(f, "    arr [shape=record, fillcolor=\"#c8e6c9\",\n");
    fprintf(f, "      label=\"{Array|"
               "data: void*\\l|"
               "size: 8\\l|"
               "capacity: 16\\l|"
               "element_size: 4\\l|"
               "mod_count: 7  ✓ match\\l|"
               "destroy_fn: NULL\\l}\"];\n\n");

    /* Data buffer */
    fprintf(f, "    buf [shape=record, fillcolor=\"#e8f5e9\",\n");
    fprintf(f, "      label=\"{Buffer (64 bytes)|"
               "{[0] 10|[1] 20|[2] 30|"
               "<idx3>[3] 40|[4] 50|[5] 60|[6] 70|[7] 80|"
               "[8..15] ·}}\"];\n");
    fprintf(f, "  }\n\n");

    /* Edges */
    fprintf(f, "  iter -> arr [label=\"  non-owning\\n  reference\", "
               "color=\"#1565c0\", penwidth=2];\n");
    fprintf(f, "  arr -> buf [label=\"  owns\", color=\"#2e7d32\"];\n");
    fprintf(f, "  iter -> buf:idx3 [label=\"  index=3\\n  points here\", "
               "style=dashed, color=\"#e65100\", penwidth=1.5];\n\n");

    /* Invalidation scenario */
    fprintf(f, "  subgraph cluster_invalid {\n");
    fprintf(f, "    label=\"After array_push() triggers realloc\";\n");
    fprintf(f, "    style=rounded; color=\"#c62828\"; penwidth=2;\n");
    fprintf(f, "    fontname=\"Helvetica\"; fontsize=11;\n\n");
    fprintf(f, "    iter_stale [shape=record, fillcolor=\"#ffcdd2\",\n");
    fprintf(f, "      label=\"{ArrayIterator|"
               "array: Array*\\l|"
               "index: 3\\l|"
               "_mod_snapshot: 7  ✗ stale\\l}\"];\n\n");

    fprintf(f, "    arr_new [shape=record, fillcolor=\"#c8e6c9\",\n");
    fprintf(f, "      label=\"{Array|"
               "data: void* (NEW addr)\\l|"
               "size: 9\\l|"
               "mod_count: 8  (changed)\\l}\"];\n");
    fprintf(f, "  }\n\n");

    fprintf(f, "  iter_stale -> arr_new "
               "[label=\"  snapshot ≠ mod_count\\n  → invalidated!\", "
               "color=\"#c62828\", style=bold];\n\n");

    /* Legend */
    fprintf(f, "  subgraph cluster_legend {\n");
    fprintf(f, "    label=\"Key Insight\";\n");
    fprintf(f, "    style=rounded; color=\"#616161\"; penwidth=1;\n");
    fprintf(f, "    fontname=\"Helvetica\"; fontsize=10;\n");
    fprintf(f, "    legend [shape=note, fillcolor=\"#fff9c4\",\n");
    fprintf(f, "      label=\"Index-based: index 3 is still valid\\l"
               "after realloc (buffer moved, index didn't).\\l\\l"
               "Pointer-based: a cached pointer into the old\\l"
               "buffer is DANGLING after realloc.\\l\\l"
               "mod_count detects the mutation so the\\l"
               "iterator can warn instead of silently\\l"
               "reading wrong data.\\l\"];\n");
    fprintf(f, "  }\n");

    fprintf(f, "}\n");
    fclose(f);

    printf("  DOT file written to %s\n", filename);
    printf("  Render: dot -Tsvg %s -o output/post_10_iterator_ownership.svg"
           "\n\n", filename);
}


/* ===========================================================================
 * 14. Helper predicates for demos
 * =========================================================================== */

static int is_even(const void *elem, const void *context)
{
    (void)context;
    return (*(const int *)elem) % 2 == 0;
}

static int is_greater_than(const void *elem, const void *context)
{
    int threshold = *(const int *)context;
    return *(const int *)elem > threshold;
}


/* ===========================================================================
 * 15. Demonstrations
 * =========================================================================== */


/* --- Demo 1: Basic iterator traversal (begin/valid/next/get) --- */

static void demo_basic_traversal(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Demo 1: Basic Iterator Traversal\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    Array *arr = ARRAY_CREATE(int, 8);
    if (!arr) return;

    int vals[] = {10, 20, 30, 40, 50, 60, 70, 80};
    for (int i = 0; i < 8; i++) {
        array_push(arr, &vals[i]);
    }

    array_visualize_ascii(arr, "Array with 8 elements", "int", print_int);

    printf("  Traversal with ArrayIterator:\n\n");
    printf("  ArrayIterator it = array_iter_begin(arr);\n");
    printf("  while (array_iter_valid(&it)) {\n");
    printf("      int *val = (int *)array_iter_get(&it);\n");
    printf("      process(*val);\n");
    printf("      array_iter_next(&it);\n");
    printf("  }\n\n");

    printf("  Step-by-step iterator positions:\n\n");

    ArrayIterator it = array_iter_begin(arr);
    int step = 0;

    while (array_iter_valid(&it)) {
        char label[64];
        int *val = (int *)array_iter_get(&it);
        snprintf(label, sizeof(label),
                 "Step %d: index=%zu, *get()=%d", step, it.index,
                 val ? *val : -1);
        visualize_iterator_position(arr, &it, label, print_int);
        array_iter_next(&it);
        step++;

        /* Show only first 3 and last step to keep output manageable */
        if (step == 3 && arr->size > 5) {
            printf("  ... (advancing through indices 3-%zu) ...\n\n",
                   arr->size - 2);
            it.index = arr->size - 1;
            step = (int)arr->size - 1;
        }
    }

    /* Show the past-end state */
    char end_label[64];
    snprintf(end_label, sizeof(end_label),
             "Step %d: index=%zu — past end, valid()=0", step, it.index);
    printf("  %s\n", end_label);
    printf("  ");
    for (size_t i = 0; i < arr->size; i++) {
        char buf[8];
        print_int(buf, sizeof(buf), element_at_unchecked(arr, i));
        printf("[%3s] ", buf);
    }
    printf(" ‖\n  ");
    for (size_t i = 0; i < arr->size; i++) {
        printf("      ");
    }
    printf(" ↑ it (past end)\n\n");

    array_destroy(arr);
}


/* --- Demo 2: Index loop vs iterator side-by-side --- */

static void demo_index_vs_iterator(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Demo 2: Index-Based Loop vs Iterator\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    Array *arr = ARRAY_CREATE(int, 8);
    if (!arr) return;

    for (int i = 1; i <= 6; i++) {
        int val = i * 10;
        array_push(arr, &val);
    }

    /* Index-based traversal */
    printf("  Index-based loop:\n");
    printf("  ─────────────────────────────────────────────────\n");
    printf("  for (size_t i = 0; i < array_size(arr); i++) {\n");
    printf("      int *val = (int *)array_get(arr, i);\n");
    printf("      printf(\"%%d \", *val);\n");
    printf("  }\n");
    printf("  Output: ");
    for (size_t i = 0; i < array_size(arr); i++) {
        int *val = ARRAY_GET(arr, int, i);
        if (val) printf("%d ", *val);
    }
    printf("\n\n");

    /* Iterator-based traversal */
    printf("  Iterator-based loop:\n");
    printf("  ─────────────────────────────────────────────────\n");
    printf("  ArrayIterator it = array_iter_begin(arr);\n");
    printf("  while (array_iter_valid(&it)) {\n");
    printf("      int *val = (int *)array_iter_get(&it);\n");
    printf("      printf(\"%%d \", *val);\n");
    printf("      array_iter_next(&it);\n");
    printf("  }\n");
    printf("  Output: ");
    {
        ArrayIterator it2 = array_iter_begin(arr);
        while (array_iter_valid(&it2)) {
            int *val = (int *)array_iter_get(&it2);
            if (val) printf("%d ", *val);
            array_iter_next(&it2);
        }
    }
    printf("\n\n");

    /* ARRAY_FOREACH macro */
    printf("  ARRAY_FOREACH macro:\n");
    printf("  ─────────────────────────────────────────────────\n");
    printf("  ARRAY_FOREACH(arr, int, val) {\n");
    printf("      printf(\"%%d \", *val);\n");
    printf("  }\n");
    printf("  Output: ");
    ARRAY_FOREACH(arr, int, val) {
        printf("%d ", *val);
    }
    printf("\n\n");

    printf("  ┌──────────────────────────────────────────────────────┐\n");
    printf("  │  All three produce the same output. The difference   │\n");
    printf("  │  is who manages the traversal state:                 │\n");
    printf("  │                                                      │\n");
    printf("  │  Index loop:      caller manages i, calls get(i)     │\n");
    printf("  │  Iterator:        it struct manages index + validity  │\n");
    printf("  │  ARRAY_FOREACH:   macro hides the iterator entirely  │\n");
    printf("  └──────────────────────────────────────────────────────┘\n\n");

    array_destroy(arr);
}


/* --- Demo 3: Filtered iteration with predicate --- */

static void demo_filtered_iteration(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Demo 3: Filtered Iteration (Predicate Callbacks)\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    Array *arr = ARRAY_CREATE(int, 16);
    if (!arr) return;

    int vals[] = {3, 14, 15, 92, 65, 35, 89, 79, 32, 38};
    for (int i = 0; i < 10; i++) {
        array_push(arr, &vals[i]);
    }

    printf("  Array: ");
    print_array_inline(arr, print_int);
    printf("\n\n");

    /* Filter: even numbers */
    printf("  Filter: even numbers (is_even predicate)\n");
    printf("  Found: ");
    {
        ArrayIterator it = array_iter_begin(arr);
        array_iter_find_next(&it, is_even, NULL);

        while (array_iter_valid(&it)) {
            int *val = (int *)array_iter_get(&it);
            if (val) printf("%d ", *val);
            array_iter_next(&it);
            array_iter_find_next(&it, is_even, NULL);
        }
    }
    printf("\n\n");

    /* Filter: greater than 50 */
    int threshold = 50;
    printf("  Filter: values > %d (is_greater_than predicate)\n", threshold);
    printf("  Found: ");
    {
        ArrayIterator it = array_iter_begin(arr);
        array_iter_find_next(&it, is_greater_than, &threshold);

        while (array_iter_valid(&it)) {
            int *val = (int *)array_iter_get(&it);
            if (val) printf("%d ", *val);
            array_iter_next(&it);
            array_iter_find_next(&it, is_greater_than, &threshold);
        }
    }
    printf("\n\n");

    printf("  The pattern: begin → find_next → (process, next, find_next)*\n");
    printf("  This composes Post 7's predicate callbacks with Post 10's\n");
    printf("  iterator to create a reusable filtered traversal.\n\n");

    array_destroy(arr);
}


/* --- Demo 4: Iterator invalidation — the central danger --- */

static void demo_invalidation(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Demo 4: Iterator Invalidation\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    /* Scenario 1: push during iteration */
    printf("  Scenario 1: array_push() during iteration\n");
    printf("  ──────────────────────────────────────────\n\n");

    Array *arr = ARRAY_CREATE(int, 4);  /* Small capacity to trigger realloc */
    if (!arr) return;

    for (int i = 1; i <= 4; i++) {
        int val = i * 10;
        array_push(arr, &val);
    }

    printf("  Array: ");
    print_array_inline(arr, print_int);
    printf("  (size=%zu, capacity=%zu, mod_count=%zu)\n\n",
           arr->size, arr->capacity, arr->mod_count);

    ArrayIterator it = array_iter_begin(arr);
    printf("  Iterator created: index=%zu, snapshot=%zu\n", it.index,
           it._mod_snapshot);
    printf("  array_iter_valid(&it) = %d  ✓\n\n", array_iter_valid(&it));

    /* Push a new element — this will trigger realloc AND increment mod_count */
    printf("  → array_push(arr, 50)  (capacity was %zu, triggers realloc)\n\n",
           arr->capacity);
    ARRAY_PUSH(arr, int, 50);

    printf("  Array after push: ");
    print_array_inline(arr, print_int);
    printf("  (size=%zu, capacity=%zu, mod_count=%zu)\n",
           arr->size, arr->capacity, arr->mod_count);
    printf("  Iterator state:   index=%zu, snapshot=%zu\n", it.index,
           it._mod_snapshot);
    printf("  array_iter_valid(&it) = %d  ✗ (invalidated!)\n",
           array_iter_valid(&it));
    printf("  array_iter_invalidated(&it) = %d\n",
           array_iter_invalidated(&it));
    printf("  array_iter_get(&it) = %s\n\n",
           array_iter_get(&it) ? "non-NULL (BUG!)" : "NULL (safe)");

    printf("  The iterator detected the modification and refuses to\n");
    printf("  return data. Without this check, the caller would silently\n");
    printf("  read stale or wrong data — or crash if realloc moved the\n");
    printf("  buffer and the iterator used a cached pointer.\n\n");

    array_destroy(arr);

    /* Scenario 2: insert shifts indices */
    printf("  Scenario 2: array_insert() shifts iterator's target\n");
    printf("  ──────────────────────────────────────────────────\n\n");

    arr = ARRAY_CREATE(int, 16);
    if (!arr) return;

    int vals[] = {10, 20, 30, 40, 50};
    for (int i = 0; i < 5; i++) {
        array_push(arr, &vals[i]);
    }

    printf("  Array: ");
    print_array_inline(arr, print_int);
    printf("\n");

    it = array_iter_at(arr, 3);
    int *before = (int *)array_iter_get(&it);
    printf("  Iterator at index 3 → value %d\n\n",
           before ? *before : -1);

    printf("  → array_insert(arr, 1, 15)  (inserts before iterator)\n\n");
    int fifteen = 15;
    array_insert(arr, 1, &fifteen);

    printf("  Array: ");
    print_array_inline(arr, print_int);
    printf("\n");
    printf("  Iterator at index 3 → ");
    printf("valid=%d (invalidated — mod_count changed)\n",
           array_iter_valid(&it));
    printf("\n");
    printf("  Even if we hadn't detected it, index 3 now holds %d,\n",
           *(int *)array_get(arr, 3));
    printf("  not 40. The insert shifted everything right. The iterator's\n");
    printf("  index is 'correct' in that it's a valid index, but it points\n");
    printf("  to a different element than when the iterator was created.\n\n");

    array_destroy(arr);
}


/* --- Demo 5: Safe removal during iteration (backward traversal) --- */

static void demo_safe_removal(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Demo 5: Safe Removal During Iteration\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    printf("  Problem: remove all even numbers from [3, 8, 14, 15, 92, 35]\n\n");

    /* The WRONG way: forward iteration with remove */
    printf("  WRONG: Forward iteration + remove (skips elements)\n");
    printf("  ────────────────────────────────────────────────────\n");
    {
        Array *arr = ARRAY_CREATE(int, 8);
        int vals[] = {3, 8, 14, 15, 92, 35};
        for (int i = 0; i < 6; i++) array_push(arr, &vals[i]);

        printf("  Before: ");
        print_array_inline(arr, print_int);
        printf("\n");

        /* This loop has a bug: when we remove index 1 (14), element 15
         * shifts from index 2 to index 1. But i increments to 2, so
         * we never examine 15. Then 92 (now at index 2) gets removed,
         * and 65 shifts down but i is already at 3... */
        size_t removed = 0;
        for (size_t i = 0; i < array_size(arr); i++) {
            int *val = (int *)array_get(arr, i);
            if (val && *val % 2 == 0) {
                array_remove(arr, i);
                removed++;
                /* BUG: i increments, skipping the element that shifted down */
            }
        }

        printf("  After:  ");
        print_array_inline(arr, print_int);
        printf("  (removed %zu, expected 3 — skipped adjacent even!)\n\n", removed);

        array_destroy(arr);
    }

    /* The RIGHT way: backward iteration */
    printf("  RIGHT: Backward iteration + remove\n");
    printf("  ──────────────────────────────────\n");
    {
        Array *arr = ARRAY_CREATE(int, 8);
        int vals[] = {3, 8, 14, 15, 92, 35};
        for (int i = 0; i < 6; i++) array_push(arr, &vals[i]);

        printf("  Before: ");
        print_array_inline(arr, print_int);
        printf("\n");

        /* Backward iteration: removing index i shifts elements AFTER i,
         * but we're moving toward index 0, so the shift never affects
         * the elements we haven't examined yet. */
        size_t removed = 0;
        for (size_t i = array_size(arr); i > 0; i--) {
            int *val = (int *)array_get(arr, i - 1);
            if (val && *val % 2 == 0) {
                array_remove(arr, i - 1);
                removed++;
            }
        }

        printf("  After:  ");
        print_array_inline(arr, print_int);
        printf("  (removed %zu ✓)\n\n", removed);

        array_destroy(arr);
    }

    /* The ALSO RIGHT way: forward with index correction */
    printf("  ALSO RIGHT: Forward iteration, don't increment on remove\n");
    printf("  ─────────────────────────────────────────────────────────\n");
    {
        Array *arr = ARRAY_CREATE(int, 8);
        int vals[] = {3, 8, 14, 15, 92, 35};
        for (int i = 0; i < 6; i++) array_push(arr, &vals[i]);

        printf("  Before: ");
        print_array_inline(arr, print_int);
        printf("\n");

        size_t removed = 0;
        size_t i = 0;
        while (i < array_size(arr)) {
            int *val = (int *)array_get(arr, i);
            if (val && *val % 2 == 0) {
                array_remove(arr, i);
                removed++;
                /* Don't increment — the next element shifted into position i */
            } else {
                i++;
            }
        }

        printf("  After:  ");
        print_array_inline(arr, print_int);
        printf("  (removed %zu ✓)\n\n", removed);

        array_destroy(arr);
    }

    /* Using iterator + resync */
    printf("  WITH ITERATOR: resync after each removal\n");
    printf("  ─────────────────────────────────────────\n");
    {
        Array *arr = ARRAY_CREATE(int, 8);
        int vals[] = {3, 8, 14, 15, 92, 35};
        for (int i = 0; i < 6; i++) array_push(arr, &vals[i]);

        printf("  Before: ");
        print_array_inline(arr, print_int);
        printf("\n");

        size_t removed = 0;
        ArrayIterator it = array_iter_begin(arr);

        while (it.index < arr->size) {
            int *val = (int *)element_at_unchecked(arr, it.index);
            if (*val % 2 == 0) {
                array_remove(arr, it.index);
                array_iter_resync(&it);   /* Re-arm after mutation */
                removed++;
                /* Don't advance — next element shifted into current index */
            } else {
                it.index++;
            }
        }

        printf("  After:  ");
        print_array_inline(arr, print_int);
        printf("  (removed %zu ✓)\n\n", removed);

        printf("  The resync() call re-arms the iterator after a deliberate\n");
        printf("  mutation. Use it ONLY when you understand why the mutation\n");
        printf("  is safe — e.g., you're removing the current element and\n");
        printf("  not advancing the index.\n\n");

        array_destroy(arr);
    }
}


/* --- Knowledge test --- */

static void knowledge_test(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Knowledge Test\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    printf("  Q: What happens to an iterator if you call array_push()\n");
    printf("     during iteration and it triggers realloc?\n\n");

    printf("  A:\n\n");

    printf("  Our index-based iterator:\n");
    printf("    - The iterator's index is still numerically valid\n");
    printf("      (index 3 is still index 3 after realloc).\n");
    printf("    - But mod_count was incremented by push(), so\n");
    printf("      the iterator's snapshot no longer matches.\n");
    printf("    - array_iter_valid() returns 0.\n");
    printf("    - array_iter_get() returns NULL.\n");
    printf("    - The iterator DETECTS the invalidation and refuses\n");
    printf("      to return potentially stale data.\n\n");

    printf("  A hypothetical pointer-based iterator (C++ style):\n");
    printf("    - The cached pointer points into the OLD buffer.\n");
    printf("    - realloc may have freed that buffer.\n");
    printf("    - Dereferencing the pointer is use-after-free:\n");
    printf("      undefined behavior.\n");
    printf("    - The iterator CANNOT detect this (no mod_count).\n");
    printf("    - Result: silent corruption, crash, or security bug.\n\n");

    printf("  This is the core tradeoff:\n\n");

    printf("  ┌─────────────────────────────────────────────────────┐\n");
    printf("  │  Approach         │ Survives   │ Detects    │ Cost  │\n");
    printf("  │                   │ realloc?   │ mutation?  │       │\n");
    printf("  ├─────────────────────────────────────────────────────┤\n");
    printf("  │  Index + mod_cnt  │ Yes        │ Yes        │ 1 cmp │\n");
    printf("  │  Raw pointer      │ No (UB!)   │ No         │ 0     │\n");
    printf("  │  Raw index loop   │ Yes        │ No         │ 0     │\n");
    printf("  └─────────────────────────────────────────────────────┘\n\n");

    printf("  The index+mod_count approach costs one comparison per\n");
    printf("  valid() call — negligible — and gives you both realloc\n");
    printf("  safety and mutation detection. The raw index loop is\n");
    printf("  simpler but can't tell you when the array changed\n");
    printf("  underneath you.\n\n");

    /* Verify error codes still work (including the new ARRAY_ERR_ITER) */
    printf("  Bonus — new error code:\n");
    printf("  ARRAY_ERR_ITER → \"%s\"\n", array_error_str(ARRAY_ERR_ITER));
    ArrayError err = array_push(NULL, NULL);
    printf("  array_push(NULL, NULL) → \"%s\" ✓\n", array_error_str(err));
}


/* ===========================================================================
 * main
 * =========================================================================== */

int main(void)
{
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Post 10: Iterators and Traversal Patterns in C\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    demo_basic_traversal();
    demo_index_vs_iterator();
    demo_filtered_iteration();
    demo_invalidation();
    demo_safe_removal();

    /* Generate Graphviz diagram for blog post image */
    generate_iterator_ownership_dot("output/post_10_iterator_ownership.dot");

    knowledge_test();

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("  End of Post 10. Next: memory layout, alignment, and\n");
    printf("  cache performance.\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    return 0;
}
