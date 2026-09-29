#!/bin/bash
# Usage: mcp.sh init | mcp.sh call <toolset_name> <tool_name> '<arguments json>' | mcp.sh raw '<jsonrpc body>'
DIR="$(cd "$(dirname "$0")" && pwd)"
URL="http://127.0.0.1:8000/mcp"
SID_FILE="${TMPDIR:-/tmp}/nanogs_mcp_session"
TIMEOUT="${MCP_TIMEOUT:-120}"

post() {
  local sid=""
  [ -f "$SID_FILE" ] && sid=$(cat "$SID_FILE")
  curl -s --max-time "$TIMEOUT" -X POST "$URL" \
    -H "Content-Type: application/json" \
    -H "Accept: application/json, text/event-stream" \
    ${sid:+-H "Mcp-Session-Id: $sid"} \
    --data-binary "$1"
}

case "$1" in
  init)
    rm -f "$SID_FILE"
    hdrs=$(mktemp)
    curl -s --max-time 30 -D "$hdrs" -X POST "$URL" \
      -H "Content-Type: application/json" \
      -H "Accept: application/json, text/event-stream" \
      --data-binary '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"claude-curl","version":"1.0"}}}' >/dev/null
    sid=$(grep -i '^mcp-session-id:' "$hdrs" | awk '{print $2}' | tr -d '\r\n')
    rm -f "$hdrs"
    [ -z "$sid" ] && { echo "no session id" >&2; exit 1; }
    echo "$sid" > "$SID_FILE"
    post '{"jsonrpc":"2.0","method":"notifications/initialized"}' >/dev/null
    echo "session $sid"
    ;;
  call)
    args="$4"; [ -z "$args" ] && args='{}'
    body=$(python3 -c 'import json,sys; print(json.dumps({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"call_tool","arguments":{"toolset_name":sys.argv[1],"tool_name":sys.argv[2],"arguments":json.loads(sys.argv[3])}}}))' "$2" "$3" "$args")
    post "$body"
    ;;
  raw)
    post "$2"
    ;;
  *)
    echo "usage: $0 init | call <toolset> <tool> '<json>' | raw '<json>'" >&2; exit 2;;
esac
