#!/usr/bin/env python3
"""SecuraCV away-access check — is your hub reachable from the internet?

A guide tells you what to do. This tells you whether you actually did it.
[`docs/away_access.md`](../docs/away_access.md) blesses exactly one way to
reach your fleet from away — an encrypted overlay (Tailscale/WireGuard),
which puts nothing on the public internet. This tool is the verifier for
that guide: run it on the hub and it reports, in plain words, whether
anything on this machine is reachable from outside your house.

It exists because the dangerous configuration is *silent*. Nobody decides
to expose Home Assistant to the internet; they enable UPnP once, or follow
a blog post that says "forward port 8123", and then nothing ever tells them
again. Port 8123 with a login form on it is scanned within hours of going
live. The failure mode of this whole area is not a wrong answer — it's the
absence of a question, so this asks it on a schedule you choose.

WHAT IT CHECKS (three things, in increasing order of how much they matter)

  1. LISTENERS — which SecuraCV/hub services are listening, and on what
     address. A service bound to 0.0.0.0 is reachable from your LAN, which
     is normal and fine on its own. Bound to 127.0.0.1 it is reachable from
     nothing but this machine.

     The exception that matters: if this host holds a globally routable
     address — which is ordinary for IPv6, where there is no NAT to hide
     behind — then a wildcard bind is reachable from the internet with no
     port forward anywhere in the picture. That case is reported on its own
     merits, because NAT-shaped intuition ("nothing is forwarded, so nothing
     is exposed") is exactly what misses it.

  2. ROUTER PORT MAPPINGS — the actual hole. We ask the LAN gateway, over
     UPnP-IGD, for its port-mapping table. This is where the real damage
     lives, because UPnP lets *any* device on the LAN open an inbound hole
     with no confirmation and no notification: a console, a torrent client,
     or a well-meaning add-on can forward a port you never chose to forward.
     A mapping that lands on this host, or on any sensitive port, is a FAIL.

     Reading that table is a walk over indices that ends when the router says
     the index is invalid, so "the end of the list" and "the request failed"
     arrive by the same door. We insist on telling them apart: anything other
     than the router's own end-of-list answer leaves the table PARTIAL and
     the run cannot report a clean bill. A truncated table that called itself
     complete is the one bug that would make this tool actively harmful.

  3. AN ENCRYPTED OVERLAY — whether Tailscale or WireGuard is actually up,
     which means up *and* addressed, not merely present in /sys/class/net.
     A down or unconfigured `wg0` counted as a working overlay would suppress
     the "you have no way in" warning and tell an owner remote access is
     ready — a lie they'd discover from a hotel. Without an overlay you have
     no blessed way in from away, and the temptation to open a hole is
     exactly what the guide is trying to head off.

WHAT IT WILL NOT DO — this tool makes NO connection to the internet, ever.
It talks to two things: this machine, and the gateway on your own LAN. It
deliberately does not use an outside "am I exposed?" probe service, because
asking a stranger to port-scan your house means telling a stranger your
address and what runs behind it. That is the same trade the whole project
refuses (see `docs/security/THREAT_MODEL.md`), and it does not get an
exception for convenience. The cost is honest and stated in the output: we
can see the mapping your router admits to, and we cannot see a hole punched
upstream of it (carrier CGNAT, a second router, a hosted tunnel daemon).

USAGE
  tools/away_access_check.py                 # check, human-readable
  tools/away_access_check.py --json          # same, machine-readable
  tools/away_access_check.py --no-router     # skip the UPnP query
  tools/away_access_check.py --timeout 5     # SSDP/SOAP patience, seconds

EXIT STATUS
  0  no inbound exposure found (warnings may still be printed)
  1  exposure found — something is reachable from the internet
  2  the check could not run (usage error)

A clean exit means "nothing exposed was found", and it is only ever printed
for checks that actually ran: a skipped, unanswered, or truncated router read
each raise a warning of their own, so an unknown never renders as an all-clear.

Stdlib only, no install step: this has to run on a hub that may be a stock
HAOS box, so it must not need pip. Both `ss` and a /proc parser are
implemented because HAOS containers frequently have neither iproute2 nor a
shell you'd expect.
"""

from __future__ import annotations

import argparse
import ipaddress
import json
import os
import re
import socket
import subprocess
import sys
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from typing import Iterable
from urllib.parse import urljoin, urlparse

# ---------------------------------------------------------------------------
# What we consider sensitive, and why. Ports are cited to the file that sets
# them so this table can't quietly drift from the code.
# ---------------------------------------------------------------------------

