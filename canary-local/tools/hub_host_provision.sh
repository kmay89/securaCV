#!/bin/sh
# securaCV hub provisioning, driven from the HAOS host — the developer console
# on port 22222 that an `authorized_keys` file on the boot partition unlocks.
#
# provision.sh needs three things the HAOS host shell doesn't have: python3,
# SUPERVISOR_TOKEN, and reachability to the Supervisor API. All three already
# exist on the hub, inside the stack itself, so this wrapper borrows them: it
# reads the Core container's Supervisor token, and runs the bundled executor
# with the Core image's own python3 on the Supervisor's internal network, with
# the add-on config tree mounted where the plan's one file-write expects it.
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

exec docker run --rm --network hassio \
  -v /mnt/data/supervisor/addon_configs:/addon_configs \
  -v "$here":/securacv:ro \
  -e SUPERVISOR_TOKEN="$token" \
  -e "SUPERVISOR_URL=http://$supervisor_ip" \
  --entrypoint python3 \
  "$image" \
  /securacv/hub_seed_apply.py --plan /securacv/hub_seed.json --assets-root /securacv "$@"
