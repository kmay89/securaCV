#!/bin/sh
set -eu
# Discover cameras from go2rtc API
# go2rtc is the standard RTSP proxy used by Home Assistant
#
# Usage: discover_cameras.sh [go2rtc_url]
# Returns: JSON array of camera configurations
#
# For tests: set GO2RTC_STREAMS_JSON to a go2rtc /api/streams payload to
# bypass the network fetch. The wizard's serve_wizard.py carries the same
# transform (_go2rtc_streams_to_cameras) and is the REFERENCE: tests/
# test_serve_wizard.py runs this script on its fixtures and fails on any row
# the two disagree about, so change the rules there first, then here.

GO2RTC_URL="${1:-http://localhost:1984}"
API_ENDPOINT="$GO2RTC_URL/api/streams"

# Fetch streams from go2rtc API (or take the injected test payload)
if [ -n "${GO2RTC_STREAMS_JSON:-}" ]; then
    streams="$GO2RTC_STREAMS_JSON"
else
    streams=$(curl -sf "$API_ENDPOINT" 2>/dev/null)
fi

if [ -z "$streams" ] || [ "$streams" = "null" ]; then
    echo "[]"
    exit 0
fi

# Transform go2rtc streams to witness-kernel camera format.
# go2rtc format: { "stream_name": { "producers": [...], "consumers": [...] } }
# Producers are OBJECTS carrying a "url" field (older builds emitted plain
# strings; both forms are handled). A URL is a non-empty string and nothing
# else: a null, a number or an object where one belongs is skipped, never
# stringified ("null" is not a camera). Streams with no usable producer URL
# are skipped too — a made-up URL would only fail later in ffmpeg with a
# worse error. One malformed stream skips that stream, not the whole
# payload: a jq error here would make run.sh fall back to "[]" and lose
# every good camera with it. Zone IDs must match zone:[a-z0-9_-]{1,64}, so
# the stream name is lowercased BEFORE the character sweep ("FrontDoor" ->
# "zone:frontdoor", not "zone:_ront_oor"). Rows come out sorted by name.
# Every `if` carries an `else`: the add-on's Alpine 3.18 ships jq 1.6.
echo "$streams" | jq -r '
    if type != "object" then [] else
    to_entries | sort_by(.key) | map(
        select(.value | type == "object") |
        ([ (.value.producers | if type == "array" then .[] else empty end)
           | if type == "object" then .url
             elif type == "string" then .
             else empty end
           | select(type == "string" and length > 0)
         ]) as $urls |
        select(($urls | length) > 0) |
        {
            name: .key,
            url: (
                # Prefer an RTSP producer URL, else the first usable one
                ($urls | map(select(startswith("rtsp://"))) | first) //
                ($urls | first)
            ),
            zone_id: "zone:\(.key | ascii_downcase | gsub("[^a-z0-9_-]"; "_") | .[0:64])",
            fps: 10,
            width: 640,
            height: 480,
            enabled: true
        }
    )
    end
'
