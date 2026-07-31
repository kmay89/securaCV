#!/usr/bin/env python3
"""Host tests for tools/away_access_check.py.

Run:  python3 tools/test_away_access_check.py    (stdlib only, no network)

The checker's whole value is its verdict, so the verdict is what gets
pinned. Every parser is fed real-world output (three `ss` dialects, both
/proc socket tables, two router SOAP dialects) and every branch of
classify() is driven directly, because a security check that reads as
covered while catching nothing is worse than no check at all.

Two properties matter more than the rest and are tested as properties, not
examples:

  - a port forward that lands on this hub FAILS, whatever port it uses;
  - the tool never fetches a non-private URL, so its "this never talks to
    the internet" promise is enforced by code rather than by intention.

Prints "ALL away_access_check TESTS PASSED" on success (CI marker).
"""

import json
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])

import away_access_check as aac  # noqa: E402

_failures = 0


def check(cond: bool, label: str) -> None:
    global _failures
    if cond:
        print(f"  ok   {label}")
    else:
        _failures += 1
        print(f"  FAIL {label}", file=sys.stderr)


# --------------------------------------------------------------------------
# Address classification
# --------------------------------------------------------------------------


def test_address_scope() -> None:
    print("address scope")
    check(aac.address_scope("127.0.0.1") == "host", "IPv4 loopback is host-only")
    check(aac.address_scope("::1") == "host", "IPv6 loopback is host-only")
    check(aac.address_scope("[::1]") == "host", "bracketed IPv6 loopback is host-only")
    check(aac.address_scope("0.0.0.0") == "all", "0.0.0.0 is every interface")
    check(aac.address_scope("::") == "all", ":: is every interface")
    check(aac.address_scope("[::]") == "all", "bracketed :: is every interface")
    check(aac.address_scope("*") == "all", "* is every interface")
    check(aac.address_scope("192.168.1.50") == "lan", "a private v4 address is LAN")
    check(aac.address_scope("100.101.102.103") == "overlay", "Tailscale CGNAT is overlay")
    check(
        aac.address_scope("fd7a:115c:a1e0::1") == "overlay",
        "Tailscale ULA is overlay",
    )
    # 100.128.0.0 is outside 100.64.0.0/10 — the boundary must not be sloppy,
    # or a real public address gets waved through as "overlay".
    check(aac.address_scope("100.128.0.1") == "lan", "just outside CGNAT is not overlay")


# --------------------------------------------------------------------------
# Listener parsing
# --------------------------------------------------------------------------

SS_MODERN = """\
udp   UNCONN 0      0            0.0.0.0:5353       0.0.0.0:*
tcp   LISTEN 0      4096         0.0.0.0:8123       0.0.0.0:*     users:(("hass",pid=411,fd=9))
tcp   LISTEN 0      128        127.0.0.1:8799       0.0.0.0:*     users:(("witnessd",pid=77,fd=3))
tcp   LISTEN 0      4096            [::]:1883          [::]:*
tcp   LISTEN 0      511    100.101.102.103:5000       0.0.0.0:*
"""

SS_WITH_HEADER = """\
Netid State  Recv-Q Send-Q Local Address:Port Peer Address:Port
tcp   LISTEN 0      4096          0.0.0.0:8123       0.0.0.0:*
"""

SS_NO_PROCESS = """\
tcp   LISTEN 0      4096          0.0.0.0:554        0.0.0.0:*
"""


def test_ss_parsing() -> None:
    print("ss parsing")
    listeners = aac.parse_ss_listeners(SS_MODERN)
    ports = sorted(l.port for l in listeners)
    check(ports == [1883, 5000, 5353, 8123, 8799], f"all five sockets parsed (got {ports})")

    by_port = {l.port: l for l in listeners}
    check(by_port[8123].address == "0.0.0.0", "wildcard bind address kept verbatim")
    check(by_port[8123].process == "hass", "process name extracted from users:(...)")
    check(by_port[8799].scope == "host", "loopback witness API is host-only")
    check(by_port[1883].scope == "all", "bracketed [::] MQTT bind is every interface")
    check(by_port[5000].scope == "overlay", "a Tailscale-bound service is overlay-only")
    check(
        by_port[1883].process == "",
        "a missing process column yields empty, not a crash",
    )

    check(
        [l.port for l in aac.parse_ss_listeners(SS_WITH_HEADER)] == [8123],
        "a header line is skipped even though -H was requested",
    )
    check(
        [l.port for l in aac.parse_ss_listeners(SS_NO_PROCESS)] == [554],
        "output with no process column still parses",
    )
    check(aac.parse_ss_listeners("") == [], "empty ss output is empty, not an error")


