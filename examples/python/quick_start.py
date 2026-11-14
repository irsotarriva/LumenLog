"""Lumen from Python: a sink written in Python, a library sink, tags, a scope,
a metric, a progress bar, an error, and the standard logging module forwarded
to Lumen.

Needs the module built with LUMEN_ENABLE_PYTHON=ON. Run it with the directory
of lumen_bindings and the repository's python/ directory (for
logging_handler) on PYTHONPATH.
"""

import logging

import lumen_bindings as lumen
from logging_handler import LumenHandler


class Printer(lumen.Sink):
    """Prints each record on stdout. Called on the library's dispatch thread."""

    def on_log(self, record):
        tags = " ".join(f"{key}={value}" for key, value in record.tags.items())
        print(f"{record.level.name} {record.message} [{tags}] in {record.source.function}()")

    def on_metric(self, record):
        print(f"METRIC {record.name}={record.value} {record.tags}")

    def on_progress(self, record):
        print(f"PROGRESS {record.label} {record.current}/{record.total}")


def main():
    core = lumen.core()
    core.add_sink(Printer(), lumen.level_at_least(lumen.LogLevel.INFO))

    # A library sink, on stderr, for warnings and above.
    config = lumen.TerminalConfig()
    config.time_format = ""
    config.inline_tags = ["app", "phase"]
    core.add_sink(lumen.TerminalSink(config), lumen.parse_predicate("level >= WARN"))

    lumen.set_process_tag("app", "python_example")
    lumen.debug("below the Printer's level")
    with lumen.scope("phase", "setup"):
        lumen.info("configured", tags={"workers": 4})
        lumen.warn("disk almost full", tags={"free_gb": 1.5})
    lumen.metric("loss", 0.25, tags={"epoch": 3})

    bar = lumen.progress("files", 2)
    bar.tick()
    bar.tick()

    try:
        lumen.parse_predicate("level >= LOUD")
    except lumen.InvalidPredicateError as error:
        lumen.error(f"bad filter: {error}")

    logger = logging.getLogger("trainer")
    logger.setLevel(logging.INFO)
    logger.addHandler(LumenHandler())
    logger.info("epoch done", extra={"epoch": 3})

    core.flush()


if __name__ == "__main__":
    main()
