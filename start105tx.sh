#!/usr/bin/env bash
set -e

echo "=== MPP startup on 105 (PING) ==="

MPP=/usr/local/mpp

# ----------------------------------------------------------------------
# 1. Start core services
# ----------------------------------------------------------------------
echo "[1/4] Starting core services..."

$MPP/net/net 5001 &
$MPP/xfr/xfr 5002 &

sleep 0.5

# ----------------------------------------------------------------------
# 2. Start FSMs, TCKs, XFR
# ----------------------------------------------------------------------
echo "[2/4] Starting FSM / TCK"

$MPP/fsm/fsm 5000 &
$MPP/tck/tck 5010 &

sleep 1

# ----------------------------------------------------------------------
# 3. Load FSM
# ----------------------------------------------------------------------
echo "[3/4] Loading FSM definition"

printf '{"verb":"PUT","resource":"fsm","body":{"fsm_text":%s,"target_sba_net":5001,"target_sba_xfr":5002,"tck_sba":5010,"run":true}}' \
  "$(jq -Rs . < /usr/local/mpp/fsm/xfr-tx.puml)" \
  | nc -u -w1 127.0.0.1 5000

sleep 0.5

# ----------------------------------------------------------------------
# 4. Enable TCK and start flow
# ----------------------------------------------------------------------
echo "[4/4] Enabling TCK"

echo '{"enable":true,"target_sba":5000}' | nc -u -w1 127.0.0.1 5010

echo "=== MPP 105 startup complete ==="
