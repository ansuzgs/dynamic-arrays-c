/* ============================================================================
 * Post 5: "Type-Safe Wrappers: Macros That Protect Your void*"
 * ============================================================================
 *
 * This file is entirely self-contained. Compile with:
 *   gcc -Wall -Wextra -std=c11 -o build/post_05 src_posts/post_05.c
 *
 * Run:
 *   ./build/post_05                       (ASCII visualization to stdout)
 *   ./build/post_05 > output/post_05.txt (save ASCII output)
 *
 * The program also writes output/post_05_api_layers.dot (Graphviz).
 * Render with:  dot -Tsvg output/post_05_api_layers.dot \
 *                        -o output/post_05_api_layers.svg
 *
 * Learning outcome: write ARRAY_PUSH(arr, int, value) macros that provide
 * compile-time type checking while using void* internally. Understand
 * macro expansion, do { } while(0) idiom, compound literals, and the
 * tradeoff between macro complexity and type safety.
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ===========================================================================
 * 1. The void* generic array (carried forward from Post 4)
 * ===========================================================================
 * This section is identical to Post 4's core. The struct, lifecycle,
 * pointer arithmetic, push, and get — all type-erased, all void*.
 *
 * The ENTIRE point of this post is what we build ON TOP of this layer.
 * =========================================================================== */

typedef struct {
    void   *data;           /* Opaque heap buffer — could hold anything      */
    size_t  size;           /* Number of elements currently stored            */
    size_t  capacity;       /* Number of element slots allocated              */
    size_t  element_size;   /* Size in bytes of each element                  */
    size_t  realloc_count;  /* Diagnostic: how many times we've grown        */
} Array;

/* --- Lifecycle --- */

