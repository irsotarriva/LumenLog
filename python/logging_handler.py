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
- tags, in this order: ``logger`` with the logger's name, ``logging.level``
  (above), then every ``extra`` field in the order given. A str, bool or
  float value is passed as it is, and an int too if it fits in int64 or
  uint64; anything else, a larger int included, as str(value). An ``extra``
  field named ``logger`` or ``logging.level`` (the handler's own tags, also
  on a record that has no ``logging.level`` tag), or named like an earlier
  tag, is passed with ``extra.`` put in front of its name, as many times as
  it takes to be new, so no value is lost and no field passes for the
  handler's own. A record holds at most 16 tags; the
  library drops the extra fields past that and counts them
  (Core.statistics().tags_dropped). The handler's own tags come first, so
  they are never the ones dropped.

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

# The tags the handler sets itself; an extra field never takes these names.
_HANDLER_TAGS = frozenset({"logger", "logging.level"})

# The ints a Lumen tag value takes as a number: int64 and uint64.
_SMALLEST_INT = -(2**63)
_LARGEST_INT = 2**64 - 1


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
    if isinstance(value, (str, bool, float)):
        return value
    if isinstance(value, int) and _SMALLEST_INT <= value <= _LARGEST_INT:
        return value
    return str(value)


def lumen_tags(record: logging.LogRecord) -> dict:
    """The tags a record is forwarded with (see the module docstring)."""
    tags = {"logger": record.name}
    if record.levelno not in _ONE_TO_ONE:
        tags["logging.level"] = record.levelname
    for key, value in vars(record).items():
        if key in _STANDARD_ATTRIBUTES:
            continue
        name = key if isinstance(key, str) else str(key)
        while name in tags or name in _HANDLER_TAGS:
            name = "extra." + name
        tags[name] = _tag_value(value)
    return tags


class LumenHandler(logging.Handler):
    """Forwards logging records to Lumen's core (see the module docstring)."""

    def emit(self, record: logging.LogRecord) -> None:
        try:
            source = (record.pathname, record.lineno, record.funcName or "")
            lumen.log(lumen_level(record.levelno), self.format(record),
                      tags=lumen_tags(record), source=source)
        except Exception:
            self.handleError(record)
