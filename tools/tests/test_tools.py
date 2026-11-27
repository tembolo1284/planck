#!/usr/bin/env python3
"""Tests for the scripts in tools/.

    python3 tools/tests/test_tools.py
"""
import os
import sys
import tempfile
import unittest

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import bench_compare  # noqa: E402
import check_style  # noqa: E402
import seeded_bugs  # noqa: E402
from seeded_bugs_catalogue import BUGS  # noqa: E402


class SeededBugs(unittest.TestCase):
    def test_plant_replaces_the_one_occurrence(self):
        self.assertEqual(seeded_bugs.plant("a < b; c < d;", "c < d", "c <= d"), "a < b; c <= d;")
        self.assertEqual(seeded_bugs.plant("keep\ndrop\nkeep\n", "drop\n", ""), "keep\nkeep\n")

    def test_plant_refuses_a_pattern_that_is_absent(self):
        with self.assertRaises(ValueError) as raised:
            seeded_bugs.plant("a < b;", "a > b", "x")
        self.assertEqual(str(raised.exception), seeded_bugs.NOT_FOUND)

    def test_plant_refuses_a_pattern_that_matches_twice(self):
        # Changing the first of two matches would plant a bug, but nobody
        # could say which one from reading the catalogue.
        with self.assertRaises(ValueError) as raised:
            seeded_bugs.plant("x = 1; y = 1;", "= 1", "= 2")
        self.assertEqual(str(raised.exception), seeded_bugs.AMBIGUOUS)

    def test_select_filters_by_label(self):
        bugs = [{"label": "capi: one"}, {"label": "bench: two"}, {"label": "capi: three"}]
        self.assertEqual([b["label"] for b in seeded_bugs.select(bugs, "^capi")],
                         ["capi: one", "capi: three"])
        self.assertEqual(len(seeded_bugs.select(bugs, None)), 3)
        self.assertEqual(seeded_bugs.select(bugs, "nothing"), [])

    def test_validate_catches_broken_entries(self):
        good = {"label": "a", "file": "f", "find": "x", "replace": "y", "tests": "T"}
        self.assertEqual(seeded_bugs.validate([good]), [])
        self.assertTrue(seeded_bugs.validate([good, dict(good)]), "a repeated label")
        self.assertTrue(seeded_bugs.validate([dict(good, replace="x")]), "a change that changes nothing")
        self.assertTrue(seeded_bugs.validate([dict(good, find="  ")]), "an empty pattern")
        broken = dict(good)
        del broken["tests"]
        self.assertTrue(seeded_bugs.validate([broken]), "a missing field")

    def test_the_catalogue_still_fits_the_code(self):
        # The cheap half of a seeded-bug run: every pattern must still be
        # there, exactly once. A refactor that moves a line shows up here in
        # a second, not after half an hour of builds.
        self.assertEqual(seeded_bugs.validate(BUGS), [])
        self.assertGreater(len(BUGS), 30)
        for bug in BUGS:
            path = os.path.join(ROOT, bug["file"])
            self.assertTrue(os.path.isfile(path), "%s: no such file %s" % (bug["label"], bug["file"]))
            with open(path) as handle:
                text = handle.read()
            self.assertEqual(text.count(bug["find"]), 1,
                             "%s: its pattern must occur exactly once in %s" % (bug["label"], bug["file"]))


