#!/usr/bin/env bash
set -e

echo "=== MPP startup on 105 (XFR SEND) ==="

MPP=/usr/local/mpp

# ----------------------------------------------------------------------
# 1. Create executable variants
# ----------------------------------------------------------------------
echo "[1/5] Preparing executables..."

cp -f $MPP/fsm/fsm $MPP/fsm/fsm-net
cp -f $MPP/fsm/fsm $MPP/fsm/fsm-xfr

cp -f $MPP/tck/tck $MPP/tck/net-tck
cp -f $MPP/tck/tck $MPP/tck/fsm-net-tck
cp -f $MPP/tck/tck $MPP/tck/fsm-xfr-tck

# ----------------------------------------------------------------------
# 2. Start core services
# ----------------------------------------------------------------------
echo "[2/5] Starting core services..."

$MPP/bls/bls 4000 &
$MPP/net/net 5000 &

sleep 0.5

# ----------------------------------------------------------------------
# 3. Start FSMs, TCKs, XFR
# ----------------------------------------------------------------------
echo "[3/5] Starting FSM / TCK / XFR..."

$MPP/fsm/fsm-net-tck 5001 &
$MPP/tck/fsm-net 5002 &
$MPP/tck/net-tck 5003 &

$MPP/xfr/xfr 6000 &
$MPP/tck/fsm-xfr-tck 6001 &
$MPP/fsm/fsm-xfr 6002 &

sleep 1

# ----------------------------------------------------------------------
# 4. Load FSMs
# ----------------------------------------------------------------------
echo "[4/5] Loading FSM definitions..."

printf '{"verb":"PUT","resource":"fsm","body":{"fsm_text":%s,"target_sba":5000,"tck_sba":5001,"run":true}}' \
  "$(jq -Rs . < /usr/local/mpp/fsm/fsm-net.puml)" \
  | nc -u -w1 127.0.0.1 5002

printf '{"verb":"PUT","resource":"fsm","body":{"fsm_text":%s,"target_sba":6000,"tck_sba":6001,"run":true}}' \
  "$(jq -Rs . < /usr/local/mpp/fsm/xfr-send.puml)" \
  | nc -u -w1 127.0.0.1 6002

sleep 0.5

# ----------------------------------------------------------------------
# 5. Enable TCKs and start flow
# ----------------------------------------------------------------------
echo "[5/5] Enabling TCKs and starting XFR..."

echo '{"enable":true,"target_sba":5000}' | nc -u -w1 127.0.0.1 5003
echo '{"enable":true,"target_sba":5002}' | nc -u -w1 127.0.0.1 5001
echo '{"enable":true,"target_sba":6002}' | nc -u -w1 127.0.0.1 6001

echo '{"belief":{"subject":"FSM.XFR.start","polarity":true}}' \
  | nc -u -w1 127.0.0.1 4000

echo "=== MPP 105 startup complete ==="