Array *array_create(size_t element_size, size_t initial_capacity)
{
    if (element_size == 0 || initial_capacity == 0) {
        fprintf(stderr, "array_create: element_size and capacity must be > 0\n");
        return NULL;
    }

    Array *arr = malloc(sizeof(Array));
    if (!arr) {
        fprintf(stderr, "array_create: malloc failed (struct)\n");
        return NULL;
    }

    arr->data = malloc(element_size * initial_capacity);
    if (!arr->data) {
        fprintf(stderr, "array_create: malloc failed (buffer)\n");
        free(arr);
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
    arr->data = NULL;
    free(arr);
}

/* --- Core pointer arithmetic --- */

static void *element_at(const Array *arr, size_t index)
{
    return (char *)arr->data + index * arr->element_size;
}

/* --- Push (void* interface) --- */

int array_push(Array *arr, const void *element)
{
    if (!arr || !element) {
        fprintf(stderr, "array_push: NULL argument\n");
        return -1;
    }

    if (arr->size >= arr->capacity) {
        size_t new_cap = arr->capacity * 2;
        if (new_cap < arr->capacity) {
            fprintf(stderr, "array_push: capacity overflow\n");
            return -1;
        }
        void *tmp = realloc(arr->data, new_cap * arr->element_size);
        if (!tmp) {
            fprintf(stderr, "array_push: realloc failed\n");
            return -1;
        }
        arr->data     = tmp;
        arr->capacity = new_cap;
        arr->realloc_count++;
    }

    memcpy(element_at(arr, arr->size), element, arr->element_size);
    arr->size++;
    return 0;
}

/* --- Get (void* interface) --- */

void *array_get(const Array *arr, size_t index)
{
    if (!arr || index >= arr->size) return NULL;
    return element_at(arr, index);
}

/* --- Set (void* interface) --- */

int array_set(Array *arr, size_t index, const void *element)
{
    if (!arr || !element || index >= arr->size) return -1;
    memcpy(element_at(arr, index), element, arr->element_size);
    return 0;
}

/* --- Utilities --- */

size_t array_size(const Array *arr)     { return arr ? arr->size     : 0; }
size_t array_capacity(const Array *arr) { return arr ? arr->capacity : 0; }


/* ===========================================================================
 * 2. TYPE-SAFE WRAPPER MACROS — The heart of this post
 * ===========================================================================
 *
 * Problem recap (from Post 4):
 *   int x = 42;
 *   array_push(arr, &x);                  // verbose, easy to forget &
 *   int *val = (int *)array_get(arr, 0);  // unsafe cast, no checking
 *
 * Goal: macros that let the caller write:
 *   ARRAY_PUSH(arr, int, 42);             // one expression, type-checked
 *   int *val = ARRAY_GET(arr, int, 0);    // typed pointer, size-verified
 *
 * The key trick: the macro creates a temporary variable of the correct
 * type, assigns the value to it, and passes &tmp to the void* function.
 * This means the compiler type-checks the assignment (value → tmp),
 * and we can verify sizeof(type) == arr->element_size at runtime.
 *
 * ---- Macro patterns used ----
 *
 * Pattern 1: do { ... } while(0)
 *   Wraps multi-statement macros so they behave like a single statement.
 *   Without it, `if (cond) MACRO(x);` would break because the macro
 *   expands to multiple statements and only the first is guarded by `if`.
 *
 * Pattern 2: Compound literal (C99)
 *   (type){value} creates a temporary of `type` initialized to `value`.
 *   It has the lifetime of the enclosing block — long enough for memcpy.
 *
 * Pattern 3: sizeof() for size verification
 *   sizeof(type) is a compile-time constant. Comparing it to
 *   arr->element_size at runtime catches type mismatches that the
 *   compiler can't see through void*.
 *
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * ARRAY_CREATE(type, capacity)
 * ---------------------------------------------------------------------------
 * Type-safe wrapper around array_create(). The caller specifies the type
 * directly instead of computing sizeof() manually.
 *
 * Usage:
 *   Array *arr = ARRAY_CREATE(int, 8);
 *   Array *arr = ARRAY_CREATE(double, 16);
 *   Array *arr = ARRAY_CREATE(Record, 4);
 *
 * Expansion:
 *   ARRAY_CREATE(int, 8)
 *   → array_create(sizeof(int), 8)
 *   → array_create(4, 8)
 * --------------------------------------------------------------------------- */

#define ARRAY_CREATE(type, capacity) \
    array_create(sizeof(type), (capacity))

/* ---------------------------------------------------------------------------
 * ARRAY_PUSH(arr, type, value)
 * ---------------------------------------------------------------------------
 * Creates a temporary of `type`, assigns `value` to it, and pushes &tmp.
 *
 * The do { } while(0) wrapper makes the macro safe in all control flow:
 *   if (condition) ARRAY_PUSH(arr, int, 42);  // works correctly
 *
 * The sizeof check catches type mismatches at runtime:
 *   Array *arr = ARRAY_CREATE(int, 4);
 *   ARRAY_PUSH(arr, double, 3.14);  // prints error: sizeof mismatch
 *
 * Usage:
 *   ARRAY_PUSH(arr, int, 42);
 *   ARRAY_PUSH(arr, double, 3.14);
 *   ARRAY_PUSH(arr, Record, ((Record){1, "Alice"}));
 *
 * Expansion (for int):
 *   ARRAY_PUSH(arr, int, 42)
 *   → do {
 *       if (sizeof(int) != (arr)->element_size) {
 *           fprintf(stderr, "ARRAY_PUSH: type size mismatch...\n");
 *       } else {
 *           int _push_tmp = (42);
 *           array_push((arr), &_push_tmp);
 *       }
 *     } while(0)
 * --------------------------------------------------------------------------- */

#define ARRAY_PUSH(arr, type, value)                                        \
    do {                                                                    \
        if (sizeof(type) != (arr)->element_size) {                          \
            fprintf(stderr,                                                 \
                "ARRAY_PUSH: type size mismatch (sizeof(%s)=%zu, "          \
                "element_size=%zu) at %s:%d\n",                             \
                #type, sizeof(type), (arr)->element_size,                   \
                __FILE__, __LINE__);                                        \
        } else {                                                            \
            type _push_tmp = (value);                                       \
            array_push((arr), &_push_tmp);                                  \
        }                                                                   \
    } while (0)

/* ---------------------------------------------------------------------------
 * ARRAY_GET(arr, type, index)
 * ---------------------------------------------------------------------------
 * Returns a typed pointer (type*) instead of void*. Includes a runtime
 * size check. If the types don't match, returns NULL instead of a
 * dangerously misinterpreted pointer.
 *
 * Usage:
 *   int *val = ARRAY_GET(arr, int, 0);
 *   if (val) printf("%d\n", *val);
 *
 * Expansion:
 *   ARRAY_GET(arr, int, 0)
 *   → ( sizeof(int) != (arr)->element_size
 *       ? (fprintf(stderr, "..."), (int *)NULL)
 *       : (int *)array_get((arr), (0)) )
 *
 * Note the comma operator: (fprintf(...), (int*)NULL) evaluates fprintf
 * for the side effect (prints error), then returns NULL. This lets us
 * print a diagnostic AND return a value in a single expression.
 * --------------------------------------------------------------------------- */

#define ARRAY_GET(arr, type, index)                                         \
    ( sizeof(type) != (arr)->element_size                                   \
      ? ( fprintf(stderr,                                                   \
              "ARRAY_GET: type size mismatch (sizeof(%s)=%zu, "             \
              "element_size=%zu) at %s:%d\n",                               \
              #type, sizeof(type), (arr)->element_size,                     \
              __FILE__, __LINE__),                                          \
          (type *)NULL )                                                    \
      : (type *)array_get((arr), (index)) )

/* ---------------------------------------------------------------------------
 * ARRAY_SET(arr, type, index, value)
 * ---------------------------------------------------------------------------
 * Type-safe wrapper around array_set(). Same pattern as ARRAY_PUSH:
 * creates a typed temporary, checks sizeof, passes &tmp.
 *
 * Usage:
 *   ARRAY_SET(arr, int, 0, 99);
 * --------------------------------------------------------------------------- */

#define ARRAY_SET(arr, type, index, value)                                  \
    do {                                                                    \
        if (sizeof(type) != (arr)->element_size) {                          \
            fprintf(stderr,                                                 \
                "ARRAY_SET: type size mismatch (sizeof(%s)=%zu, "           \
                "element_size=%zu) at %s:%d\n",                             \
                #type, sizeof(type), (arr)->element_size,                   \
                __FILE__, __LINE__);                                        \
        } else {                                                            \
            type _set_tmp = (value);                                        \
            array_set((arr), (index), &_set_tmp);                           \
        }                                                                   \
    } while (0)

/* ---------------------------------------------------------------------------
 * ARRAY_FOREACH(arr, type, var_name)
 * ---------------------------------------------------------------------------
 * Iterates over the array with a typed pointer. Expands into a for-loop
 * header — the caller provides the loop body in braces.
 *
 * Usage:
 *   ARRAY_FOREACH(arr, int, val) {
 *       printf("%d\n", *val);
 *   }
 *
 * Expansion:
 *   for (size_t _i = 0, _ok = (sizeof(int)==arr->element_size);
 *        _i < (arr)->size && _ok;
 *        _i++)
 *       for (int *val = (int *)element_at((arr), _i);
 *            val;
 *            val = NULL)
 *
 * The inner for-loop is a trick: it executes the body exactly once per
 * iteration (val is non-NULL on entry, set to NULL after one pass).
 * This lets us declare `val` inside the loop header without requiring C99
 * block scope tricks.
 * --------------------------------------------------------------------------- */

#define ARRAY_FOREACH(arr, type, var_name)                                  \
    for (size_t _fe_i = 0,                                                  \
                _fe_ok = (sizeof(type) == (arr)->element_size);              \
         _fe_i < (arr)->size && _fe_ok;                                     \
         _fe_i++)                                                           \
        for (type *var_name = (type *)element_at((arr), _fe_i);             \
             var_name;                                                      \
             var_name = NULL)


/* ===========================================================================
 * 3. ASCII Visualization
 * ===========================================================================
 * Shows the macro expansion alongside the memory state. This visualization
 * adds a "macro layer" view above the raw byte layout from Post 4.
 * =========================================================================== */

typedef void (*PrintElementFn)(const void *element, char *buf, size_t buf_size);

static void print_hex(const void *element, size_t elem_size,
                      char *buf, size_t buf_size)
{
    const unsigned char *bytes = (const unsigned char *)element;
    size_t pos = 0;
    for (size_t b = 0; b < elem_size && pos + 3 < buf_size; b++) {
        int written = snprintf(buf + pos, buf_size - pos, "%02x ", bytes[b]);
        if (written > 0) pos += (size_t)written;
    }
    if (pos > 0 && buf[pos - 1] == ' ') buf[pos - 1] = '\0';
}

void array_visualize_ascii(const Array *arr, const char *label,
                           const char *type_name, PrintElementFn printer)
{
    if (!arr) {
        printf("(NULL array)\n");
        return;
    }

    size_t total_used  = arr->size * arr->element_size;
    size_t total_alloc = arr->capacity * arr->element_size;

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║  %-64s║\n", label ? label : "Array State");
    printf("╠══════════════════════════════════════════════════════════════════╣\n");

    /* --- API layer info --- */
    if (type_name) {
        printf("║  Macro API:  ARRAY_CREATE(%s, %zu)                ",
               type_name, arr->capacity);
        printf("          ║\n");
    }
    printf("║  Internal:   void* (erased)   element_size: %-4zu bytes       ",
           arr->element_size);
    printf("  ║\n");
    printf("║  size: %-5zu   capacity: %-5zu   reallocs: %-5zu              ║\n",
           arr->size, arr->capacity, arr->realloc_count);
    printf("╠══════════════════════════════════════════════════════════════════╣\n");

    /* --- Dual-view: macro call ↔ internal operation --- */
    printf("║  API mapping (macro → internal):                                ║\n");
    if (type_name) {
        printf("║    ARRAY_PUSH(arr, %s, val)  → { %s tmp=val; "
               "array_push(arr,&tmp); }\n",
               type_name, type_name);
        printf("║    ARRAY_GET(arr, %s, i)     → (%s*)array_get(arr, i)"
               "               \n",
               type_name, type_name);
    }
    printf("╠══════════════════════════════════════════════════════════════════╣\n");

    /* --- Byte-level layout --- */
    printf("║  Memory layout:                                                 ║\n");

    size_t display_count = arr->size < 8 ? arr->size : 8;
    int truncated = arr->size > 8;

    for (size_t i = 0; i < display_count; i++) {
        void *elem = element_at(arr, i);
        size_t offset = i * arr->element_size;

        char hex_buf[128];
        print_hex(elem, arr->element_size, hex_buf, sizeof(hex_buf));

        char val_buf[64] = "";
        if (printer) {
            printer(elem, val_buf, sizeof(val_buf));
        }

        if (printer) {
            printf("║  [%zu] +%-4zu │ %-24s │ %s\n",
                   i, offset, hex_buf, val_buf);
        } else {
            printf("║  [%zu] +%-4zu │ %-24s │\n",
                   i, offset, hex_buf);
        }
    }

    if (truncated) {
        printf("║  ... (%zu more elements)\n", arr->size - display_count);
    }

    /* Show unused slots */
    size_t unused = arr->capacity - arr->size;
    if (unused > 0 && unused <= 4) {
        for (size_t i = 0; i < unused; i++) {
            size_t offset = (arr->size + i) * arr->element_size;
            printf("║  [·] +%-4zu │ %-24s │ (unused)\n",
                   offset, "-- -- -- --");
        }
    } else if (unused > 4) {
        printf("║  [·] +%-4zu to +%-4zu  (%zu unused slots)\n",
               arr->size * arr->element_size,
               (arr->capacity - 1) * arr->element_size, unused);
    }

    printf("╠══════════════════════════════════════════════════════════════════╣\n");
    double util = arr->capacity > 0
        ? 100.0 * (double)arr->size / (double)arr->capacity : 0.0;
    printf("║  %zuB used / %zuB allocated = %.1f%% utilization               ║\n",
           total_used, total_alloc, util);
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
}


/* ===========================================================================
 * 4. Graphviz DOT generator: User-facing API vs Internal API layers
 * ===========================================================================
 * This diagram shows the two-layer architecture:
 *   - Top layer: type-safe macros (what the user calls)
 *   - Bottom layer: void* functions (what actually runs)
 *   - Arrows show how macros delegate to the internal API
 *
 * This is NOT the memory layout diagram from Post 4. This is an
 * architectural diagram showing the abstraction layers.
 * =========================================================================== */

void generate_api_layers_dot(const char *filename)
{
    FILE *f = fopen(filename, "w");
    if (!f) {
        fprintf(stderr, "generate_api_layers_dot: cannot open %s\n", filename);
        return;
    }

    fprintf(f, "digraph TypeSafeWrappers {\n");
    fprintf(f, "  rankdir=TB;\n");
    fprintf(f, "  bgcolor=\"#fafafa\";\n");
    fprintf(f, "  node [fontname=\"Courier\", fontsize=11];\n");
    fprintf(f, "  edge [fontname=\"Courier\", fontsize=9];\n");
    fprintf(f, "  compound=true;\n\n");

    /* --- User-facing macro API layer --- */
    fprintf(f, "  subgraph cluster_macro_api {\n");
    fprintf(f, "    label=\"User-Facing API (Type-Safe Macros)\";\n");
    fprintf(f, "    style=filled; fillcolor=\"#d4edda\"; color=\"#28a745\";\n");
    fprintf(f, "    fontname=\"Courier\"; fontsize=12;\n\n");
    fprintf(f, "    m_create [label=\"ARRAY_CREATE(type, cap)\", "
                   "shape=box, style=filled, fillcolor=\"#c3e6cb\"];\n");
    fprintf(f, "    m_push   [label=\"ARRAY_PUSH(arr, type, val)\", "
                   "shape=box, style=filled, fillcolor=\"#c3e6cb\"];\n");
    fprintf(f, "    m_get    [label=\"ARRAY_GET(arr, type, idx)\", "
                   "shape=box, style=filled, fillcolor=\"#c3e6cb\"];\n");
    fprintf(f, "    m_set    [label=\"ARRAY_SET(arr, type, idx, val)\", "
                   "shape=box, style=filled, fillcolor=\"#c3e6cb\"];\n");
    fprintf(f, "    m_for    [label=\"ARRAY_FOREACH(arr, type, ptr)\", "
                   "shape=box, style=filled, fillcolor=\"#c3e6cb\"];\n");
    fprintf(f, "  }\n\n");

    /* --- Safety checks in the middle --- */
    fprintf(f, "  subgraph cluster_checks {\n");
    fprintf(f, "    label=\"Compile-Time + Runtime Checks\";\n");
    fprintf(f, "    style=filled; fillcolor=\"#fff3cd\"; color=\"#ffc107\";\n");
    fprintf(f, "    fontname=\"Courier\"; fontsize=12;\n\n");
    fprintf(f, "    chk_sizeof [label=\"sizeof(type) == "
                   "arr->element_size?\","
                   " shape=diamond, style=filled, fillcolor=\"#ffeeba\"];\n");
    fprintf(f, "    chk_tmp    [label=\"type tmp = value;\\n"
                   "(compiler type-checks)\","
                   " shape=box, style=filled, fillcolor=\"#ffeeba\"];\n");
    fprintf(f, "  }\n\n");

    /* --- Internal void* API layer --- */
    fprintf(f, "  subgraph cluster_internal {\n");
    fprintf(f, "    label=\"Internal API (void* + memcpy)\";\n");
    fprintf(f, "    style=filled; fillcolor=\"#e8f4fd\"; color=\"#0066cc\";\n");
    fprintf(f, "    fontname=\"Courier\"; fontsize=12;\n\n");
    fprintf(f, "    i_create [label=\"array_create(sizeof, cap)\", "
                   "shape=box, style=filled, fillcolor=\"#b8daff\"];\n");
    fprintf(f, "    i_push   [label=\"array_push(arr, &tmp)\", "
                   "shape=box, style=filled, fillcolor=\"#b8daff\"];\n");
    fprintf(f, "    i_get    [label=\"(type*)array_get(arr, idx)\", "
                   "shape=box, style=filled, fillcolor=\"#b8daff\"];\n");
    fprintf(f, "    i_set    [label=\"array_set(arr, idx, &tmp)\", "
                   "shape=box, style=filled, fillcolor=\"#b8daff\"];\n");
    fprintf(f, "    i_elem   [label=\"element_at(arr, i)\\n"
                   "(char*)data + i*elem_size\","
                   " shape=box, style=filled, fillcolor=\"#b8daff\"];\n");
    fprintf(f, "  }\n\n");

    /* --- Edges: macros → checks → internal --- */
    fprintf(f, "  m_create -> i_create [label=\"  sizeof(type)\"];\n");
    fprintf(f, "  m_push   -> chk_sizeof [label=\"  check size\"];\n");
    fprintf(f, "  m_push   -> chk_tmp [label=\"  create tmp\"];\n");
    fprintf(f, "  chk_tmp  -> i_push [label=\"  &tmp\"];\n");
    fprintf(f, "  m_get    -> chk_sizeof;\n");
    fprintf(f, "  chk_sizeof -> i_get [label=\"  cast result\"];\n");
    fprintf(f, "  m_set    -> chk_sizeof;\n");
    fprintf(f, "  chk_sizeof -> i_set;\n");
    fprintf(f, "  m_for    -> i_elem [label=\"  typed iteration\"];\n\n");

    /* --- Legend --- */
    fprintf(f, "  legend [shape=note, style=filled, fillcolor=\"#f8f9fa\",\n");
    fprintf(f, "    fontsize=9,\n");
    fprintf(f, "    label=\"Pattern: Type-Safe Wrappers\\n\\n"
                   "User writes: ARRAY_PUSH(arr, int, 42)\\n"
                   "Compiler sees: int tmp = 42;\\n"
                   "Runtime checks: sizeof(int)==element_size\\n"
                   "Executes: memcpy(slot, &tmp, 4)\"];\n");

    fprintf(f, "}\n");
    fclose(f);
    printf("  → DOT written to %s\n", filename);
}


/* ===========================================================================
 * 5. Printers for demonstrations
 * =========================================================================== */

static void print_int(const void *elem, char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "%d", *(const int *)elem);
}