PROC_TCP = """\
  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode
   0: 00000000:1FBB 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 12345 1
   1: 0100007F:225F 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 12346 1
   2: 0100007F:E1D6 0201A8C0:C9C4 01 00000000:00000000 00:00000000 00000000     0        0 12347 1
"""

PROC_TCP6 = """\
  sl  local_address                         remote_address                        st
   0: 00000000000000000000000000000000:075B 00000000000000000000000000000000:0000 0A
   1: 00000000000000000000000001000000:2260 00000000000000000000000000000000:0000 0A
"""


def test_proc_parsing() -> None:
    print("/proc socket parsing")
    v4 = aac.parse_proc_net_listeners(PROC_TCP)
    check(len(v4) == 2, f"only LISTEN (0A) rows are taken (got {len(v4)})")
    check(v4[0].address == "0.0.0.0" and v4[0].port == 8123, "0.0.0.0:8123 decoded")
    check(v4[1].address == "127.0.0.1" and v4[1].port == 8799, "127.0.0.1:8799 decoded")

    v6 = aac.parse_proc_net_listeners(PROC_TCP6, ipv6=True)
    check(len(v6) == 2, "both IPv6 listeners parsed")
    check(v6[0].address == "::" and v6[0].port == 1883, ":::1883 decoded")
    check(v6[1].address == "::1" and v6[1].port == 8800, "::1:8800 decoded")
    check(aac.parse_proc_net_listeners("header only\n") == [], "empty table is empty")


# --------------------------------------------------------------------------
# UPnP / router parsing
# --------------------------------------------------------------------------

SSDP_REPLY = (
    b"HTTP/1.1 200 OK\r\n"
    b"CACHE-CONTROL: max-age=120\r\n"
    b"ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
    b"LOCATION: http://192.168.1.1:5000/rootDesc.xml\r\n"
    b"\r\n"
)

IGD_DESCRIPTION = """<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0">
  <device>
    <deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>
    <serviceList>
      <service>
        <serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType>
        <controlURL>/ctl/L3F</controlURL>
      </service>
    </serviceList>
    <deviceList><device>
      <deviceList><device>
        <serviceList>
          <service>
            <serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>
            <controlURL>/ctl/IPConn</controlURL>
          </service>
        </serviceList>
      </device></deviceList>
    </device></deviceList>
  </device>
</root>
"""

MAPPING_SOAP = """<?xml version="1.0"?>
<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/">
 <s:Body>
  <u:GetGenericPortMappingEntryResponse
      xmlns:u="urn:schemas-upnp-org:service:WANIPConnection:1">
   <NewRemoteHost></NewRemoteHost>
   <NewExternalPort>8123</NewExternalPort>
   <NewProtocol>TCP</NewProtocol>
   <NewInternalPort>8123</NewInternalPort>
   <NewInternalClient>192.168.1.50</NewInternalClient>
   <NewEnabled>1</NewEnabled>
   <NewPortMappingDescription>hass</NewPortMappingDescription>
   <NewLeaseDuration>0</NewLeaseDuration>
  </u:GetGenericPortMappingEntryResponse>
 </s:Body>
</s:Envelope>
"""

MAPPING_SOAP_DISABLED = MAPPING_SOAP.replace(
    "<NewEnabled>1</NewEnabled>", "<NewEnabled>0</NewEnabled>"
)


