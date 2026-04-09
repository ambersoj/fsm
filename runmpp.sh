while true; do
  echo '{"tick":true}' | nc -u -w1 127.0.0.1 5000
  sleep 0.01
done