typedef struct {
    int  id;
    char name[16];
} Record;

static void print_record(const void *elem, char *buf, size_t buf_size)
{
    const Record *r = (const Record *)elem;
    snprintf(buf, buf_size, "{id=%d, \"%s\"}", r->id, r->name);
}


/* ===========================================================================
 * 6. Demonstrations
 * =========================================================================== */

/* --- Demo 1: Basic usage — macros vs raw API --- */

static void demo_macro_vs_raw(void)
{
    printf("\n");
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("  Demo 1: Macro API vs Raw void* API (side by side)\n");
    printf("═══════════════════════════════════════════════════════════════════\n");

    /* ---- The old way (Post 4) ---- */
    printf("\n  --- The old way (Post 4): raw void* ---\n\n");
    printf("    Array *arr = array_create(sizeof(int), 4);\n");
    printf("    int x = 42;\n");
    printf("    array_push(arr, &x);                    // verbose\n");
    printf("    int *val = (int *)array_get(arr, 0);    // unsafe cast\n");

    Array *raw = array_create(sizeof(int), 4);
    int x = 42;
    array_push(raw, &x);
    int *raw_val = (int *)array_get(raw, 0);
    printf("    result: %d\n", *raw_val);
    array_destroy(raw);

    /* ---- The new way (Post 5) ---- */
    printf("\n  --- The new way (Post 5): type-safe macros ---\n\n");
    printf("    Array *arr = ARRAY_CREATE(int, 4);\n");
    printf("    ARRAY_PUSH(arr, int, 42);               // one expression\n");
    printf("    int *val = ARRAY_GET(arr, int, 0);       // typed pointer\n");

    Array *safe = ARRAY_CREATE(int, 4);
    ARRAY_PUSH(safe, int, 42);
    int *safe_val = ARRAY_GET(safe, int, 0);
    printf("    result: %d\n", safe_val ? *safe_val : -1);

    /* Push more values to show the array working */
    ARRAY_PUSH(safe, int, 99);
    ARRAY_PUSH(safe, int, 7);
    ARRAY_PUSH(safe, int, 256);
    ARRAY_PUSH(safe, int, -13);

    array_visualize_ascii(safe, "Array<int> via ARRAY_PUSH macro",
                          "int", print_int);
    array_destroy(safe);
}

