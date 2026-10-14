#!/usr/bin/env bash
# SecuraCV Frigate sidecar entrypoint.
#
# Supervises the three daemons that turn Frigate MQTT events into a sealed,
# privacy-preserving witness log:
#   witness_api        — event API + capability token (:8799, every interface
#                        in the container by default so Home Assistant can
#                        pair across the LAN; see SECURACV_API_BIND)
#   frigate_bridge     — Frigate MQTT events → sealed log
#   event_mqtt_bridge  — sealed log → HA MQTT Discovery sensors (optional);
#                        publishes the retained, kernel-signed announce Home
#                        Assistant discovers the kernel by; also keeps the
#                        fleet roll-call (/data/fleet_peers.json, the
#                        Canaries it hears on the broker) that witness_api's
#                        GET /api/fleet serves. No witnessd runs here —
#                        Frigate owns the cameras.
#
# Environment contract (only FRIGATE_MQTT_HOST is required):
#   FRIGATE_MQTT_HOST       broker hostname Frigate publishes to   (required*)
#                           (hostname, IPv4, or IPv6 literal)
#   FRIGATE_MQTT_PORT       broker port                            (1883)
#   MQTT_BROKER_ADDR        full host:port override; replaces the two above
#                           (bracket IPv6 literals: [fd00::5]:1883)
#   MQTT_USERNAME           broker auth                            (none)
#   MQTT_PASSWORD           broker auth                            (none)
#   FRIGATE_TOPIC_PREFIX    Frigate's mqtt.topic_prefix            (frigate)
#   FRIGATE_MQTT_TOPIC      full topic override                    (<prefix>/events)
#   FRIGATE_ENABLE_REVIEWS  seal <prefix>/reviews alerts instead   (false)
#                           of every tracked object (Frigate 0.14+)
#   VERIFY_INTERVAL_SECS    auto-verification cadence, 0 disables  (3600)
#   FRIGATE_CAMERAS         comma-separated camera allowlist       (all)
#   FRIGATE_LABELS          comma-separated label allowlist        (bridge default)
#   FRIGATE_MIN_CONFIDENCE  minimum detection confidence           (0.5)
#   RETENTION_DAYS          sealed-event retention                 (7)
#   TIME_BUCKET_MINUTES     timestamp coarsening bucket            (10)
#   SECURACV_PUBLISH        publish HA Discovery + the announce    (true)
#   HA_DISCOVERY_PREFIX     HA discovery prefix                    (homeassistant)
#   MQTT_TOPIC_PREFIX       state topic prefix                     (witness)
#   POLL_INTERVAL           publisher poll seconds                 (30)
#   BROKER_WAIT_SECS        max seconds to wait for the broker     (30)
#   SECURACV_API_BIND       all|loopback — where witness_api        (all)
#                           listens. all: 0.0.0.0:8799 in the container, the
#                           address the quickstart publishes so Home
#                           Assistant can pair; loopback: 127.0.0.1:8799,
#                           this container only (no pairing, no Wall)
#   SECURACV_ANNOUNCE_HOST  this docker host's LAN name or address  (unset)
#                           — named in the announce so Home Assistant
#                           prefills the kernel URL; unset, the announce
#                           carries no host (a host is a network identifier)
#                           and Home Assistant prefills its broker's host
#   SECURACV_FLEET_ROLLCALL 1 → GET /api/fleet lists every Canary   (unset)
#                           the bridge hears (the Witness Wall's roll-call);
#                           unset → this kernel's own row only
#   DEVICE_KEY_SEED         64-hex signing seed; if unset, read from
#                           /run/secrets/device_key_seed, then /data/device_key,
#                           else auto-generated and persisted (0600)
#
# Subcommands (run inside the running container: `docker exec securacv <cmd>`;
# the image installs `pair`, `unpair` and `doctor` as shims onto this file):
#   entrypoint.sh           run the sidecar (default)
#   entrypoint.sh pair      issue the 6-digit code Home Assistant redeems,
#                           printed ONCE with the kernel key fingerprint
#   entrypoint.sh unpair <id>
#                           revoke one paired Home Assistant token (ids: the
#                           `pair` response, `doctor`, or `unpair` alone)
#   entrypoint.sh doctor    setup checks that pass on an idle, fresh install
#   entrypoint.sh doctor --live
#                           the same, then listen 30 s for a Frigate event
#   entrypoint.sh mint-viewer-token --label <s> [--base-url <url>]
#   entrypoint.sh revoke-viewer-token <id>
#                           pair (or unpair) a Witness Wall for verification
set -euo pipefail

DATA_DIR="${DATA_DIR:-/data}"
DB_PATH="$DATA_DIR/witness.db"
KEY_FILE="$DATA_DIR/device_key"
TOKEN_FILE="$DATA_DIR/api_token"
CONFIG_FILE="$DATA_DIR/witness_config.json"
# The fleet roll-call summary event_mqtt_bridge writes and witness_api's
# GET /api/fleet reads. All three daemons share this one container's /data,
# so it sits beside the database and the token: the directory exists before
# the bridge's first atomic write (which never creates a parent), and the
# volume users already back up carries the public key pinned on first sight
# for every Canary the bridge has heard. Written 0600: it also holds
# per-room wellbeing words. The config block and pub_args below read this
# one variable, so the kernel and the bridge can never name two files.
FLEET_PEERS_FILE="$DATA_DIR/fleet_peers.json"
# The two pairing files witness_api keeps beside the capability token (its
# defaults, so nothing here names them to the kernel): the outstanding
# code's sha256 + expiry + guess budget, and the sha256 of every paired
# Home Assistant token. This script only ever READS them, for `doctor` —
# it never learns a code (the clear code exists on `pair`'s stdout only).
PAIRING_CODE_FILE="$DATA_DIR/pairing_code"
PAIRED_TOKENS_FILE="$DATA_DIR/paired_tokens.json"
# The kernel's port inside the container; the quickstart publishes it 1:1.
API_PORT=8799

