# MemTrack (C++) — Dynamic Memory Allocation Analysis Tool + AI Fix Suggestions

A small C++ library that overloads `operator new` / `operator delete`
to track every heap allocation, detect memory leaks, and report which
function (and file/line) leaked memory — then optionally asks **Google
Gemini** for a one-line suggested fix for each leak found.

Inspired by `dmalloc`, ported to idiomatic C++.

## Two-part design

| Part | Language | Job |
|------|----------|-----|
| **Tracker** | C++ | Detects leaks, prints report, writes `leaks.json` |
| **Advisor** | Python | Reads `leaks.json`, asks Gemini for fixes, prints suggestions |

These are **deliberately separate programs**, not one combined binary.
C++ doesn't have a built-in, dependency-free way to make HTTPS requests
— you'd normally need `libcurl`, which requires dev headers and a
working package manager. Rather than fight platform-specific curl setup
(especially painful on older/standalone MinGW installs on Windows),
the C++ side just writes a small JSON file, and a ~70-line Python
script (using only the standard library — no `pip install` needed)
handles the network call. This is a completely normal real-world
pattern: different languages for different jobs, glued together by a
file or a queue.

## Files (6 total)

| File                | Purpose                                                       |
|---------------------|------------------------------------------------------------------|
| `memtrack.hpp`      | Declares tracked `operator new`/`delete` + `NEW(Type)` macro    |
| `memtrack.cpp`      | Tracking logic: linked list, report, writes `leaks.json`        |
| `demo.cpp`          | Example program that leaks memory on purpose                     |
| `gemini_advisor.py` | Reads `leaks.json`, calls Gemini, prints suggested fixes          |
| `Makefile`          | For Linux/Mac users with `make`. Windows: see Build & Run below |

## The demo: 4 genuinely different leak patterns

`demo.cpp` doesn't just leak the same way four times — it demonstrates
four distinct *reasons* code leaks, all detectable by this tracker:

1. **`create_user()` — forgot to free.** Allocated, used, function ends, nobody ever called `delete`.
2. **`process_order()` — early return skips the free.** The `delete` exists in the code, but an early `return` exits before reaching it.
3. **`risky_operation()` — exception skips the free.** Same shape as #2, but the skip is caused by a thrown exception unwinding the stack instead of a `return`.
4. **`update_counter()` — pointer overwritten before its old value was freed.** There IS a `delete` in this function — it just doesn't match the *first* allocation, because the pointer got reassigned to a new allocation first.