/* --- Demo 2: Type mismatch detection --- */

static void demo_type_mismatch(void)
{
    printf("\n");
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("  Demo 2: Type mismatch detection\n");
    printf("═══════════════════════════════════════════════════════════════════\n");

    Array *arr = ARRAY_CREATE(int, 4);
    ARRAY_PUSH(arr, int, 10);
    ARRAY_PUSH(arr, int, 20);

    printf("\n  Created array with ARRAY_CREATE(int, 4)\n");
    printf("  Stored: 10, 20 via ARRAY_PUSH(arr, int, ...)\n\n");

    /* This triggers the sizeof mismatch check */
    printf("  Now attempting: ARRAY_PUSH(arr, double, 3.14)\n");
    printf("  Expected: error message (sizeof(double)=8 != element_size=4)\n\n");
    ARRAY_PUSH(arr, double, 3.14);

    printf("\n  Now attempting: ARRAY_GET(arr, double, 0)\n");
    printf("  Expected: error + NULL return\n\n");
    double *bad = ARRAY_GET(arr, double, 0);
    printf("  Result: %s\n", bad ? "GOT VALUE (bad!)" : "NULL (correct!)");

    printf("\n  Array is UNCHANGED — mismatch was caught before corruption:\n");
    array_visualize_ascii(arr, "Array<int> after rejected double push",
                          "int", print_int);
    array_destroy(arr);
}