# Logs go to stderr: several helpers are called inside $(...) command
# substitutions, where anything on stdout would pollute the captured value.
log() { echo "[securacv] $*" >&2; }
die() { echo "[securacv] ERROR: $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Configuration resolution
# ---------------------------------------------------------------------------

resolve_broker_addr() {
    if [ -n "${MQTT_BROKER_ADDR:-}" ]; then
        echo "$MQTT_BROKER_ADDR"
        return
    fi
    if [ -z "${FRIGATE_MQTT_HOST:-}" ]; then
        die "FRIGATE_MQTT_HOST is not set.
  Point it at the MQTT broker Frigate publishes to (the 'mqtt: host:' value
  in your frigate config), e.g.  FRIGATE_MQTT_HOST=mosquitto
  Or set MQTT_BROKER_ADDR=host:port directly."
    fi
    local host="${FRIGATE_MQTT_HOST}" port="${FRIGATE_MQTT_PORT:-1883}"
    case "$host" in
        \[*\])
            # Bracketed IPv6 literal — append the port.
            ;;
        *:*:*)
            # Bare IPv6 literal: bracket it, so both the shell-side split
            # below and the Rust bridges ([ipv6]:port) parse it the same way.
            host="[${host}]"
            ;;
        *:*)
            # Someone put host:port into FRIGATE_MQTT_HOST — accept it as-is
            # rather than producing host:port:port.
            echo "$host"
            return
            ;;
    esac
    echo "${host}:${port}"
}

# The device key seed this container already has, from the environment, the
# docker secret or the persisted file — or nothing. Never generates one:
# `doctor` and the pairing subcommands must report on the identity the
# daemons use, not mint a second one.
find_device_key_seed() {
    if [ -n "${DEVICE_KEY_SEED:-}" ]; then
        echo "$DEVICE_KEY_SEED"
        return
    fi
    if [ -s /run/secrets/device_key_seed ]; then
        head -n1 /run/secrets/device_key_seed | tr -d '[:space:]'
        return
    fi
    if [ -s "$KEY_FILE" ]; then
        head -n1 "$KEY_FILE" | tr -d '[:space:]'
        return
    fi
    echo ""
}

resolve_device_key_seed() {
    local seed
    seed=$(find_device_key_seed)
    if [ -n "$seed" ]; then
        echo "$seed"
        return
    fi
    if ! seed=$(head -c32 /dev/urandom | od -An -tx1 | tr -d ' \n'); then
        die "failed to generate a device key seed"
    fi
    (
        umask 077
        printf '%s\n' "$seed" > "$KEY_FILE"
    )
    chmod 600 "$KEY_FILE"
    log "Generated a new device key seed and persisted it to $KEY_FILE"
    log "BACK THIS FILE UP: without it, sealed-log signatures cannot be re-derived."
    echo "$seed"
}

# The public Home Assistant device id for a seed: pwk_ + the first 8 hex of
# sha256("securacv:ha-device-id:v1:" + seed). The id lands on retained MQTT
# topics every broker client can read, so it must carry ZERO bits of the
# signing seed users are told to back up — hence a domain-separated hash,
# never a prefix of the seed. The same derivation event_mqtt_bridge,
# frigate_bridge and the add-on's run.sh use, so every producer names one
# seed the same way.
ha_device_id_for_seed() {
    printf 'pwk_%s' "$(printf '%s' "securacv:ha-device-id:v1:$1" | sha256sum | cut -c1-8)"
}

# Where witness_api listens inside the container, from SECURACV_API_BIND.
# Sets API_BIND_MODE and API_ADDR for the callers (run's config block, the
# doctor's report, pair's hint) and, in `all` mode only, exports the
# kernel's cleartext acknowledgment: witness_api refuses a non-loopback
# bind without WITNESS_API_ALLOW_INSECURE=1 (src/api/mod.rs), so the two
# must flip together — a bind alone would make the container refuse to
# start. Not a $(...) helper on purpose: the export has to land in this
# shell.
#
#   all (default)       0.0.0.0:8799 — the quickstart publishes this port
#                       from the first start so Home Assistant can pair over
#                       the LAN. What that opens to anyone who reaches the
#                       port: GET /health (liveness), POST /api/pair (redeems
#                       the code `pair` printed; 5 wrong guesses burn it and
#                       every refusal is the same 401) and GET /api/fleet
#                       (this kernel's own row unless SECURACV_FLEET_ROLLCALL
#                       widens it). Every data route needs a token: the
#                       rotating capability token, which never leaves the
#                       container, or the paired token Home Assistant earned.
#   loopback            127.0.0.1:8799 — reachable from this container only
#                       (the HEALTHCHECK and `docker exec` still work). No
#                       Home Assistant pairing and no Witness Wall.
configure_api_bind() {
    # Self-contained on purpose (the port defaults here too): the add-on's
    # config-render test lifts this one function out of the file and runs
    # it in an emptied environment to prove the real mapping.
    API_BIND_MODE="${SECURACV_API_BIND:-all}"
    case "$API_BIND_MODE" in
        loopback)
            API_ADDR="127.0.0.1:${API_PORT:-8799}"
            ;;
        all)
            API_ADDR="0.0.0.0:${API_PORT:-8799}"
            export WITNESS_API_ALLOW_INSECURE=1
            ;;
        *)
            die "SECURACV_API_BIND='$API_BIND_MODE' is not a bind mode.
  Use 'all' (the default: 0.0.0.0:${API_PORT:-8799} in the container, published
  by the quickstart so Home Assistant can pair) or 'loopback' (the API answers
  inside this container only — no Home Assistant pairing, no Witness Wall)."
            ;;
    esac
}

