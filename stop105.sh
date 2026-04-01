#!/usr/bin/env bash
set -e

echo "=== Stopping MPP on 105 ==="

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
# 1. FSM
# ----------------------------------------------------------------------
echo "[1/3] Stopping FSM..."
kill_port 5001

# ----------------------------------------------------------------------
# 2. TCK
# ----------------------------------------------------------------------
echo "[2/3] Stopping TCK..."
kill_port 5002

# ----------------------------------------------------------------------
# 3. XFR / NET
# ----------------------------------------------------------------------
echo "[3/3] Stopping NET..."
kill_port 5000

echo "=== MPP 105 stopped ==="
