"""A logging.Handler that forwards records of the standard logging module to Lumen.

Usage::

    import logging
    from logging_handler import LumenHandler

    logging.getLogger().addHandler(LumenHandler())
    logging.getLogger("trainer").info("epoch done", extra={"epoch": 3})

Each logging.LogRecord becomes one Lumen log record:

- message: the handler's formatted text (by default the message, plus the
  traceback when there is exc_info);
- level: the highest Lumen level at or below the record's level, so DEBUG,
  INFO, WARNING and ERROR map to DEBUG, INFO, WARN and ERROR, anything below
  DEBUG to TRACE, and CRITICAL to ERROR. CRITICAL is not mapped to FATAL,
  because lumen.fatal() terminates the process, which a logging call must
  never do. A record whose level is not one of the four standard levels
  that map one to one (CRITICAL, or a custom level) also gets the tag
  ``logging.level`` with its level name, so nothing is lost;
- source: the record's pathname, lineno and funcName (the logging call site);
- tags: ``logger`` with the logger's name, and every ``extra`` field. Values
  that are str, bool, int or float are passed as they are, anything else as
  str(value). A record holds at most 16 tags; the library drops and counts
  the rest (Core.statistics().tags_dropped).

An error while forwarding goes to logging's own handleError(), like any
other handler's.
"""

import logging

import lumen_bindings as lumen

# Attributes every LogRecord has; anything else on a record came from `extra`.
_STANDARD_ATTRIBUTES = frozenset(
    vars(logging.LogRecord("", logging.INFO, "", 0, "", None, None))
) | {"message", "asctime"}

_ONE_TO_ONE = frozenset({logging.DEBUG, logging.INFO, logging.WARNING, logging.ERROR})


def lumen_level(levelno: int) -> lumen.LogLevel:
    """The Lumen level a logging level maps to (see the module docstring)."""
    if levelno >= logging.ERROR:
        return lumen.LogLevel.ERROR
    if levelno >= logging.WARNING:
        return lumen.LogLevel.WARN
    if levelno >= logging.INFO:
        return lumen.LogLevel.INFO
    if levelno >= logging.DEBUG:
        return lumen.LogLevel.DEBUG
    return lumen.LogLevel.TRACE


def _tag_value(value):
    if isinstance(value, (str, bool, int, float)):
        return value
    return str(value)


class LumenHandler(logging.Handler):
    """Forwards logging records to Lumen's core (see the module docstring)."""

    def emit(self, record: logging.LogRecord) -> None:
        try:
            tags = {
                key: _tag_value(value)
                for key, value in vars(record).items()
                if key not in _STANDARD_ATTRIBUTES
            }
            tags["logger"] = record.name
            if record.levelno not in _ONE_TO_ONE:
                tags["logging.level"] = record.levelname
            source = (record.pathname, record.lineno, record.funcName or "")
            lumen.log(lumen_level(record.levelno), self.format(record), tags=tags, source=source)
        except Exception:
            self.handleError(record)
