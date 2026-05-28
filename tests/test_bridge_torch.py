#!/usr/bin/env python3
"""Integration tests for the PyTorch bridge.

Run with: python tests/test_bridge_torch.py

Requires torch and the lumen_bindings module available.
"""

import sys

try:
    import torch
except ImportError:
    print("SKIP: torch not available")
    sys.exit(0)

try:
    import lumen_bindings as lumen
except ImportError:
    print("SKIP: lumen_bindings module not available (build with LUMEN_ENABLE_PYTHON=ON)")
    sys.exit(0)

import time
import unittest


class TestTorchBridge(unittest.TestCase):
    def setUp(self):
        self.captured = []

        def on_metric(rec):
            self.captured.append(rec)

        sink = lumen.CallbackSink(on_metric=on_metric)
        self.__sink_id = lumen.core().add_sink(sink, lumen.always())

    def tearDown(self):
        lumen.core().remove_sink(self.__sink_id)

    def test_metric_name_value_roundtrip(self):
        lumen.metric("torch.loss", 0.123)
        lumen.flush()
        time.sleep(0.05)

        self.assertGreaterEqual(len(self.captured), 1)
        rec = self.captured[-1]
        self.assertEqual(rec.name, "torch.loss")
        self.assertAlmostEqual(rec.value, 0.123, places=5)


if __name__ == "__main__":
    unittest.main()