/* --- Demo 3: Struct usage with macros --- */

static void demo_struct_macros(void)
{
    printf("\n");
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("  Demo 3: Structs with type-safe macros\n");
    printf("═══════════════════════════════════════════════════════════════════\n");

    Array *arr = ARRAY_CREATE(Record, 4);

    /* Compound literal syntax for pushing structs */
    ARRAY_PUSH(arr, Record, ((Record){1, "Alice"}));
    ARRAY_PUSH(arr, Record, ((Record){2, "Bob"}));
    ARRAY_PUSH(arr, Record, ((Record){3, "Charlie"}));

    printf("\n  Pushed 3 records via ARRAY_PUSH(arr, Record, ...)\n");

    array_visualize_ascii(arr, "Array<Record> via type-safe macros",
                          "Record", print_record);

    /* ARRAY_GET returns Record*, not void* */
    printf("\n  Retrieval via ARRAY_GET(arr, Record, i):\n");
    for (size_t i = 0; i < array_size(arr); i++) {
        Record *r = ARRAY_GET(arr, Record, i);
        if (r) {
            printf("    arr[%zu] = {id=%d, name=\"%s\"}\n", i, r->id, r->name);
        }
    }

    array_destroy(arr);
}

/* --- Demo 4: ARRAY_FOREACH --- */