# What GET /api/fleet discloses to anyone who reaches the published port,
# from SECURACV_FLEET_ROLLCALL into the kernel's WITNESS_API_FLEET_ROLLCALL:
# the kernel's own row (`self`, the default — a sidecar is published for
# Home Assistant pairing, not to roll-call the LAN) or every Canary the
# bridge hears (`full`, the Witness Wall's opt-in). Exported, because the
# kernel reads it from its environment.
configure_fleet_rollcall() {
    case "${SECURACV_FLEET_ROLLCALL:-}" in
        1|true|TRUE|True|full)
            export WITNESS_API_FLEET_ROLLCALL=full
            ;;
        ""|0|false|FALSE|False|self)
            export WITNESS_API_FLEET_ROLLCALL=self
            ;;
        *)
            die "SECURACV_FLEET_ROLLCALL='${SECURACV_FLEET_ROLLCALL}' is not a switch.
  Set it to 1 so GET /api/fleet lists every Canary the bridge hears (the
  Witness Wall on your Apple TV), or leave it unset: the roll-call then
  names this kernel alone."
            ;;
    esac
}

# The host part of the URL Home Assistant confirms, as `pair` prints it:
# SECURACV_ANNOUNCE_HOST when set (bracketed if it is a bare IPv6 literal),
# else a placeholder the operator fills in by reading their own network.
announce_host_for_url() {
    local host="${SECURACV_ANNOUNCE_HOST:-}"
    if [ -z "$host" ]; then
        echo "<this docker host>"
        return
    fi
    case "$host" in
        \[*\]) echo "$host" ;;
        *:*:*) echo "[$host]" ;;
        *) echo "$host" ;;
    esac
}

tcp_check() {
    local host="$1" port="$2"
    (exec 3<>"/dev/tcp/${host}/${port}") 2>/dev/null
}

# Split "host[:port]" without mangling IPv6 literals. Only a single
# trailing :<digits> counts as a port; "[::1]:1883" and "[::1]" keep the
# address inside the brackets; a bare IPv6 literal ("fd00::5") is all host
# (add brackets to give one a port). The old ${1%:*}/${1##*:} split turned
# "::1" into host ":" port "1".
split_addr_host() {
    local addr="$1"
    case "$addr" in
        \[*\]*) addr="${addr#\[}"; echo "${addr%%\]*}" ;;
        *:*:*)  echo "$addr" ;;
        *:*)
            local tail="${addr##*:}"
            if [[ "$tail" =~ ^[0-9]+$ ]]; then
                echo "${addr%:*}"
            else
                echo "$addr"
            fi
            ;;
        *) echo "$addr" ;;
    esac
}
split_addr_port() {
    local addr="$1" default_port="${FRIGATE_MQTT_PORT:-1883}"
    case "$addr" in
        \[*\]:*)
            local tail="${addr##*\]:}"
            if [[ "$tail" =~ ^[0-9]+$ ]]; then
                echo "$tail"
            else
                echo "$default_port"
            fi
            ;;
        \[*\]) echo "$default_port" ;;
        *:*:*) echo "$default_port" ;;
        *:*)
            local tail="${addr##*:}"
            if [[ "$tail" =~ ^[0-9]+$ ]]; then
                echo "$tail"
            else
                echo "$default_port"
            fi
            ;;
        *) echo "$default_port" ;;
    esac
}

# ---------------------------------------------------------------------------
# pairing state — read from the files witness_api owns, never the code
# ---------------------------------------------------------------------------

# How many Home Assistant tokens are paired: one "id" key per entry in the
# paired-token set (the file holds sha256 hashes only). 0 when none — and
# "none" includes a set whose last token was just revoked (`{"tokens":[]}`),
# where grep finds nothing and exits 1; under `set -o pipefail` that would
# otherwise abort `doctor` right before its pairing line.
paired_token_count() {
    if [ -s "$PAIRED_TOKENS_FILE" ]; then
        { grep -o '"id"' "$PAIRED_TOKENS_FILE" || true; } | wc -l | tr -d '[:space:]'
    else
        echo 0
    fi
}

# The expiry (epoch seconds) of the outstanding pairing code, or nothing.
# The file carries the code's sha256, never the digits, so reading it
# cannot disclose a code — and this script never prints anything from it
# but the minutes left.
pairing_code_expires() {
    if [ -s "$PAIRING_CODE_FILE" ]; then
        sed -n 's/.*"expires":\([0-9][0-9]*\).*/\1/p' "$PAIRING_CODE_FILE" | head -n1
    fi
}

# The lines `doctor` prints about pairing. Informational: a fresh install
# is unpaired, and that is a hint, not a failure.
pairing_state_report() {
    local tokens expires now left_min
    tokens=$(paired_token_count)
    if [ "$tokens" -eq 1 ]; then
        echo "✓ paired: yes (1 token)"
    elif [ "$tokens" -gt 1 ]; then
        echo "✓ paired: yes ($tokens tokens)"
    else
        echo "- paired: no"
        echo "  - Pair Home Assistant: docker exec securacv pair, then enter the code it prints"
        echo "    in Settings → Devices & Services → the discovered 'SecuraCV sidecar' card → Configure."
    fi
    expires=$(pairing_code_expires)
    now=$(date +%s)
    if [ -n "$expires" ] && [ "$expires" -gt "$now" ]; then
        left_min=$(( (expires - now + 59) / 60 ))
        echo "- pairing code outstanding: yes (expires in ${left_min} min)"
    else
        echo "- pairing code outstanding: no"
    fi
}

# ---------------------------------------------------------------------------
# doctor — diagnose the integration without changing anything
# ---------------------------------------------------------------------------

