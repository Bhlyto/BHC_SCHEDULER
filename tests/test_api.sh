#!/usr/bin/env bash
# tests/test_api.sh  —  Plan de test complet (Linux / macOS)
 Usage: API_KEY=<votre-cle> ./tests/test_api.sh
#        API_KEY=<cle> BASE=http://192.168.1.10:8080 ./tests/test_api.sh

BASE="${BASE:-http://localhost:8080}"
KEY="${API_KEY:-changeme}"
JOB_TIMEOUT="${JOB_TIMEOUT:-15}"   # secondes max d'attente par job
PASS=0; FAIL=0; SKIP=0

GREEN='\033[0;32m'; RED='\033[0;31m'; YEL='\033[0;33m'; CYA='\033[0;36m'; RST='\033[0m'

pass() { echo -e "${GREEN}  [PASS]${RST} $1";            PASS=$((PASS+1)); }
fail() { echo -e "${RED}  [FAIL]${RST} $1  >> $2";      FAIL=$((FAIL+1)); }
skip() { echo -e "${YEL}  [SKIP]${RST} $1  ($2)";       SKIP=$((SKIP+1)); }

check() {
    local desc="$1" pat="$2" actual="$3"
    if echo "$actual" | grep -qE "$pat"; then pass "$desc"
    else fail "$desc" "expected '$pat' in: $actual"; fi
}

req() {
    local method="$1" path="$2" body="$3"
    local args=(-s -X "$method" "$BASE$path" -H "X-API-Key: $KEY" -H "Content-Type: application/json")
    [ -n "$body" ] && args+=(-d "$body")
    curl "${args[@]}"
}

req_code() {
    local method="$1" path="$2"
    curl -s -o /dev/null -w '%{http_code}' -X "$method" "$BASE$path" -H "X-API-Key: $KEY" -H "Content-Type: application/json"
}

wait_job() {
    local id="$1" deadline=$(( $(date +%s) + JOB_TIMEOUT ))
    while [ $(date +%s) -lt $deadline ]; do
        sleep 0.5
        local r; r=$(req GET "/jobs/$id")
        echo "$r" | grep -qE 'FINISHED|FAILED|CANCELLED' && { echo "$r"; return; }
    done
    req GET "/jobs/$id"
}

echo -e "${CYA}╔══════════════════════════════════════════════╗"
echo -e "║   BHC-SCHEDULER  —  Plan de test complet    ║"
echo -e "╚══════════════════════════════════════════════╝${RST}"

# ╔══════════════════════════════════════════╗
# ║  1. AUTHENTIFICATION                     ║
# ╚══════════════════════════════════════════╝
echo -e "\n${CYA}══ 1. Authentification${RST}"

code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE/jobs")
check 'Requête sans clé → 401' '401' "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE/jobs" -H "X-API-Key: INVALID")
check 'Mauvaise clé → 401' '401' "$code"

code=$(req_code GET '/jobs')
check 'Bonne clé → 200 sur /jobs' '200' "$code"

# ╔══════════════════════════════════════════╗
# ║  2. SOUMISSION / VALIDATION DE JOB       ║
# ╚══════════════════════════════════════════╝
echo -e "\n${CYA}══ 2. Soumission de job${RST}"

r=$(req POST '/jobs' '{"command":"echo hello","priority":10,"cores":1,"ram_mb":64}')
check 'Soumission minimale → IN_QUEUE'     'IN_QUEUE' "$r"
check 'Réponse contient un id UUID'         '[0-9a-f-]{36}' "$r"
JOB_BASIC=$(echo "$r" | grep -oE '"id":"[^"]+"' | head -1 | cut -d'"' -f4)
echo "  Job ID basique: $JOB_BASIC"

r=$(req POST '/jobs' '{"command":"echo full","priority":5,"cores":1,"gpu":0,"ram_mb":128,"disk_mb":256}')
check 'Soumission avec tous les champs → IN_QUEUE' 'IN_QUEUE' "$r"

r=$(req POST '/jobs' '{"priority":1}')
check 'Sans command → erreur 400'           '400|error|command' "$r"

