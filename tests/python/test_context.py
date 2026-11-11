"""Ambient context from Python: thread tags, process tags and scopes."""

import threading
import unittest

from support import LumenTestCase, lumen

# lumen/limits.hpp
MAX_THREAD_TAGS = 8
MAX_PROCESS_TAGS = 16


class TagLayerTest(LumenTestCase):
    def test_thread_tag_is_added_to_records_of_the_thread(self):
        lumen.set_thread_tag("worker", "a")
        lumen.set_thread_tag("rank", 3)
        lumen.info("tagged")
        self.assertEqual(self.only_log().tags, {"worker": "a", "rank": "3"})

    def test_setting_a_thread_tag_again_replaces_its_value(self):
        lumen.set_thread_tag("phase", "warmup")
        lumen.set_thread_tag("phase", "run")
        lumen.info("replaced")
        self.assertEqual(self.only_log().tags, {"phase": "run"})

    def test_removed_and_cleared_thread_tags_are_gone(self):
        lumen.set_thread_tag("a", "1")
        lumen.set_thread_tag("b", "2")
        lumen.set_thread_tag("c", "3")
        self.assertTrue(lumen.remove_thread_tag("a"))
        self.assertFalse(lumen.remove_thread_tag("a"))
        lumen.info("after remove")
        lumen.clear_thread_tags()
        lumen.info("after clear")
        logs = self.flushed_logs()
        self.assertEqual([r.tags for r in logs], [{"b": "2", "c": "3"}, {}])

    def test_process_tags_are_set_removed_and_cleared(self):
        lumen.set_process_tag("host", "node-1")
        lumen.set_process_tag("ok", True)
        lumen.info("both")
        self.assertTrue(lumen.remove_process_tag("ok"))
        self.assertFalse(lumen.remove_process_tag("ok"))
        lumen.info("one")
        lumen.clear_process_tags()
        lumen.info("none")
        logs = self.flushed_logs()
        self.assertEqual([r.tags for r in logs], [{"host": "node-1", "ok": "true"},
                                                  {"host": "node-1"}, {}])

    def test_process_tag_reaches_records_of_other_threads(self):
        lumen.set_process_tag("job", 12)
        thread = threading.Thread(target=lumen.info, args=("other thread",))
        thread.start()
        thread.join()
        self.assertEqual(self.only_log().tags, {"job": "12"})

    def test_full_thread_table_raises_and_stays_unchanged(self):
        for i in range(MAX_THREAD_TAGS):
            lumen.set_thread_tag(f"k{i}", i)
        with self.assertRaises(lumen.TagTableFullError) as raised:
            lumen.set_thread_tag("one_more", "x")
        self.assertIsInstance(raised.exception, lumen.LumenError)
        lumen.set_thread_tag("k0", "replaced")  # an existing key still fits
        lumen.info("full")
        expected = {f"k{i}": str(i) for i in range(MAX_THREAD_TAGS)}
        expected["k0"] = "replaced"
        self.assertEqual(self.only_log().tags, expected)

    def test_full_process_table_raises_and_stays_unchanged(self):
        for i in range(MAX_PROCESS_TAGS):
            lumen.set_process_tag(f"p{i}", i)
        with self.assertRaises(lumen.TagTableFullError):
            lumen.set_process_tag("one_more", "x")
        lumen.info("full")
        self.assertEqual(self.only_log().tags, {f"p{i}": str(i) for i in range(MAX_PROCESS_TAGS)})

    def test_unsupported_tag_value_raises_type_error(self):
        with self.assertRaises(TypeError):
            lumen.set_thread_tag("k", object())
        with self.assertRaises(TypeError):
            lumen.set_process_tag("k", None)

    def test_explicit_beats_scope_beats_thread_beats_process(self):
        lumen.set_process_tag("k", "process")
        lumen.set_process_tag("p", "process")
        lumen.set_thread_tag("k", "thread")
        lumen.set_thread_tag("t", "thread")
        with lumen.scope("k", "scope"), lumen.scope("s", "scope"):
            lumen.info("explicit", tags={"k": "explicit"})
            lumen.info("scope")
        lumen.info("thread")
        lumen.clear_thread_tags()
        lumen.info("process")
        logs = self.flushed_logs()
        self.assertEqual([r.tags["k"] for r in logs], ["explicit", "scope", "thread", "process"])
        self.assertEqual(logs[1].tags, {"k": "scope", "s": "scope", "t": "thread",
                                        "p": "process"})