# Plain `doctor` runs the setup checks — every one of them passes on an
# idle, freshly started install (nothing has to walk past a camera):
# broker reachable, credentials accepted, witness_api answering, the
# capability token written, the retained announce Home Assistant discovers
# the kernel by present, the pairing state, and the sealed log intact.
# `doctor --live` adds the 30 s listen for real Frigate traffic.
doctor() {
    local live=0 arg
    for arg in "$@"; do
        case "$arg" in
            --live) live=1 ;;
            *) die "doctor: unknown argument '$arg' (the only option is --live)" ;;
        esac
    done
    local failures=0
    local addr host port topic prefix state_prefix publish
    addr=$(resolve_broker_addr)
    host=$(split_addr_host "$addr")
    port=$(split_addr_port "$addr")
    prefix="${FRIGATE_TOPIC_PREFIX:-frigate}"
    topic="${FRIGATE_MQTT_TOPIC:-${prefix}/events}"
    state_prefix="${MQTT_TOPIC_PREFIX:-witness}"
    publish="${SECURACV_PUBLISH:-true}"
    local listen_secs="${DOCTOR_LISTEN_SECS:-30}"
    local announce_wait="${DOCTOR_ANNOUNCE_WAIT_SECS:-20}"
    configure_api_bind
    configure_fleet_rollcall

    echo "SecuraCV sidecar doctor"
    echo "  broker: $addr   frigate topic: $topic"
    echo "  api bind: $API_BIND_MODE ($API_ADDR inside the container)"
    if [ "$WITNESS_API_FLEET_ROLLCALL" = full ]; then
        echo "  fleet roll-call: full (GET /api/fleet lists every Canary the bridge hears)"
    else
        echo "  fleet roll-call: self (GET /api/fleet lists this kernel only)"
    fi
    if [ "$API_BIND_MODE" = "all" ]; then
        echo "  home assistant: pairs over the published port — docker exec securacv pair"
        if [ -n "${SECURACV_ANNOUNCE_HOST:-}" ]; then
            echo "                  kernel URL it is offered: http://$(announce_host_for_url):$API_PORT"
        else
            echo "                  kernel URL it is offered: its broker's host, port $API_PORT (editable there;"
            echo "                  set SECURACV_ANNOUNCE_HOST to prefill this docker host's address)"
        fi
    else
        echo "  home assistant: cannot reach this API (loopback). SECURACV_API_BIND=all plus the"
        echo "                  published 8799 (ports: - \"8799:8799\" in compose) opens pairing."
    fi
    echo

    # 1) Broker TCP reachability
    if tcp_check "$host" "$port"; then
        echo "✓ Broker $addr is reachable"
    else
        echo "✗ Cannot reach $addr (TCP)"
        echo "  - Is the broker container running and on the same docker network?"
        echo "  - Does FRIGATE_MQTT_HOST match the broker's service name?"
        failures=$((failures + 1))
    fi

    # 2) Broker auth: a short subscribe must not be rejected
    local auth_args=()
    [ -n "${MQTT_USERNAME:-}" ] && auth_args+=(-u "$MQTT_USERNAME")
    [ -n "${MQTT_PASSWORD:-}" ] && auth_args+=(-P "$MQTT_PASSWORD")
    # '$SYS' below is a literal MQTT topic, not a shell variable.
    # shellcheck disable=SC2016
    if mosquitto_sub -h "$host" -p "$port" "${auth_args[@]}" -t '$SYS/#' -C 1 -W 5 >/dev/null 2>&1 \
       || mosquitto_sub -h "$host" -p "$port" "${auth_args[@]}" -t "$topic" -E -W 5 >/dev/null 2>&1; then
        echo "✓ Broker accepts our credentials"
    else
        echo "✗ Broker rejected the connection (auth?)"
        echo "  - Set MQTT_USERNAME / MQTT_PASSWORD to match the broker's password file."
        failures=$((failures + 1))
    fi

    # 3) The kernel API answers. Probed on loopback, which both bind modes
    #    include — the same URL the HEALTHCHECK uses.
    if curl -fsS --max-time 5 "http://127.0.0.1:$API_PORT/health" >/dev/null 2>&1; then
        echo "✓ witness_api answers http://127.0.0.1:$API_PORT/health"
    else
        echo "✗ witness_api is not answering http://127.0.0.1:$API_PORT/health"
        echo "  - Ran this with 'docker compose run'? The daemons live in the running container:"
        echo "    docker exec securacv doctor"
        echo "  - Otherwise: docker logs securacv — the first lines say what refused to start."
        failures=$((failures + 1))
    fi

    # 4) The rotating capability token the daemons share. It never leaves
    #    the container; Home Assistant gets its own token by pairing.
    if [ -s "$TOKEN_FILE" ]; then
        echo "✓ Capability token present ($TOKEN_FILE; rotating, never leaves the container)"
    else
        echo "✗ No capability token at $TOKEN_FILE (witness_api has not started, or /data is not writable)"
        failures=$((failures + 1))
    fi

    # 5) The retained announce Home Assistant discovers the kernel by. The
    #    bridge publishes it once it has fetched the kernel's identity, so
    #    a just-started sidecar may need a few seconds; the wait is bounded.
    local seed device_id announce_topic announce
    seed=$(find_device_key_seed)
    device_id=""
    [ -n "$seed" ] && device_id=$(ha_device_id_for_seed "$seed")
    if [ "$publish" != "true" ]; then
        echo "- Announce: not published (SECURACV_PUBLISH=$publish) — Home Assistant shows no discovered card"
    else
        announce_topic="$state_prefix/${device_id:-+}/announce"
        if announce=$(mosquitto_sub -h "$host" -p "$port" "${auth_args[@]}" -t "$announce_topic" -C 1 -W "$announce_wait" 2>/dev/null) \
           && [ -n "$announce" ]; then
            case "$announce" in
                *'"pairing":"required"'*)
                    echo "✓ Retained announce present on $announce_topic (Home Assistant shows the discovered card)" ;;
                *)
                    echo "✗ Retained announce on $announce_topic does not say pairing is required"
                    echo "  - This sidecar expects Home Assistant to pair with a code; the bridge's flags are off."
                    failures=$((failures + 1)) ;;
            esac
            if [ -z "${SECURACV_ANNOUNCE_HOST:-}" ]; then
                case "$announce" in
                    *'"host":'*)
                        echo "✗ The announce names a host although SECURACV_ANNOUNCE_HOST is unset"
                        failures=$((failures + 1)) ;;
                esac
            fi
        else
            echo "✗ No retained announce on $announce_topic within ${announce_wait}s"
            echo "  - The bridge publishes it shortly after start; a sidecar started seconds ago may"
            echo "    simply not have yet — run doctor again."
            echo "  - Check the broker lets this user publish under '$state_prefix/' (ACLs), and that"
            echo "    Home Assistant's MQTT integration uses the SAME broker."
            failures=$((failures + 1))
        fi
    fi

    # 6) Pairing state — never the code.
    pairing_state_report

    # 7) Frigate traffic: only with --live, because it needs someone to
    #    walk past a camera. Without it, say how to run that check.
    if [ "$live" -eq 1 ]; then
        echo "… Listening on $topic for ${listen_secs}s (walk past a camera to trigger an event)"
        if mosquitto_sub -h "$host" -p "$port" "${auth_args[@]}" -t "$topic" -C 1 -W "$listen_secs" >/dev/null 2>&1; then
            echo "✓ Frigate traffic seen on $topic"
        else
            echo "✗ No messages on $topic within ${listen_secs}s"
            echo "  - Is 'mqtt: enabled: true' set in your frigate config?"
            echo "  - Does Frigate's mqtt.topic_prefix match FRIGATE_TOPIC_PREFIX ('$prefix')?"
            echo "  - Debug by hand: mosquitto_sub -h $host -p $port -t '$topic' -v"
            failures=$((failures + 1))
        fi
    else
        echo "- Frigate traffic: not checked (doctor --live listens on $topic for ${listen_secs}s)"
    fi

    # 8) Sealed log integrity (only if the kernel has opened one already)
    if [ -s "$DB_PATH" ] && [ -n "$seed" ]; then
        if DEVICE_KEY_SEED="$seed" log_verify --db "$DB_PATH" >/dev/null 2>&1; then
            echo "✓ Sealed log at $DB_PATH verifies (hash chain + signatures)"
        else
            echo "✗ Sealed log verification FAILED — run 'log_verify --db $DB_PATH' for details"
            failures=$((failures + 1))
        fi
    else
        echo "- No sealed log yet at $DB_PATH (the kernel has not started here)"
    fi

    echo
    if [ "$failures" -eq 0 ]; then
        echo "All checks passed."
    else
        echo "$failures check(s) failed."
    fi
    return "$failures"
}

