#include "memtrack.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace std;

// IMPORTANT DESIGN NOTE:
// We track allocations in a hand-rolled linked list built with raw
// malloc/free (NOT std::unordered_map / std::string / std::vector for
// the tracking storage itself). Why: any STL container allocates memory
// via global operator new internally. If our operator new tried to
// insert into an STL map, that insertion would call operator new again
// -> infinite recursion -> stack overflow. Raw malloc/free sidesteps
// that completely. (std::vector<LeakInfo> is fine to use AFTER all
// tracking is done, e.g. when building the report -- see mt_report below.)

struct AllocNode {
    void *ptr;
    size_t size;
    char file[64];
    int line;
    char func[32];
    AllocNode *next;
};

static AllocNode *g_head = nullptr;
static size_t g_total_allocated = 0;
static size_t g_total_freed = 0;
static int g_alloc_count = 0;
static int g_free_count = 0;

static void track_alloc(void *ptr, size_t size, const char *file, int line, const char *func) {
    AllocNode *node = static_cast<AllocNode*>(malloc(sizeof(AllocNode)));
    if (!node) return;  // tracking failed; allocation itself still succeeds

    node->ptr = ptr;
    node->size = size;
    node->line = line;
    snprintf(node->file, sizeof(node->file), "%s", file);
    snprintf(node->func, sizeof(node->func), "%s", func);

    node->next = g_head;
    g_head = node;

    g_total_allocated += size;
    g_alloc_count++;
}

// Returns true if found & removed, false if not tracked.
static bool untrack_alloc(void *ptr) {
    AllocNode **cur = &g_head;
    while (*cur) {
        if ((*cur)->ptr == ptr) {
            AllocNode *to_remove = *cur;
            g_total_freed += to_remove->size;
            g_free_count++;
            *cur = to_remove->next;
            free(to_remove);
            return true;
        }
        cur = &(*cur)->next;
    }
    return false;
}

// ---- Tracked "new": called via the NEW(...) macro, e.g.
//      Type *p = NEW(Type)(ctor_args);
// which expands to: new (__FILE__, __LINE__, __func__) Type(ctor_args)
void *operator new(size_t size, const char *file, int line, const char *func) {
    void *ptr = malloc(size);
    if (!ptr) throw bad_alloc();
    track_alloc(ptr, size, file, line, func);
    return ptr;
}

// Required to pair with the placement new above (only invoked by the
// compiler if a constructor throws during NEW(...); not used otherwise).
void operator delete(void *ptr, const char *, int, const char *) noexcept {
    untrack_alloc(ptr);
    free(ptr);
}

// ---- Plain global new/delete: catches anything allocated WITHOUT the
// NEW(...) macro (e.g. std::string, std::vector, plain "new Foo").
// We still count these in totals, but call site is unknown since plain
// new/delete carries no file/line/func information.
void *operator new(size_t size) {
    void *ptr = malloc(size);
    if (!ptr) throw bad_alloc();
    track_alloc(ptr, size, "unknown", 0, "unknown");
    return ptr;
}

void operator delete(void *ptr) noexcept {
    if (!ptr) return;
    if (!untrack_alloc(ptr)) {
        fprintf(stderr,
            "[memtrack] WARNING: delete on untracked pointer %p "
            "(possible double delete or invalid pointer)\n", ptr);
        return;
    }
    free(ptr);
}

// Sized delete (C++14+): compiler may prefer this overload when it knows
// the size at the call site. We just forward to the unsized version.
void operator delete(void *ptr, size_t) noexcept {
    operator delete(ptr);
}

// Escapes backslashes for valid JSON output -- matters on Windows where
// file paths look like "C:\Users\you\demo.cpp". Writes into a fixed
// buffer using only raw char operations (no std::string, kept consistent
// with the rest of this file's "use malloc/free style, not STL" rule).
static void json_escape_path(const char *in, char *out, size_t out_size) {
    size_t j = 0;
    for (size_t i = 0; in[i] != '\0' && j + 2 < out_size; i++) {
        if (in[i] == '\\' || in[i] == '"') {
            out[j++] = '\\';
        }
        out[j++] = in[i];
    }
    out[j] = '\0';
}

// ---- Report -------------------------------------------------------
void mt_report() {
    printf("\n========== MEMORY REPORT ==========\n");
    printf("Total allocations : %d\n", g_alloc_count);
    printf("Total frees       : %d\n", g_free_count);
    printf("Bytes allocated   : %zu\n", g_total_allocated);
    printf("Bytes freed       : %zu\n", g_total_freed);
    printf("Bytes leaked      : %zu\n", g_total_allocated - g_total_freed);

    if (!g_head) {
        printf("\nNo memory leaks detected.\n");
        return;
    }

    printf("\n--- Leak Details (by call site) ---\n");
    FILE *leak_file = fopen("leaks.json", "w");
    if (leak_file) fprintf(leak_file, "[\n");
    bool first = true;

    for (AllocNode *cur = g_head; cur; cur = cur->next) {
        if (cur->line == 0) {
            printf("LEAK: %zu bytes (address %p) - call site unknown "
                   "(allocated via plain new / STL, not NEW macro)\n",
                   cur->size, cur->ptr);
            continue;  // no useful file/line to hand to the AI advisor
        }

        printf("LEAK: %zu bytes allocated in %s() at %s:%d (address %p)\n",
               cur->size, cur->func, cur->file, cur->line, cur->ptr);

        if (leak_file) {
            if (!first) fprintf(leak_file, ",\n");
            char escaped_file[128];
            json_escape_path(cur->file, escaped_file, sizeof(escaped_file));
            fprintf(leak_file,
                "  {\"size\": %zu, \"file\": \"%s\", \"line\": %d, \"func\": \"%s\"}",
                cur->size, escaped_file, cur->line, cur->func);
            first = false;
        }
    }

    if (leak_file) {
        fprintf(leak_file, "\n]\n");
        fclose(leak_file);
        printf("====================================\n");
        printf("\nLeak details written to leaks.json\n");
        printf("Run: python gemini_advisor.py   (to get AI-suggested fixes)\n");
    } else {
        printf("====================================\n");
    }
}