SENSITIVE_PORTS: dict[int, str] = {
    8123: "Home Assistant — the hub UI and its login form",
    8799: "witness event API (src/config.rs: DEFAULT_API_ADDR)",
    8800: "break-glass server (src/bin/break_glass_serve.rs)",
    1883: "MQTT broker — every event on the hub, unauthenticated by default",
    5000: "Frigate UI",
    8971: "Frigate UI (authenticated port)",
    8554: "go2rtc / RTSP restream — live camera video",
    554: "RTSP — live camera video",
    5900: "VNC",
    3389: "RDP",
    22: "SSH",
}

# Interface-name prefixes that mean "this is an encrypted overlay, not the
# open internet". A service bound only to one of these is reachable solely
# by devices that already hold a key.
OVERLAY_IFACE_PREFIXES = ("tailscale", "wg", "wt", "zt", "nebula")

# Tailscale hands out addresses from the CGNAT range 100.64.0.0/10. An
# address in that range is overlay-only by construction.
TAILSCALE_CGNAT = ipaddress.ip_network("100.64.0.0/10")

PASS, WARN, FAIL = "pass", "warn", "fail"


# ---------------------------------------------------------------------------
# Data
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Listener:
    """One listening socket on this host."""

    address: str
    port: int
    process: str = ""

    @property
    def scope(self) -> str:
        """Who can reach this socket: 'host', 'overlay', 'lan', or 'all'."""
        return address_scope(self.address)


@dataclass(frozen=True)
class PortMapping:
    """One inbound port-forward the router admits to."""

    external_port: int
    internal_port: int
    internal_client: str
    protocol: str = "TCP"
    description: str = ""
    enabled: bool = True


# How much of the router's port-forward table we actually managed to read.
# This is a four-state fact, not a boolean, because "we asked" and "we know"
# are different things and conflating them is how a tool like this reports
# PASS on an exposed hub.
ROUTER_READ = "read"  # walked to the router's own end-of-list marker
ROUTER_PARTIAL = "partial"  # enumeration broke off mid-table — table unknown
ROUTER_UNANSWERED = "unanswered"  # no gateway answered; nothing was read
ROUTER_SKIPPED = "skipped"  # --no-router; the operator declined the check
ROUTER_CONCLUSIVE = frozenset({ROUTER_READ})


@dataclass(frozen=True)
class SoapResult:
    """One SOAP call's outcome, keeping 'the end' apart from 'it broke'.

    UPnP has no "count the mappings" call: you request index 0, 1, 2… until
    the router says the index is invalid, and *that error is the end of the
    list*. Which means every other error looks identical unless you insist on
    telling them apart — and if you don't, one timed-out request truncates the
    table and the mapping at the next index becomes invisible.
    """

    body: str | None = None
    end_of_list: bool = False
    failed: bool = False


@dataclass
class Finding:
    level: str
    title: str
    detail: str
    fix: str = ""


@dataclass
class Report:
    findings: list[Finding] = field(default_factory=list)
    listeners: list[Listener] = field(default_factory=list)
    mappings: list[PortMapping] = field(default_factory=list)
    overlays: list[str] = field(default_factory=list)
    router_state: str = ROUTER_READ
    global_addresses: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)

    @property
    def verdict(self) -> str:
        if any(f.level == FAIL for f in self.findings):
            return FAIL
        if any(f.level == WARN for f in self.findings):
            return WARN
        return PASS


# ---------------------------------------------------------------------------
# Pure helpers — everything here is testable without a network or a router
# ---------------------------------------------------------------------------


def address_scope(address: str) -> str:
    """Classify a bind address by who can reach it.

    'host'    loopback only — nothing off this machine
    'overlay' a Tailscale/WireGuard address — key holders only
    'lan'     a specific LAN address — devices on your network
    'all'     wildcard (0.0.0.0, ::, *) — every interface this host has
    """
    if address in ("*", "0.0.0.0", "::", "[::]", ""):
        return "all"
    try:
        ip = ipaddress.ip_address(address.strip("[]"))
    except ValueError:
        return "lan"
    if ip.is_loopback:
        return "host"
    if ip.version == 4 and ip in TAILSCALE_CGNAT:
        return "overlay"
    if ip.version == 6 and str(ip).startswith("fd7a:115c:a1e0"):  # Tailscale ULA
        return "overlay"
    return "lan"


def parse_ss_listeners(output: str) -> list[Listener]:
    """Parse `ss -H -lntu` output into listeners.

    Tolerant on purpose: `ss` differs across iproute2 versions in whether it
    prints a header even with -H, how it pads columns, and whether a process
    column exists at all. We key off the LISTEN state and the second-to-last
    address-shaped column rather than fixed offsets.
    """
    listeners: list[Listener] = []
    for raw in output.splitlines():
        line = raw.strip()
        if not line or line.startswith(("Netid", "State")):
            continue
        parts = line.split()
        if "LISTEN" not in parts and "UNCONN" not in parts:
            continue
        # The local address is the first token containing ':' that follows
        # the queue columns; peer address follows it.
        addr_tokens = [p for p in parts if ":" in p and not p.startswith("users:")]
        if not addr_tokens:
            continue
        local = addr_tokens[0]
        host, _, port_s = local.rpartition(":")
        if not port_s.isdigit():
            continue
        process = ""
        m = re.search(r'users:\(\("([^"]+)"', line)
        if m:
            process = m.group(1)
        listeners.append(Listener(address=host or "*", port=int(port_s), process=process))
    return listeners


