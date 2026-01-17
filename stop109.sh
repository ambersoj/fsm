#!/usr/bin/env bash
set -e

echo "=== Stopping MPP on 109 ==="

kill_port() {
  local port=$1
  local pid

  pid=$(lsof -ti :"$port" 2>/dev/null || true)

  if [[ -n "$pid" ]]; then
    echo "Stopping process on port $port (pid $pid)"
    kill "$pid"
    sleep 0.3
    if kill -0 "$pid" 2>/dev/null; then
      echo "  pid $pid still alive, forcing"
      kill -9 "$pid"
    fi
  else
    echo "No process on port $port"
  fi
}

# ----------------------------------------------------------------------
# 1. FSMs
# ----------------------------------------------------------------------
echo "[1/4] Stopping FSMs..."
kill_port 6002
kill_port 5002

# ----------------------------------------------------------------------
# 2. TCKs
# ----------------------------------------------------------------------
echo "[2/4] Stopping TCKs..."
kill_port 6001
kill_port 5001
kill_port 5003

# ----------------------------------------------------------------------
# 3. XFR / NET
# ----------------------------------------------------------------------
echo "[3/4] Stopping XFR and NET..."
kill_port 6000
kill_port 5000

# ----------------------------------------------------------------------
# 4. BLS
# ----------------------------------------------------------------------
echo "[4/4] Stopping BLS..."
kill_port 4000

echo "=== MPP 109 stopped ==="
