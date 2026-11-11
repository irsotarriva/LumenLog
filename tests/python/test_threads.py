"""Python threads: logging from several at once, per-thread tags, and sink
registration while a Python sink is receiving records."""

import threading
import unittest

from support import LumenTestCase, lumen, run_child

THREADS = 6
RECORDS_PER_THREAD = 500


class ThreadTest(LumenTestCase):
    def test_every_record_from_several_threads_arrives_once_with_its_thread_tag(self):
        start = threading.Barrier(THREADS)

        def produce(worker):
            lumen.set_thread_tag("worker", worker)
            start.wait()
            for i in range(RECORDS_PER_THREAD):
                lumen.info(f"{worker}:{i}")

        threads = [threading.Thread(target=produce, args=(w,)) for w in range(THREADS)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        logs = self.flushed_logs()
        self.assertEqual(sorted(r.message for r in logs),
                         sorted(f"{w}:{i}" for w in range(THREADS)
                                for i in range(RECORDS_PER_THREAD)))
        for record in logs:
            self.assertEqual(record.tags, {"worker": record.message.split(":")[0]})
        # Records of one thread keep their order.
        for w in range(THREADS):
            mine = [int(r.message.split(":")[1]) for r in logs if r.tags["worker"] == str(w)]
            self.assertEqual(mine, list(range(RECORDS_PER_THREAD)))

    def test_records_of_different_threads_carry_different_thread_ids(self):
        thread = threading.Thread(target=lumen.info, args=("other",))
        thread.start()
        thread.join()
        lumen.info("main")
        logs = self.flushed_logs()
        self.assertEqual([r.message for r in logs], ["other", "main"])
        self.assertNotEqual(logs[0].thread_id, logs[1].thread_id)


class SinkChurnTest(unittest.TestCase):
    def test_adding_removing_and_flushing_while_a_python_sink_receives_records_finishes(self):
        # A hang here (the GIL held while waiting for the dispatch thread)
        # fails through run_child's timeout instead of hanging the suite.
        result = run_child(self, """
            import sys
            import threading
            import time
            import lumen_bindings as lumen

            # Every add/remove gives the GIL away and waits to get it back;
            # a short switch interval only makes that wait shorter.
            sys.setswitchinterval(0.0001)

            received = []
            lumen.core().add_sink(lumen.CallbackSink(on_log=received.append))
            stop = threading.Event()

            # Paced: every record costs the dispatch thread a GIL hand-over,
            # and a producer that outruns the Python sink only fills the
            # queue, which flush() then has to drain.
            def produce():
                n = 0
                while not stop.is_set():
                    lumen.info("load")
                    time.sleep(0.0001)
                    n += 1
                    if n % 64 == 0:
                        received.clear()

            producer = threading.Thread(target=produce)
            producer.start()
            for i in range(20000):
                sink = lumen.CallbackSink(on_log=lambda r: None) if i % 2 else lumen.NullSink()
                lumen.core().remove_sink(lumen.core().add_sink(sink))
                if i % 100 == 0:
                    lumen.core().flush()
            stop.set()
            producer.join()
            lumen.core().flush()
            print("done", flush=True)
        """, timeout=120)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "done\n")


if __name__ == "__main__":
    unittest.main()
