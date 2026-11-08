#!/bin/sh
# Reenvía el evento de hook de Claude Code (o de Codex, con el argumento "codex") al panel local.
# Eventos normales: no escribe nada en stdout y nunca falla; si el panel está apagado, la sesión
# sigue igual.
# PermissionRequest: espera (hasta ~10 min) la decisión que se tome en la placa y la imprime en
# stdout en el formato de hook. Si se contesta en la Mac o no hay respuesta, no imprime nada y el
# diálogo normal decide.
base=http://127.0.0.1:8765
qs=""
if [ "$1" = "codex" ]; then
  # Codex no tiene registro de sesiones: se manda el PID del proceso codex que corre el hook
  # para que el panel sepa si la sesión sigue viva.
  pid=$PPID
  while [ -n "$pid" ] && [ "$pid" -gt 1 ]; do
    case "$(ps -o comm= -p "$pid" 2>/dev/null)" in *codex*) break ;; esac
    pid=$(ps -o ppid= -p "$pid" 2>/dev/null | tr -d ' ')
  done
  qs="?src=codex&pid=${pid:-0}"
fi
payload=$(cat)
case "$payload" in
  *'"hook_event_name":"PermissionRequest"'* | *'"hook_event_name": "PermissionRequest"'*)
    resp=$(printf '%s' "$payload" | curl -s -m 595 -X POST -H 'Content-Type: application/json' \
      --data-binary @- "$base/permiso$qs" 2>/dev/null)
    case "$resp" in
      '{"hookSpecificOutput"'*) printf '%s' "$resp" ;;
    esac
    exit 0
    ;;
esac
printf '%s' "$payload" | curl -s -m 1 -o /dev/null -X POST -H 'Content-Type: application/json' \
  --data-binary @- "$base/hook$qs" >/dev/null 2>&1
exit 0
