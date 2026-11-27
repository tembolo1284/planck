#!/usr/bin/env python3
"""Check the house rules that no compiler checks.

    tools/check_style.py [REPOSITORY]

  1. Every header has a classic include guard: #ifndef NAME, then
     #define NAME. No #pragma once. Under include/planck the name must
     follow from the path: include/planck/core/types.hpp is
     PLANCK_CORE_TYPES_HPP.
  2. No long dashes anywhere: neither the em dash nor the en dash.
  3. No tab characters, no trailing spaces, and a newline at the end of
     every file.

Prints one line per problem as file:line: message. Exit status is 1 if
there were any.
"""
import os
import sys

TEXT = (".hpp", ".h", ".cpp", ".c", ".py", ".sh", ".md", ".txt", ".cmake", ".json", ".map")
HEADERS = (".hpp", ".h")
SKIP_DIRECTORIES = {"build", ".git", "bench-results", "__pycache__", "_deps"}
# Built from their code points so that this file passes its own check.
LONG_DASHES = {chr(0x2014): "em dash", chr(0x2013): "en dash"}


def expected_guard(relative):
    """The guard name a header under include/planck must use, or None."""
    parts = relative.replace(os.sep, "/").split("/")
    if parts[:2] != ["include", "planck"]:
        return None
    stem = "_".join(parts[1:]).upper()
    return stem.replace(".", "_").replace("-", "_")


def check_guard(relative, lines):
    """Problems with a header's include guard, as (line number, message)."""
    problems = []
    directives = [(number, line.strip()) for number, line in enumerate(lines, 1)
                  if line.lstrip().startswith("#")]
    for number, text in directives:
        if text.replace(" ", "").startswith("#pragmaonce"):
            problems.append((number, "#pragma once: use an #ifndef guard"))
    if len(directives) < 2 or not directives[0][1].startswith("#ifndef "):
        problems.append((1, "the first directive must be #ifndef GUARD"))
        return problems
    guard = directives[0][1].split()[1]
    second = directives[1][1].split()
    if second[:2] != ["#define", guard]:
        problems.append((directives[1][0], "expected #define %s right after the #ifndef" % guard))
    wanted = expected_guard(relative)
    if wanted is not None and guard != wanted:
        problems.append((directives[0][0], "guard is %s, the path says %s" % (guard, wanted)))
    return problems


def check_text(lines, raw):
    """Problems any text file can have, as (line number, message)."""
    problems = []
    for number, line in enumerate(lines, 1):
        body = line.rstrip("\n")
        if "\t" in body:
            problems.append((number, "tab character"))
        if body != body.rstrip():
            problems.append((number, "trailing whitespace"))
        for dash, name in LONG_DASHES.items():
            if dash in body:
                problems.append((number, "%s: break the sentence up instead" % name))
    if raw and not raw.endswith("\n"):
        problems.append((len(lines), "no newline at the end of the file"))
    return problems


def check_file(root, relative):
    path = os.path.join(root, relative)
    try:
        with open(path, encoding="utf-8") as handle:
            raw = handle.read()
    except UnicodeDecodeError:
        return [(1, "not valid UTF-8")]
    lines = raw.splitlines(True)
    problems = check_text(lines, raw)
    if relative.endswith(HEADERS):
        problems += check_guard(relative, lines)
    return sorted(problems)


def files_under(root):
    for directory, names, files in os.walk(root):
        names[:] = sorted(name for name in names if name not in SKIP_DIRECTORIES)
        for name in sorted(files):
            if name.endswith(TEXT) or name == "CMakeLists.txt":
                yield os.path.relpath(os.path.join(directory, name), root)


def main(argv=None):
    arguments = sys.argv[1:] if argv is None else argv
    root = arguments[0] if arguments else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    count = 0
    checked = 0
    for relative in files_under(root):
        checked += 1
        for number, message in check_file(root, relative):
            print("%s:%d: %s" % (relative, number, message))
            count += 1
    if count:
        print("%d problems in %d files" % (count, checked))
        return 1
    print("%d files, no problems" % checked)
    return 0


if __name__ == "__main__":
    sys.exit(main())
