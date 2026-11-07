"""Every kernel answer the client decodes is read under a byte cap.

The bundle download was always capped (`EXPORT_BUNDLE_MAX_BYTES`); the JSON
reads — identity, status, events, the sealed log, a pairing answer, a
refusal body — used to be `resp.json()` with no bound, so whatever answered
at the kernel's URL could make Home Assistant buffer an unbounded body.
Now `API_JSON_MAX_BYTES` holds all of them: a declared Content-Length over
the cap is refused before a byte is read, and a chunked body is refused
the moment it crosses the cap, having read at most cap + 1 bytes.
"""

from __future__ import annotations

import json

import pytest

from . import conftest  # noqa: F401  (installs ha stubs at import time)
from .conftest import run

from ..api import (
    SecuraCVApi,
    SecuraCVApiResponseError,
    async_pair_kernel,
)
from ..const import API_JSON_MAX_BYTES


class _Stream:
    """An aiohttp-style StreamReader: `read(n)` serves at most n bytes."""

    def __init__(self, body: bytes) -> None:
        self._body = body
        self.served = 0

    async def read(self, n: int = -1) -> bytes:
        chunk = self._body[self.served : self.served + n] if n >= 0 else self._body[self.served :]
        self.served += len(chunk)
        return chunk


class _Resp:
    def __init__(self, status: int, body: bytes, *, content_length: int | None = None) -> None:
        self.status = status
        self.content = _Stream(body)
        self.content_length = content_length

    async def __aenter__(self):
        return self

    async def __aexit__(self, *exc):
        return False


class _Session:
    def __init__(self, *responses: _Resp) -> None:
        self._responses = list(responses)

    def request(self, method, url, headers=None, timeout=None, **kwargs):
        return self._responses.pop(0)

    def post(self, url, json=None, timeout=None):
        return self._responses.pop(0)


def _api(*responses: _Resp) -> SecuraCVApi:
    return SecuraCVApi("http://kernel:8799", "tok", _Session(*responses))


def test_a_declared_length_over_the_cap_is_refused_before_a_byte_is_read():
    resp = _Resp(200, b"{}", content_length=API_JSON_MAX_BYTES + 1)
    with pytest.raises(SecuraCVApiResponseError, match="too large"):
        run(_api(resp).async_get_identity())
    assert resp.content.served == 0


def test_a_chunked_body_over_the_cap_is_refused_having_read_at_most_cap_plus_one():
    body = b'{"events": ["' + b"x" * (API_JSON_MAX_BYTES + 4096) + b'"]}'
    resp = _Resp(200, body)
    with pytest.raises(SecuraCVApiResponseError, match="too large"):
        run(_api(resp).async_get_events())
    assert resp.content.served == API_JSON_MAX_BYTES + 1


def test_a_body_within_the_cap_is_decoded_as_before():
    payload = {"device_id": "pwk_1a2b3c4d", "verifying_key": "ab" * 32}
    body = json.dumps(payload).encode()
    assert run(_api(_Resp(200, body, content_length=len(body))).async_get_identity()) == payload
    # Empty and non-object answers map to {} / None the way they always did.
    assert run(_api(_Resp(200, b"")).async_get_events()) == {}
    assert run(_api(_Resp(200, b"[1, 2]")).async_get_events()) == {}


def test_a_body_that_is_not_json_is_a_response_error():
    with pytest.raises(SecuraCVApiResponseError, match="invalid JSON"):
        run(_api(_Resp(200, b"<html>login</html>")).async_get_status())


def test_the_pair_answer_and_a_refusal_body_are_held_to_the_same_cap():
    too_big = b'{"token": "' + b"a" * (API_JSON_MAX_BYTES + 1) + b'"}'
    with pytest.raises(SecuraCVApiResponseError, match="too large"):
        run(async_pair_kernel(_Session(_Resp(200, too_big)), "http://kernel:8799", "123456"))
    # A 403 whose refusal body is oversized is read as "not the limit word":
    # a plain response error, never a buffered body.
    refusal = _Resp(403, b'{"error": "' + b"b" * (API_JSON_MAX_BYTES + 1) + b'"}')
    with pytest.raises(SecuraCVApiResponseError, match="unexpected status: 403"):
        run(async_pair_kernel(_Session(refusal), "http://kernel:8799", "123456"))
    assert refusal.content.served <= API_JSON_MAX_BYTES + 1
