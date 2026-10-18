"""securacv.staged_update — one Canary at a time, abort on the first failure.

Drives rollout.py with a fake hass whose update entities flip to the new
version after a few polls (or never), a fake clock and a fake sleep, so a
ten-minute timeout runs in microseconds. docs/OTA.md §5.
"""

from __future__ import annotations

import types
from typing import Any

from . import conftest  # noqa: F401  (installs the base HA stubs)
from .conftest import run

from .. import rollout, services  # noqa: E402


class _State:
    def __init__(self, state: str, **attributes: Any) -> None:
        self.state = state
        self.attributes = dict(attributes)


class _FakeHass:
    """States that change on a schedule, and a service recorder.

    ``flip_after[entity]`` = how many install polls until the entity reports
    the new version; None = never (the node that bricks or hangs).
    """

    def __init__(self, states: dict[str, _State], flip_after: dict[str, int | None]) -> None:
        self._states = states
        self._flip_after = flip_after
        self.calls: list[tuple[str, str, dict[str, Any]]] = []
        self._polls: dict[str, int] = {}
        self.clock = 0.0
        self.states = types.SimpleNamespace(get=self._get)
        self.services = types.SimpleNamespace(async_call=self._call)

    def _get(self, entity_id: str) -> _State | None:
        return self._states.get(entity_id)

    async def _call(self, domain: str, service: str, data: dict[str, Any], blocking: bool = False) -> None:
        self.calls.append((domain, service, dict(data)))
        self._polls[data["entity_id"]] = 0

    async def sleep(self, seconds: float) -> None:
        self.clock += seconds
        for entity_id, remaining in list(self._polls.items()):
            self._polls[entity_id] = remaining + 1
            after = self._flip_after.get(entity_id)
            if after is not None and self._polls[entity_id] >= after:
                st = self._states[entity_id]
                st.attributes["installed_version"] = st.attributes["latest_version"]
                st.attributes["in_progress"] = False
                st.state = "off"

    def now(self) -> float:
        return self.clock


def _fleet() -> dict[str, _State]:
    return {
        "update.hallway_firmware": _State("on", installed_version="2.4.16", latest_version="2.5.0"),
        "update.porch_firmware": _State("on", installed_version="2.4.16", latest_version="2.5.0"),
        "update.bedside_firmware": _State("off", installed_version="2.5.0", latest_version="2.5.0"),
        "update.shed_firmware": _State("on", installed_version="2.4.16", latest_version="2.5.0"),
    }


def test_plan_installs_only_nodes_that_offer_an_update() -> None:
    states = _fleet()
    states["update.mute_firmware"] = _State("on", installed_version="1.0.0")  # no latest: not pushed blind
    states["update.gone_firmware"] = _State("unavailable")
    to_install, skipped = rollout.plan(states.get, sorted(states))
    assert to_install == ["update.hallway_firmware", "update.porch_firmware", "update.shed_firmware"]
    assert skipped == ["update.bedside_firmware", "update.gone_firmware", "update.mute_firmware"]
    assert rollout.plan(states.get, ["update.nothing"]) == ([], ["update.nothing"])


def test_a_rollout_installs_one_node_at_a_time_and_waits_for_each() -> None:
    hass = _FakeHass(_fleet(), {"update.hallway_firmware": 2, "update.porch_firmware": 3, "update.shed_firmware": 1})
    result = run(rollout.async_run(
        hass, sorted(_fleet()), node_timeout_s=600, poll_s=5, clock=hass.now, sleep=hass.sleep))
    assert result.installed == ["update.hallway_firmware", "update.porch_firmware", "update.shed_firmware"]
    assert result.skipped == ["update.bedside_firmware"]
    assert result.failed == [] and not result.aborted and result.reason is None
    # One install call per node, in order, each after the previous reported.
    assert [c[2]["entity_id"] for c in hass.calls] == result.installed
    assert all(c[:2] == ("update", "install") for c in hass.calls)
    assert hass.clock == (2 + 3 + 1) * 5


def test_the_first_node_that_never_reports_aborts_the_rest() -> None:
    hass = _FakeHass(_fleet(), {"update.hallway_firmware": 1, "update.porch_firmware": None})
    result = run(rollout.async_run(
        hass, sorted(_fleet()), node_timeout_s=600, poll_s=5, clock=hass.now, sleep=hass.sleep))
    assert result.installed == ["update.hallway_firmware"]
    assert result.failed == ["update.porch_firmware"]
    assert result.not_attempted == ["update.shed_firmware"]
    assert result.aborted and "update.porch_firmware" in str(result.reason) and "600 s" in str(result.reason)
    assert [c[2]["entity_id"] for c in hass.calls] == ["update.hallway_firmware", "update.porch_firmware"]
    assert result.as_dict()["aborted"] is True


def test_a_node_still_in_progress_is_not_done() -> None:
    st = _State("on", installed_version="2.5.0", latest_version="2.5.0", in_progress=True)
    assert not rollout.node_done(st, "2.5.0")
    st.attributes["in_progress"] = False
    st.state = "off"
    assert rollout.node_done(st, "2.5.0")
    assert not rollout.node_done(None, "2.5.0")


def test_the_action_is_registered_with_the_others() -> None:
    assert services.SERVICE_STAGED_UPDATE in services.SERVICES
    # The field validator (the stub voluptuous never runs schemas, so it is
    # called directly): one id or a list, a list out, nothing else.
    assert services._entity_ids("update.hallway_firmware") == ["update.hallway_firmware"]
    assert services._entity_ids(["update.a", "update.b"]) == ["update.a", "update.b"]
    for bad in ([], [""], [1]):
        try:
            services._entity_ids(bad)
        except ValueError:
            pass
        else:
            raise AssertionError(f"{bad!r} accepted")
    assert rollout.MIN_NODE_TIMEOUT_S <= rollout.DEFAULT_NODE_TIMEOUT_S <= rollout.MAX_NODE_TIMEOUT_S
    for key in ("rollout_no_update_entities", "rollout_not_update_entities", "rollout_aborted"):
        assert key in services.REFUSALS
