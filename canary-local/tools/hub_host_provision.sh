#!/bin/sh
# securaCV hub provisioning, driven from the HAOS host — the developer console
# on port 22222 that an `authorized_keys` file on the boot partition unlocks.
#
# provision.sh needs three things the HAOS host shell doesn't have: python3,
# SUPERVISOR_TOKEN, and reachability to the Supervisor API. All three already
# exist on the hub, inside the stack itself, so this wrapper borrows them: it
# reads the Core container's Supervisor token, and runs the bundled executor
# with the Core image's own python3 on the Supervisor's internal network, with
# the add-on config tree mounted where the plan's one file-write expects it
# (and, only when `--with broker_tls` is asked for, the hub's ssl folder
# mounted read-only where that step's file check looks).
# Nothing is downloaded and nothing new is installed to make that possible.
#
#   preview (changes nothing):  sh host_provision.sh --dry-run
#   provision:                  sh host_provision.sh
#
# Safe to re-run: the executor is idempotent and never overwrites an existing
# Frigate config. The token stays in process environment — never printed.
set -eu
here="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

# Wrong-context guard first: no docker here means this is NOT the HAOS host
# console — saying "wait for boot" would send someone waiting on the wrong fix.
if ! command -v docker >/dev/null 2>&1; then
  echo "host_provision.sh: no docker here, so this isn't the HAOS host console (port 22222)." >&2
  echo "From the Advanced SSH & Web Terminal add-on, run provision.sh instead — same result." >&2
  exit 1
fi

if ! docker inspect homeassistant >/dev/null 2>&1; then
  echo "host_provision.sh: Home Assistant Core isn't running yet." >&2
  echo "First boot downloads Core before starting it — give it a few more minutes and re-run." >&2
  exit 1
fi

token="$(docker inspect -f '{{range .Config.Env}}{{println .}}{{end}}' homeassistant | sed -n 's/^SUPERVISOR_TOKEN=//p' | head -n 1)"
if [ -z "$token" ]; then
  echo "host_provision.sh: couldn't read the Supervisor token from the Core container." >&2
  exit 1
fi

# The Core image carries the python3 the executor needs. The Supervisor's IP on
# the internal `hassio` network is read from the running container, so the API
# is reached without relying on any DNS alias existing on a raw `docker run`.
image="$(docker inspect -f '{{.Config.Image}}' homeassistant)"
supervisor_ip="$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' hassio_supervisor)"
if [ -z "$supervisor_ip" ]; then
  echo "host_provision.sh: couldn't find the Supervisor container's address." >&2
  exit 1
fi

# The opt-in broker_tls step checks /ssl for the operator's certificate and
# key (it never writes there). The Supervisor keeps that tree on the host in
# its data directory beside addon_configs — INFERRED from that mount's
# convention, not yet proven on a hub, hence one overridable string — so it is
# mounted read-only, and ONLY when the feature is asked for: an unconditional
# bind of a missing host path would make docker create it, empty and
# root-owned. If it isn't there, say so and let the executor's own "cannot see
# /ssl" refusal stand rather than reporting a missing certificate.
ssl_src="${SECURACV_HOST_SSL_DIR:-/mnt/data/supervisor/ssl}"
ssl_mount=""
prev=""
for arg in "$@"; do
  if { [ "$prev" = "--with" ] && [ "$arg" = "broker_tls" ]; } || [ "$arg" = "--with=broker_tls" ]; then
    if [ -d "$ssl_src" ]; then
      ssl_mount="$ssl_src:/ssl:ro"
    else
      echo "host_provision.sh: $ssl_src is not here, so the broker_tls step will not see /ssl." >&2
    fi
  fi
  prev="$arg"
done

# ${ssl_mount:+-v "$ssl_mount"} expands to the two words `-v <src>:/ssl:ro`
# when set and to nothing at all when empty — POSIX sh has no arrays.
exec docker run --rm --network hassio \
  -v /mnt/data/supervisor/addon_configs:/addon_configs \
  ${ssl_mount:+-v "$ssl_mount"} \
  -v "$here":/securacv:ro \
  -e SUPERVISOR_TOKEN="$token" \
  -e "SUPERVISOR_URL=http://$supervisor_ip" \
  --entrypoint python3 \
  "$image" \
  /securacv/hub_seed_apply.py --plan /securacv/hub_seed.json --assets-root /securacv "$@"
