#!/bin/sh
# Discover cameras from go2rtc API
# go2rtc is the standard RTSP proxy used by Home Assistant
#
# Usage: discover_cameras.sh [go2rtc_url]
# Returns: JSON array of camera configurations

GO2RTC_URL="${1:-http://localhost:1984}"
API_ENDPOINT="$GO2RTC_URL/api/streams"

# Fetch streams from go2rtc API
streams=$(curl -sf "$API_ENDPOINT" 2>/dev/null)

if [ -z "$streams" ] || [ "$streams" = "null" ]; then
    echo "[]"
    exit 0
fi

# Transform go2rtc streams to witness-kernel camera format
# go2rtc format: { "stream_name": { "producers": [...], "consumers": [...] } }
# We want RTSP URLs from the producers

echo "$streams" | jq -r '
    to_entries | map(
        select(.value.producers != null and (.value.producers | length) > 0) |
        {
            name: .key,
            url: (
                # Try to find an RTSP producer URL
                (.value.producers | map(select(. | startswith("rtsp://"))) | first) //
                # Fall back to any producer
                (.value.producers | first) //
                "stub://\(.key)"
            ),
            zone_id: "zone:\(.key | gsub("[^a-z0-9_-]"; "_") | .[0:64])",
            fps: 10,
            width: 640,
            height: 480,
            enabled: true
        }
    )
'