# Exit (non-zero, on purpose: compose restart policies and the e2e harness
# rely on a real failure) with a message that says exactly what to fix.
broker_unreachable_exit() {
    local addr="$1" waited="$2"
    cat >&2 <<EOF
[securacv] ------------------------------------------------------------------
[securacv] ERROR: no MQTT broker is answering at ${addr} (waited ${waited}s).
[securacv]
[securacv] The sidecar listens to the same broker Frigate publishes to, so it
[securacv] cannot start without one. Three things fix almost every case:
[securacv]
[securacv]   1. Point FRIGATE_MQTT_HOST at your broker. It must be the
[securacv]      hostname or IP as seen FROM THIS CONTAINER — usually the
[securacv]      broker's compose service name, or the docker host's LAN IP.
[securacv]      In docker/sidecar/quickstart.compose.yml it is the line:
[securacv]          FRIGATE_MQTT_HOST: "mosquitto"   # <- edit this value
[securacv]
[securacv]   2. Make sure the broker container is running and on the same
[securacv]      docker network as this one (a broker in another compose
[securacv]      project is NOT, unless you join its network).
[securacv]
[securacv]   3. No broker at all yet? Use the quickstart that bundles one:
[securacv]          docker compose -f quickstart-with-broker.compose.yml up -d
[securacv]      (file: docker/sidecar/quickstart-with-broker.compose.yml)
[securacv]
[securacv] To diagnose interactively (the container cannot stay up, so run a
[securacv] throwaway one; the broker checks are the ones that matter here):
[securacv]     docker compose run --rm securacv doctor
[securacv] ------------------------------------------------------------------
EOF
    exit 1
}

# ---------------------------------------------------------------------------
# operator subcommands against the running kernel's config and key
# ---------------------------------------------------------------------------

# kernel_cli_env SUBCOMMAND — resolve the config the running sidecar
# rendered and its device key into this shell's environment, so a
# witness_api subcommand run here signs, pins and files against the same
# kernel the daemons serve. Never generates a key: a receipt or a pairing
# code that pinned a fresh one would pin a key nothing signs with.
kernel_cli_env() {
    [ -s "$CONFIG_FILE" ] \
        || die "no $CONFIG_FILE yet: start the sidecar once (docker compose up -d), then run this inside it (docker exec securacv $1 ...)"
    if [ -z "$(find_device_key_seed)" ]; then
        die "no device key (DEVICE_KEY_SEED, /run/secrets/device_key_seed or $KEY_FILE): a receipt must pin the key the running sidecar signs with"
    fi
    DEVICE_KEY_SEED=$(find_device_key_seed)
    export DEVICE_KEY_SEED
    export WITNESS_CONFIG="$CONFIG_FILE"
}

