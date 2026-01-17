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
