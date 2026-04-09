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
# 2. Start FSM and TCK
# ----------------------------------------------------------------------
echo "[2/4] Starting FSM / TCK"

#$MPP/fsm/fsm 5000 &
$MPP/tck/tck 5010 &

sleep 0.5

# ----------------------------------------------------------------------
# 3. Configure TCK
# ----------------------------------------------------------------------
echo "[3/4] Configuring TCK for FSM"

echo '{"target_sba":5000}' | nc -u -w1 127.0.0.1 5010

sleep 0.5

# ----------------------------------------------------------------------
# 4. Load FSM and START
# ----------------------------------------------------------------------
echo "[4/4] Loading FSM definition and STARTing"

printf '{"verb":"PUT","resource":"fsm","body":{"fsm_text":%s,"target_sba_net":5001,"target_sba_xfr":5002,"tck_sba":5010,"obs_sba":5020}}' \
  "$(jq -Rs . < /usr/local/mpp/fsm/xfr-tx.puml)" \
  | nc -u -w1 127.0.0.1 5000

sleep 0.5

echo '{"step":true}' | nc -u -w1 127.0.0.1 5010

echo "=== MPP 105 startup complete ==="
