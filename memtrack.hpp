#ifndef MEMTRACK_HPP
#define MEMTRACK_HPP

#include <cstddef>
#include <new>

using namespace std;

// Tracked overload: records file/line/func at the call site.
void *operator new(size_t size, const char *file, int line, const char *func);
void  operator delete(void *ptr, const char *file, int line, const char *func) noexcept;

// Macro: rewrites NEW(Type) into the tracked placement-new above.
// Usage:  int* p = NEW(int);
//         Foo*  f = NEW(Foo)(arg1, arg2);   // pass ctor args after the macro
#define NEW(Type) new (__FILE__, __LINE__, __func__) Type

// Call this once (e.g. at the end of main()) to print the allocation/leak
// report. Also triggers the AI advisor (see ai_advisor.hpp) on any leaks
// found, if an API key is configured.
void mt_report();

#endif
