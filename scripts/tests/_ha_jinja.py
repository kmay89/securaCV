"""Home Assistant's template environment, as far as the template tests need it.

Home Assistant renders every template (an MQTT discovery value_template, an
automation's message, a markdown card) in its TemplateEnvironment
(homeassistant/helpers/template.py, read from the 2025.4.4 wheel, which pins
Jinja2==3.1.6): an ImmutableSandboxedEnvironment whose undefined is
LoggingUndefined, which logs a warning whenever an undefined value is
printed, iterated or tested, and which replaces three of jinja2's own
filters with its forgiving ones:

  round  forgiving_round: rounds as jinja2 does, but returns an int at
         precision 0, so `17.0 | round(0)` prints "17" where plain jinja2
         prints "17.0"
  int    forgiving_int_filter: jinja2's do_int, but input it cannot convert
         is a template error unless a default is given (jinja2 returns 0)
  float  forgiving_float_filter: likewise for float

A test that renders with plain jinja2 holds a template to what jinja2
prints, not to what Home Assistant prints, so the tests in this directory
render through environment() below. `default`, `count`, `is defined` and
the rest are jinja2's own in Home Assistant too.

The three filters are copied from that file's logic, not imported:
Home Assistant is not installed in the jobs that run these tests, jinja2 is
(lint.yml installs it at Home Assistant's pin).
"""

from __future__ import annotations

import math

import jinja2
import jinja2.filters
from jinja2.sandbox import ImmutableSandboxedEnvironment

_SENTINEL = object()


def _no_default(function: str, value) -> None:
    # helpers/template.py raise_no_default: a ValueError the render surfaces
    raise ValueError(f"Template error: {function} got invalid input '{value}' "
                     "but no default was specified")


def forgiving_round(value, precision=0, method="common", default=_SENTINEL):
    try:
        multiplier = float(10**precision)
        if method == "ceil":
            value = math.ceil(float(value) * multiplier) / multiplier
        elif method == "floor":
            value = math.floor(float(value) * multiplier) / multiplier
        elif method == "half":
            value = round(float(value) * 2) / 2
        else:
            value = round(float(value), precision)
        return int(value) if precision == 0 else value
    except (ValueError, TypeError):
        if default is _SENTINEL:
            _no_default("round", value)
        return default


def forgiving_int_filter(value, default=_SENTINEL, base=10):
    result = jinja2.filters.do_int(value, default=default, base=base)
    if result is _SENTINEL:
        _no_default("int", value)
    return result


def forgiving_float_filter(value, default=_SENTINEL):
    try:
        return float(value)
    except (ValueError, TypeError):
        if default is _SENTINEL:
            _no_default("float", value)
        return default


def environment(warnings: list[str] | None = None) -> ImmutableSandboxedEnvironment:
    """A sandboxed environment with Home Assistant's undefined and filters.
    Every warning its LoggingUndefined would log is appended to `warnings`."""
    log = warnings if warnings is not None else []

    class LoggingUndefined(jinja2.Undefined):
        def __str__(self):
            log.append(self._undefined_message)
            return super().__str__()

        def __iter__(self):
            log.append(self._undefined_message)
            return super().__iter__()

        def __bool__(self):
            log.append(self._undefined_message)
            return super().__bool__()

    env = ImmutableSandboxedEnvironment(undefined=LoggingUndefined)
    env.filters["round"] = forgiving_round
    env.filters["int"] = forgiving_int_filter
    env.filters["float"] = forgiving_float_filter
    return env