def parse_proc_net_listeners(text: str, *, ipv6: bool = False) -> list[Listener]:
    """Parse /proc/net/tcp or /proc/net/tcp6 for sockets in LISTEN (0A).

    The fallback for hubs with no iproute2 — common inside HAOS containers.
    Addresses are hex and, for IPv4, little-endian per word.
    """
    listeners: list[Listener] = []
    for raw in text.splitlines()[1:]:
        parts = raw.split()
        if len(parts) < 4 or parts[3] != "0A":
            continue
        local = parts[1]
        hex_addr, _, hex_port = local.partition(":")
        try:
            port = int(hex_port, 16)
        except ValueError:
            continue
        listeners.append(Listener(address=_hex_to_ip(hex_addr, ipv6=ipv6), port=port))
    return listeners


def _hex_to_ip(hex_addr: str, *, ipv6: bool) -> str:
    """Convert a /proc/net hex address to its printable form."""
    if not ipv6:
        packed = bytes.fromhex(hex_addr)[::-1]
        return str(ipaddress.ip_address(packed))
    # IPv6 is stored as four little-endian 32-bit words.
    words = [hex_addr[i : i + 8] for i in range(0, 32, 8)]
    packed = b"".join(bytes.fromhex(w)[::-1] for w in words)
    return str(ipaddress.ip_address(packed))


def parse_ssdp_location(response: bytes | str) -> str | None:
    """Pull the LOCATION header out of an SSDP M-SEARCH reply."""
    text = response.decode("utf-8", "replace") if isinstance(response, bytes) else response
    for line in text.splitlines():
        name, _, value = line.partition(":")
        if name.strip().upper() == "LOCATION":
            return value.strip() or None
    return None


def parse_igd_control_url(description_xml: str, base_url: str) -> tuple[str, str] | None:
    """Find the WAN connection service in an IGD device description.

    Returns (absolute control URL, service type), or None when the gateway
    exposes no WAN connection service — which means no mappings to read.
    """
    try:
        root = ET.fromstring(description_xml)
    except ET.ParseError:
        return None
    ns = "{urn:schemas-upnp-org:device-1-0}"
    wanted = (
        "urn:schemas-upnp-org:service:WANIPConnection:2",
        "urn:schemas-upnp-org:service:WANIPConnection:1",
        "urn:schemas-upnp-org:service:WANPPPConnection:1",
    )
    services = {}
    for svc in root.iter(f"{ns}service"):
        st = svc.findtext(f"{ns}serviceType")
        curl = svc.findtext(f"{ns}controlURL")
        if st and curl:
            services[st.strip()] = curl.strip()
    for st in wanted:
        if st in services:
            return urljoin(base_url, services[st]), st
    return None


def parse_port_mapping(soap_xml: str) -> PortMapping | None:
    """Parse one GetGenericPortMappingEntry SOAP response."""
    try:
        root = ET.fromstring(soap_xml)
    except ET.ParseError:
        return None
    values: dict[str, str] = {}
    for el in root.iter():
        tag = el.tag.rpartition("}")[2]
        if tag.startswith("New") and el.text is not None:
            values[tag] = el.text.strip()
    if "NewInternalPort" not in values or "NewExternalPort" not in values:
        return None
    try:
        external = int(values["NewExternalPort"])
        internal = int(values["NewInternalPort"])
    except ValueError:
        return None
    enabled_raw = values.get("NewEnabled", "1").strip().lower()
    return PortMapping(
        external_port=external,
        internal_port=internal,
        internal_client=values.get("NewInternalClient", ""),
        protocol=values.get("NewProtocol", "TCP"),
        description=values.get("NewPortMappingDescription", ""),
        enabled=enabled_raw not in ("0", "false", "no"),
    )


def parse_overlay_interfaces(names: Iterable[str]) -> list[str]:
    """Keep only interface names that look like encrypted overlays.

    Name-shape only — whether the interface is up and addressed is a separate
    question, answered by read_overlay_interfaces().
    """
    return sorted(n for n in names if n.startswith(OVERLAY_IFACE_PREFIXES))