class ScopeTest(LumenTestCase):
    def test_scope_tag_is_present_only_inside_the_block(self):
        lumen.info("before")
        with lumen.scope("epoch", 2):
            lumen.info("inside")
        lumen.info("after")
        self.assertEqual([r.tags for r in self.flushed_logs()], [{}, {"epoch": "2"}, {}])

    def test_inner_scope_wins_a_key_and_the_outer_value_returns(self):
        with lumen.scope("stage", "outer"):
            with lumen.scope("stage", "inner"):
                lumen.info("inner")
            lumen.info("outer")
        self.assertEqual([r.tags for r in self.flushed_logs()],
                         [{"stage": "inner"}, {"stage": "outer"}])

    def test_scope_is_left_when_the_block_raises(self):
        with self.assertRaises(KeyError):
            with lumen.scope("doomed", "yes"):
                raise KeyError("x")
        lumen.info("after")
        self.assertEqual(self.only_log().tags, {})

    def test_leaving_an_outer_scope_first_closes_the_inner_one_too(self):
        outer = lumen.scope("outer", 1)
        inner = lumen.scope("inner", 2)
        outer.__enter__()
        inner.__enter__()
        outer.__exit__(None, None, None)
        lumen.info("both closed")
        inner.__exit__(None, None, None)  # already closed: nothing happens
        lumen.info("still closed")
        self.assertEqual([r.tags for r in self.flushed_logs()], [{}, {}])

    def test_entering_an_entered_scope_raises_value_error(self):
        s = lumen.scope("k", "v")
        with s:
            with self.assertRaises(ValueError):
                s.__enter__()
            lumen.info("once")
        self.assertEqual(self.only_log().tags, {"k": "v"})

    def test_leaving_a_scope_that_was_not_entered_raises_value_error(self):
        with self.assertRaises(ValueError):
            lumen.scope("k", "v").__exit__(None, None, None)

    def test_scope_object_can_be_entered_again_after_it_was_left(self):
        s = lumen.scope("round", "r")
        for _ in range(2):
            with s:
                lumen.info("in")
        self.assertEqual([r.tags for r in self.flushed_logs()], [{"round": "r"}] * 2)

    def test_leaving_a_scope_on_another_thread_raises_and_leaves_it_open(self):
        s = lumen.scope("owner", "main")
        s.__enter__()
        errors = []

        def leave():
            try:
                s.__exit__(None, None, None)
            except RuntimeError as e:
                errors.append(e)

        thread = threading.Thread(target=leave)
        thread.start()
        thread.join()
        lumen.info("still open")
        s.__exit__(None, None, None)
        lumen.info("closed")
        self.assertEqual(len(errors), 1)
        self.assertEqual([r.tags for r in self.flushed_logs()], [{"owner": "main"}, {}])

    def test_scope_of_one_thread_is_not_seen_by_another(self):
        with lumen.scope("only", "main"):
            thread = threading.Thread(target=lumen.info, args=("other",))
            thread.start()
            thread.join()
        self.assertEqual(self.only_log().tags, {})

    def test_thread_ending_inside_a_scope_tags_its_record_and_exits_cleanly(self):
        # The scope is closed when the thread ends; under ASan a guard left
        # behind in a destroyed thread context would be reported.
        def open_and_return():
            lumen.scope("abandoned", "yes").__enter__()
            lumen.info("inside")

        thread = threading.Thread(target=open_and_return)
        thread.start()
        thread.join()
        self.assertEqual(self.only_log().tags, {"abandoned": "yes"})


if __name__ == "__main__":
    unittest.main()
