"""The HTTP client for the Privacy Witness Kernel's event API.

Three credentials reach the kernel, and the client treats them alike on the
wire (`Authorization: Bearer <token>`):

  - the rotating capability token read from a file the kernel rewrites every
    ten-minute bucket (`token_file`; the app writes /config/api_token) — on a
    401 the client re-reads the file once and retries;
  - a long-lived paired token from POST /api/pair (`paired`), revocable by id;
  - a pasted capability token (`static`), which goes stale within minutes.

Every error is one of the three `SecuraCVApi*Error` classes so the
coordinator and the config flow decide what to do without reading status
codes. Nothing here is ever logged at a level that could carry a token.
"""
from __future__ import annotations

import asyncio
import logging
import re
import time
from dataclasses import dataclass
from typing import Any

import aiohttp

from .const import EXPORT_BUNDLE_MAX_BYTES, PAIRED_TOKEN_LABEL

_LOGGER = logging.getLogger(__name__)

# Seconds per request. Built into a ClientTimeout at call time (never at import).
REQUEST_TIMEOUT_SECONDS = 10
# The token is 64 hex chars; cap the read so a misconfigured path (device
# node, huge file) can't balloon memory.
TOKEN_FILE_READ_CAP = 4096
# The kernel names a bundle download `securacv-events-<bucket start>.json`
# (src/api/mod.rs); anything else in the header is not a filename we keep.
BUNDLE_FILENAME_RE = re.compile(r'filename="?(securacv-events-\d+\.json)"?')


class SecuraCVApiError(Exception):
    """Base error for the SecuraCV API client."""


class SecuraCVApiAuthError(SecuraCVApiError):
    """The kernel refused the credential (401)."""


class SecuraCVApiConnectionError(SecuraCVApiError):
    """The kernel could not be reached."""


class SecuraCVApiResponseError(SecuraCVApiError):
    """The kernel answered with something other than the contract."""


class SecuraCVApiPairLimitError(SecuraCVApiResponseError):
    """`POST /api/pair` answered 403 `paired_token_limit`: the kernel already
    holds its limit of paired tokens. The code was NOT consumed — the
    operator revokes one (`docker exec securacv unpair <id>`, `witness_api
    revoke-paired-token <id>`) and the same code pairs."""


@dataclass(frozen=True)
class PairResult:
    """What POST /api/pair hands back on success (contract C2)."""

    token: str
    token_id: str
    device_id: str
    verifying_key: str
    kernel_version: str


@dataclass(frozen=True)
class BundleDownload:
    """GET /export/bundle: the kernel's filename and the bytes, verbatim."""

    filename: str
    content: bytes


def bundle_filename(content_disposition: str | None, fallback: str) -> str:
    """The kernel's filename from a Content-Disposition header, else `fallback`."""
    if content_disposition:
        match = BUNDLE_FILENAME_RE.search(content_disposition)
        if match:
            return match.group(1)
    return fallback