This matters for the AI advisor step below: a leak report alone can't
tell these apart (all four just show up as "allocated here, never
freed"). Telling them apart requires looking at the actual code shape
around the leak.

## How the leak detector works (C++ side)

C++ doesn't let you `#define new` like a function macro — `new` is an
operator. So we **overload `operator new`/`operator delete`** globally,
and use a placement-new trick to still capture the call site:

```cpp
#define NEW(Type) new (__FILE__, __LINE__, __func__) Type
// usage: User *u = NEW(User)("Alice");
```

Every tracked allocation is stored in a **hand-rolled linked list**
built with raw `malloc`/`free` — not an STL container — because an STL
container would itself call `operator new` internally, which is *our
own overloaded operator*, causing infinite recursion.

At report time, whatever is still in the linked list was never freed —
that's a leak, with exact file/line/function. If any leaks are found,
`mt_report()` also writes them to `leaks.json` in the current directory:

```json
[
  {"size": 32, "file": "demo.cpp", "line": 12, "func": "create_user"},
  {"size": 32, "file": "demo.cpp", "line": 12, "func": "create_user"}
]
```

## Two AI advisor scripts — pick one

| Script             | Provider | Notes                                              |
|---------------------|----------|------------------------------------------------------|
| `gemini_advisor.py` | Google Gemini (`gemini-2.5-flash`) | Free tier, 1,500 requests/day |
| `groq_advisor.py`   | Groq (`llama-3.3-70b-versatile`)   | Free tier, runs on custom LPU hardware, generally fast responses |

Both do exactly the same thing — read `leaks.json`, build the same
source-context-aware prompt, print the same `Category/Why/Fix` format —
just talking to a different provider with a different request format.
Use whichever key you have. If one provider is slow or temporarily
overloaded, try the other without changing anything else in the project.

## How the AI advisor works (Python side)

`gemini_advisor.py` reads `leaks.json`, and for each leak it does
something more useful than just looking at the file/line/func: it
**reads the actual source file and pulls a window of real code** around
the leaking line (6 lines before, 4 lines after, by default). That
snippet is sent to Gemini along with a structured prompt asking it to:

1. **Classify** the leak as one of: forgot to free / early return skips
   free / exception skips free / pointer overwritten before free / other
2. **Explain** specifically why *this* code leaks, referencing what's
   actually in the snippet
3. **Suggest** a fix specific to that case

This is the difference between asking "why did this leak?" with no
context (which always gets the same generic answer: "you probably
forgot to call delete") versus showing the AI the actual early-return,
the actual `throw`, or the actual pointer reassignment — at which point
its answer changes based on what's really there, because there's
something real to reason about. Run the demo and you should see four
genuinely different classifications and explanations, not four copies
of the same sentence.

## Build & Run

### Step 1 — get an API key (Gemini or Groq, your choice)

- Gemini: get one free at **https://aistudio.google.com/apikey**
- Groq: get one free at **https://console.groq.com/keys**

**Treat this key like a password — never paste it directly into a
chat, commit it to a repo, or share a terminal screenshot containing
it.** If you ever do paste it somewhere by accident, go back to that
page and delete/regenerate it immediately.

### Step 2 — set it as an environment variable

**Windows (PowerShell):**
```powershell
$env:GEMINI_API_KEY="your-key-here"
# or, if using Groq instead:
$env:GROQ_API_KEY="your-key-here"
```
This only lasts for the current PowerShell window — you'll need to set
it again if you close and reopen PowerShell.

**Mac/Linux (bash):**
```bash
export GEMINI_API_KEY="your-key-here"
# or, if using Groq instead:
export GROQ_API_KEY="your-key-here"
```

### Step 3 — build the C++ tracker

**Windows (PowerShell, MinGW g++, no `make` needed):**
```powershell
g++ -Wall -Wextra -std=c++17 -g -o demo.exe demo.cpp memtrack.cpp
.\demo.exe
```

**Mac/Linux (with `make`):**
```bash
make run
```

This prints the leak report and writes `leaks.json` if any leaks were found.

### Step 4 — run the AI advisor

Using Gemini:
```bash
python gemini_advisor.py
```
Using Groq instead:
```bash
python groq_advisor.py
```
(Windows: `python` should work the same way in PowerShell, assuming
Python is installed and on your PATH. Try `py gemini_advisor.py` if
`python` isn't recognized.)

## Example output

**C++ side:**
```
Created user: Alice
Order invalid, aborting early.
Buffer allocated, about to do something risky...
Caught exception: simulated failure
Counter started at 1
Counter reassigned to 2

========== MEMORY REPORT ==========
Total allocations : 6
Total frees       : 2
Bytes allocated   : 90
Bytes freed       : 46
Bytes leaked      : 44

--- Leak Details (by call site) ---
LEAK: 4 bytes allocated in update_counter() at demo.cpp:51 (address 0x...)
LEAK: 4 bytes allocated in risky_operation() at demo.cpp:39 (address 0x...)
LEAK: 4 bytes allocated in process_order() at demo.cpp:23 (address 0x...)
LEAK: 32 bytes allocated in create_user() at demo.cpp:15 (address 0x...)
====================================

Leak details written to leaks.json
Run: python gemini_advisor.py   (to get AI-suggested fixes)
```

**Python side (answers will vary in exact wording, but should
correctly classify each of the four cases differently):**
```
--- AI Suggested Fixes (Gemini) --- (4 leak(s))

[update_counter() at demo.cpp:51, 4 bytes]
Category: pointer overwritten before free
Why: 'counter' is reassigned to a new allocation on line 55 before the
first allocation (holding value 1) is ever freed, making it unreachable.
Fix: Call delete counter; immediately before reassigning it on line 55.

[risky_operation() at demo.cpp:39, 4 bytes]
Category: exception skips free
Why: The thrown exception on line 43 unwinds the stack before the
commented-out delete on line 45 can run.
Fix: Wrap the allocation in a smart pointer like unique_ptr<int>, or
use a try/catch with delete in the catch block before rethrowing.

[process_order() at demo.cpp:23, 4 bytes]
Category: early return skips free
Why: The early return on line 27 (when order_is_valid is false) exits
the function before reaching the delete at the end.
Fix: Add delete order_id; before the early return, or switch to a
smart pointer so cleanup happens automatically.

[create_user() at demo.cpp:15, 32 bytes]
Category: forgot to free
Why: The User object is allocated and used, but no delete call exists
anywhere in the function.
Fix: Add delete u; at the end of create_user(), or use unique_ptr<User>.
```

## A note on temporary errors (HTTP 429 and 503)

Two error codes are worth knowing the difference between:

- **429 Too Many Requests** — *you've* sent too many calls too fast (a
  free-tier quota cap). The script avoids this with a small 2-second
  delay between consecutive leak requests.
- **503 Service Unavailable** — *Google's* servers are temporarily
  overloaded, unrelated to your key or code. This happens during high
  demand periods and is common with free-tier traffic.

Both are temporary, and the script automatically retries both with
exponential backoff (4s, 8s, 16s) before giving up. Other errors (like
a bad key, which shows as `403`) fail immediately instead of retrying,
since retrying won't fix those.

If you still see persistent 429s or 503s after the automatic retries,
wait a minute or two and run `python gemini_advisor.py` again — your
`leaks.json` is already saved, so you don't need to rebuild or rerun
the C++ demo first.

## A note on `using namespace std;`

This project uses `using namespace std;` globally instead of `std::`
prefixes. Worth knowing for an interview: this is fine in a small,
single-purpose project like this, but in larger codebases it's
generally avoided in headers, because it pulls every name from `std`
into the global namespace and can cause silent name clashes as a
project grows.

## What it catches

- **Memory leaks** — `new`'d via `NEW(Type)`, never `delete`d, with exact file/line/function
- **Double delete / invalid delete** — warns instead of crashing
- **Untracked allocations** — plain `new`/STL allocations counted in totals, flagged as "call site unknown" (and skipped by the AI advisor, since there's no useful location to suggest a fix for)
- **AI-suggested fixes** — one-line recommendation per leak, when configured

## What it deliberately leaves out (good interview discussion points)

- **No array new/delete (`new[]`/`delete[]`) support** — different operator pair, skipped to keep the demo focused
- **No thread safety** — the linked list needs a mutex for multi-threaded use
- **O(n) lookup per delete** — explained above why a hash table isn't a free upgrade here
- **C++ and Python communicate via a file, not a live connection** — simplest possible integration; a production version might use a local socket or just call a Python subprocess directly from C++, but a flat file is trivial to debug (you can just open `leaks.json` and read it) and has zero dependency overhead

## Talking points for the interview

1. **Why overload global `operator new`/`delete` instead of wrapping `malloc`/`free` like in C?**
   Idiomatic C++ allocation goes through `new`/`delete`; overloading the global operators is the standard way debug allocators hook in.
2. **Why avoid STL containers inside the tracker itself?**
   Walk through the recursion risk — the allocator can't depend on something that depends on the allocator.
3. **Why split the AI feature into a separate Python script instead of calling the API from C++?**
   C++ has no built-in HTTP client; doing it "properly" needs libcurl, which means dev headers, linker flags, and platform-specific package managers. Python's standard library already has everything needed for a simple REST call. Using the right tool for each job, glued together by a plain file, is a legitimate and common architecture — not a workaround.
4. **What would you change for production?**
   Add a hash table for O(1) lookups, thread safety via a mutex, and probably call the AI step via subprocess from C++ directly instead of a manual two-step run, while keeping the same file-based handoff for simplicity.