def parse_iface_flags(text: str) -> bool:
    """IFF_UP (bit 0) from the hex value in /sys/class/net/<if>/flags."""
    try:
        return bool(int(text.strip(), 16) & 0x1)
    except ValueError:
        return False


def parse_ip_addr_addresses(text: str) -> set[str]:
    """Pull bare addresses out of `ip -o addr show` output.

    Each line looks like:
      2: eth0    inet 192.168.1.50/24 brd … scope global eth0
      3: eth0    inet6 2001:db8::1/64 scope global
    """
    found: set[str] = set()
    for line in text.splitlines():
        parts = line.split()
        for i, token in enumerate(parts):
            if token in ("inet", "inet6") and i + 1 < len(parts):
                found.add(parts[i + 1].split("/")[0])
    return found


def parse_upnp_error_code(xml: str) -> int | None:
    """The UPnPError errorCode from a SOAP fault body, if there is one."""
    try:
        root = ET.fromstring(xml)
    except ET.ParseError:
        return None
    for el in root.iter():
        if el.tag.rpartition("}")[2] == "errorCode" and el.text:
            try:
                return int(el.text.strip())
            except ValueError:
                return None
    return None


# ---------------------------------------------------------------------------
# The verdict — pure, so the tests can drive every branch
# ---------------------------------------------------------------------------


def classify(
    listeners: list[Listener],
    mappings: list[PortMapping],
    overlays: list[str],
    *,
    router_state: str = ROUTER_READ,
    local_addresses: set[str] | None = None,
) -> Report:
    """Turn raw observations into findings. This is the whole opinion of the tool.

    One rule governs the rest: **this function never reports PASS for something
    it did not look at.** An unread router table, a truncated enumeration, or a
    skipped check all produce a warning of their own, because "we found no
    exposure" and "we could not tell" are answers an owner would act on very
    differently, and only one of them is what silence means.
    """
    report = Report(
        listeners=listeners,
        mappings=mappings,
        overlays=overlays,
        router_state=router_state,
    )
    local_addresses = local_addresses or set()
    report.global_addresses = sorted(a for a in local_addresses if is_globally_routable(a))

    # (1) The hole itself. A mapping pointed at this host is a FAIL whatever
    #     port it lands on — we know something here is internet-reachable.
    #     A mapping on a sensitive port is a FAIL even if it points at another
    #     device, because it is almost certainly a camera or a second hub.
    for m in mappings:
        if not m.enabled:
            continue
        hits_us = m.internal_client in local_addresses
        sensitive = m.internal_port in SENSITIVE_PORTS or m.external_port in SENSITIVE_PORTS
        if not (hits_us or sensitive):
            continue
        what = SENSITIVE_PORTS.get(m.internal_port) or SENSITIVE_PORTS.get(m.external_port, "")
        target = "this hub" if hits_us else m.internal_client
        report.findings.append(
            Finding(
                level=FAIL,
                title=f"Your router forwards {m.protocol} port {m.external_port} from the internet",
                detail=(
                    f"Inbound {m.protocol}/{m.external_port} is forwarded to "
                    f"{m.internal_client}:{m.internal_port} ({target})."
                    + (f" That port is {what}." if what else "")
                    + (
                        f' The router labels the mapping "{m.description}".'
                        if m.description
                        else " The mapping has no description, which usually means UPnP opened it"
                        " automatically rather than a person choosing it."
                    )
                ),
                fix=(
                    "Delete this port forward in your router's admin page, and turn off UPnP "
                    "so it cannot come back. Then set up an overlay instead — "
                    "docs/away_access.md."
                ),
            )
        )

    # (2) UPnP being answerable at all is a standing risk even with a clean
    #     table today, because any device on the LAN can add a mapping
    #     tomorrow, silently. That finding is raised by run_check(), which is
    #     the layer that knows whether the gateway answered; classify() stays
    #     a pure function of what was observed.

    # (3) No overlay means no blessed way in — the state that ends in a
    #     port forward a month later.
    if not overlays:
        report.findings.append(
            Finding(
                level=WARN,
                title="No encrypted overlay is running on this hub",
                detail=(
                    "No Tailscale or WireGuard interface is up, so there is currently no "
                    "safe way to reach this hub while you're away."
                ),
                fix="Install Tailscale — the whole path is in docs/away_access.md, about ten minutes.",
            )
        )

    # (4) Sensitive services on a wildcard bind. What this means depends
    #     entirely on whether this host has a globally routable address, and
    #     an overlay has nothing to do with it either way — a wildcard socket
    #     accepts traffic from every interface the host has, including a
    #     public one. This is the exposure that needs no port forward at all,
    #     which is exactly why NAT-shaped intuition misses it: an ISP that
    #     hands out real IPv6 to LAN hosts gives you no NAT to hide behind.
    wide = [l for l in listeners if l.port in SENSITIVE_PORTS and l.scope == "all"]
    if wide:
        listed = ", ".join(str(l.port) for l in sorted(wide, key=lambda x: x.port))
        if report.global_addresses:
            report.findings.append(
                Finding(
                    level=WARN,
                    title="Sensitive services are on a wildcard bind, and this host has a public address",
                    detail=(
                        f"Port(s) {listed} are bound to every interface, and this host holds a "
                        f"globally routable address ({', '.join(report.global_addresses)}). "
                        "A wildcard socket answers on that address too, so these services may be "
                        "reachable from the internet with no port forward involved — this is the "
                        "usual shape of accidental IPv6 exposure. Whether traffic actually "
                        "arrives depends on a firewall this tool cannot inspect, so treat it as "
                        "unresolved rather than safe."
                    ),
                    fix=(
                        "Confirm your firewall drops inbound connections on that address, or bind "
                        "these services to the overlay/loopback address instead of 0.0.0.0."
                    ),
                )
            )
        elif not overlays:
            report.findings.append(
                Finding(
                    level=WARN,
                    title="Sensitive services are listening on every interface",
                    detail=(
                        f"Port(s) {listed} are bound to 0.0.0.0, so anything that can route to "
                        "this host can reach them. On a LAN with no public address that is normal "
                        "and expected; it becomes the whole problem the moment a port forward or "
                        "a tunnel appears."
                    ),
                    fix=(
                        "Nothing to do if you're on a trusted LAN. If you want belt and braces, "
                        "bind them to the overlay address once Tailscale is up."
                    ),
                )
            )

    # (5) The check we could not complete. This must be a finding rather than
    #     a footnote: notes do not move the verdict, so an unread table would
    #     otherwise let an exposed hub print PASS and exit 0 — the single
    #     worst thing this tool could do.
    if router_state != ROUTER_READ:
        unread = {
            ROUTER_SKIPPED: (
                "The router's port-forward table was not checked (--no-router). That is the "
                "check that finds actual exposure, so this run cannot tell you whether any "
                "exists."
            ),
            ROUTER_UNANSWERED: (
                "No gateway answered the UPnP query, so the port-forward table was never read. "
                "That is good news if UPnP is switched off — but a port forward configured by "
                "hand lives in that same table, and this run did not see it."
            ),
            ROUTER_PARTIAL: (
                "Reading the router's port-forward table broke off partway through, so the "
                "mappings listed here are an incomplete prefix of the real table. A forward "
                "after the point it stopped would not appear."
            ),
        }[router_state]
        report.findings.append(
            Finding(
                level=WARN,
                title="The router's port-forward table is unknown",
                detail=unread,
                fix=(
                    "Open your router's admin page and read the port-forwarding list yourself. "
                    "It should be empty."
                ),
            )
        )

    return report