class SecuraCVApi:
    """API client for the SecuraCV Privacy Witness Kernel Event API."""

    def __init__(
        self,
        base_url: str,
        token: str,
        session: aiohttp.ClientSession,
        token_file: str | None = None,
    ) -> None:
        """Initialize the API client."""
        self._base_url = base_url.rstrip("/")
        self._token = token
        self._session = session
        self._token_file = token_file
        self._refresh_lock = asyncio.Lock()
        # True after a token-file read failed (missing, unreadable). The
        # coordinator reads it to tell "the kernel rotated and the file will
        # catch up" from "nothing can ever authenticate this entry again".
        self._token_file_unreadable = False

    @property
    def base_url(self) -> str:
        """The kernel URL this client talks to."""
        return self._base_url

    @property
    def token_file(self) -> str | None:
        """The rotating-token file path, when one is configured."""
        return self._token_file

    @property
    def token_file_unreadable(self) -> bool:
        """Whether the last token-file read failed."""
        return self._token_file_unreadable

    def _read_token_file(self) -> str | None:
        """Blocking read of the token file (run in executor)."""
        if not self._token_file:
            return None
        try:
            with open(self._token_file, encoding="utf-8") as fh:
                token = fh.read(TOKEN_FILE_READ_CAP).strip()
        except OSError as err:
            _LOGGER.debug("could not read token file %s: %s", self._token_file, err)
            self._token_file_unreadable = True
            return None
        self._token_file_unreadable = False
        return token or None

    async def _async_refresh_token(self, current_token: str) -> bool:
        """Re-read the token file; True if a token differing from
        `current_token` is now in place (whether loaded here or by a
        concurrent task that refreshed first).

        The kernel rotates its capability token every 10-minute bucket and
        rewrites the token file (src/api/mod.rs, privacy_witness_kernel/run.sh
        writes it to /config/api_token). A statically configured token
        therefore goes stale within minutes; when a token file is configured
        we re-read it on 401 so the integration follows the rotation.
        """
        if not self._token_file:
            return False
        async with self._refresh_lock:
            if self._token != current_token:
                # Another task already refreshed while we waited; retry
                # with what it loaded instead of re-reading the file.
                return True
            loop = asyncio.get_running_loop()
            token = await loop.run_in_executor(None, self._read_token_file)
            if token is None or token == current_token:
                return False
            self._token = token
            return True

    async def _async_request_json(
        self,
        method: str,
        path: str,
        *,
        none_on_404: bool = False,
        authenticated: bool = True,
    ) -> dict[str, Any] | None:
        """Send one request, retrying once with a re-read token on 401."""

        async def _read_json(resp: Any) -> dict[str, Any]:
            try:
                data = await resp.json()
            except aiohttp.ContentTypeError as err:
                raise SecuraCVApiResponseError(f"invalid JSON response: {err}") from err
            return data if isinstance(data, dict) else {}

        return await self._async_request(
            method, path, _read_json, none_on_404=none_on_404, authenticated=authenticated
        )

    async def _async_request(
        self,
        method: str,
        path: str,
        read: Any,
        *,
        none_on_404: bool = False,
        authenticated: bool = True,
    ) -> Any:
        """Send one request and hand the 200 response to `read`.

        Retries once with a re-read token on 401 (the rotating token file).
        `read` is an async callable over the aiohttp response — JSON for the
        state endpoints, the raw bytes for a bundle download — so the retry
        and the error mapping live here once.
        """
        # Token-file-only setups start with an empty token; prime it from the
        # file so the first request isn't a guaranteed 401 round-trip (and so
        # a restart across a rotation boundary still has the one 401-retry in
        # reserve for a stale file).
        if authenticated and not self._token and self._token_file:
            await self._async_refresh_token("")
        url = f"{self._base_url}{path}"
        for attempt in (0, 1):
            current_token = self._token
            headers = {"Authorization": f"Bearer {current_token}"} if authenticated else {}
            try:
                async with self._session.request(
                    method, url, headers=headers, timeout=aiohttp.ClientTimeout(total=REQUEST_TIMEOUT_SECONDS)
                ) as resp:
                    if resp.status == 401:
                        if (
                            authenticated
                            and attempt == 0
                            and await self._async_refresh_token(current_token)
                        ):
                            continue
                        raise SecuraCVApiAuthError("unauthorized")
                    if none_on_404 and resp.status == 404:
                        return None
                    if resp.status != 200:
                        raise SecuraCVApiResponseError(
                            f"unexpected status: {resp.status}"
                        )
                    return await read(resp)
            except (aiohttp.ClientError, asyncio.TimeoutError) as err:
                raise SecuraCVApiConnectionError("unable to reach API") from err
        raise SecuraCVApiAuthError("unauthorized")

    async def _async_get_json(
        self, path: str, *, none_on_404: bool = False, authenticated: bool = True
    ) -> dict[str, Any] | None:
        """GET a kernel endpoint, retrying once with a re-read token on 401."""
        return await self._async_request_json(
            "GET", path, none_on_404=none_on_404, authenticated=authenticated
        )

    async def async_get_events(self) -> dict[str, Any]:
        """Fetch events from the kernel Event API."""
        data = await self._async_get_json("/events")
        return data if data is not None else {}

    async def async_get_latest_event(self) -> dict[str, Any] | None:
        """Fetch the latest event from the kernel."""
        return await self._async_get_json("/events/latest", none_on_404=True)

    async def async_get_status(self) -> dict[str, Any] | None:
        """Fetch the kernel's storage endurance & health report.

        Returns None when the kernel predates the /status endpoint or has
        storage-health monitoring disabled (404), so storage sensors stay
        empty instead of erroring on older kernels.
        """
        return await self._async_get_json("/status", none_on_404=True)

    async def async_get_digest(self) -> dict[str, Any] | None:
        """Fetch the daily digest (the bridge publishes the same document)."""
        return await self._async_get_json("/digest", none_on_404=True)

    async def async_get_health(self) -> dict[str, Any]:
        """Fetch the unauthenticated liveness probe."""
        data = await self._async_get_json("/health", authenticated=False)
        return data if data is not None else {}

    async def async_get_identity(self) -> dict[str, Any] | None:
        """Fetch GET /api/identity (contract C4): device id, verifying key,
        kernel version, chain head, the kernel's own last verify.

        None on a kernel that predates the endpoint (404), so an older app
        keeps working with the URL-keyed device until it is updated.
        """
        return await self._async_get_json("/api/identity", none_on_404=True)

    async def async_revoke_paired_token(self, token_id: str) -> None:
        """DELETE /api/pair/<id>: a paired token may revoke itself.

        A 404 is "already gone" — the operator ran `unpair`, or the kernel
        is the app (token-file mode, pairing off) and never held the id —
        and is not an error; a 403 (`forbidden`: a paired bearer naming
        another id) still raises, as it should.
        """
        await self._async_request_json("DELETE", f"/api/pair/{token_id}", none_on_404=True)

    async def async_get_sealed_log(self, from_seq: int | None = None) -> dict[str, Any]:
        """GET /api/sealed-log[?from_seq=<n>] (contract C4 as amended).

        The checkpoint-anchored sealed-log document: the verifying key, the
        anchor, the entries exactly as stored (each with its `seq`), and the
        chain head. With `from_seq` only entries newer than that row are
        served, so a poller that remembers its last verified head downloads
        the increment. Receipt-less on the kernel (a state read).
        """
        path = "/api/sealed-log"
        if from_seq is not None:
            path = f"{path}?from_seq={int(from_seq)}"
        data = await self._async_request_json("GET", path)
        return data if data is not None else {}

    async def async_download_export_bundle(self, window_hours: int) -> BundleDownload:
        """GET /export/bundle?last=<hours>h: the signed bundle, verbatim.

        The bytes are kept exactly as the kernel sent them (the receipt
        signs the artifact hash; a re-serialization would break offline
        verification) and the kernel's filename is kept from the
        Content-Disposition header. The download is capped so a misbehaving
        kernel cannot balloon memory. This is the one receipted read the
        integration makes (a disclosure act, D5).
        """
        hours = int(window_hours)
        # The kernel names the file by the current 10-minute bucket start.
        fallback = f"securacv-events-{int(time.time()) // 600 * 600}.json"

        async def _read_bytes(resp: Any) -> BundleDownload:
            length = getattr(resp, "content_length", None)
            if isinstance(length, int) and length > EXPORT_BUNDLE_MAX_BYTES:
                raise SecuraCVApiResponseError(f"export bundle too large: {length} bytes")
            content = await resp.read()
            if len(content) > EXPORT_BUNDLE_MAX_BYTES:
                raise SecuraCVApiResponseError(
                    f"export bundle too large: {len(content)} bytes"
                )
            disposition = None
            headers = getattr(resp, "headers", None)
            if headers is not None:
                disposition = headers.get("Content-Disposition")
            return BundleDownload(
                filename=bundle_filename(disposition, fallback), content=bytes(content)
            )

        result = await self._async_request("GET", f"/export/bundle?last={hours}h", _read_bytes)
        if not isinstance(result, BundleDownload):
            raise SecuraCVApiResponseError("export bundle download produced no body")
        return result