# viewer_token SUBCOMMAND [ARGS...] — witness_api's mint-viewer-token /
# revoke-viewer-token, for a Witness Wall. Run it inside the running
# container:
#   docker exec securacv entrypoint.sh mint-viewer-token \
#       --label "living room tv" --base-url http://<docker-host-ip>:8799
# stdout is the pairing receipt, one JSON line and the token's only copy;
# the kernel keeps its sha256 in /data/viewer_tokens.json (beside the
# capability token, 0600) and reads that file per request, so a mint or a
# revoke takes effect without a restart.
viewer_token() {
    kernel_cli_env "$1"
    exec witness_api "$@"
}

# One field of a compact serde_json line (`"name":"value"` or `"name":123`),
# without jq in the image. Prints nothing when the field is absent.
json_line_field() {
    local line="$1" name="$2"
    printf '%s' "$line" | sed -n "s/.*\"$name\":\"\\([^\"]*\\)\".*/\\1/p; s/.*\"$name\":\\([0-9][0-9]*\\).*/\\1/p" | head -n1
}

# pair [--ttl-secs <n>] — issue the 6-digit code Home Assistant redeems at
# POST /api/pair (one outstanding code at a time; this replaces it) and
# print the ceremony card once. The code comes from `witness_api
# issue-pairing-code`, which files only its sha256 (with a 5-guess budget
# and the expiry) beside the capability token; the daemon log never sees
# the digits — only this command's stdout does, so `docker exec` is the
# one place a code ever appears.
pair() {
    kernel_cli_env pair
    configure_api_bind
    local receipt errlog
    errlog=$(mktemp)
    # stdout: the receipt, one JSON line, parsed below and never echoed.
    # stderr: witness_api's own sentence (it repeats the code) — kept back
    # unless the command fails, so the card below is the one copy.
    if ! receipt=$(witness_api issue-pairing-code --label "docker exec securacv pair" "$@" 2>"$errlog"); then
        cat "$errlog" >&2
        rm -f "$errlog"
        die "issue-pairing-code failed (see above). An older kernel without pairing support prints 'unknown subcommand' here."
    fi
    rm -f "$errlog"
    local code expires attempts fingerprint
    code=$(json_line_field "$receipt" pairing_code)
    expires=$(json_line_field "$receipt" expires)
    attempts=$(json_line_field "$receipt" attempts)
    fingerprint=$(json_line_field "$receipt" fingerprint)
    [[ "$code" =~ ^[0-9]{6}$ ]] || die "issue-pairing-code printed no 6-digit code"
    [[ "$fingerprint" =~ ^[0-9a-f]{8}$ ]] || die "issue-pairing-code printed no key fingerprint"
    [[ "$expires" =~ ^[0-9]+$ ]] || die "issue-pairing-code printed no expiry"
    [[ "$attempts" =~ ^[0-9]+$ ]] || attempts=5
    local now minutes
    now=$(date +%s)
    minutes=$(( (expires - now + 59) / 60 ))
    [ "$minutes" -lt 1 ] && minutes=1

    printf 'SecuraCV pairing code: %s   (valid %s minutes, %s attempts)\n' "$code" "$minutes" "$attempts"
    printf 'Kernel key fingerprint: %s   (Home Assistant shows the same fingerprint before it pairs)\n' "$fingerprint"
    printf 'Kernel URL to confirm in Home Assistant: http://%s:%s\n' "$(announce_host_for_url)" "$API_PORT"
    printf 'In Home Assistant: Settings → Devices & Services → the discovered "SecuraCV sidecar" card → Configure → enter the code.\n'

    if [ "$API_BIND_MODE" != "all" ]; then
        log "SECURACV_API_BIND=$API_BIND_MODE: Home Assistant cannot reach this API to redeem the code. Set SECURACV_API_BIND=all and publish the port (ports: - \"8799:8799\" in compose)."
    fi
}

# unpair <id> — revoke one paired Home Assistant token; its next request is
# refused, no restart needed. Without an id, list the paired tokens (id,
# label, minted — never the token) so the operator can pick one.
unpair() {
    kernel_cli_env unpair
    case $# in
        0)
            echo "usage: docker exec securacv unpair <id>" >&2
            echo "paired tokens on this kernel (id, label, minted):" >&2
            witness_api list-paired-tokens
            exit 2
            ;;
        1)
            exec witness_api revoke-paired-token "$1"
            ;;
        *)
            die "unpair takes exactly one argument, the token id (run 'unpair' alone to list them)"
            ;;
    esac
}

# ---------------------------------------------------------------------------
# run — supervise the three daemons
# ---------------------------------------------------------------------------

# The daemons `run` started, and whether a stop was asked for. Globals,
# because the signal trap that reads them runs outside run()'s scope.
DAEMON_PIDS=()
SHUTDOWN_REQUESTED=0
# How long a forwarded shutdown waits for the daemons' own clean exits
# before ending what is left and exiting anyway. Docker's default stop
# grace is 10 s; past it the container is SIGKILLed and exits 137 for a
# stop that was asked for. The daemons bound their own clean paths well
# under this (the bridge: 2 s for its last acks, 5 s for the disconnect).
STOP_GRACE_SECS="${SECURACV_STOP_GRACE_SECS:-8}"

# `docker stop` sends SIGTERM to this script (PID 1). A bash that dies
# without forwarding it leaves the daemons to the SIGKILL ten seconds
# later — so the bridge could never clear its retained announce (a removed
# sidecar would keep offering a "discovered" card) and frigate_bridge
# could never seal its clean-shutdown record. Forward it, then let run()
# wait for them.
stop_daemons() {
    SHUTDOWN_REQUESTED=1
    log "shutdown signal received; stopping the daemons"
    if [ "${#DAEMON_PIDS[@]}" -gt 0 ]; then
        kill -TERM "${DAEMON_PIDS[@]}" 2>/dev/null || true
    fi
}

