"""Python logging.Handler that forwards log records to Lumen's C++ broker.

Each log record is forwarded to the appropriate Lumen level function
(info, warn, error, debug).

Usage::

    import lumen_bindings
    from logging_handler import LumenHandler

    handler = LumenHandler()
    logging.getLogger().addHandler(handler)
    logging.info("hello from python")
"""

import logging

_LEVEL_MAP = {
    logging.DEBUG: "debug",
    logging.INFO: "info",
    logging.WARNING: "warn",
    logging.ERROR: "error",
    logging.CRITICAL: "error",
}


class LumenHandler(logging.Handler):
    def __init__(self, level: int = logging.NOTSET):
        super().__init__(level)

    def emit(self, record: logging.LogRecord) -> None:
        try:
            import lumen_bindings as lumen

            msg = self.format(record)
            method_name = _LEVEL_MAP.get(record.levelno, "info")
            method = getattr(lumen, method_name, lumen.info)
            method(msg)

        except Exception:
            self.handleError(record)
