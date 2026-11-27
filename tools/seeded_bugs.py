#!/usr/bin/env python3
"""Plant bugs one at a time and check that a test notices each.

    tools/seeded_bugs.py                 run every bug in the catalogue
    tools/seeded_bugs.py --only capi     only bugs whose label contains "capi"
    tools/seeded_bugs.py --list          name the bugs and stop

A test suite that passes says little unless it would fail when the code is
wrong. This checks that directly. For each entry in seeded_bugs_catalogue.py
it makes one small, deliberate change to the source, rebuilds, and runs the
tests that ought to catch it. A bug that every test still passes with has
"survived", and means a test is missing.

The repository itself is never touched. Everything happens in a copy, which
is built once, checked to pass as it stands, and then changed and restored
for each bug in turn. The copy is removed at the end unless --keep is given.

Exit status is 0 only if every bug was caught.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

CAUGHT = "caught"
SURVIVED = "SURVIVED"
NO_BUILD = "did not compile"
NOT_FOUND = "PATTERN NOT FOUND"
AMBIGUOUS = "PATTERN NOT UNIQUE"

# Outcomes that mean the catalogue or the tests need attention.
BAD = (SURVIVED, NOT_FOUND, AMBIGUOUS)


def plant(text, find, replace):
    """Return text with its one occurrence of find replaced.

    Raises ValueError naming the outcome if find is absent or occurs more
    than once: a pattern that matches twice would make it unclear which bug
    was planted.
    """
    count = text.count(find)
    if count == 0:
        raise ValueError(NOT_FOUND)
    if count > 1:
        raise ValueError(AMBIGUOUS)
    return text.replace(find, replace, 1)


def select(bugs, pattern):
    """The bugs whose label matches the regular expression, in order."""
    if not pattern:
        return list(bugs)
    matcher = re.compile(pattern)
    return [bug for bug in bugs if matcher.search(bug["label"])]


def validate(bugs):
    """Complain about catalogue entries that cannot be run. Returns messages."""
    problems = []
    seen = set()
    for index, bug in enumerate(bugs):
        where = "entry %d" % index
        for key in ("label", "file", "find", "replace", "tests"):
            if not isinstance(bug.get(key), str):
                problems.append("%s: missing or non-text '%s'" % (where, key))
        label = bug.get("label")
        if label in seen:
            problems.append("%s: label '%s' is used twice" % (where, label))
        seen.add(label)
        if bug.get("find") == bug.get("replace"):
            problems.append("%s: find and replace are the same" % where)
        if isinstance(bug.get("find"), str) and not bug["find"].strip():
            problems.append("%s: find is empty" % where)
    return problems


def run(command, cwd, log, timeout=None):
    """Run a command, appending its output to the log. Returns its status."""
    with open(log, "a") as out:
        out.write("\n$ %s\n" % " ".join(command))
        out.flush()
        try:
            return subprocess.run(command, cwd=cwd, stdout=out, stderr=subprocess.STDOUT,
                                  timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            out.write("\n(timed out)\n")
            return 124


def copy_repository(source, destination):
    ignore = shutil.ignore_patterns("build", "build-*", ".git", "bench-results", "__pycache__")
    shutil.copytree(source, destination, ignore=ignore, symlinks=True)


class Workbench:
    """A private copy of the repository, configured and built."""

    def __init__(self, source, directory, jobs, compiler):
        self.directory = directory
        self.build = os.path.join(directory, "seeded-build")
        self.log = os.path.join(directory, "seeded_bugs.log")
        self.jobs = str(jobs)
        self.stamp = 0.0
        copy_repository(source, directory)
        configure = ["cmake", "-S", directory, "-B", self.build, "-G", "Ninja",
                     "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DPLANCK_CONTRACTS=checked"]
        if compiler:
            configure.append("-DCMAKE_CXX_COMPILER=" + compiler)
        if run(configure, directory, self.log) != 0:
            raise RuntimeError("configure failed, see " + self.log)

    def compile(self):
        return run(["cmake", "--build", self.build, "--parallel", self.jobs],
                   self.directory, self.log) == 0

    def test(self, pattern, limit):
        # Each test carries its own time limit from the build (120 seconds),
        # which is what ends a test that a bug has made hang. `limit` caps
        # the whole run, in case ctest itself gets stuck.
        command = ["ctest", "--test-dir", self.build, "--parallel", self.jobs, "-R", pattern]
        return run(command, self.directory, self.log, timeout=limit) == 0

    def write(self, path, text):
        """Write a source file so that the build is certain to notice.

        The build tool rebuilds what is newer than its output. Bugs can be
        planted and removed several times within one second, and some file
        systems keep times only to the second. So every write is stamped at
        least a second later than the one before, ahead of the clock if
        need be. Without this a binary built from one bug could be tested
        as if it were the next.
        """
        with open(path, "w") as handle:
            handle.write(text)
        self.stamp = max(time.time(), self.stamp + 1.0)
        os.utime(path, (self.stamp, self.stamp))

    def try_bug(self, bug, limit):
        path = os.path.join(self.directory, bug["file"])
        if not os.path.isfile(path):
            return NOT_FOUND
        with open(path) as handle:
            original = handle.read()
        try:
            changed = plant(original, bug["find"], bug["replace"])
        except ValueError as problem:
            return str(problem)
        self.write(path, changed)
        try:
            if not self.compile():
                return NO_BUILD
            return SURVIVED if self.test(bug["tests"], limit) else CAUGHT
        finally:
            # Put the source back. The next build picks the change up, so
            # no stale binary outlives the bug it was built from.
            self.write(path, original)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--only", metavar="REGEX", help="run only bugs whose label matches")
    parser.add_argument("--list", action="store_true", help="name the bugs and stop")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--timeout", type=int, default=900, metavar="SECONDS",
                        help="longest the tests for one bug may take in all (default 900)")
    parser.add_argument("--compiler", metavar="CXX", help="C++ compiler to build the copy with")
    parser.add_argument("--keep", action="store_true", help="leave the working copy behind")
    arguments = parser.parse_args(argv)

    from seeded_bugs_catalogue import BUGS
    problems = validate(BUGS)
    if problems:
        print("\n".join(problems))
        return 2
    bugs = select(BUGS, arguments.only)
    if arguments.list:
        for bug in bugs:
            print("%-58s %s" % (bug["label"], bug["file"]))
        return 0
    if not bugs:
        print("no bug matches '%s'" % arguments.only)
        return 2

    source = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    parent = tempfile.mkdtemp(prefix="planck-seeded-")
    directory = os.path.join(parent, "planck")
    status = 2
    try:
        print("working copy: %s" % directory)
        bench = Workbench(source, directory, arguments.jobs, arguments.compiler)
        print("building it as it stands...")
        if not bench.compile():
            print("the unchanged tree does not build. See %s" % bench.log)
            return 2
        every = "|".join(sorted({bug["tests"] for bug in bugs}))
        print("checking that the tests pass before anything is changed...")
        if not bench.test(every, arguments.timeout):
            print("tests fail on the unchanged tree. Fix that first. See %s" % bench.log)
            return 2

        results = []
        for number, bug in enumerate(bugs, 1):
            started = time.time()
            outcome = bench.try_bug(bug, arguments.timeout)
            results.append((bug, outcome))
            print("[%2d/%d] %-16s %-58s %4.0fs" % (number, len(bugs), outcome, bug["label"],
                                                  time.time() - started))
            sys.stdout.flush()

        counts = {}
        for _, outcome in results:
            counts[outcome] = counts.get(outcome, 0) + 1
        print("\n%d bugs: %s" % (len(results), ", ".join(
            "%d %s" % (count, outcome) for outcome, count in sorted(counts.items()))))
        bad = [(bug, outcome) for bug, outcome in results if outcome in BAD]
        for bug, outcome in bad:
            print("  %s: %s (%s, tests '%s')" % (outcome, bug["label"], bug["file"], bug["tests"]))
        if counts.get(NO_BUILD):
            print("  Bugs that did not compile prove nothing about the tests. Reword them.")
        status = 1 if bad else 0
        return status
    finally:
        if arguments.keep or status == 2:
            print("left behind: %s" % parent)
        else:
            shutil.rmtree(parent, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
