# Reaching your fleet from away

How to see your hub when you're not at home — for free, with nothing of yours
on the public internet, and no monthly subscription.

This page exists because the question has a *popular* answer and a *correct*
answer, and they are not the same one. The popular answer — forward port 8123,
put a certificate on it — puts a login form for your house on the open
internet. The correct answer takes about ten minutes and exposes nothing at
all. There is no middle ground here, which is why this page blesses exactly
one path instead of offering a menu.

## TL;DR

- **The one blessed path is [Tailscale](#the-blessed-path-tailscale).** Free
  for personal use, about ten minutes, and it opens **no port** on your
  router. Your hub stays invisible to the internet.
- **Never forward a port to your hub.** Not `8123`, not RTSP, not "just for a
  week." [Why, in detail.](#what-never-to-do)
- **Alerts already work with none of this.** Push notifications reach your
  phone from anywhere for free and need no inbound access —
  see [`design/alert_relay.md`](design/alert_relay.md). Most people who open a
  port only ever wanted this.
- **Verify, don't assume.** `python3 tools/away_access_check.py` tells you
  whether your hub is actually reachable from outside. A guide is advice; the
  checker is a verdict.
- **You never have to pay for any of this.** [What a subscription would buy
  you](#what-a-subscription-would-buy-you), honestly.

## Three different things people call "remote access"

Almost every bad setup starts by conflating these:

| What you want | What it needs | Cost |
|---|---|---|
| **Know when something happened** | Nothing inbound at all — the hub pushes out | Free, already working |
| **Look at the hub on demand from away** | A way in. **This page.** | Free (Tailscale) |
| **Someone else's cloud holding your footage** | — | Never. It's an invariant, not a price |

The third is out of scope permanently: SecuraCV's whole thesis is that footage
never leaves the device ([`security/THREAT_MODEL.md`](security/THREAT_MODEL.md),
[`security/SECURITY_MODEL.md`](security/SECURITY_MODEL.md)). The first is
already solved and free. This page is about the second.

## The blessed path: Tailscale

Tailscale builds an encrypted WireGuard mesh between *your own* devices. Your
phone talks to your hub directly, over a tunnel both ends dial out to
establish. Nothing listens on your public IP, so there is no door for anyone
to knock on — a port scan of your house finds exactly what it found before you
started: nothing.

The free personal tier covers 100 devices and 3 users, which is more than a
house needs.

### On the hub

**Home Assistant OS:** Settings → Apps → App Store → **Tailscale** →
Install → Start. Open the app's Web UI and follow the login link.

**A Debian / Raspberry Pi OS hub** (the [full-stack path](full_stack_setup.md)):

```sh
curl -fsSL https://tailscale.com/install.sh | sh
sudo tailscale up
tailscale ip -4          # note this address — it's how you'll reach the hub
```

### On your phone and laptop

Install the Tailscale app, sign in with the **same account**, and open
`http://<the address from tailscale ip -4>:8123`. That's the whole thing.
No certificate, no DNS record, no port forward, no router configuration.

### Three settings that decide whether it still works in six months

These are the difference between a setup that lasts and one that quietly dies
while you're away — which is precisely when you find out.

1. **Turn off key expiry for the hub.** Tailscale expires node keys on a
   schedule by default. When the hub's key expires it drops off the tailnet
   silently, and you discover it from a hotel. In the Tailscale admin console,
   open the hub's machine → **Disable key expiry**. Do this the day you set it
   up. It is the single most common way this setup "stops working for no
   reason."

2. **Do not advertise your whole LAN as a subnet route** unless you have
   thought about it. It's the obvious-looking option and it hands every device
   on your tailnet — including a laptop you might lose — a route to every
   device in your house. Install Tailscale on the hub only and reach the
   cameras *through* the hub's UI, which is the access control you already
   have.

3. **Remove stale machines.** Reflashing the hub registers a new node and
   leaves the old one in the list. Prune them, so the machine list stays
   something you can actually audit.

### If you can't use Tailscale

Listed in descending order of how much we'd recommend them. Each is a real
option; none is as good as the one above for most people.

- **[Headscale](https://github.com/juanfont/headscale) + Tailscale clients** —
  the same protocol with a coordination server you host yourself, so no third
  party sees even your device metadata. Ideologically the cleanest option here
  and the right one for a zero-third-party household. Costs you a server to
  keep running and patched.

- **Plain WireGuard on the router** — no account, no third party at all. Many
  consumer routers, and OPNsense/pfSense, have it built in. This *does* need
  one inbound UDP port open, which sounds like it contradicts everything
  above — it doesn't, and the distinction is worth being precise about: a
  WireGuard endpoint does not reply to an unauthenticated packet **at all**.
  To a scanner it is indistinguishable from a closed port. That is a
  categorically different thing from an HTTP server that answers every stranger
  with a login form. Cost: you manage keys by hand.

- **Cloudflare Tunnel — only with Cloudflare Access in front of it.** Without
  an Access policy you have simply published your login page with extra steps,
  which is the failure this page exists to prevent. Note the honest trade even
  when configured correctly: Cloudflare terminates TLS, so they can read your
  traffic. Worth knowing that this makes it *less* private than the paid
  option it's often chosen to avoid.

## What never to do

None of these are hypothetical. Every one is a common recommendation
somewhere on the internet.

| Don't | What actually happens |
|---|---|
| **Forward port 8123** | Your hub's login form is on the public internet. Scanners index it within hours of it going live, and credential-stuffing traffic against it is continuous, not occasional. Home Assistant has shipped authentication-bypass advisories; on a forwarded port, one of those is a break-in rather than a patch note. |
| **DuckDNS + Let's Encrypt + a port forward** | The most-recommended dangerous setup in this whole ecosystem. The padlock makes it *feel* solved, and that's the trap: **TLS protects the traffic, not the door.** You have encrypted the conversation between a stranger and your front door. The exposure is unchanged. |
| **Leave UPnP enabled** | You never chose a hole; a device chose one for you. UPnP lets anything on your LAN open an inbound port with no confirmation and no notification — a console, an app, a torrent client. This is why the checker asks your router directly instead of trusting your memory. |
| **Forward RTSP (554) or MQTT (1883)** | That's live camera video and every event on the hub, usually with no authentication in front of either. |
| **Assume NAT is protecting you** | It isn't, on IPv6. If your ISP gives LAN devices real IPv6 addresses — increasingly the default — then a service bound to `0.0.0.0`/`[::]` can be reachable from the internet **with no port forward at all**, because there's no NAT in the path to require one. Your firewall is the only thing standing there. The checker flags this case specifically, since "nothing is forwarded" is exactly the reasoning that misses it. |
| **"Just for a week"** | There is no such thing. Nobody comes back to close it. Set the overlay up instead — it's faster than the port forward was. |

The kernel's own services default to loopback for exactly this reason: the
event API binds `127.0.0.1:8799` ([`src/config.rs`](../src/config.rs)) and the
break-glass server `127.0.0.1:8800`
([`src/bin/break_glass_serve.rs`](../src/bin/break_glass_serve.rs)). Nothing in
SecuraCV asks you to widen that, and nothing in it needs UPnP.

## Verify it — the checker

Prose can be misread and memory drifts. Run this on the hub:

```sh
python3 tools/away_access_check.py
```

It reports three things: which sensitive services are listening and on what
address, **what your router's port-forward table actually contains**, and
whether an encrypted overlay is up (up *and* addressed — a `wg0` that exists
but is down is not a way home, and counting it as one would hide the warning
you needed). Exit status is `0` when nothing reachable was found and `1` when
something is, so it drops straight into a cron job or a health check. `--json`
for machine-readable output.

**It will not give you a clean bill for a check it couldn't run.** Reading a
router's table means asking for mapping 0, 1, 2… until the router says the
index is invalid — so "the list ended" and "the request timed out" arrive by
the same door, and a tool that conflates them will happily truncate the table
and still print a confident summary. This one insists on the router's own
end-of-list answer; anything else, along with a gateway that never replied or
a run with `--no-router`, comes back **INCONCLUSIVE** rather than OK.

```
SecuraCV away-access check
============================================================

FAIL — something here is reachable from the internet.

  Encrypted overlay : none found
  Router forwards   : 1 active inbound mapping(s)
  Services watched  : 2 listening on sensitive ports

[FAIL] Your router forwards TCP port 8123 from the internet
    Inbound TCP/8123 is forwarded to 192.168.1.50:8123 (this hub). That port
    is Home Assistant — the hub UI and its login form. The mapping has no
    description, which usually means UPnP opened it automatically rather than
    a person choosing it.
    Fix: Delete this port forward in your router's admin page, and turn off
    UPnP so it cannot come back. Then set up an overlay instead —
    docs/away_access.md.
```

**The checker never contacts the internet.** It reads this machine and asks
your own gateway over UPnP-IGD; that's all. It deliberately does not use an
outside "am I exposed?" probe service, because asking a stranger to port-scan
your house means telling a stranger your address and what runs behind it —
the same trade the project refuses everywhere else, with no exception for
convenience.

That honesty has a price, and the tool prints it: it can see the mapping your
router admits to, and it **cannot** see a hole punched upstream of your router
— carrier-grade NAT, a second router, or a tunnel daemon dialing out from
inside. If you run any of those, check them yourself.

## Alerts need none of this

Worth repeating, because it's the step most people skip on their way to
opening a port: **notifications already work from anywhere, for free.** The
Home Assistant companion app delivers push over FCM/APNs with no subscription
and no inbound access, and the hub can drive ntfy, Pushover, Matrix, and
others just as well. The full channel-by-owner-type breakdown is in
[`design/alert_relay.md`](design/alert_relay.md).

If what you actually wanted was "tell me when something happens," you're
already done and this page was optional.

## What a subscription would buy you

Home Assistant Cloud (Nabu Casa) is $6.50/month or $65/year. Being straight
about it, since the comparison is the reason you're on this page:

| What it sells | Whether you need to pay for it |
|---|---|
| Remote access to the UI | **No.** Tailscale is free and exposes less — there's no public hostname at all. |
| Push notifications | **No.** Free already, as above. |
| Text-to-speech / speech-to-text | **No.** Piper and Whisper run locally on the hub. |
| Alexa / Google Assistant | **This one is real.** Doing it yourself means an AWS Lambda or a Google Cloud project, and — importantly — a publicly reachable HTTPS endpoint, which puts you back on the path this page is talking you out of. If you want voice assistants, paying is the sane option. |
| Funding Home Assistant's development | **Also real, and the best reason on the list.** It pays for HA, ESPHome, and Z-Wave JS. If you use those daily, $65/year is cheap. |

Note what isn't on that list: **security**. You do not pay for security here.
The free path is either the safest option available (an overlay, nothing
exposed) or the worst (a port forward). Money isn't what moves you between
them.

SecuraCV itself will never charge you for reaching your own devices — see
[`strategy/05-market-and-cost-comparison.md`](strategy/05-market-and-cost-comparison.md),
where "never paywall privacy" is the stated line and the paid tier is
attestation, not access.

## Related

- [Home Assistant setup](homeassistant_setup.md) — getting the hub itself
  running. Home Assistant's own documentation is the authority on HA
  specifics; this page deliberately doesn't restate it.
- [The full stack, end to end](full_stack_setup.md) — flash the hub, boot it,
  add cameras.
- [Alert relay design](design/alert_relay.md) — how notifications get to you
  without a cloud holding anything.
- [Threat model](security/THREAT_MODEL.md) — why "no remote access surface" is
  listed as a mitigation, and why a port forward silently deletes it.
- [Network coexistence](network_coexistence.md) — the *other* networking page,
  about radio and airtime rather than reachability.