class BenchCompare(unittest.TestCase):
    @staticmethod
    def rows(**medians):
        # p99 is twice the median, p99.9 four times.
        return {name: {"p50_ns": value, "p99_ns": 2 * value, "p999_ns": 4 * value}
                for name, value in medians.items()}

    def test_change_is_growth_in_percent(self):
        self.assertAlmostEqual(bench_compare.change(100.0, 125.0), 25.0)
        self.assertAlmostEqual(bench_compare.change(100.0, 80.0), -20.0)
        self.assertEqual(bench_compare.change(0.0, 50.0), 0.0)

    def test_flags_what_got_slower_and_only_that(self):
        old = self.rows(steady=100.0, slower=100.0, faster=100.0, gone=100.0)
        new = self.rows(steady=105.0, slower=130.0, faster=70.0, fresh=100.0)
        lines, slower, missing, added = bench_compare.compare(old, new, 10.0, 25.0, 10.0)
        self.assertEqual(slower, ["slower"])
        self.assertEqual(missing, ["gone"])
        self.assertEqual(added, ["fresh"])
        verdicts = {name: verdict for name, _, _, _, verdict in lines}
        self.assertEqual(verdicts, {"steady": "", "slower": "slower", "faster": "faster"})

    def test_a_worse_tail_alone_is_enough(self):
        old = {"x": {"p50_ns": 100.0, "p99_ns": 200.0, "p999_ns": 400.0}}
        new = {"x": {"p50_ns": 100.0, "p99_ns": 260.0, "p999_ns": 400.0}}
        self.assertEqual(bench_compare.compare(old, new, 10.0, 25.0, 10.0)[1], ["x"])
        new["x"]["p99_ns"] = 240.0
        self.assertEqual(bench_compare.compare(old, new, 10.0, 25.0, 10.0)[1], [])

    def test_rows_too_small_to_time_are_never_flagged(self):
        old = self.rows(tiny=2.0)
        new = self.rows(tiny=4.0)  # twice as slow, and still only 8 ns at p99
        lines, slower, _, _ = bench_compare.compare(old, new, 10.0, 25.0, 10.0)
        self.assertEqual(slower, [])
        self.assertEqual(lines[0][4], "small")
        self.assertEqual(bench_compare.compare(old, new, 10.0, 25.0, 1.0)[1], ["tiny"])

    def test_reads_files_and_directories(self):
        header = "benchmark,samples,min_ns,p50_ns,p90_ns,p99_ns,p999_ns,p9999_ns,max_ns,mean_ns\n"
        with tempfile.TemporaryDirectory() as directory:
            first = os.path.join(directory, "bench.csv")
            second = os.path.join(directory, "loadgen.csv")
            with open(first, "w") as handle:
                handle.write(header + "book.add,10,1,16.2,3,34.8,75.7,6,7,8\n")
            with open(second, "w") as handle:
                handle.write(header + "order.sent_to_answer,10,1,9000,3,27000,5,6,7,8\n")
            with open(os.path.join(directory, "notes.txt"), "w") as handle:
                handle.write("not a csv file\n")
            one = bench_compare.read_rows(first)
            self.assertEqual(one, {"book.add": {"p50_ns": 16.2, "p99_ns": 34.8, "p999_ns": 75.7}})
            both = bench_compare.read_rows(directory)
            self.assertEqual(sorted(both), ["book.add", "order.sent_to_answer"])
            self.assertEqual(bench_compare.main([first, first]), 0)
            self.assertEqual(bench_compare.main([first, os.path.join(directory, "missing.csv")]), 2)
            with open(second, "w") as handle:
                handle.write("something,else\n1,2\n")
            self.assertEqual(bench_compare.main([first, second]), 2)


class CheckStyle(unittest.TestCase):
    def test_guard_names_follow_the_path(self):
        self.assertEqual(check_style.expected_guard("include/planck/core/types.hpp"),
                         "PLANCK_CORE_TYPES_HPP")
        self.assertEqual(check_style.expected_guard("include/planck/book/order_book.hpp"),
                         "PLANCK_BOOK_ORDER_BOOK_HPP")
        self.assertIsNone(check_style.expected_guard("bench/flow.hpp"))

    def test_accepts_a_classic_guard(self):
        lines = ["// comment\n", "#ifndef PLANCK_CORE_TYPES_HPP\n", "#define PLANCK_CORE_TYPES_HPP\n",
                 "#include <x>\n", "#endif\n"]
        self.assertEqual(check_style.check_guard("include/planck/core/types.hpp", lines), [])

    def test_rejects_pragma_once_a_missing_define_and_a_wrong_name(self):
        pragma = ["#pragma once\n", "#include <x>\n"]
        self.assertTrue(any("pragma" in message for _, message in
                            check_style.check_guard("bench/x.hpp", pragma)))
        no_define = ["#ifndef A_HPP\n", "#include <x>\n", "#endif\n"]
        self.assertTrue(check_style.check_guard("bench/x.hpp", no_define))
        mismatched = ["#ifndef A_HPP\n", "#define B_HPP\n", "#endif\n"]
        self.assertTrue(check_style.check_guard("bench/x.hpp", mismatched))
        wrong_name = ["#ifndef TYPES_HPP\n", "#define TYPES_HPP\n", "#endif\n"]
        self.assertEqual(check_style.check_guard("bench/types.hpp", wrong_name), [])
        self.assertTrue(check_style.check_guard("include/planck/core/types.hpp", wrong_name))

    def test_text_rules(self):
        clean = "one\ntwo\n"
        self.assertEqual(check_style.check_text(clean.splitlines(True), clean), [])
        for bad, word in (("a\tb\n", "tab"), ("a \n", "trailing"), ("a\nb", "newline"),
                          ("a " + chr(0x2014) + " b\n", "em dash"),
                          ("1" + chr(0x2013) + "2\n", "en dash")):
            problems = check_style.check_text(bad.splitlines(True), bad)
            self.assertEqual(len(problems), 1, repr(bad))
            self.assertIn(word, problems[0][1])

    def test_the_repository_passes(self):
        problems = []
        for relative in check_style.files_under(ROOT):
            problems += ["%s:%d: %s" % (relative, number, message)
                         for number, message in check_style.check_file(ROOT, relative)]
        self.assertEqual(problems, [])


if __name__ == "__main__":
    unittest.main()
