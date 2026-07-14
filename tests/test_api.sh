#!/usr/bin/env bash
set -u

BASE="${BASE:-http://localhost:8099}"
KEY="${API_KEY:-}"
PASS=0
FAIL=0

if [ -z "$KEY" ]; then
  echo "API_KEY is required"
  exit 2
fi

request() {
  local method="$1" path="$2" body="${3:-}"
  local args=(-sS -X "$method" "$BASE$path" -H "X-API-Key: $KEY")
  if [ -n "$body" ]; then
    args+=(-H "Content-Type: application/json" --data "$body")
  fi
  curl "${args[@]}" -w $'\n%{http_code}'
}

assert_code() {
  local description="$1" expected="$2" response="$3"
  local code="${response##*$'\n'}"
  if [ "$code" = "$expected" ]; then
    echo "PASS: $description"
    PASS=$((PASS + 1))
  else
    echo "FAIL: $description (expected $expected, got $code)"
    FAIL=$((FAIL + 1))
  fi
}

assert_body() {
  local description="$1" pattern="$2" response="$3"
  local body="${response%$'\n'*}"
  if printf '%s' "$body" | grep -qE "$pattern"; then
    echo "PASS: $description"
    PASS=$((PASS + 1))
  else
    echo "FAIL: $description (body: $body)"
    FAIL=$((FAIL + 1))
  fi
}

unauthorized=$(curl -sS "$BASE/jobs" -w $'\n%{http_code}')
assert_code "missing API key is rejected" 401 "$unauthorized"

me=$(request GET /auth/me)
assert_code "authenticated identity endpoint" 200 "$me"
assert_body "identity includes role" '"role":"(admin|user)"' "$me"

raw=$(request POST /jobs '{"command":"echo unsafe"}')
assert_code "raw commands are rejected in app_only mode" 400 "$raw"

job=$(request POST /jobs '{"app_id":"app1","parameters":{"enable_logging":false,"parallel_mode":true,"dry_run":false},"req_cores":9999}')
assert_code "registered application job is accepted" 201 "$job"
assert_body "server-owned app resources override client values" '"req_cores":4' "$job"

injection=$(request POST /jobs '{"app_id":"app2","parameters":{"algorithm":"fast;whoami","verbose":false}}')
assert_code "shell metacharacters are rejected" 400 "$injection"

unknown=$(request POST /jobs '{"app_id":"app1","parameters":{"unknown":true}}')
assert_code "unknown app parameters are rejected" 400 "$unknown"

invalid_app=$(request POST /admin/apps '{"app_id":"invalid-schema-smoke","name":"Invalid schema","command_template":"echo","req_cores":1,"req_ram_mb":0,"req_disk_mb":0,"req_gpu":0,"fields":[{"name":"unsafe'\''field","type":"text"}]}')
assert_code "unsafe application field names are rejected" 400 "$invalid_app"

key_label="api-smoke-revoke-$$"
secondary_key=$(request POST /admin/keys "{\"label\":\"$key_label\",\"role\":\"admin\",\"user_id\":\"\"}")
assert_code "secondary admin key is created" 201 "$secondary_key"

keys=$(request GET /admin/keys)
assert_code "API keys can be listed" 200 "$keys"
keys_body="${keys%$'\n'*}"
key_hash=$(printf '%s' "$keys_body" | tr '{' '\n' | grep -F "\"label\":\"$key_label\"" | sed -n 's/.*"key_hash":"\([0-9a-f]\{64\}\)".*/\1/p' | head -n 1)
if [ -n "$key_hash" ]; then
  revoke=$(request DELETE /admin/keys "{\"key_hash\":\"$key_hash\"}")
  assert_code "API key is revoked by administrative hash" 200 "$revoke"
else
  echo "FAIL: key listing omits the administrative hash"
  FAIL=$((FAIL + 1))
fi

traversal=$(request GET '/jobs/%2e%2e/log')
assert_code "job path traversal is hidden" 404 "$traversal"

held=$(request POST /jobs '{"app_id":"app1","parameters":{"enable_logging":false,"parallel_mode":false,"dry_run":true},"input_files":["input data.txt"]}')
assert_code "job with expected input is accepted" 201 "$held"
held_body="${held%$'\n'*}"
held_id=$(printf '%s' "$held_body" | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')
assert_body "job waits in HELD state" '"status":"HELD"' "$held"

if [ -n "$held_id" ]; then
  upload=$(curl -sS -X POST "$BASE/jobs/$held_id/input/input%20data.txt" \
    -H "X-API-Key: $KEY" -H "Content-Type: application/octet-stream" \
    --data-binary 'test-data' -w $'\n%{http_code}')
  assert_code "URL-decoded safe filename uploads" 200 "$upload"
else
  echo "FAIL: held job id was not returned"
  FAIL=$((FAIL + 1))
fi

headers=$(curl -sS -D - -o /dev/null "$BASE/")
if printf '%s' "$headers" | grep -qi '^X-Content-Type-Options: nosniff'; then
  echo "PASS: security headers are present"
  PASS=$((PASS + 1))
else
  echo "FAIL: security headers are missing"
  FAIL=$((FAIL + 1))
fi

csp=$(printf '%s' "$headers" | grep -i '^Content-Security-Policy:' || true)
if printf '%s' "$csp" | grep -q "script-src 'self'" && \
   ! printf '%s' "$csp" | grep -qE 'script-src[^;]*unsafe-inline'; then
  echo "PASS: CSP blocks inline JavaScript"
  PASS=$((PASS + 1))
else
  echo "FAIL: CSP still allows inline JavaScript ($csp)"
  FAIL=$((FAIL + 1))
fi

echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
