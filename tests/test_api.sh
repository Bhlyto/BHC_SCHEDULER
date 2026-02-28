#!/usr/bin/env bash
# tests/test_api.sh
# Integration test: exercises all API routes using curl.
# Usage: API_KEY=<your-key> ./tests/test_api.sh

BASE="http://localhost:8080"
KEY="${API_KEY:-changeme}"
PASS=0
FAIL=0

check() {
    local desc="$1" expected="$2" actual="$3"
    if echo "$actual" | grep -q "$expected"; then
        echo "  PASS  $desc"
        PASS=$((PASS+1))
    else
        echo "  FAIL  $desc  (expected '$expected' in: $actual)"
        FAIL=$((FAIL+1))
    fi
}

echo "=== Orchestrator API Tests ==="

# ── Auth check ────────────────────────────────────
echo ""
echo "-- Auth --"
r=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/jobs")
check "No key → 401" "401" "$r"

# ── Submit job ────────────────────────────────────
echo ""
echo "-- Submit job --"
r=$(curl -s -X POST "$BASE/jobs" \
    -H "X-API-Key: $KEY" \
    -H "Content-Type: application/json" \
    -d '{"command":"echo hello","priority":10,"cores":1,"ram_mb":64}')
check "201 created" "IN_QUEUE" "$r"
JOB_ID=$(echo "$r" | grep -o '"id":"[^"]*"' | cut -d'"' -f4)
echo "  Job ID: $JOB_ID"

# ── List jobs ─────────────────────────────────────
echo ""
echo "-- List jobs --"
r=$(curl -s "$BASE/jobs" -H "X-API-Key: $KEY")
check "List contains job" "$JOB_ID" "$r"

# ── Get job ───────────────────────────────────────
echo ""
echo "-- Get job --"
r=$(curl -s "$BASE/jobs/$JOB_ID" -H "X-API-Key: $KEY")
check "Get job by id" "$JOB_ID" "$r"

# ── Upload input ──────────────────────────────────
echo ""
echo "-- Upload input --"
echo "hello world" > /tmp/test_input.txt
r=$(curl -s -X POST "$BASE/jobs/$JOB_ID/input/test_input.txt" \
    -H "X-API-Key: $KEY" \
    --data-binary @/tmp/test_input.txt)
check "Upload bytes > 0" "bytes" "$r"

# ── Resources ─────────────────────────────────────
echo ""
echo "-- Resources --"
r=$(curl -s "$BASE/resources" -H "X-API-Key: $KEY")
check "Resources list" "cores_total" "$r"

# ── Wait for job to finish ────────────────────────
echo ""
echo "-- Wait for FINISHED (up to 10s) --"
for i in $(seq 1 20); do
    STATUS=$(curl -s "$BASE/jobs/$JOB_ID" -H "X-API-Key: $KEY" | grep -o '"status":"[^"]*"' | cut -d'"' -f4)
    echo "  status: $STATUS"
    if [ "$STATUS" = "FINISHED" ] || [ "$STATUS" = "FAILED" ]; then break; fi
    sleep 0.5
done
check "Job reached terminal state" "FINISHED\|FAILED" "$STATUS"

# ── Provision: add machine ────────────────────────
echo ""
echo "-- Provision --"
r=$(curl -s -X POST "$BASE/provision" \
    -H "X-API-Key: $KEY" \
    -H "Content-Type: application/json" \
    -d '{"id":"worker1","hostname":"worker1.local","ip":"192.168.1.10","cores":8,"gpu_count":1,"ram_mb":16384,"disk_mb":204800}')
check "Add machine" "ok" "$r"

r=$(curl -s -X DELETE "$BASE/provision/worker1" -H "X-API-Key: $KEY")
check "Remove machine" "ok" "$r"

# ── Cancel a queued job ───────────────────────────
echo ""
echo "-- Cancel job --"
r=$(curl -s -X POST "$BASE/jobs" \
    -H "X-API-Key: $KEY" \
    -H "Content-Type: application/json" \
    -d '{"command":"sleep 9999","priority":99,"cores":1}')
CJ=$(echo "$r" | grep -o '"id":"[^"]*"' | cut -d'"' -f4)
r=$(curl -s -X DELETE "$BASE/jobs/$CJ" -H "X-API-Key: $KEY")
check "Cancel job" "CANCELLED" "$r"

# ── Summary ───────────────────────────────────────
echo ""
echo "=============================="
echo "  Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
