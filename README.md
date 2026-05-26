# Dynamic Arrays in C: From malloc to Production

A blog series that builds a dynamic array library in C from scratch — one post at a time, every line compilable.

No hand-waving. No pseudocode. Every post produces a working `.c` file you can compile and run.

---

## What This Is

Most C tutorials either stop at `malloc`/`free` or dump a 2000-line library on you and say "read the source." This series bridges the gap: 15 posts that incrementally build a generic, production-quality dynamic array, explaining every design decision along the way.

Each post introduces exactly one concept, compiles to a standalone program, and includes ASCII/Graphviz visualizations of memory layout so you can *see* what the code does — not just read about it.

## Posts

| # | Title | Key Concept | Code |
|---|-------|-------------|------|
| 1 | [Hello, Array](posts/01_hello_array.md) | `malloc`, `free`, struct-based ownership | [post_01.c](src_posts/post_01.c) |
| 2 | Growing Pains | `realloc`, automatic capacity management | *coming soon* |
| 3 | The Growth Factor Debate | Amortized analysis, 1.5× vs 2× | *coming soon* |
| 4 | Type Erasure | `void*` generics, `memcpy`, byte-level layout | *coming soon* |
| 5 | Type-Safe Wrappers | Macros, `_Generic` (C11), compile-time checks | *coming soon* |
| 6 | Error Handling Strategies | OOM recovery, preserving array state | *coming soon* |
| 7 | Function Pointers and Callbacks | Comparators, destructors, `foreach` | *coming soon* |
| 8 | Bounds Checking and Defensive APIs | Debug vs release, safe accessors | *coming soon* |
| 9 | Insert, Remove, and the Cost of Shifting | `memmove`, swap-remove | *coming soon* |
| 10 | Iterators and Traversal | Iterator structs, invalidation rules | *coming soon* |
| 11 | Memory Layout and Cache Performance | Alignment, padding, SoA vs AoS | *coming soon* |
| 12 | Benchmarking Methodology | `clock_gettime`, statistical rigor | *coming soon* |
| 13 | Shrink-to-Fit and Memory Reclamation | Hysteresis, shrink policies | *coming soon* |
| 14 | Production API Design | Opaque types, header/impl split | *coming soon* |
| 15 | The Final Comparison | vs `std::vector`, vs glib `GArray` | *coming soon* |

## Who This Is For

You know C basics — pointers, structs, `printf`. You want to understand how real C libraries manage memory and expose generic APIs without templates, inheritance, or a garbage collector. Maybe you write embedded code, do CTFs, or you're a Rust/C++ developer curious about how C solves the same problems.

## Building

Every post is a self-contained C file. No external dependencies — just a C11 compiler.

```bash
# Compile and run any post
gcc -std=c11 -Wall -Wextra -o post_01 src_posts/post_01.c
./post_01

# Or use the Makefile
make post_01
make all
```

Requirements: `gcc` or `clang` with C11 support. Optional: `graphviz` for rendering `.dot` visualizations.

## What You'll Know by the End

After post 15, you'll be able to:

- Implement a generic dynamic array in C using `void*` + `memcpy`
- Choose and justify a growth factor with amortized analysis
- Write type-safe macro wrappers over type-erased internals
- Handle `realloc` failures without corrupting existing data
- Implement sort/search/foreach via function pointer callbacks
- Benchmark operations and interpret the results honestly
- Design a clean public API with opaque types and documented contracts
- Explain the tradeoffs vs `std::vector` and production C libraries

## License

Code: MIT. Prose: CC BY 4.0.