def test_upnp_parsing() -> None:
    print("UPnP parsing")
    check(
        aac.parse_ssdp_location(SSDP_REPLY) == "http://192.168.1.1:5000/rootDesc.xml",
        "LOCATION header extracted from an SSDP reply",
    )
    check(aac.parse_ssdp_location(b"HTTP/1.1 200 OK\r\n\r\n") is None, "no LOCATION is None")

    found = aac.parse_igd_control_url(IGD_DESCRIPTION, "http://192.168.1.1:5000/rootDesc.xml")
    check(found is not None, "WAN connection service located in a nested device tree")
    if found:
        url, service_type = found
        check(url == "http://192.168.1.1:5000/ctl/IPConn", "control URL resolved against the base")
        check(service_type.endswith("WANIPConnection:1"), "service type returned for the SOAP call")
    check(
        aac.parse_igd_control_url("<root></root>", "http://192.168.1.1/") is None,
        "a gateway with no WAN service yields None, not a crash",
    )
    check(
        aac.parse_igd_control_url("not xml at all", "http://192.168.1.1/") is None,
        "unparseable description yields None, not a crash",
    )

    m = aac.parse_port_mapping(MAPPING_SOAP)
    check(m is not None, "a mapping response parses")
    if m:
        check(m.external_port == 8123 and m.internal_port == 8123, "ports parsed")
        check(m.internal_client == "192.168.1.50", "internal client parsed")
        check(m.description == "hass" and m.protocol == "TCP", "description and protocol parsed")
        check(m.enabled is True, "NewEnabled=1 is enabled")
    disabled = aac.parse_port_mapping(MAPPING_SOAP_DISABLED)
    check(disabled is not None and disabled.enabled is False, "NewEnabled=0 is disabled")
    check(aac.parse_port_mapping("<junk") is None, "malformed SOAP yields None, not a crash")
    check(
        aac.parse_port_mapping("<a><NewExternalPort>x</NewExternalPort></a>") is None,
        "a non-numeric port yields None, not a crash",
    )


def test_overlay_interfaces() -> None:
    print("overlay interfaces")
    names = ["lo", "eth0", "wlan0", "tailscale0", "wg0", "docker0", "br-1a2b"]
    check(
        aac.parse_overlay_interfaces(names) == ["tailscale0", "wg0"],
        "only encrypted overlays are recognised",
    )
    check(aac.parse_overlay_interfaces(["lo", "eth0"]) == [], "no overlay is empty")


# --------------------------------------------------------------------------
# The promise: never talk to the internet
# --------------------------------------------------------------------------


def test_private_url_guard() -> None:
    print("private-URL guard (the no-internet promise)")
    check(aac._is_private_url("http://192.168.1.1:5000/d.xml"), "RFC1918 address allowed")
    check(aac._is_private_url("http://10.0.0.1/d.xml"), "10/8 allowed")
    check(aac._is_private_url("http://172.16.5.4/d.xml"), "172.16/12 allowed")
    check(aac._is_private_url("http://127.0.0.1:80/d.xml"), "loopback allowed")
    check(aac._is_private_url("http://[fe80::1]/d.xml"), "link-local v6 allowed")
    check(not aac._is_private_url("http://8.8.8.8/d.xml"), "a public address is refused")
    check(not aac._is_private_url("http://93.184.216.34/x"), "another public address is refused")
    check(
        not aac._is_private_url("http://router.example.com/d.xml"),
        "a hostname is refused — it could resolve anywhere",
    )
    check(not aac._is_private_url("not a url"), "junk is refused")
    check(
        aac._http_get("http://8.8.8.8/rootDesc.xml", 0.1) is None,
        "the fetcher itself refuses a public URL before any socket is opened",
    )
    check(
        aac._soap_call("http://8.8.8.8/ctl", "urn:x", "Act", "", 0.1) is None,
        "the SOAP caller refuses a public URL before any socket is opened",
    )


# --------------------------------------------------------------------------
# The verdict
# --------------------------------------------------------------------------

US = {"192.168.1.50"}
SAFE_LISTENERS = [aac.Listener("127.0.0.1", 8123), aac.Listener("127.0.0.1", 8799)]


def test_classify_clean() -> None:
    print("verdict — a correctly set up hub")
    report = aac.classify(SAFE_LISTENERS, [], ["tailscale0"], local_addresses=US)
    check(report.verdict == aac.PASS, f"overlay up, no mappings → PASS (got {report.verdict})")
    check(report.findings == [], "a clean hub produces no findings at all")


def test_classify_forward_to_us() -> None:
    print("verdict — a port forward pointed at this hub")
    mapping = aac.PortMapping(8123, 8123, "192.168.1.50", description="hass")
    report = aac.classify(SAFE_LISTENERS, [mapping], ["tailscale0"], local_addresses=US)
    check(report.verdict == aac.FAIL, "a forward to this hub FAILS")
    fail = [f for f in report.findings if f.level == aac.FAIL]
    check(len(fail) == 1, "exactly one failure is raised")
    check("8123" in fail[0].title, "the failure names the port")
    check("Home Assistant" in fail[0].detail, "the failure names what is exposed")
    check("UPnP" in fail[0].fix or "router" in fail[0].fix, "the failure says how to fix it")

    # The property, not the example: ANY port forwarded to us is a failure,
    # including one nobody thought to put on the sensitive list.
    odd = aac.PortMapping(47111, 47111, "192.168.1.50")
    odd_report = aac.classify(SAFE_LISTENERS, [odd], ["tailscale0"], local_addresses=US)
    check(odd_report.verdict == aac.FAIL, "an unlisted port forwarded to this hub still FAILS")


