/* ============================================================================
 * Post 4: "Type Erasure: Generic Arrays with void* and memcpy"
 * ============================================================================
 *
 * This file is entirely self-contained. Compile with:
 *   gcc -Wall -Wextra -Wpedantic -std=c11 -o build/post_04 src_posts/post_04.c
 *
 * Run:
 *   ./build/post_04                       (ASCII visualization to stdout)
 *   ./build/post_04 > output/post_04.txt (save ASCII output)
 *
 * The program also writes output/post_04_layout.dot (Graphviz).
 * Render with:  dot -Tsvg output/post_04_layout.dot -o output/post_04_layout.svg
 *
 * Learning outcome: store any fixed-size type (int, float, struct) in the
 * same array implementation using void* + memcpy. Understand the pointer
 * arithmetic that makes it work: (char *)data + index * element_size.
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * 1. The struct: now type-agnostic
 * ---------------------------------------------------------------------------
 * The key change from Posts 1-3: `int *data` becomes `void *data`, and we
 * add `element_size` to remember how many bytes each element occupies.
 *
 * The array no longer knows what it stores — just how big each element is.
 * This is "type erasure": the type information is erased at the API boundary.
 * The caller knows what they stored; the array only knows byte counts.
 * --------------------------------------------------------------------------- */

typedef struct {
    void   *data;           /* Opaque heap buffer — could hold anything      */
    size_t  size;           /* Number of elements currently stored            */
    size_t  capacity;       /* Number of element slots allocated              */
    size_t  element_size;   /* Size in bytes of each element                  */
    size_t  realloc_count;  /* Diagnostic: how many times we've grown        */
} Array;

/* ---------------------------------------------------------------------------
 * 2. Lifecycle: create and destroy
 * ---------------------------------------------------------------------------
 * Identical pattern to Posts 1-3, but now the allocation size is
 * capacity * element_size instead of capacity * sizeof(int).
 * --------------------------------------------------------------------------- */

