#!/usr/bin/env bash
# Masaüstünde çalan sesi CRD'nin okuduğu boruya yazar.
# Öntanımlı çıkış değişmez: hoparlör çalmaya devam eder, bağlanan istemci de duyar.
# CRD 157, CHROME_REMOTE_DESKTOP_AUDIO_PIPE doluysa bu boruyu (48 kHz, s16le, stereo) okur.
set -u

RUNTIME="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
DIR="${RUNTIME}/crd-for-linux-audio"
FIFO="${DIR}/fifo"
PIDFILE="${DIR}/kayit.pid"
LOG="${DIR}/kayit.log"

monitor_sink() {
  local sink
  sink="$(pactl get-default-sink 2>/dev/null || true)"
  [[ -n "${sink}" ]] || return 1
  # pw-cat hedefi PipeWire düğüm adıdır. "sink.monitor" düğüm değildir;
  # bulunamazsa kayıt öntanımlı kaynağa, yani mikrofona düşer.
  printf '%s\n' "${sink}"
}

ensure_fifo() {
  mkdir -p "${DIR}"
  chmod 700 "${DIR}"
  if [[ ! -p "${FIFO}" ]]; then
    rm -f "${FIFO}"
    mkfifo -m 600 "${FIFO}"
  fi
}

loop_alive() {
  [[ -f "${PIDFILE}" ]] || return 1
  local pid
  pid="$(cat "${PIDFILE}" 2>/dev/null || true)"
  [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null
}

cmd_loop() {
  cd /
  local mon now kid
  while true; do
    mon="$(monitor_sink)" || {
      sleep 1
      continue
    }
    # stream.capture.sink: hedef hoparlörün monitör kapılarıdır, mikrofon değil.
    pw-cat -r --raw --format s16 --rate 48000 --channels 2 \
      -P 'stream.capture.sink=true' \
      --target "${mon}" "${FIFO}" &
    kid=$!
    while kill -0 "${kid}" 2>/dev/null; do
      now="$(monitor_sink)" || now=""
      if [[ -n "${now}" && "${now}" != "${mon}" ]]; then
        kill "${kid}" 2>/dev/null || true
        break
      fi
      sleep 1
    done
    wait "${kid}" 2>/dev/null || true
    sleep 0.2
  done
}

cmd_start() {
  ensure_fifo
  if loop_alive; then
    printf '%s\n' "${FIFO}"
    return 0
  fi
  mkdir -p "${DIR}"
  # setsid: kayıt, başlatan kabuk kapanınca da ayakta kalır.
  setsid "$0" --loop >>"${LOG}" 2>&1 < /dev/null &
  echo $! >"${PIDFILE}"
  printf '%s\n' "${FIFO}"
}

cmd_stop() {
  if loop_alive; then
    local pid
    pid="$(cat "${PIDFILE}")"
    pkill -P "${pid}" 2>/dev/null || true
    kill "${pid}" 2>/dev/null || true
  fi
  rm -f "${PIDFILE}"
}

cmd_path() {
  printf '%s\n' "${FIFO}"
}

case "${1:-}" in
  --start) cmd_start ;;
  --stop) cmd_stop ;;
  --loop) cmd_loop ;;
  --path) cmd_path ;;
  *)
    echo "kullanım: $0 --start|--stop|--path" >&2
    exit 1
    ;;
esac