def test_classify_forward_elsewhere() -> None:
    print("verdict — forwards that point at other devices")
    console = aac.PortMapping(3074, 3074, "192.168.1.99", description="Xbox")
    report = aac.classify(SAFE_LISTENERS, [console], ["tailscale0"], local_addresses=US)
    check(report.verdict == aac.PASS, "a games console's own forward is not our business")

    camera = aac.PortMapping(554, 554, "192.168.1.77", description="")
    cam_report = aac.classify(SAFE_LISTENERS, [camera], ["tailscale0"], local_addresses=US)
    check(cam_report.verdict == aac.FAIL, "RTSP forwarded to any device on the LAN FAILS")
    check(
        "no description" in cam_report.findings[0].detail,
        "an undescribed mapping is called out as probably-UPnP",
    )

    disabled = aac.PortMapping(8123, 8123, "192.168.1.50", enabled=False)
    off_report = aac.classify(SAFE_LISTENERS, [disabled], ["tailscale0"], local_addresses=US)
    check(off_report.verdict == aac.PASS, "a disabled mapping is not an open hole")


def test_classify_warnings() -> None:
    print("verdict — warnings")
    report = aac.classify(SAFE_LISTENERS, [], [], local_addresses=US)
    check(report.verdict == aac.WARN, "no overlay warns but does not fail")
    titles = " ".join(f.title for f in report.findings)
    check("overlay" in titles, "the missing overlay is named")

    wide = [aac.Listener("0.0.0.0", 8123), aac.Listener("0.0.0.0", 1883)]
    wide_report = aac.classify(wide, [], [], local_addresses=US)
    check(wide_report.verdict == aac.WARN, "wildcard binds warn, never fail on their own")
    detail = " ".join(f.detail for f in wide_report.findings)
    check("8123" in detail and "1883" in detail, "both wildcard ports are named")

    quiet = aac.classify(wide, [], ["tailscale0"], local_addresses=US)
    check(
        quiet.verdict == aac.PASS,
        "wildcard binds stop mattering once an overlay is the way in",
    )

    skipped = aac.classify(SAFE_LISTENERS, [], ["tailscale0"], router_checked=False, local_addresses=US)
    check(
        any("skipped" in n for n in skipped.notes),
        "--no-router is disclosed in the output rather than passing silently",
    )


# --------------------------------------------------------------------------
# Output
# --------------------------------------------------------------------------


def test_rendering() -> None:
    print("rendering")
    mapping = aac.PortMapping(8123, 8123, "192.168.1.50", description="hass")
    report = aac.classify(
        [aac.Listener("0.0.0.0", 8123, "hass")], [mapping], [], local_addresses=US
    )

    text = aac.render_text(report)
    check("FAIL" in text, "the text report leads with the verdict")
    check("docs/away_access.md" in text, "the text report points at the guide")
    check(
        "cannot see a hole punched upstream" in text,
        "the text report states its own blind spot instead of implying completeness",
    )

    payload = json.loads(aac.render_json(report))
    check(payload["verdict"] == "fail", "JSON carries the verdict")
    check(payload["mappings"][0]["external_port"] == 8123, "JSON carries the mapping")
    check(payload["listeners"][0]["scope"] == "all", "JSON carries the listener scope")
    check(len(payload["findings"]) >= 1, "JSON carries the findings")

    clean = aac.classify(SAFE_LISTENERS, [], ["tailscale0"], local_addresses=US)
    check("PASS" in aac.render_text(clean), "a clean hub renders as PASS")


def test_cli_argument_errors() -> None:
    print("CLI")
    try:
        aac.main(["--timeout", "0"])
        check(False, "a non-positive timeout is rejected")
    except SystemExit as exc:
        check(exc.code == 2, "a bad argument exits 2, per the documented status")


def main() -> int:
    test_address_scope()
    test_ss_parsing()
    test_proc_parsing()
    test_upnp_parsing()
    test_overlay_interfaces()
    test_private_url_guard()
    test_classify_clean()
    test_classify_forward_to_us()
    test_classify_forward_elsewhere()
    test_classify_warnings()
    test_rendering()
    test_cli_argument_errors()

    if _failures:
        print(f"{_failures} FAILURE(S)", file=sys.stderr)
        return 1
    print("ALL away_access_check TESTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
