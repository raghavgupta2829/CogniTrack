r"""
gemini_advisor.py

Reads leaks.json (written by the C++ memtrack tool), pulls the actual
source code around each leak's reported line, and asks Gemini to (1)
classify WHICH KIND of leak it is and (2) suggest a specific fix.
Uses only Python's standard library (urllib) -- no "pip install" needed.

Why send source code instead of just file/line/func?
A leak report only tells you WHERE memory was allocated and that it was
never freed. It can't tell you WHY -- forgot to free, an early return
skipped the free, an exception skipped it, or the pointer got
overwritten before the old value was freed. Those are different bugs
with different fixes. Giving the AI a few lines of real code around the
leak lets it actually tell those cases apart, instead of giving the same
generic "you forgot delete" answer every time.

Usage:
    1. Get a free API key:  https://aistudio.google.com/apikey
    2. Set it as an environment variable (PowerShell):
           $env:GEMINI_API_KEY="your-key-here"
       (Mac/Linux):
           export GEMINI_API_KEY="your-key-here"
    3. Run the C++ demo first to generate leaks.json:
           .\demo.exe
    4. Then run this script:
           python gemini_advisor.py
"""

import json
import os
import sys
import time
import urllib.request
import urllib.error

GEMINI_URL = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent"

# How many lines of source to show before/after the leaking line.
CONTEXT_LINES_BEFORE = 6
CONTEXT_LINES_AFTER = 4


def get_source_context(file_path: str, line_num: int) -> str:
    """Reads a window of lines around line_num from file_path, with line
    numbers, so the AI can see the actual code shape (early return,
    try/throw, reassignment, etc.) instead of just a single line."""
    if not os.path.exists(file_path):
        return "(source file not found -- could not load context)"

    with open(file_path, "r") as f:
        lines = f.readlines()

    start = max(0, line_num - 1 - CONTEXT_LINES_BEFORE)
    end = min(len(lines), line_num + CONTEXT_LINES_AFTER)

    snippet_lines = []
    for i in range(start, end):
        marker = ">>" if (i + 1) == line_num else "  "
        snippet_lines.append(f"{marker} {i + 1}: {lines[i].rstrip()}")
    return "\n".join(snippet_lines)


def build_prompt(leak: dict, source_context: str) -> str:
    return f"""A C++ memory leak was detected by a custom allocator tracker.

Leak details:
- {leak['size']} bytes allocated in function '{leak['func']}'
- Location: {leak['file']}:{leak['line']} (marked with >> below)

Surrounding source code:
{source_context}

Tasks:
1. Classify this leak as ONE of: "forgot to free", "early return skips free",
   "exception skips free", "pointer overwritten before free", or "other"
   (pick the single best match based on the actual code shown).
2. In one sentence, explain specifically why THIS code leaks (reference
   what you see in the snippet, not a generic explanation).
3. In one sentence, suggest the specific fix for this exact case.

Respond in exactly this format, no extra commentary:
Category: <category>
Why: <one sentence>
Fix: <one sentence>"""


def call_gemini(api_key: str, prompt: str, max_retries: int = 4) -> str:
    """Sends one prompt to Gemini, returns the text reply (or an error string).
    Retries with increasing delay on:
      - 429 (rate limited -- free tier quota, easy to hit with back-to-back calls)
      - 503 (server temporarily overloaded -- Google's servers, not our fault,
             usually clears within seconds to a couple minutes)
    Other errors (e.g. 403 bad key, 404 bad model name) fail immediately,
    since retrying won't fix those."""
    body = json.dumps({
        "contents": [{"parts": [{"text": prompt}]}]
    }).encode("utf-8")

    req = urllib.request.Request(
        GEMINI_URL,
        data=body,
        headers={
            "Content-Type": "application/json",
            "x-goog-api-key": api_key,
            "User-Agent": "memtrack-gemini-advisor/1.0",
        },
        method="POST",
    )

    for attempt in range(max_retries):
        try:
            with urllib.request.urlopen(req, timeout=15) as resp:
                data = json.loads(resp.read().decode("utf-8"))
            break
        except urllib.error.HTTPError as e:
            if e.code in (429, 503) and attempt < max_retries - 1:
                wait = 2 ** (attempt + 2)  # 4s, 8s, 16s -- exponential backoff
                reason = "rate limited" if e.code == 429 else "server overloaded"
                print(f"  ({reason} [{e.code}], waiting {wait}s before retry...)")
                time.sleep(wait)
                continue
            return f"(API error {e.code}: {e.reason})"
        except urllib.error.URLError as e:
            return f"(network error: {e.reason})"
    else:
        return "(Gemini still unavailable after retries -- try again in a minute or two)"

    try:
        return data["candidates"][0]["content"]["parts"][0]["text"].strip()
    except (KeyError, IndexError):
        return f"(unexpected response shape: {data})"


def main():
    api_key = os.environ.get("GEMINI_API_KEY")
    if not api_key:
        print("GEMINI_API_KEY environment variable not set.")
        print("Get a key at https://aistudio.google.com/apikey then run:")
        print('  PowerShell:  $env:GEMINI_API_KEY="your-key-here"')
        print('  Mac/Linux:   export GEMINI_API_KEY="your-key-here"')
        sys.exit(1)

    if not os.path.exists("leaks.json"):
        print("leaks.json not found. Run the demo first (it writes this file")
        print("automatically whenever a leak is detected): .\\demo.exe")
        sys.exit(1)

    with open("leaks.json", "r") as f:
        leaks = json.load(f)

    if not leaks:
        print("leaks.json is empty -- no leaks to analyze.")
        return

    print(f"\n--- AI Suggested Fixes (Gemini) --- ({len(leaks)} leak(s))\n")

    for i, leak in enumerate(leaks):
        if i > 0:
            time.sleep(2)  # small gap between calls to avoid bursting the rate limit

        source_context = get_source_context(leak["file"], leak["line"])
        prompt = build_prompt(leak, source_context)
        suggestion = call_gemini(api_key, prompt)

        print(f"[{leak['func']}() at {leak['file']}:{leak['line']}, {leak['size']} bytes]")
        print(suggestion)
        print()


if __name__ == "__main__":
    main()