static void demo_foreach(void)
{
    printf("\n");
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("  Demo 4: ARRAY_FOREACH iteration\n");
    printf("═══════════════════════════════════════════════════════════════════\n");

    Array *arr = ARRAY_CREATE(int, 8);
    for (int i = 1; i <= 6; i++) {
        ARRAY_PUSH(arr, int, i * 10);
    }

    printf("\n  Array contains: ");
    ARRAY_FOREACH(arr, int, val) {
        printf("%d ", *val);
    }
    printf("\n");

    /* Compute sum using ARRAY_FOREACH */
    int sum = 0;
    ARRAY_FOREACH(arr, int, val) {
        sum += *val;
    }
    printf("  Sum via ARRAY_FOREACH: %d\n", sum);

    /* Show the macro expansion conceptually */
    printf("\n  ARRAY_FOREACH(arr, int, val) { sum += *val; }\n");
    printf("  expands to:\n");
    printf("    for (size_t _i = 0; _i < arr->size; _i++)\n");
    printf("      for (int *val = (int*)element_at(arr, _i); val; val=NULL)\n");
    printf("        { sum += *val; }\n");

    array_destroy(arr);
}

/* --- Demo 5: Macro expansion visualization --- */

static void demo_macro_expansion(void)
{
    printf("\n");
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("  Demo 5: What the preprocessor actually generates\n");
    printf("═══════════════════════════════════════════════════════════════════\n");

    printf("\n  Source code:\n");
    printf("    ARRAY_PUSH(arr, int, 42);\n");

    printf("\n  After preprocessing (conceptual):\n");
    printf("    do {\n");
    printf("        if (sizeof(int) != (arr)->element_size) {\n");
    printf("            fprintf(stderr, \"ARRAY_PUSH: type size mismatch "
           "(sizeof(int)=4, element_size=%%zu)\\n\",\n");
    printf("                    (arr)->element_size);\n");
    printf("        } else {\n");
    printf("            int _push_tmp = (42);\n");
    printf("            array_push((arr), &_push_tmp);\n");
    printf("        }\n");
    printf("    } while (0);\n");

    printf("\n  Key observations:\n");
    printf("    1. do { } while(0) makes the macro a single statement\n");
    printf("    2. sizeof(int) is computed at COMPILE time (always 4)\n");
    printf("    3. arr->element_size is checked at RUNTIME\n");
    printf("    4. int _push_tmp = 42; lets the compiler type-check the "
           "assignment\n");
    printf("    5. &_push_tmp is passed to void* array_push — type erased\n");
    printf("    6. #type stringifies the type name for error messages\n");

    printf("\n  Source code:\n");
    printf("    int *val = ARRAY_GET(arr, int, 0);\n");

    printf("\n  After preprocessing (conceptual):\n");
    printf("    int *val = ( sizeof(int) != (arr)->element_size\n");
    printf("                 ? ( fprintf(stderr, \"...\"), (int *)NULL )\n");
    printf("                 : (int *)array_get((arr), (0)) );\n");

    printf("\n  Key observations:\n");
    printf("    1. ARRAY_GET is an EXPRESSION, not a statement (returns a "
           "value)\n");
    printf("    2. Ternary operator: check first, then either error or cast\n");
    printf("    3. Comma operator: print error AND return NULL\n");
    printf("    4. The cast (int*) provides typed access — no manual cast\n");
}

