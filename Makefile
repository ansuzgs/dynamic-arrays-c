# ============================================================================
# Dynamic Arrays in C — Blog Series
# ============================================================================
#
# Each post_XX.c is self-contained (struct + functions + main).
# This Makefile compiles, runs, captures output, and renders Graphviz.
#
# Usage:
#   make              Build all posts
#   make run          Build + run all (generates .txt and .dot)
#   make viz          Build + run + render .dot → .svg
#   make post_01      Build, run, and visualize a single post
#   make clean        Remove build artifacts
#   make help         Show this help
# ============================================================================

CC       = gcc
CFLAGS   = -Wall -Wextra -Wpedantic -std=c11 -g
DOT      = dot

# Auto-detect posts from src/ directory
SRCS     = $(sort $(wildcard src/post_*.c))
NUMS     = $(patsubst src/post_%.c,%,$(SRCS))
BINS     = $(patsubst src/post_%.c,build/post_%,$(SRCS))

.PHONY: all run viz clean help $(addprefix post_,$(NUMS))

# ── Default: build only ─────────────────────────────────────────────────────
all: $(BINS)

# ── Build a single post ─────────────────────────────────────────────────────
build/post_%: src/post_%.c | build
	$(CC) $(CFLAGS) -o $@ $<

# ── Run a post, capture stdout → .txt, let it write .dot files ──────────────
output/post_%.txt: build/post_% | output
	cd $(CURDIR) && ./$< > $@ 2>&1 || true

# ── Render every .dot in output/ → .svg ─────────────────────────────────────
# (dots are generated as a side effect of running the binary)
output/%.svg: output/%.dot
	$(DOT) -Tsvg $< -o $@

# ── Convenience: make post_01 → build + run + render ────────────────────────
define POST_RULE
post_$(1): build/post_$(1) output/post_$(1).txt
	@# Render any .dot files this post generated
	@for dot in output/post_$(1)*.dot; do \
		[ -f "$$$$dot" ] && $(DOT) -Tsvg "$$$$dot" -o "$$$${dot%.dot}.svg" 2>/dev/null; \
	done; true
	@echo "✓ post_$(1) done"
endef
$(foreach n,$(NUMS),$(eval $(call POST_RULE,$(n))))

# ── Batch targets ───────────────────────────────────────────────────────────
run: $(patsubst %,output/post_%.txt,$(NUMS))

viz: run
	@for dot in output/*.dot; do \
		[ -f "$$dot" ] && $(DOT) -Tsvg "$$dot" -o "$${dot%.dot}.svg" 2>/dev/null; \
	done; true
	@echo "✓ All visualizations rendered"

# ── Directories ─────────────────────────────────────────────────────────────
build output:
	mkdir -p $@

# ── Clean ───────────────────────────────────────────────────────────────────
clean:
	rm -rf build/

# ── Help ────────────────────────────────────────────────────────────────────
help:
	@echo "Dynamic Arrays in C — Blog Series"
	@echo ""
	@echo "  make              Build all posts"
	@echo "  make run          Build + run all (→ output/*.txt, output/*.dot)"
	@echo "  make viz          Build + run + render .dot → .svg"
	@echo "  make post_01      Build, run, and visualize post 01"
	@echo "  make clean        Remove build/ directory"
	@echo ""
	@echo "Posts found: $(NUMS)"