def is_globally_routable(address: str) -> bool:
    """True when an address is reachable from the internet at large.

    Deliberately conservative in the safe direction: anything unparseable is
    treated as not-global, so this can never manufacture a scary finding out
    of a string we failed to understand — it can only fail to raise one.
    """
    try:
        ip = ipaddress.ip_address(address.strip("[]"))
    except ValueError:
        return False
    if ip.version == 4 and ip in TAILSCALE_CGNAT:
        return False
    return not (
        ip.is_private
        or ip.is_loopback
        or ip.is_link_local
        or ip.is_multicast
        or ip.is_reserved
        or ip.is_unspecified
    )


# ---------------------------------------------------------------------------
# IO — the thin layer the tests replace
# ---------------------------------------------------------------------------


def read_listeners() -> list[Listener]:
    """Prefer `ss`, fall back to /proc — HAOS containers often lack iproute2."""
    try:
        out = subprocess.run(
            ["ss", "-H", "-lntu"], capture_output=True, text=True, timeout=10, check=False
        )
        if out.returncode == 0 and out.stdout.strip():
            return parse_ss_listeners(out.stdout)
    except (OSError, subprocess.SubprocessError):
        pass

    listeners: list[Listener] = []
    for path, is_v6 in (("/proc/net/tcp", False), ("/proc/net/tcp6", True)):
        try:
            with open(path, encoding="utf-8") as fh:
                listeners.extend(parse_proc_net_listeners(fh.read(), ipv6=is_v6))
        except OSError:
            continue
    return listeners


def read_overlay_interfaces() -> list[str]:
    """Overlay interfaces that are actually usable, not merely present.

    A `wg0` that exists but is administratively down, or up with no address,
    is not a way home. Counting it as one is worse than counting nothing: it
    suppresses the "you have no way in" warning and tells the owner remote
    access is ready, which they find out is false at the worst moment.
    """
    try:
        candidates = parse_overlay_interfaces(os.listdir("/sys/class/net"))
    except OSError:
        return []

    usable: list[str] = []
    for name in candidates:
        if not _iface_is_up(name):
            continue
        if not _iface_has_address(name):
            continue
        usable.append(name)
    return usable