/* --- Demo 6: ARRAY_SET with type safety --- */

static void demo_set_macro(void)
{
    printf("\n");
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("  Demo 6: ARRAY_SET with type safety\n");
    printf("═══════════════════════════════════════════════════════════════════\n");

    Array *arr = ARRAY_CREATE(int, 4);
    ARRAY_PUSH(arr, int, 100);
    ARRAY_PUSH(arr, int, 200);
    ARRAY_PUSH(arr, int, 300);

    printf("\n  Before: ");
    ARRAY_FOREACH(arr, int, val) {
        printf("%d ", *val);
    }
    printf("\n");

    ARRAY_SET(arr, int, 1, 999);
    printf("  After ARRAY_SET(arr, int, 1, 999): ");
    ARRAY_FOREACH(arr, int, val) {
        printf("%d ", *val);
    }
    printf("\n");

    /* Attempt wrong type */
    printf("\n  Attempting ARRAY_SET(arr, double, 0, 1.5):\n");
    ARRAY_SET(arr, double, 0, 1.5);
    printf("  Array unchanged: ");
    ARRAY_FOREACH(arr, int, val) {
        printf("%d ", *val);
    }
    printf("\n");

    array_destroy(arr);
}

/* --- Knowledge test --- */

static void knowledge_test(void)
{
    printf("\n");
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("  Knowledge Test\n");
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("\n");
    printf("  Q: Write a macro ARRAY_GET(arr, type, index) that returns\n");
    printf("     a typed pointer. What happens if the user passes the\n");
    printf("     wrong type?\n");
    printf("\n");
    printf("  A: #define ARRAY_GET(arr, type, index)                  \\\n");
    printf("       ( sizeof(type) != (arr)->element_size              \\\n");
    printf("         ? ( fprintf(stderr, \"type mismatch\"), (type*)NULL ) \\\n");
    printf("         : (type *)array_get((arr), (index)) )\n");
    printf("\n");
    printf("     If the user passes the wrong type:\n");
    printf("     - sizeof(wrong_type) != arr->element_size\n");
    printf("     - The ternary takes the error branch\n");
    printf("     - An error is printed with the mismatched sizes\n");
    printf("     - NULL is returned (not a corrupted pointer)\n");
    printf("     - The array data is never accessed with the wrong type\n");
    printf("\n");
    printf("     What it CANNOT catch:\n");
    printf("     - Types with the same sizeof (int vs float on most\n");
    printf("       platforms, both 4 bytes). The size check passes,\n");
    printf("       but the bytes are interpreted differently.\n");
    printf("     - This is a fundamental limitation of size-only checking.\n");
    printf("       For same-size type safety, you'd need typeof() (GCC)\n");
    printf("       or _Generic (C11) — discussed in the tradeoffs section.\n");
}


/* ===========================================================================
 * main
 * =========================================================================== */

int main(void)
{
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("  Post 5: Type-Safe Wrappers — Macros That Protect Your void*\n");
    printf("═══════════════════════════════════════════════════════════════════\n");

    demo_macro_vs_raw();
    demo_type_mismatch();
    demo_struct_macros();
    demo_foreach();
    demo_macro_expansion();
    demo_set_macro();

    /* Generate the API layers diagram */
    generate_api_layers_dot("output/post_05_api_layers.dot");

    knowledge_test();

    printf("\n═══════════════════════════════════════════════════════════════════\n");
    printf("  End of Post 5. Next: error handling strategies.\n");
    printf("═══════════════════════════════════════════════════════════════════\n\n");

    return 0;
}
