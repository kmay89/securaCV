"""Unit tests for SecuraCVApi capability-token refresh.

The kernel rotates its capability token every 10-minute bucket and
rewrites the token file (src/api/mod.rs; the add-on writes it to
/config/api_token). These tests pin the client behavior that keeps the
Kernel HTTP mode alive across rotations:

  - On 401 with a token_file configured, the client re-reads the file
    and retries exactly once with the fresh token.
  - On 401 with no token_file (or an unchanged/unreadable file), the
    client raises SecuraCVApiAuthError without retrying.
  - 404 on /events/latest still maps to None (no events yet).
"""

from __future__ import annotations

import pytest

from . import conftest  # noqa: F401  (installs ha stubs at import time)
from .conftest import run

from .. import (
    SecuraCVApi,
    SecuraCVApiAuthError,
)


class _FakeResponse:
    def __init__(self, status: int, payload=None):
        self.status = status
        self._payload = payload

    async def json(self):
        return self._payload

    async def __aenter__(self):
        return self

    async def __aexit__(self, *exc):
        return False


class _FakeSession:
    """Returns queued responses; records the Authorization header of
    every request so tests can assert which token was presented."""

    def __init__(self, responses):
        self._responses = list(responses)
        self.auth_headers: list[str] = []

    def get(self, url, headers=None, timeout=None):
        self.auth_headers.append((headers or {}).get("Authorization", ""))
        return self._responses.pop(0)


def test_401_with_token_file_rereads_and_retries(tmp_path):
    token_file = tmp_path / "api_token"
    token_file.write_text("rotated-token\n")

    session = _FakeSession(
        [
            _FakeResponse(401),
            _FakeResponse(200, {"events": []}),
        ]
    )
    api = SecuraCVApi(
        "http://kernel:8799", "stale-token", session, token_file=str(token_file)
    )

    result = run(api.async_get_events())

    assert result == {"events": []}
    assert session.auth_headers == [
        "Bearer stale-token",
        "Bearer rotated-token",
    ]


def test_401_without_token_file_raises_immediately():
    session = _FakeSession([_FakeResponse(401)])
    api = SecuraCVApi("http://kernel:8799", "stale-token", session)

    with pytest.raises(SecuraCVApiAuthError):
        run(api.async_get_events())
    # No retry happened: only the single original request.
    assert session.auth_headers == ["Bearer stale-token"]


def test_401_with_unchanged_file_token_raises_without_retry(tmp_path):
    token_file = tmp_path / "api_token"
    token_file.write_text("stale-token")

    session = _FakeSession([_FakeResponse(401)])
    api = SecuraCVApi(
        "http://kernel:8799", "stale-token", session, token_file=str(token_file)
    )

    with pytest.raises(SecuraCVApiAuthError):
        run(api.async_get_events())
    assert session.auth_headers == ["Bearer stale-token"]


def test_401_twice_with_rotating_file_raises_after_one_retry(tmp_path):
    token_file = tmp_path / "api_token"
    token_file.write_text("also-stale")

    session = _FakeSession([_FakeResponse(401), _FakeResponse(401)])
    api = SecuraCVApi(
        "http://kernel:8799", "stale-token", session, token_file=str(token_file)
    )

    with pytest.raises(SecuraCVApiAuthError):
        run(api.async_get_events())
    assert session.auth_headers == [
        "Bearer stale-token",
        "Bearer also-stale",
    ]


def test_401_with_missing_file_raises_without_retry(tmp_path):
    session = _FakeSession([_FakeResponse(401)])
    api = SecuraCVApi(
        "http://kernel:8799",
        "stale-token",
        session,
        token_file=str(tmp_path / "does-not-exist"),
    )

    with pytest.raises(SecuraCVApiAuthError):
        run(api.async_get_events())
    assert session.auth_headers == ["Bearer stale-token"]


def test_latest_event_404_maps_to_none():
    session = _FakeSession([_FakeResponse(404)])
    api = SecuraCVApi("http://kernel:8799", "token", session)

    assert run(api.async_get_latest_event()) is None


def test_empty_token_primed_from_file_before_first_request(tmp_path):
    """Token-file-only setups must not burn a guaranteed 401 on startup."""
    token_file = tmp_path / "api_token"
    token_file.write_text("primed-token")

    session = _FakeSession([_FakeResponse(200, {"events": []})])
    api = SecuraCVApi(
        "http://kernel:8799", "", session, token_file=str(token_file)
    )

    assert run(api.async_get_events()) == {"events": []}
    assert session.auth_headers == ["Bearer primed-token"]


def test_stale_file_at_boot_recovers_after_kernel_rotates(tmp_path):
    """Restart across a rotation boundary: the primed token is stale, the
    kernel rotates + rewrites the file while rejecting it, and the single
    401-retry must pick up the fresh token."""
    token_file = tmp_path / "api_token"
    token_file.write_text("stale-from-last-bucket")

    class _RotatingSession(_FakeSession):
        def get(self, url, headers=None, timeout=None):
            resp = super().get(url, headers=headers, timeout=timeout)
            # Mimic the kernel rotating its bucket on the first request:
            # rejects the stale token but rewrites the token file.
            token_file.write_text("fresh-after-rotation")
            return resp

    session = _RotatingSession(
        [_FakeResponse(401), _FakeResponse(200, {"events": []})]
    )
    api = SecuraCVApi(
        "http://kernel:8799", "", session, token_file=str(token_file)
    )

    assert run(api.async_get_events()) == {"events": []}
    assert session.auth_headers == [
        "Bearer stale-from-last-bucket",
        "Bearer fresh-after-rotation",
    ]


def test_concurrent_refresh_waiter_reuses_already_loaded_token(tmp_path):
    """If another task already swapped the token while we waited on the
    lock, the waiter must retry with it instead of re-reading the file or
    raising."""
    api = SecuraCVApi(
        "http://kernel:8799",
        "old-token",
        _FakeSession([]),
        token_file=str(tmp_path / "unreadable-does-not-matter"),
    )

    async def scenario():
        # Simulate a concurrent task having refreshed first.
        api._token = "refreshed-by-peer"
        return await api._async_refresh_token("old-token")

    assert run(scenario()) is True


def test_oversized_token_file_is_capped(tmp_path):
    token_file = tmp_path / "api_token"
    token_file.write_text("x" * 100_000)

    api = SecuraCVApi(
        "http://kernel:8799", "", _FakeSession([]), token_file=str(token_file)
    )
    token = api._read_token_file()
    assert token is not None and len(token) == 4096


def test_latest_event_survives_rotation(tmp_path):
    token_file = tmp_path / "api_token"
    token_file.write_text("fresh")

    session = _FakeSession(
        [
            _FakeResponse(401),
            _FakeResponse(200, {"event_type": "presence_in_restricted_zone"}),
        ]
    )
    api = SecuraCVApi(
        "http://kernel:8799", "stale", session, token_file=str(token_file)
    )

    result = run(api.async_get_latest_event())
    assert result == {"event_type": "presence_in_restricted_zone"}
    assert session.auth_headers[-1] == "Bearer fresh"