Array *array_create(size_t element_size, size_t initial_capacity)
{
    if (element_size == 0) {
        fprintf(stderr, "array_create: element_size must be > 0\n");
        return NULL;
    }
    if (initial_capacity == 0) {
        fprintf(stderr, "array_create: initial_capacity must be > 0\n");
        return NULL;
    }

    Array *arr = malloc(sizeof(Array));
    if (!arr) {
        fprintf(stderr, "array_create: failed to allocate struct\n");
        return NULL;
    }

    arr->data = malloc(element_size * initial_capacity);
    if (!arr->data) {
        fprintf(stderr, "array_create: failed to allocate buffer (%zu bytes)\n",
                element_size * initial_capacity);
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

/* ---------------------------------------------------------------------------
 * 3. The core pointer arithmetic
 * ---------------------------------------------------------------------------
 * This is the single most important function in the file. It computes the
 * address of element `index` inside the opaque buffer.
 *
 * Why cast to char*?
 *   void* arithmetic is undefined in standard C. You cannot write
 *   (void *)data + offset — the compiler doesn't know the unit size.
 *   char* has a defined unit of 1 byte, so (char *)data + n advances
 *   exactly n bytes. That's what we need.
 *
 * Formula: element_address = (char *)data + index * element_size
 *
 * For an array of int (element_size=4):
 *   element 0 → base + 0*4 = base + 0
 *   element 1 → base + 1*4 = base + 4
 *   element 2 → base + 2*4 = base + 8
 *
 * For an array of a 20-byte struct:
 *   element 0 → base + 0*20 = base + 0
 *   element 1 → base + 1*20 = base + 20
 *   element 2 → base + 2*20 = base + 40
 *
 * The array doesn't care whether element_size is 4 or 20 or 1024.
 * It's just byte arithmetic.
 * --------------------------------------------------------------------------- */

static void *element_at(const Array *arr, size_t index)
{
    return (char *)arr->data + index * arr->element_size;
}

/* ---------------------------------------------------------------------------
 * 4. Push: memcpy replaces direct assignment
 * ---------------------------------------------------------------------------
 * In Posts 1-3, push was simple:
 *   arr->data[arr->size] = value;
 *
 * That worked because `data` was `int*` and `value` was `int`. The compiler
 * knew the size and generated the right mov instruction.
 *
 * With void*, we can't assign — we don't have a type for the compiler to
 * work with. Instead, we copy element_size bytes from the caller's pointer
 * into the array's buffer using memcpy.
 *
 * The caller passes a POINTER to the value, not the value itself:
 *   int x = 42;
 *   array_push(arr, &x);    // &x is a pointer to 4 bytes of int data
 *
 * memcpy(destination, source, n_bytes) does a raw byte copy. It doesn't
 * know or care about types — it just copies bytes. That's exactly what
 * type erasure needs.
 * --------------------------------------------------------------------------- */

int array_push(Array *arr, const void *element)
{
    if (!arr || !element) {
        fprintf(stderr, "array_push: NULL argument\n");
        return -1;
    }

    /* Grow if needed — same 2x strategy from Post 2 */
    if (arr->size >= arr->capacity) {
        size_t new_cap = arr->capacity * 2;
        if (new_cap < arr->capacity) {
            /* Overflow check: if doubling overflows size_t, bail */
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

    /*
     * Copy element_size bytes from the caller's pointer into the slot.
     *
     *   destination: element_at(arr, arr->size)  — the next empty slot
     *   source:      element                     — caller's data
     *   count:       arr->element_size            — how many bytes
     */
    memcpy(element_at(arr, arr->size), element, arr->element_size);
    arr->size++;

    return 0;
}

/* ---------------------------------------------------------------------------
 * 5. Get: returns a void* that the caller must cast
 * ---------------------------------------------------------------------------
 * In Posts 1-3, array_get copied the value into an output parameter:
 *   int array_get(arr, index, &out)  →  out = arr->data[index]
 *
 * With void*, we can't do that — we don't know the type of `out`.
 * Instead, we return a pointer into the array's buffer. The caller
 * casts it to the type they know they stored:
 *
 *   int *val = (int *)array_get(arr, 2);
 *   printf("%d\n", *val);
 *
 * WARNING: this pointer is invalidated by any push that triggers realloc.
 * Same rule as Post 2: store indices, not pointers.
 * --------------------------------------------------------------------------- */

void *array_get(const Array *arr, size_t index)
{
    if (!arr || index >= arr->size) {
        return NULL;
    }
    return element_at(arr, index);
}

/* ---------------------------------------------------------------------------
 * 6. Set: overwrite an existing element
 * --------------------------------------------------------------------------- */

int array_set(Array *arr, size_t index, const void *element)
{
    if (!arr || !element || index >= arr->size) {
        return -1;
    }
    memcpy(element_at(arr, index), element, arr->element_size);
    return 0;
}

/* ---------------------------------------------------------------------------
 * 7. Utility functions
 * --------------------------------------------------------------------------- */

size_t array_size(const Array *arr)     { return arr ? arr->size     : 0; }
size_t array_capacity(const Array *arr) { return arr ? arr->capacity : 0; }

/* ---------------------------------------------------------------------------
 * 8. ASCII Visualization: byte-level memory layout
 * ---------------------------------------------------------------------------
 * This is the educational heart of the post. Unlike previous visualizations
 * that showed values in their cells, this one shows raw bytes — because
 * the array doesn't know what the values mean. It only knows bytes.
 *
 * For each element, we show:
 *   - The element's byte offset from the base pointer
 *   - The raw hex bytes
 *   - An optional interpreted value (if the caller provides a printer)
 *
 * The visualization drives home the key insight: the array is just a
 * flat sequence of bytes. element_size determines where one element
 * ends and the next begins. Without element_size, the bytes are
 * indistinguishable.
 * --------------------------------------------------------------------------- */

/* Callback type for printing a single element (type-specific) */
typedef void (*PrintElementFn)(const void *element, char *buf, size_t buf_size);

/* Default: print raw hex bytes */
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
                           PrintElementFn printer)
{
    if (!arr) {
        printf("(NULL array)\n");
        return;
    }

    size_t total_bytes_used  = arr->size * arr->element_size;
    size_t total_bytes_alloc = arr->capacity * arr->element_size;

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║  %s%-56s║\n", label ? label : "", label ? "" : "Array State");
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    printf("║  type: void* (erased)    element_size: %-4zu bytes          ║\n",
           arr->element_size);
    printf("║  size: %-5zu             capacity: %-5zu                   ║\n",
           arr->size, arr->capacity);
    printf("║  data: %-18p  reallocs: %-5zu                ║\n",
           arr->data, arr->realloc_count);
    printf("╠══════════════════════════════════════════════════════════════╣\n");

    /* Show byte-level layout for each element */
    printf("║  Byte-level memory layout:                                  ║\n");
    printf("║                                                              ║\n");

    /* Determine how many elements to display (cap at 8 for readability) */
    size_t display_count = arr->size < 8 ? arr->size : 8;
    int truncated = arr->size > 8;

    for (size_t i = 0; i < display_count; i++) {
        void *elem = element_at(arr, i);
        size_t offset = i * arr->element_size;

        /* Hex representation */
        char hex_buf[128];
        print_hex(elem, arr->element_size, hex_buf, sizeof(hex_buf));

        /* Interpreted value (if printer provided) */
        char val_buf[64] = "";
        if (printer) {
            printer(elem, val_buf, sizeof(val_buf));
        }

        if (printer) {
            printf("║  [%zu] offset +%-4zu │ %-28s │ %s\n",
                   i, offset, hex_buf, val_buf);
        } else {
            printf("║  [%zu] offset +%-4zu │ %-28s │\n",
                   i, offset, hex_buf);
        }
    }

    if (truncated) {
        printf("║  ... (%zu more elements not shown)                         \n",
               arr->size - display_count);
    }

    /* Show unused capacity */
    size_t unused = arr->capacity - arr->size;
    if (unused > 0 && unused <= 4) {
        for (size_t i = 0; i < unused; i++) {
            size_t offset = (arr->size + i) * arr->element_size;
            printf("║  [·] offset +%-4zu │ %-28s │ (unused)\n",
                   offset, "-- -- -- --");
        }
    } else if (unused > 4) {
        size_t offset_start = arr->size * arr->element_size;
        size_t offset_end   = (arr->capacity - 1) * arr->element_size;
        printf("║  [·] offset +%-4zu to +%-4zu                    "
               "│ (%zu unused slots)\n",
               offset_start, offset_end, unused);
    }

    printf("║                                                              ║\n");
    printf("╠══════════════════════════════════════════════════════════════╣\n");

    double util = arr->capacity > 0
        ? 100.0 * (double)arr->size / (double)arr->capacity : 0.0;
    printf("║  %zuB used / %zuB allocated = %.1f%% utilization",
           total_bytes_used, total_bytes_alloc, util);
    /* Pad to fill the box */
    int chars_printed = 0;
    chars_printed = (int)total_bytes_used;  /* rough estimate for padding */
    (void)chars_printed;
    printf("\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n");
}

/* ---------------------------------------------------------------------------
 * 9. Graphviz DOT generator
 * ---------------------------------------------------------------------------
 * Generates a diagram showing:
 *   - The Array struct (metadata) with its fields
 *   - The opaque data buffer as a sequence of byte ranges
 *   - Arrows showing the relationship between void* and the typed data
 *
 * This visualization helps the reader see that the array's view of memory
 * (uniform byte ranges) differs from the caller's view (typed elements).
 *
 * The bytes and typed nodes use HTML-like labels (<table>) instead of
 * record-style labels.  This gives Graphviz an exact bounding box for
 * every cell, so columns align perfectly regardless of content width.
 * It also avoids the need to escape record-separator characters (|, {, })
 * inside element values — only standard HTML entities matter (&amp; &lt;
 * &gt; &quot;).
 * --------------------------------------------------------------------------- */

/* Write `src` into `dst` with HTML-entity escaping (&, <, >, "). */
static void dot_html_escape(FILE *f, const char *src)
{
    for (; *src; src++) {
        switch (*src) {
        case '&':  fputs("&amp;",  f); break;
        case '<':  fputs("&lt;",   f); break;
        case '>':  fputs("&gt;",   f); break;
        case '"':  fputs("&quot;", f); break;
        default:   fputc(*src, f);     break;
        }
    }
}

void array_generate_dot(const Array *arr, const char *filename,
                        const char *type_label, PrintElementFn printer)
{
    FILE *f = fopen(filename, "w");
    if (!f) {
        fprintf(stderr, "array_generate_dot: cannot open %s\n", filename);
        return;
    }

    fprintf(f, "digraph GenericArray {\n");
    fprintf(f, "  rankdir=LR;\n");
    fprintf(f, "  bgcolor=\"#fafafa\";\n");
    fprintf(f, "  node [fontname=\"Courier\", fontsize=11];\n");
    fprintf(f, "  edge [fontname=\"Courier\", fontsize=9];\n");

    /* --- Metadata struct (record label is fine here — no special chars) --- */
    fprintf(f, "  metadata [shape=record, style=filled, fillcolor=\"#e8f4fd\",\n");
    fprintf(f, "    label=\"{Array (metadata)"
                  "|data: void* \\u2192 %p"
                  "|size: %zu"
                  "|capacity: %zu"
                  "|element_size: %zu"
                  "}\"];\n",
            arr->data, arr->size, arr->capacity, arr->element_size);

    size_t show = arr->size < 6 ? arr->size : 6;

    /* --- Array's view: opaque byte ranges (HTML table) --- */
    fprintf(f, "  subgraph cluster_array_view {\n");
    fprintf(f, "    label=\"Array's view (bytes)\";\n");
    fprintf(f, "    style=dashed; color=\"#999999\";\n");
    fprintf(f, "    fontname=\"Courier\"; fontsize=10;\n");
    fprintf(f, "    bytes [shape=none, margin=0, label=<\n");
    fprintf(f, "      <table border=\"0\" cellborder=\"1\" cellspacing=\"0\""
                  " cellpadding=\"4\" bgcolor=\"#fff3cd\">\n");

    for (size_t i = 0; i < show; i++) {
        size_t off = i * arr->element_size;
        fprintf(f, "        <tr><td>+%zu</td><td>%zu B</td></tr>\n",
                off, arr->element_size);
    }
    if (arr->size > show) {
        fprintf(f, "        <tr><td>...</td><td>%zu more</td></tr>\n",
                arr->size - show);
    }
    if (arr->capacity > arr->size) {
        size_t unused = arr->capacity - arr->size;
        fprintf(f, "        <tr><td>unused</td><td>%zu slot%s</td></tr>\n",
                unused, unused == 1 ? "" : "s");
    }

    fprintf(f, "      </table>\n");
    fprintf(f, "    >];\n");
    fprintf(f, "  }\n");

    /* --- Caller's view: typed elements (HTML table) --- */
    fprintf(f, "  subgraph cluster_caller_view {\n");
    fprintf(f, "    label=\"Caller's view (%s)\";\n",
            type_label ? type_label : "typed");
    fprintf(f, "    style=dashed; color=\"#28a745\";\n");
    fprintf(f, "    fontname=\"Courier\"; fontsize=10;\n");
    fprintf(f, "    typed [shape=none, margin=0, label=<\n");
    fprintf(f, "      <table border=\"0\" cellborder=\"1\" cellspacing=\"0\""
                  " cellpadding=\"4\" bgcolor=\"#d4edda\">\n");

    for (size_t i = 0; i < show; i++) {
        char val_buf[64] = "?";
        if (printer) {
            printer(element_at(arr, i), val_buf, sizeof(val_buf));
        }
        fprintf(f, "        <tr><td>[%zu]</td><td>", i);
        dot_html_escape(f, val_buf);
        fprintf(f, "</td></tr>\n");
    }
    if (arr->size > show) {
        fprintf(f, "        <tr><td>...</td><td>%zu more</td></tr>\n",
                arr->size - show);
    }

    fprintf(f, "      </table>\n");
    fprintf(f, "    >];\n");
    fprintf(f, "  }\n");

    /* --- Edges --- */
    fprintf(f, "  metadata -> bytes [label=\"  void*\\n(opaque)\", "
                  "style=bold, color=\"#0066cc\"];\n");
    fprintf(f, "  bytes -> typed [label=\"  cast to %s*\\n"
                  "(caller restores type)\", "
                  "style=dashed, color=\"#28a745\"];\n",
            type_label ? type_label : "T");

    /* --- Legend --- */
    fprintf(f, "  legend [shape=note, style=filled, fillcolor=\"#f8f9fa\",\n");
    fprintf(f, "    fontsize=9,\n");
    fprintf(f, "    label=\"Type Erasure:\\n"
                  "Array stores raw bytes.\\n"
                  "Caller casts back to original type.\\n"
                  "element_size = %zu bytes\"];\n",
            arr->element_size);

    fprintf(f, "}\n");
    fclose(f);
    printf("  → DOT written to %s\n", filename);
}


/* ===========================================================================
 * 10. Demonstrations
 * ===========================================================================
 * We demonstrate the same array implementation storing three different types:
 *   - int       (4 bytes per element)
 *   - double    (8 bytes per element)
 *   - struct    (20 bytes per element)
 *
 * The point: the Array code is identical in all three cases. Only
 * sizeof() and the cast at retrieval change.
 * =========================================================================== */

/* --- Printers for each type (used by visualize and DOT) --- */

static void print_int(const void *elem, char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "%d", *(const int *)elem);
}

static void print_double(const void *elem, char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "%.2f", *(const double *)elem);
}

/* A sample struct with mixed fields */
typedef struct {
    int    id;
    char   name[16];
} Record;

static void print_record(const void *elem, char *buf, size_t buf_size)
{
    const Record *r = (const Record *)elem;
    snprintf(buf, buf_size, "{id=%d, \"%s\"}", r->id, r->name);
}

/* --- Demo 1: int array --- */

static void demo_int_array(void)
{
    printf("\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  Demo 1: Array<int>   (element_size = %zu bytes)\n",
           sizeof(int));
    printf("═══════════════════════════════════════════════════════════════\n");

    Array *arr = array_create(sizeof(int), 4);
    if (!arr) return;

    int values[] = {10, 20, 30, 40, 50};
    for (int i = 0; i < 5; i++) {
        printf("  push(%d)\n", values[i]);
        array_push(arr, &values[i]);
    }

    array_visualize_ascii(arr, "Array<int> after 5 pushes", print_int);

    /* Demonstrate retrieval with cast */
    printf("\n  Retrieval (caller casts void* → int*):\n");
    for (size_t i = 0; i < array_size(arr); i++) {
        int *val = (int *)array_get(arr, i);
        printf("    arr[%zu] = %d\n", i, *val);
    }

    array_generate_dot(arr, "output/post_04_int_layout.dot", "int", print_int);
    array_destroy(arr);
}

/* --- Demo 2: double array --- */

static void demo_double_array(void)
{
    printf("\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  Demo 2: Array<double>   (element_size = %zu bytes)\n",
           sizeof(double));
    printf("═══════════════════════════════════════════════════════════════\n");

    Array *arr = array_create(sizeof(double), 4);
    if (!arr) return;

    double values[] = {3.14, 2.718, 1.414, 1.732};
    for (int i = 0; i < 4; i++) {
        printf("  push(%.3f)\n", values[i]);
        array_push(arr, &values[i]);
    }

    array_visualize_ascii(arr, "Array<double> after 4 pushes", print_double);

    printf("\n  Retrieval (caller casts void* → double*):\n");
    for (size_t i = 0; i < array_size(arr); i++) {
        double *val = (double *)array_get(arr, i);
        printf("    arr[%zu] = %.3f\n", i, *val);
    }

    array_destroy(arr);
}

/* --- Demo 3: struct array --- */

static void demo_struct_array(void)
{
    printf("\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  Demo 3: Array<Record>   (element_size = %zu bytes)\n",
           sizeof(Record));
    printf("═══════════════════════════════════════════════════════════════\n");

    Array *arr = array_create(sizeof(Record), 4);
    if (!arr) return;

    Record records[] = {
        {1, "Alice"},
        {2, "Bob"},
        {3, "Charlie"},
    };

    for (int i = 0; i < 3; i++) {
        printf("  push({id=%d, \"%s\"})\n", records[i].id, records[i].name);
        array_push(arr, &records[i]);
    }

    array_visualize_ascii(arr, "Array<Record> after 3 pushes", print_record);

    printf("\n  Retrieval (caller casts void* → Record*):\n");
    for (size_t i = 0; i < array_size(arr); i++) {
        Record *r = (Record *)array_get(arr, i);
        printf("    arr[%zu] = {id=%d, name=\"%s\"}\n", i, r->id, r->name);
    }

    /* Side-by-side byte comparison */
    printf("\n  Byte comparison — same array code, different element_size:\n");
    printf("  ┌────────────┬───────────────┬───────────────────────────┐\n");
    printf("  │ Type       │ element_size  │ Address of element [1]    │\n");
    printf("  ├────────────┼───────────────┼───────────────────────────┤\n");
    printf("  │ int        │ %zu bytes      │ base + 1 × %-2zu = base + %-3zu│\n",
           sizeof(int), sizeof(int), sizeof(int));
    printf("  │ double     │ %zu bytes      │ base + 1 × %-2zu = base + %-3zu│\n",
           sizeof(double), sizeof(double), sizeof(double));
    printf("  │ Record     │ %-2zu bytes     │ base + 1 × %-2zu = base + %-3zu│\n",
           sizeof(Record), sizeof(Record), sizeof(Record));
    printf("  └────────────┴───────────────┴───────────────────────────┘\n");

    array_generate_dot(arr, "output/post_04_layout.dot", "Record", print_record);
    array_destroy(arr);
}

/* --- Demo 4: the pointer arithmetic explained step by step --- */

static void demo_pointer_arithmetic(void)
{
    printf("\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  Demo 4: Pointer arithmetic walkthrough\n");
    printf("═══════════════════════════════════════════════════════════════\n");

    Array *arr = array_create(sizeof(Record), 4);
    if (!arr) return;

    Record r1 = {1, "Alice"};
    Record r2 = {2, "Bob"};
    Record r3 = {3, "Charlie"};
    array_push(arr, &r1);
    array_push(arr, &r2);
    array_push(arr, &r3);

    printf("\n  element_size = %zu bytes (sizeof(Record))\n", arr->element_size);
    printf("  base address = %p\n\n", arr->data);

    for (size_t i = 0; i < arr->size; i++) {
        char *base = (char *)arr->data;
        char *elem = base + i * arr->element_size;
        Record *r  = (Record *)elem;

        printf("  element[%zu]:\n", i);
        printf("    formula:  (char*)%p + %zu × %zu\n",
               arr->data, i, arr->element_size);
        printf("    =         (char*)%p + %zu\n",
               arr->data, i * arr->element_size);
        printf("    =         %p\n", (void *)elem);
        printf("    value:    {id=%d, name=\"%s\"}\n\n", r->id, r->name);
    }

    array_destroy(arr);
}

/* --- Demo 5: what happens if element_size is wrong --- */

static void demo_wrong_element_size(void)
{
    printf("\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  Demo 5: Type mismatch — what goes wrong\n");
    printf("═══════════════════════════════════════════════════════════════\n");

    /*
     * Create an array with element_size = sizeof(int) = 4,
     * but store a double (8 bytes) into it.
     *
     * This is the price of type erasure: the compiler can't catch this.
     * memcpy will copy only 4 of the 8 bytes, silently corrupting data.
     */
    Array *arr = array_create(sizeof(int), 4);
    if (!arr) return;

    double pi = 3.14159;
    printf("\n  Created array with element_size = %zu (for int)\n", sizeof(int));
    printf("  Storing a double (%.5f, %zu bytes) — THIS IS A BUG!\n",
           pi, sizeof(double));
    printf("  memcpy will copy only %zu of %zu bytes.\n\n",
           sizeof(int), sizeof(double));

    /* This "works" but is wrong — only 4 bytes are copied */
    array_push(arr, &pi);

    /* Read it back as int (the array thinks it's an int) */
    int *as_int = (int *)array_get(arr, 0);
    printf("  Read back as int:    %d  (garbage — half the bytes of pi)\n",
           *as_int);

    /* Read it back as double (wrong — only 4 bytes were stored) */
    double readback = 0.0;
    memcpy(&readback, array_get(arr, 0), sizeof(int)); /* only 4 bytes */
    printf("  Read back as double: %.5f  (truncated — missing 4 bytes)\n",
           readback);

    printf("\n  Lesson: type erasure trades compile-time safety for flexibility.\n");
    printf("  If you pass the wrong sizeof(), the compiler won't help you.\n");
    printf("  Post 5 introduces macros that restore type safety.\n");

    array_destroy(arr);
}

/* --- Knowledge test --- */

static void knowledge_test(void)
{
    printf("\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  Knowledge Test\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("\n");
    printf("  Q: How do you access element i in a void* array with\n");
    printf("     element_size = 20? Write the pointer arithmetic.\n");
    printf("\n");
    printf("  A: void *elem = (char *)arr->data + i * 20;\n");
    printf("\n");
    printf("     Cast data to char* (1-byte unit), then advance\n");
    printf("     i * 20 bytes. The result is a void* pointing to\n");
    printf("     the start of element i. Cast to the actual type\n");
    printf("     to read the value:\n");
    printf("\n");
    printf("       MyStruct *s = (MyStruct *)elem;\n");
    printf("       printf(\"%%d\\n\", s->field);\n");
    printf("\n");
    printf("     Without element_size, you can't compute this offset.\n");
    printf("     That single field is what makes generic arrays possible.\n");
}

/* ===========================================================================
 * main
 * =========================================================================== */

int main(void)
{
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  Post 4: Type Erasure — Generic Arrays with void* and memcpy\n");
    printf("═══════════════════════════════════════════════════════════════\n");

    demo_int_array();
    demo_double_array();
    demo_struct_array();
    demo_pointer_arithmetic();
    demo_wrong_element_size();
    knowledge_test();

    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("  End of Post 4. Next: type-safe wrapper macros.\n");
    printf("═══════════════════════════════════════════════════════════════\n\n");

    return 0;
}