# ╔══════════════════════════════════════════╗
# ║  3. LECTURE / LISTE DES JOBS             ║
# ╚══════════════════════════════════════════╝
echo -e "\n${CYA}══ 3. Lecture des jobs${RST}"

r=$(req GET '/jobs')
check 'GET /jobs contient le job soumis'    "$JOB_BASIC" "$r"

r=$(req GET "/jobs/$JOB_BASIC")
check 'GET /jobs/:id retourne le bon id'    "$JOB_BASIC" "$r"
check 'GET /jobs/:id contient command'      'echo hello' "$r"
check 'GET /jobs/:id contient status'       'IN_QUEUE|STARTING|RUNNING|FINISHED|FAILED' "$r"

r=$(req GET '/jobs/JOB_ID')
check 'Job inexistant → 404'                '404|not.found' "$r"

# ╔══════════════════════════════════════════╗
# ║  4. EXÉCUTION ET ÉTAT TERMINAL           ║
# ╚══════════════════════════════════════════╝
echo -e "\n${CYA}══ 4. Exécution — job simple${RST}"

r=$(wait_job "$JOB_BASIC")
check 'Job echo hello → FINISHED'          'FINISHED' "$r"
check 'Exit code = 0'                       '"exit_code":0' "$r"
check 'machine_id renseigné'                'machine_id' "$r"

r=$(req POST '/jobs' '{"command":"/bin/false","priority":1,"cores":1,"ram_mb":64}')
FAIL_ID=$(echo "$r" | grep -oE '"id":"[^"]+"' | head -1 | cut -d'"' -f4)
r=$(wait_job "$FAIL_ID")
check 'Commande qui échoue → FAILED'        'FAILED' "$r"

# ╔══════════════════════════════════════════╗
# ║  5. LOGS DU JOB                          ║
# ╚══════════════════════════════════════════╝
echo -e "\n${CYA}══ 5. Logs du job${RST}"

r=$(req POST '/jobs' '{"command":"echo stdout_test; echo stderr_test >&2","priority":1,"cores":1,"ram_mb":64}')
LOG_ID=$(echo "$r" | grep -oE '"id":"[^"]+"' | head -1 | cut -d'"' -f4)
r=$(wait_job "$LOG_ID" ); check 'Job log → FINISHED' 'FINISHED' "$r"

stdout_content=$(curl -s "$BASE/jobs/$LOG_ID/log" -H "X-API-Key: $KEY")
stdout_code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE/jobs/$LOG_ID/log" -H "X-API-Key: $KEY")
check 'GET /jobs/:id/log → 200'             '200' "$stdout_code"
check 'GET /jobs/:id/log contient stdout'   'stdout_test' "$stdout_content"

stderr_content=$(curl -s "$BASE/jobs/$LOG_ID/log/stderr" -H "X-API-Key: $KEY")
stderr_code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE/jobs/$LOG_ID/log/stderr" -H "X-API-Key: $KEY")
check 'GET /jobs/:id/log/stderr → 200'      '200' "$stderr_code"
check 'GET /jobs/:id/log/stderr contient'   'stderr_test' "$stderr_content"

code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE/jobs/JOB_ID/log" -H "X-API-Key: $KEY")
check 'Log job inexistant → 404'            '404' "$code"

# ╔══════════════════════════════════════════╗
# ║  6. UPLOAD / DOWNLOAD FICHIER            ║
# ╚══════════════════════════════════════════╝
echo -e "\n${CYA}══ 6. Upload / Download${RST}"

r=$(req POST '/jobs' '{"command":"cat input/data.txt","priority":1,"cores":1,"ram_mb":64}')
IO_ID=$(echo "$r" | grep -oE '"id":"[^"]+"' | head -1 | cut -d'"' -f4)

echo 'hello-from-test' > /tmp/orch_test_data.txt
r=$(curl -s -X POST "$BASE/jobs/$IO_ID/input/data.txt" \
    -H "X-API-Key: $KEY" --data-binary @/tmp/orch_test_data.txt)
check 'Upload input → bytes > 0'            'bytes' "$r"

