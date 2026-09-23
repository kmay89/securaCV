"""Never advertise unbuilt: FUTURE_* stays out of the ALL_* sets, in-package.

docs/feature-flags.md rule 2 says a declared-but-unimplemented capability
lives in a ``FUTURE_*`` list and must not appear in the corresponding
``ALL_*`` list until it is wired end-to-end. ``scripts/lint_feature_flags.sh``
(check B) greps ``const.py`` for that; this file is the same rule as a unit
test beside the constants, so it fails where the code is edited rather than
in a shell script three directories away — and it covers the half the shell
lint originally did not: FUTURE_TAMPER_TYPES had no gate at all, because the
per-type tamper list in binary_sensor.py was an inline literal with no
ALL_TAMPER_TYPES to compare against.

Four properties, per list pair:

  1. disjoint — no FUTURE_* member is advertised;
  2. complete — every ``TRANSPORT_*`` / ``TAMPER_*`` string constant in
     const.py is in exactly one of the two lists (a new constant cannot land
     in neither and quietly become un-gated);
  3. no duplicates in either list (entity creation iterates them);
  4. the binary_sensor.py per-type tables are keyed by exactly the advertised
     list — a row for a FUTURE_* type would be dead weight, and an advertised
     type without a row would KeyError at discovery time.
"""

from __future__ import annotations

from . import conftest  # noqa: F401  (installs the base HA stubs)

# Reuse the platform stubs the hardening tests install (idempotent).
from .test_mqtt_payload_hardening import _install_platform_stubs

_install_platform_stubs()

from .. import binary_sensor as bs_platform  # noqa: E402
from .. import const  # noqa: E402
from ..const import (  # noqa: E402
    ALL_TAMPER_TYPES,
    ALL_TRANSPORTS,
    FUTURE_TAMPER_TYPES,
    FUTURE_TRANSPORTS,
)


def _declared(prefix: str) -> set[str]:
    """Every string constant in const.py named ``<prefix>_*``."""
    return {
        value
        for name, value in vars(const).items()
        if name.startswith(prefix) and isinstance(value, str)
    }


# ─── transports ───────────────────────────────────────────────────────


def test_future_transports_are_not_advertised() -> None:
    assert set(FUTURE_TRANSPORTS).isdisjoint(ALL_TRANSPORTS), (
        "a FUTURE_* transport is in ALL_TRANSPORTS — devices must never "
        "advertise a transport no firmware can report on"
    )


def test_every_transport_constant_is_in_exactly_one_list() -> None:
    declared = _declared("TRANSPORT_")
    assert declared, "no TRANSPORT_* constants found — did const.py move?"
    assert set(ALL_TRANSPORTS) | set(FUTURE_TRANSPORTS) == declared, (
        "a TRANSPORT_* constant is in neither ALL_TRANSPORTS nor "
        "FUTURE_TRANSPORTS (or a list names something that is not a constant)"
    )


def test_transport_lists_have_no_duplicates() -> None:
    assert len(ALL_TRANSPORTS) == len(set(ALL_TRANSPORTS))
    assert len(FUTURE_TRANSPORTS) == len(set(FUTURE_TRANSPORTS))


# ─── tamper types ─────────────────────────────────────────────────────


def test_future_tamper_types_are_not_advertised() -> None:
    assert set(FUTURE_TAMPER_TYPES).isdisjoint(ALL_TAMPER_TYPES), (
        "a FUTURE_* tamper type is in ALL_TAMPER_TYPES — the integration must "
        "never advertise a tamper type no device can raise"
    )


def test_every_tamper_constant_is_in_exactly_one_list() -> None:
    declared = _declared("TAMPER_")
    assert declared, "no TAMPER_* constants found — did const.py move?"
    assert set(ALL_TAMPER_TYPES) | set(FUTURE_TAMPER_TYPES) == declared, (
        "a TAMPER_* constant is in neither ALL_TAMPER_TYPES nor "
        "FUTURE_TAMPER_TYPES (or a list names something that is not a constant)"
    )


def test_tamper_lists_have_no_duplicates() -> None:
    assert len(ALL_TAMPER_TYPES) == len(set(ALL_TAMPER_TYPES))
    assert len(FUTURE_TAMPER_TYPES) == len(set(FUTURE_TAMPER_TYPES))


# ─── the entity tables are keyed by the advertised lists ──────────────


def test_tamper_sensor_table_matches_the_advertised_list() -> None:
    assert set(bs_platform.TAMPER_TYPE_SENSORS) == set(ALL_TAMPER_TYPES), (
        "binary_sensor.TAMPER_TYPE_SENSORS must have exactly one row per "
        "ALL_TAMPER_TYPES entry: a missing row KeyErrors at discovery, a row "
        "for a FUTURE_* type advertises what no firmware emits"
    )


def test_transport_sensor_table_matches_the_advertised_list() -> None:
    assert set(bs_platform.TRANSPORT_SENSORS) == set(ALL_TRANSPORTS), (
        "binary_sensor.TRANSPORT_SENSORS must have exactly one row per "
        "ALL_TRANSPORTS entry"
    )