async def _error_word(resp: aiohttp.ClientResponse) -> str | None:
    """The `error` word of a JSON refusal body, None when the body is not that."""
    try:
        body = await resp.json()
    except (aiohttp.ContentTypeError, ValueError):
        return None
    if isinstance(body, dict) and isinstance(body.get("error"), str):
        return body["error"]
    return None


async def async_pair_kernel(
    session: aiohttp.ClientSession, url: str, code: str, *, label: str = PAIRED_TOKEN_LABEL
) -> PairResult:
    """Exchange a 6-digit pairing code for a long-lived paired token.

    The one unauthenticated write on the kernel. Raises
    SecuraCVApiAuthError for the kernel's constant 401 (wrong, burned,
    expired or absent code — the kernel answers all four the same so this
    client cannot be an oracle for the pairing state),
    SecuraCVApiPairLimitError for the kernel's 403 `paired_token_limit` (a
    full paired set; the right code was presented and is still valid),
    SecuraCVApiResponseError for any other status (a plain 403 included),
    SecuraCVApiConnectionError when the kernel cannot be reached. The code
    is never logged.
    """
    base = url.rstrip("/")
    try:
        async with session.post(
            f"{base}/api/pair",
            json={"code": code, "label": label},
            timeout=aiohttp.ClientTimeout(total=REQUEST_TIMEOUT_SECONDS),
        ) as resp:
            if resp.status == 401:
                raise SecuraCVApiAuthError("invalid_pairing_code")
            if resp.status == 403 and await _error_word(resp) == "paired_token_limit":
                raise SecuraCVApiPairLimitError("paired_token_limit")
            if resp.status != 200:
                raise SecuraCVApiResponseError(f"unexpected status: {resp.status}")
            try:
                data = await resp.json()
            except aiohttp.ContentTypeError as err:
                raise SecuraCVApiResponseError(f"invalid JSON response: {err}") from err
    except (aiohttp.ClientError, asyncio.TimeoutError) as err:
        raise SecuraCVApiConnectionError("unable to reach API") from err
    if not isinstance(data, dict):
        raise SecuraCVApiResponseError("pair response is not an object")
    try:
        return PairResult(
            token=str(data["token"]),
            token_id=str(data["token_id"]),
            device_id=str(data["device_id"]),
            verifying_key=str(data["verifying_key"]),
            kernel_version=str(data.get("kernel_version", "")),
        )
    except KeyError as err:
        raise SecuraCVApiResponseError(f"pair response missing {err}") from err
