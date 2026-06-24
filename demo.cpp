#include <cstdio>
#include <stdexcept>
#include "memtrack.hpp"

using namespace std;

struct User {
    char name[32];
    User(const char *n) { snprintf(name, sizeof(name), "%s", n); }
};

// LEAK 1: Simple forgot-to-free.
// Allocated, used, function ends -- nobody ever called delete.
void create_user() {
    User *u = NEW(User)("Alice");
    printf("Created user: %s\n", u->name);
    // missing: delete u;
}

// LEAK 2: Early return skips the free.
// The delete exists below, but this path exits before reaching it.
void process_order(bool order_is_valid) {
    int *order_id = NEW(int);
    *order_id = 1001;

    if (!order_is_valid) {
        printf("Order invalid, aborting early.\n");
        return;              // <-- order_id leaks here
    }

    printf("Processing order #%d\n", *order_id);
    delete order_id;
}

// LEAK 3: Exception thrown before the free runs.
// Same shape as LEAK 2, but the skip is caused by an exception
// unwinding the stack instead of an early return.
void risky_operation() {
    int *buffer = NEW(int);
    *buffer = 99;

    printf("Buffer allocated, about to do something risky...\n");
    throw runtime_error("simulated failure");

    // delete buffer;  -- unreachable, never runs
}

// LEAK 4: Pointer overwritten before its old value was freed.
// There IS a delete here -- it just doesn't match the first allocation.
void update_counter() {
    int *counter = NEW(int);
    *counter = 1;
    printf("Counter started at %d\n", *counter);

    counter = NEW(int);   // <-- old counter (value 1) is now unreachable
    *counter = 2;
    printf("Counter reassigned to %d\n", *counter);

    delete counter;        // only frees the SECOND allocation
}

int main() {
    create_user();                 // LEAK 1

    process_order(false);          // LEAK 2 (invalid order, exits early)

    try {
        risky_operation();         // LEAK 3
    } catch (const exception &e) {
        printf("Caught exception: %s\n", e.what());
    }

    update_counter();              // LEAK 4

    mt_report();      // print final allocation/leak report
                       // (writes leaks.json if any leaks were found --
                       // run gemini_advisor.py afterward for AI suggestions)
    return 0;
}