r=$(wait_job "$IO_ID" 20); check 'Job avec input → FINISHED' 'FINISHED' "$r"

code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE/jobs/$IO_ID/output" -H "X-API-Key: $KEY")
if [ "$code" = '200' ]; then pass 'Download output → 200'
elif [ "$code" = '404' ]; then skip 'Download output' 'aucun fichier output (normal pour cat)'
else fail 'Download output accessible' "code=$code"; fi

rm -f /tmp/orch_test_data.txt

# ╔══════════════════════════════════════════╗
# ║  7. ANNULATION DE JOB                    ║
# ╚══════════════════════════════════════════╝
echo -e "\n${CYA}══ 7. Annulation${RST}"

r=$(req POST '/jobs' '{"command":"sleep 9999","priority":99,"cores":1,"ram_mb":64}')
CANCEL_ID=$(echo "$r" | grep -oE '"id":"[^"]+"' | head -1 | cut -d'"' -f4)
sleep 0.3
r=$(req DELETE "/jobs/$CANCEL_ID")
check 'DELETE /jobs/:id → CANCELLED'        'CANCELLED|ok' "$r"

r=$(req GET "/jobs/$CANCEL_ID")
check 'Status après cancel = CANCELLED'     'CANCELLED' "$r"

r=$(req DELETE '/jobs/JOB_ID')
check 'Cancel job inexistant → 404/error'   '404|not.found|error' "$r"

# ╔══════════════════════════════════════════╗
# ║  8. RESSOURCES ET MACHINES               ║
# ╚══════════════════════════════════════════╝
echo -e "\n${CYA}══ 8. Ressources${RST}"

r=$(req GET '/resources')
check 'GET /resources présent'              '.' "$r"
check '/resources contient cores'           'cores' "$r"

# ╔══════════════════════════════════════════╗
# ║  9. PROVISIONING DYNAMIQUE               ║
# ╚══════════════════════════════════════════╝
echo -e "\n${CYA}══ 9. Provisioning dynamique${RST}"

m='{"id":"test-worker","hostname":"test-worker.local","ip":"192.168.99.10","cores":8,"ram_mb":16384,"disk_mb":204800}'
r=$(req POST '/provision' "$m")
check 'POST /provision ajoute machine'      'true|ok' "$r"

r=$(req GET '/resources')
check '/resources contient test-worker'     'test-worker' "$r"

r=$(req DELETE '/provision/test-worker')
check 'DELETE /provision/:id supprime'      'true|ok' "$r"

r=$(req GET '/resources')
if ! echo "$r" | grep -q 'test-worker'; then pass 'Machine supprimée absente de /resources'
else fail 'Machine supprimée absente de /resources' 'toujours présente'; fi

r=$(req DELETE '/provision/machine-inexistante')
check 'Suppression machine inexistante → erreur' '404|not.found|error' "$r"

# ╔══════════════════════════════════════════╗
# ║  10. STATISTIQUES                        ║
# ╚══════════════════════════════════════════╝
echo -e "\n${CYA}══ 10. Statistiques${RST}"

r=$(req GET '/stats')
check 'GET /stats accessible'               '.' "$r"
check '/stats contient jobs'                'jobs' "$r"
check '/stats contient total'               'total' "$r"

# ╔══════════════════════════════════════════╗
# ║  11. SSE — EVENTS TEMPS RÉEL             ║
# ╚══════════════════════════════════════════╝
echo -e "\n${CYA}══ 11. SSE /jobs/events${RST}"

# Lit 1 seconde de l'event stream et vérifie le Content-Type
sse_head=$(curl -s -I --max-time 1 "$BASE/jobs/events" -H "X-API-Key: $KEY" 2>&1 || true)
check 'GET /jobs/events → text/event-stream' 'text/event-stream' "$sse_head"

# Vérifie qu'un événement arrive après soumission d'un job
outfile=$(mktemp)
curl -s -N --max-time 3 "$BASE/jobs/events" -H "X-API-Key: $KEY" > "$outfile" 2>&1 &
SSE_PID=$!
req POST '/jobs' '{"command":"echo sse_trigger","priority":1,"cores":1,"ram_mb":64}' > /dev/null
sleep 2; kill $SSE_PID 2>/dev/null; wait $SSE_PID 2>/dev/null
sse_data=$(cat "$outfile"); rm -f "$outfile"
check 'SSE reçoit un événement job_status'  'job_status|data:' "$sse_data"