def _iface_is_up(name: str) -> bool:
    """IFF_UP from /sys/class/net/<if>/flags.

    operstate is the tempting field and the wrong one: WireGuard and
    Tailscale interfaces report "unknown" even when perfectly healthy, so
    an operstate check would reject every working overlay on the hub.
    """
    try:
        with open(f"/sys/class/net/{name}/flags", encoding="utf-8") as fh:
            return parse_iface_flags(fh.read())
    except OSError:
        return False


def _iface_has_address(name: str) -> bool:
    """Does this interface hold any IP address?

    IPv6 is readable from /proc without tooling; IPv4 needs an ioctl, so we
    ask `ip` and — if it isn't installed — fall back to accepting the
    interface on its up-state alone rather than rejecting a healthy overlay
    because the box is missing iproute2.
    """
    try:
        with open("/proc/net/if_inet6", encoding="utf-8") as fh:
            if any(line.split()[-1:] == [name] for line in fh if line.strip()):
                return True
    except OSError:
        pass
    try:
        out = subprocess.run(
            ["ip", "-o", "-4", "addr", "show", "dev", name],
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
        if out.returncode == 0:
            return bool(out.stdout.strip())
    except (OSError, subprocess.SubprocessError):
        return True  # no iproute2 — don't punish a probably-fine interface
    return False


def read_local_addresses() -> set[str]:
    """Every IP this host answers on — used to tell 'forwarded to us' apart,
    and to spot a globally routable address that makes a wildcard bind
    reachable with no port forward at all."""
    addrs: set[str] = set()
    try:
        hostname = socket.gethostname()
        for info in socket.getaddrinfo(hostname, None):
            addrs.add(info[4][0])
    except OSError:
        pass
    try:
        out = subprocess.run(
            ["ip", "-o", "addr", "show"], capture_output=True, text=True, timeout=5, check=False
        )
        if out.returncode == 0:
            addrs.update(parse_ip_addr_addresses(out.stdout))
    except (OSError, subprocess.SubprocessError):
        pass
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # Connectionless: this picks the default route's source address
        # without sending a single packet.
        s.connect(("10.255.255.255", 1))
        addrs.add(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    return addrs


def discover_igd(timeout: float = 3.0) -> str | None:
    """SSDP M-SEARCH for an InternetGatewayDevice on the local link only."""
    msg = (
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        'MAN: "ssdp:discover"\r\n'
        "MX: 2\r\n"
        "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
        "\r\n"
    ).encode()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
    sock.settimeout(timeout)
    try:
        sock.sendto(msg, ("239.255.255.250", 1900))
        while True:
            data, _ = sock.recvfrom(65507)
            location = parse_ssdp_location(data)
            if location and _is_private_url(location):
                return location
    except (socket.timeout, OSError):
        return None
    finally:
        sock.close()


def _is_private_url(url: str) -> bool:
    """Refuse to fetch anything that isn't on a private address.

    A gateway that answers SSDP with a public LOCATION is either broken or
    hostile; either way this tool's promise is that it never talks to the
    internet, and that promise is enforced here rather than assumed.
    """
    host = urlparse(url).hostname
    if not host:
        return False
    try:
        ip = ipaddress.ip_address(host)
    except ValueError:
        return False
    return ip.is_private or ip.is_link_local or ip.is_loopback


class _NoRedirects(urllib.request.HTTPRedirectHandler):
    """Refuse to follow redirects at all.

    _is_private_url() validates the URL we *ask* for, but urllib follows
    redirects on its own — so a malicious or compromised gateway could answer
    a LAN request with `302 Location: https://somewhere-public/` and urllib
    would dutifully fetch it, breaking the promise this tool makes about
    never contacting the internet. UPnP has no legitimate need for redirects,
    so the safest handling is also the simplest: don't follow any. Returning
    None here makes urllib raise the HTTPError instead, which our callers
    already treat as a failure rather than as data.
    """

    def redirect_request(self, req, fp, code, msg, headers, newurl):  # noqa: D102 - urllib hook
        return None


_OPENER = urllib.request.build_opener(_NoRedirects)


def _http_get(url: str, timeout: float) -> str | None:
    if not _is_private_url(url):
        return None
    try:
        with _OPENER.open(url, timeout=timeout) as resp:  # noqa: S310 - LAN-only, guarded above
            return resp.read().decode("utf-8", "replace")
    except (urllib.error.URLError, OSError, ValueError):
        return None


# Error codes a router returns for "that index does not exist", i.e. the
# documented end of the mapping table. 713 SpecifiedArrayIndexInvalid and
# 714 NoSuchEntryInArray are the specified ones; 402 InvalidArgs is what a
# number of consumer routers send instead. Our arguments are fixed and
# well-formed, so a 402 here can only mean the index ran off the end.
END_OF_LIST_CODES = frozenset({402, 713, 714})


def _soap_call(url: str, service_type: str, action: str, body: str, timeout: float) -> SoapResult:
    if not _is_private_url(url):
        return SoapResult(failed=True)
    envelope = (
        '<?xml version="1.0"?>'
        '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
        's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">'
        f'<s:Body><u:{action} xmlns:u="{service_type}">{body}</u:{action}></s:Body>'
        "</s:Envelope>"
    ).encode()
    req = urllib.request.Request(
        url,
        data=envelope,
        headers={
            "Content-Type": 'text/xml; charset="utf-8"',
            "SOAPAction": f'"{service_type}#{action}"',
        },
    )
    try:
        with _OPENER.open(req, timeout=timeout) as resp:  # noqa: S310 - LAN-only, guarded above
            return SoapResult(body=resp.read().decode("utf-8", "replace"))
    except urllib.error.HTTPError as exc:
        # A SOAP fault. Only the router's own "no such index" is the end of
        # the table; every other fault (including a refused redirect) is a
        # failure we must not mistake for having reached the end.
        try:
            fault = exc.read().decode("utf-8", "replace")
        except (OSError, ValueError):
            return SoapResult(failed=True)
        code = parse_upnp_error_code(fault)
        if code in END_OF_LIST_CODES:
            return SoapResult(end_of_list=True)
        return SoapResult(failed=True)
    except (urllib.error.URLError, OSError, ValueError):
        return SoapResult(failed=True)


def read_port_mappings(
    timeout: float = 3.0, max_entries: int = 128
) -> tuple[list[PortMapping], str]:
    """Ask the LAN gateway for its port-mapping table.

    Returns (mappings, router_state). Walking the table means calling
    GetGenericPortMappingEntry with an increasing index until the router says
    the index is invalid — and *only* that answer ends the walk. A timeout,
    an unexpected fault, or a body we can't parse leaves the table PARTIAL,
    because a mapping sitting at the next index would otherwise vanish from a
    report that still called itself complete.
    """
    location = discover_igd(timeout)
    if not location:
        return [], ROUTER_UNANSWERED
    description = _http_get(location, timeout)
    if not description:
        return [], ROUTER_UNANSWERED
    found = parse_igd_control_url(description, location)
    if not found:
        # The gateway is there but exposes no WAN connection service, so it
        # has no mapping table to read — not the same as reading an empty one.
        return [], ROUTER_PARTIAL
    control_url, service_type = found

    mappings: list[PortMapping] = []
    for index in range(max_entries):
        result = _soap_call(
            control_url,
            service_type,
            "GetGenericPortMappingEntry",
            f"<NewPortMappingIndex>{index}</NewPortMappingIndex>",
            timeout,
        )
        if result.end_of_list:
            return mappings, ROUTER_READ
        if result.failed or result.body is None:
            return mappings, ROUTER_PARTIAL
        mapping = parse_port_mapping(result.body)
        if mapping is None:
            return mappings, ROUTER_PARTIAL
        mappings.append(mapping)
    # Ran to the cap without the router ever saying "no such index" — a table
    # this long is not something to quietly truncate and call complete.
    return mappings, ROUTER_PARTIAL


# ---------------------------------------------------------------------------
# Presentation
# ---------------------------------------------------------------------------

VERDICT_LINE = {
    PASS: "PASS — nothing on this hub is reachable from the internet.",
    WARN: "OK, with notes — no inbound exposure found, but read the warnings.",
    FAIL: "FAIL — something here is reachable from the internet.",
}


def headline(report: Report) -> str:
    """The one line most people will read. It must not overstate the run.

    "No inbound exposure found" is a claim about the whole hub, and it is only
    honest when the router's table was actually read. When it wasn't, the same
    warning-level verdict means something quite different — not "you're fine,
    with notes" but "the main question went unanswered" — and the headline is
    where that distinction has to land, because it's the part a hurried reader
    takes away.
    """
    if report.verdict == WARN and report.router_state not in ROUTER_CONCLUSIVE:
        return (
            "INCONCLUSIVE — nothing exposed turned up in what could be checked, "
            "but the router's port-forward table was not one of those things."
        )
    return VERDICT_LINE[report.verdict]


def render_text(report: Report) -> str:
    out: list[str] = []
    out.append("SecuraCV away-access check")
    out.append("=" * 60)
    out.append("")
    out.append(headline(report))
    out.append("")

    if report.overlays:
        out.append(f"  Encrypted overlay : up ({', '.join(report.overlays)})")
    else:
        out.append("  Encrypted overlay : none found")
    enabled = [m for m in report.mappings if m.enabled]
    router_line = {
        ROUTER_READ: f"{len(enabled)} active inbound mapping(s)",
        ROUTER_PARTIAL: f"UNKNOWN — read broke off after {len(enabled)} mapping(s)",
        ROUTER_UNANSWERED: "UNKNOWN — no gateway answered",
        ROUTER_SKIPPED: "not checked (--no-router)",
    }[report.router_state]
    out.append(f"  Router forwards   : {router_line}")
    watched = [l for l in report.listeners if l.port in SENSITIVE_PORTS]
    out.append(f"  Services watched  : {len(watched)} listening on sensitive ports")
    out.append("")

    if report.findings:
        for f in report.findings:
            out.append(f"[{f.level.upper()}] {f.title}")
            out.append(f"    {f.detail}")
            if f.fix:
                out.append(f"    Fix: {f.fix}")
            out.append("")

    if watched:
        out.append("Listening services on ports we care about:")
        for l in sorted(watched, key=lambda x: x.port):
            where = {
                "host": "this machine only",
                "overlay": "overlay only",
                "lan": "your LAN",
                "all": "every interface",
            }[l.scope]
            name = SENSITIVE_PORTS[l.port]
            proc = f" [{l.process}]" if l.process else ""
            out.append(f"  {l.port:>5}  {where:<18} {name}{proc}")
        out.append("")

    for note in report.notes:
        out.append(f"Note: {note}")
    if report.notes:
        out.append("")

    out.append(
        "This check never contacts the internet — it reads this machine and asks your\n"
        "own gateway. It therefore cannot see a hole punched upstream of your router\n"
        "(carrier-grade NAT, a second router, or a hosted tunnel daemon)."
    )
    out.append("Full guide: docs/away_access.md")
    return "\n".join(out)


def render_json(report: Report) -> str:
    return json.dumps(
        {
            "verdict": report.verdict,
            "overlays": report.overlays,
            "router_state": report.router_state,
            "global_addresses": report.global_addresses,
            "findings": [
                {"level": f.level, "title": f.title, "detail": f.detail, "fix": f.fix}
                for f in report.findings
            ],
            "mappings": [
                {
                    "protocol": m.protocol,
                    "external_port": m.external_port,
                    "internal_client": m.internal_client,
                    "internal_port": m.internal_port,
                    "description": m.description,
                    "enabled": m.enabled,
                }
                for m in report.mappings
            ],
            "listeners": [
                {
                    "address": l.address,
                    "port": l.port,
                    "scope": l.scope,
                    "process": l.process,
                    "service": SENSITIVE_PORTS.get(l.port, ""),
                }
                for l in report.listeners
                if l.port in SENSITIVE_PORTS
            ],
            "notes": report.notes,
        },
        indent=2,
        sort_keys=True,
    )


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def run_check(*, check_router: bool = True, timeout: float = 3.0) -> Report:
    listeners = read_listeners()
    overlays = read_overlay_interfaces()
    local_addresses = read_local_addresses()

    mappings: list[PortMapping] = []
    router_state = ROUTER_SKIPPED
    if check_router:
        mappings, router_state = read_port_mappings(timeout)

    report = classify(
        listeners,
        mappings,
        overlays,
        router_state=router_state,
        local_addresses=local_addresses,
    )

    # A gateway that answered at all is a gateway with UPnP switched on, and
    # that is worth saying even when today's table is clean: it means any
    # device here can open a hole tomorrow without telling anyone.
    if router_state in (ROUTER_READ, ROUTER_PARTIAL):
        report.findings.append(
            Finding(
                level=WARN,
                title="UPnP is enabled on your router",
                detail=(
                    "Your gateway answered a UPnP query, which means any device on this "
                    "network can open an inbound port by itself, with no confirmation and no "
                    "notification. Whatever the table says today is not a property that stays "
                    "true on its own."
                ),
                fix="Turn UPnP off in your router's admin page. Nothing in SecuraCV needs it.",
            )
        )
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="away_access_check.py",
        description="Check whether this SecuraCV hub is reachable from the internet.",
    )
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    parser.add_argument(
        "--no-router",
        action="store_true",
        help="skip the UPnP port-mapping query (leaves the most important check unrun)",
    )
    parser.add_argument(
        "--timeout", type=float, default=3.0, help="seconds to wait for SSDP/SOAP (default 3)"
    )
    args = parser.parse_args(argv)

    if args.timeout <= 0:
        parser.error("--timeout must be positive")

    report = run_check(check_router=not args.no_router, timeout=args.timeout)
    print(render_json(report) if args.json else render_text(report))
    return 1 if report.verdict == FAIL else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(2)
