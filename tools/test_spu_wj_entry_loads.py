#!/usr/bin/env python3
"""Deterministic tests for whole-job entry-stub register liveness."""

import unittest

from spu_wj_lifter import compute_entry_loads


class EntryLoadTests(unittest.TestCase):
    def loads(self, leaders, entries, carry, uses, defs, succ, cuts=()):
        return compute_entry_loads(
            leaders, entries, carry, uses, defs, succ, set(cuts))

    def test_disjoint_entries_do_not_load_each_others_registers(self):
        got = self.loads([0x10, 0x20], [0x10, 0x20], {},
                         {0x10: {3}, 0x20: {9}},
                         {0x10: set(), 0x20: set()},
                         {0x10: (), 0x20: ()})
        self.assertEqual(got[0x10], {3})
        self.assertEqual(got[0x20], {9})

    def test_successor_use_propagates_and_definition_kills(self):
        got = self.loads([0x10, 0x20], [0x10, 0x20], {},
                         {0x10: {2}, 0x20: {4, 7}},
                         {0x10: {4}, 0x20: set()},
                         {0x10: (0x20,), 0x20: ()})
        self.assertEqual(got[0x10], {2, 7})
        self.assertEqual(got[0x20], {4, 7})

    def test_loop_reaches_fixed_point(self):
        got = self.loads([0x10, 0x20, 0x30], [0x10], {},
                         {0x10: {1}, 0x20: {2}, 0x30: {3}},
                         {0x10: set(), 0x20: set(), 0x30: set()},
                         {0x10: (0x20,), 0x20: (0x30,),
                          0x30: (0x20,)})
        self.assertEqual(got[0x10], {1, 2, 3})

    def test_publication_carry_is_a_pseudo_read(self):
        got = self.loads([0x10, 0x20], [0x10], {0x20: {11}},
                         {0x10: set(), 0x20: set()},
                         {0x10: set(), 0x20: set()},
                         {0x10: (0x20,), 0x20: ()})
        self.assertEqual(got[0x10], {11})

    def test_call_reload_cut_does_not_pull_continuation_liveness_back(self):
        got = self.loads([0x10, 0x20], [0x10, 0x20], {},
                         {0x10: {5}, 0x20: {12}},
                         {0x10: set(), 0x20: set()},
                         {0x10: (0x20,), 0x20: ()}, cuts={0x10})
        self.assertEqual(got[0x10], {5})
        self.assertEqual(got[0x20], {12})

    def test_publish_without_reload_keeps_fallthrough_live(self):
        got = self.loads([0x10, 0x20], [0x10], {},
                         {0x10: {5}, 0x20: {12}},
                         {0x10: set(), 0x20: set()},
                         {0x10: (0x20,), 0x20: ()})
        self.assertEqual(got[0x10], {5, 12})


if __name__ == "__main__":
    unittest.main()