# ╔══════════════════════════════════════════╗
# ║  12. MULTI-MACHINE                       ║
# ╚══════════════════════════════════════════╝
echo -e "\n${CYA}══ 12. Multi-machine${RST}"

req POST '/provision' '{"id":"mm-node1","hostname":"mm-node1","ip":"10.0.0.1","cores":2,"ram_mb":4096,"disk_mb":51200}' > /dev/null
req POST '/provision' '{"id":"mm-node2","hostname":"mm-node2","ip":"10.0.0.2","cores":2,"ram_mb":4096,"disk_mb":51200}' > /dev/null

r=$(req POST '/jobs' '{"command":"echo multi","priority":1,"cores":4,"ram_mb":256,"disk_mb":1024}')
MM_ID=$(echo "$r" | grep -oE '"id":"[^"]+"' | head -1 | cut -d'"' -f4)
check 'Job multi-machine → soumis'          'IN_QUEUE|STARTING|RUNNING' "$r"

r=$(wait_job "$MM_ID" 20)
check 'Job multi-machine → FINISHED'        'FINISHED' "$r"
nm=$(echo "$r" | grep -oE '"n_machines":[0-9]+' | grep -oE '[0-9]+' || echo '1')
if [ "$nm" -gt 1 ]; then pass "n_machines=$nm (multi-machine confirmé)"
else skip 'Multi-machine effectif' "n_machines=$nm — machines locales peut-être suffisantes"; fi

req DELETE '/provision/mm-node1' > /dev/null
req DELETE '/provision/mm-node2' > /dev/null

# ╔══════════════════════════════════════════╗
# ║  13. PRIORITÉ                            ║
# ╚══════════════════════════════════════════╝
echo -e "\n${CYA}══ 13. Priorité${RST}"

r=$(req POST '/jobs' '{"command":"echo low",   "priority":100,"cores":1,"ram_mb":64}')
LOW_ID=$(echo "$r" | grep -oE '"id":"[^"]+"' | head -1 | cut -d'"' -f4)
r=$(req POST '/jobs' '{"command":"echo high",  "priority":1,  "cores":1,"ram_mb":64}')
HIGH_ID=$(echo "$r" | grep -oE '"id":"[^"]+"' | head -1 | cut -d'"' -f4)

r=$(wait_job "$HIGH_ID" 15)
check 'Job haute priorité → FINISHED'       'FINISHED' "$r"

# ╔══════════════════════════════════════════╗
# ║  14. PURGE                               ║
# ╚══════════════════════════════════════════╝
echo -e "\n${CYA}══ 14. Purge${RST}"

# Submit a quick job and wait for completion
r=$(req POST '/jobs' '{"command":"echo purge_test","priority":1,"cores":1,"ram_mb":64}')
PURGE_JID=$(echo "$r" | grep -oE '"id":"[^"]+"' | head -1 | cut -d'"' -f4)
wait_job "$PURGE_JID" 15 > /dev/null

# Purge all terminal jobs
r=$(req DELETE '/jobs')
check 'DELETE /jobs → deleted field'   '"deleted":[0-9]' "$r"
check 'DELETE /jobs → cleaned field'   '"cleaned":[0-9]' "$r"

# Confirm purged job is gone
r=$(req GET "/jobs/$PURGE_JID")
check 'Job purgé → absent (404)'       '404\|not.found\|error' "$r"

# ══════════════════════════════════════════
#  RÉSULTAT FINAL
# ══════════════════════════════════════════
echo ''
echo -e "${CYA}╔═══════════════════════════════════╗"
echo -e "║  PASS: $PASS   FAIL: $FAIL   SKIP: $SKIP"
echo -e "╚═══════════════════════════════════╝${RST}"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1


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
    -H "Content-Type: application/octet-stream" \
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
