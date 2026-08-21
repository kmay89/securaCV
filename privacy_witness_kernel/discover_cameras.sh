#!/bin/sh
set -eu
# Discover cameras from go2rtc API
# go2rtc is the standard RTSP proxy used by Home Assistant
#
# Usage: discover_cameras.sh [go2rtc_url]
# Returns: JSON array of camera configurations
#
# For tests: set GO2RTC_STREAMS_JSON to a go2rtc /api/streams payload to
# bypass the network fetch (tests/test_run_lib.sh exercises the transform).

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
# strings; both forms are handled). Streams with no usable producer URL are
# skipped — a made-up URL would only fail later in ffmpeg with a worse error.
# Zone IDs must match zone:[a-z0-9_-]{1,64}, so the stream name is lowercased
# BEFORE the character sweep ("FrontDoor" -> "zone:frontdoor", not
# "zone:_ront_oor").
echo "$streams" | jq -r '
    to_entries | map(
        select(.value.producers != null and (.value.producers | length) > 0) |
        ([ .value.producers[]
           | if type == "object" then (.url // empty) else tostring end
           | select(length > 0)
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
'