# wait_daemons_bounded SECS — wait for every daemon in DAEMON_PIDS to exit,
# at most SECS; then SIGKILL whatever is left and name it. Returns 0 when
# they all exited on their own, 1 when any had to be ended. bash reaps a
# background child the moment it exits, so `kill -0` is a liveness test
# here, not a zombie test.
wait_daemons_bounded() {
    local grace="$1" ticks=0 pid alive
    while :; do
        alive=()
        for pid in "${DAEMON_PIDS[@]}"; do
            if kill -0 "$pid" 2>/dev/null; then alive+=("$pid"); fi
        done
        if [ "${#alive[@]}" -eq 0 ]; then
            wait 2>/dev/null || true
            return 0
        fi
        if [ "$ticks" -ge "$((grace * 10))" ]; then
            log "daemon(s) ${alive[*]} did not exit within ${grace}s of SIGTERM; ending them"
            kill -KILL "${alive[@]}" 2>/dev/null || true
            wait 2>/dev/null || true
            return 1
        fi
        sleep 0.1
        ticks=$((ticks + 1))
    done
}

run() {
    local addr host port
    addr=$(resolve_broker_addr)
    host=$(split_addr_host "$addr")
    port=$(split_addr_port "$addr")

    DEVICE_KEY_SEED=$(resolve_device_key_seed)
    export DEVICE_KEY_SEED

    local retention_days="${RETENTION_DAYS:-7}"
    local retention_secs=$((retention_days * 86400))
    local bucket_min="${TIME_BUCKET_MINUTES:-10}"
    local bucket_secs=$((bucket_min * 60))
    local prefix="${FRIGATE_TOPIC_PREFIX:-frigate}"
    local topic="${FRIGATE_MQTT_TOPIC:-${prefix}/events}"
    local publish="${SECURACV_PUBLISH:-true}"
    # The api block names the roll-call file only while the bridge keeps it
    # (as the add-on's run.sh does): with publishing off, GET /api/fleet lists
    # this kernel alone and an older file is kept for its pins, not served.
    local fleet_peers_api=""
    if [ "$publish" = "true" ]; then
        fleet_peers_api=", \"fleet_peers_path\": \"$FLEET_PEERS_FILE\""
    fi

    # Resolved before the broker wait so a mistyped mode fails in the first
    # line of the log, not after thirty seconds of waiting for mosquitto.
    configure_api_bind
    configure_fleet_rollcall

    log "broker=$addr topic=$topic retention=${retention_days}d bucket=${bucket_min}m publish=$publish api_bind=$API_BIND_MODE fleet_rollcall=$WITNESS_API_FLEET_ROLLCALL"
    if [ "$API_BIND_MODE" = "all" ]; then
        local rollcall_words="this kernel's own row only"
        [ "$WITNESS_API_FLEET_ROLLCALL" = full ] && rollcall_words="every Canary the bridge hears — SECURACV_FLEET_ROLLCALL=1"
        log "SECURACV_API_BIND=all: witness_api binds $API_ADDR. Across a published 8799 anyone on your LAN can reach GET /health, POST /api/pair (redeems the code 'docker exec securacv pair' prints; five wrong guesses burn it) and GET /api/fleet ($rollcall_words). Every data route needs a token: the rotating capability token (never leaves this container) or the paired token Home Assistant earns. Keep the port on your own network; it is plaintext HTTP."
    else
        log "SECURACV_API_BIND=loopback: witness_api binds $API_ADDR — this container only. Home Assistant cannot pair and no Witness Wall can read the roll-call; set SECURACV_API_BIND=all (the default) and publish 8799 for either."
    fi
    if [ -n "${SECURACV_ANNOUNCE_HOST:-}" ]; then
        log "SECURACV_ANNOUNCE_HOST=$SECURACV_ANNOUNCE_HOST: the announce names this host, so Home Assistant prefills http://$(announce_host_for_url):$API_PORT"
    fi

    # The broker may still be starting: compose `depends_on` (and the e2e
    # harness) only order container startup, they don't wait for mosquitto
    # to be listening. Retry briefly before giving up.
    local broker_wait="${BROKER_WAIT_SECS:-30}" waited=0
    until tcp_check "$host" "$port"; do
        if [ "$waited" -ge "$broker_wait" ]; then
            broker_unreachable_exit "$addr" "$waited"
        fi
        if [ "$waited" -eq 0 ]; then
            log "waiting for MQTT broker $addr to accept connections (up to ${broker_wait}s)..."
        fi
        sleep 1
        waited=$((waited + 1))
    done

    cat > "$CONFIG_FILE" <<EOF
{
  "db_path": "$DB_PATH",
  "ruleset_id": "ruleset:frigate_v1",
  "api": {
    "addr": "$API_ADDR",
    "token_path": "$TOKEN_FILE"$fleet_peers_api
  },
  "retention": {
    "seconds": $retention_secs
  }
}
EOF
    export WITNESS_CONFIG="$CONFIG_FILE"

    # The broker is another container, so a non-loopback address is expected.
    # This is safe for the same reason as in the HA add-on: only sanitized
    # event metadata flows over MQTT, never raw media, and every event still
    # passes contract enforcement before it is sealed.
    export ALLOW_REMOTE_MQTT=true

    trap stop_daemons TERM INT

    witness_api &
    DAEMON_PIDS+=($!)
    log "witness_api started (PID ${DAEMON_PIDS[-1]})"

    for _ in $(seq 1 30); do
        [ -s "$TOKEN_FILE" ] && break
        sleep 1
    done
    [ -s "$TOKEN_FILE" ] || die "witness_api did not write its capability token to $TOKEN_FILE"

    # The public HA device id must carry ZERO bits of the signing seed: see
    # ha_device_id_for_seed. (It changed once, from a seed prefix to this
    # hash — entity ids sensor.pwk_<id>_* moved with it.) Both daemons name
    # it: the bridge's retained `<prefix>/frigate_link` (contract C5) must
    # sit beside the publisher's state topics under the same `pwk_` id, or
    # Home Assistant shows the Frigate link on a device it does not have.
    local ha_device_id
    ha_device_id=$(ha_device_id_for_seed "$DEVICE_KEY_SEED")

    local bridge_args=(
        --mqtt-broker-addr "$addr"
        --frigate-topic-prefix "$prefix"
        --mqtt-topic-prefix "${MQTT_TOPIC_PREFIX:-witness}"
        --ha-device-id "$ha_device_id"
        --db-path "$DB_PATH"
        --bucket-size-secs "$bucket_secs"
        --retention-secs "$retention_secs"
    )
    # Legacy full-topic override wins over the prefix when explicitly set.
    [ -n "${FRIGATE_MQTT_TOPIC:-}" ] && bridge_args+=(--frigate-topic "$FRIGATE_MQTT_TOPIC")
    [ "${FRIGATE_ENABLE_REVIEWS:-false}" = "true" ] && bridge_args+=(--enable-reviews)
    [ -n "${FRIGATE_MIN_CONFIDENCE:-}" ] && bridge_args+=(--min-confidence "$FRIGATE_MIN_CONFIDENCE")
    [ -n "${FRIGATE_CAMERAS:-}" ] && bridge_args+=(--cameras "$FRIGATE_CAMERAS")
    [ -n "${FRIGATE_LABELS:-}" ] && bridge_args+=(--labels "$FRIGATE_LABELS")
    [ -n "${MQTT_USERNAME:-}" ] && bridge_args+=(--mqtt-username "$MQTT_USERNAME")
    # MQTT_PASSWORD stays in the environment (clap reads it there) — never on
    # argv, which anything in the container can read from /proc/<pid>/cmdline.

    frigate_bridge "${bridge_args[@]}" &
    DAEMON_PIDS+=($!)
    log "frigate_bridge started (PID ${DAEMON_PIDS[-1]})"

    if [ "$publish" = "true" ]; then
        local pub_args=(
            --daemon
            --mqtt-broker-addr "$addr"
            --api-token-path "$TOKEN_FILE"
            --fleet-peers-path "$FLEET_PEERS_FILE"
            --ha-discovery-prefix "${HA_DISCOVERY_PREFIX:-homeassistant}"
            --mqtt-topic-prefix "${MQTT_TOPIC_PREFIX:-witness}"
            --ha-device-id "$ha_device_id"
            --poll-interval "${POLL_INTERVAL:-30}"
            # The kernel's bucket, so the publisher's start cursor (one
            # bucket back on start, never the whole log) sits on the same
            # grid frigate_bridge seals on.
            --bucket-size-secs "$bucket_secs"
            --verify-interval-secs "${VERIFY_INTERVAL_SECS:-3600}"
            # The announce (retained, signed by the kernel key through the
            # API): the API's LAN posture, which the bridge cannot observe
            # itself, and that Home Assistant must pair with a code — a mode
            # word, never "a code is pending". The host rides along only when
            # the operator named one; a host is a network identifier.
            --announce-api-bind "$API_BIND_MODE"
            --announce-pairing required
        )
        [ -n "${SECURACV_ANNOUNCE_HOST:-}" ] && pub_args+=(--announce-api-host "$SECURACV_ANNOUNCE_HOST")
        [ -n "${MQTT_USERNAME:-}" ] && pub_args+=(--mqtt-username "$MQTT_USERNAME")
        # MQTT_PASSWORD stays in the environment (clap reads it there) — never
        # on argv; see frigate_bridge above.
        event_mqtt_bridge "${pub_args[@]}" &
        DAEMON_PIDS+=($!)
        log "event_mqtt_bridge started (PID ${DAEMON_PIDS[-1]})"
        log "fleet roll-call: $FLEET_PEERS_FILE (GET /api/fleet on $API_ADDR serves $WITNESS_API_FLEET_ROLLCALL)"
        log "announce: retained on ${MQTT_TOPIC_PREFIX:-witness}/$ha_device_id/announce — Home Assistant shows a discovered 'SecuraCV sidecar' card; pair it with: docker exec securacv pair"
    else
        log "HA Discovery publishing disabled (SECURACV_PUBLISH=$publish); no announce is published (Home Assistant discovers nothing), no bridge listens for Canaries, so GET /api/fleet lists this kernel only (an earlier roll-call file is kept for its pins, not served)"
    fi

    # Propagate the first child exit so the container restarts on failure —
    # unless the exit is the shutdown this script asked for.
    local status=0
    wait -n "${DAEMON_PIDS[@]}" || status=$?
    if [ "$SHUTDOWN_REQUESTED" -eq 1 ]; then
        # Give each daemon its clean exit (the bridge clears its retained
        # announce and publishes offline; frigate_bridge seals its record)
        # — bounded, so a daemon that cannot finish never turns a requested
        # stop into Docker's SIGKILL and a non-zero exit.
        if wait_daemons_bounded "$STOP_GRACE_SECS"; then
            log "sidecar stopped"
        else
            log "sidecar stopped (a daemon had to be ended; its retained MQTT state may be stale — see docs/frigate_integration.md, Clean shutdown and recovery)"
        fi
        exit 0
    fi
    log "a service exited (status $status); shutting the sidecar down"
    kill -TERM "${DAEMON_PIDS[@]}" 2>/dev/null || true
    wait_daemons_bounded "$STOP_GRACE_SECS" || true
    exit "$status"
}

case "${1:-run}" in
    doctor) shift; doctor "$@" ;;
    pair) shift; pair "$@" ;;
    unpair) shift; unpair "$@" ;;
    run) run ;;
    mint-viewer-token|revoke-viewer-token) viewer_token "$@" ;;
    *) exec "$@" ;;
esac
