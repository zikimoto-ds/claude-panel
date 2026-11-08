# claude-panel

Panel físico para una **Waveshare ESP32-S3-Touch-AMOLED-1.75C** (pantalla redonda 466×466) que muestra las sesiones de **Claude Code** y **Codex** abiertas en una Mac:

- Cada sesión es un muñequito pixel (naranja = Claude, azul = Codex) animado según su estado: trabajando, pide permiso, espera tu respuesta o en espera.
- Aviso y pitido cuando una sesión pide permiso; **aprobar o rechazar desde la placa** (firma HMAC, una sola vez por permiso).
- Anillo con el consumo del bloque de 5 h y porcentaje semanal.
- Crear sesiones por voz: botón BOOT → whisper.cpp local en la Mac → confirmar en la pantalla → se abre Terminal con `claude`.
- Funciona en la red local o por un túnel (streaming, pensado para el plan gratis de ngrok).

## Piezas

| Archivo | Qué hace |
|---|---|
| `servidor.py` | Servidor HTTP (solo biblioteca estándar). Lee `~/.claude/sessions`, los transcripts y los hooks; arma el estado para la placa. |
| `hook.sh` | Hook de Claude Code y Codex (`hook.sh codex`). Reenvía eventos al servidor; en `PermissionRequest` espera la decisión de la placa. |
| `voz.py` | Transcripción local con whisper.cpp y lanzamiento de sesiones. |
| `panel.html` | Vista web del panel (consulta cada 1 s; no dejarla abierta por el túnel). |
| `firmware/` | PlatformIO + Arduino (pioarduino), Arduino_GFX, U8g2, ArduinoJson, SensorLib. Drivers ES8311/ES7210 de Espressif (Apache-2.0 / MIT). |

## Puesta en marcha

1. `cp .env.example .env.local` y define `PANEL_TOKEN`.
2. `cp firmware/include/secrets.example.h firmware/include/secrets.h` y llena WiFi, el mismo token y el nombre mDNS de la Mac.
3. Servidor: `python3 -I servidor.py` (o como agente de launchd).
4. Hooks en `~/.claude/settings.json` para `SessionStart`, `UserPromptSubmit`, `Notification`, `PostToolUse`, `Stop`, `SessionEnd` (`hook.sh`), `PermissionRequest` (`hook.sh`, timeout 600) y `PreToolUse` con matcher `AskUserQuestion`. En `~/.codex/hooks.json` los mismos eventos con `hook.sh codex` (Codex pide aprobarlos con `/hooks`).
5. Voz: `brew install whisper-cpp` y descarga `ggml-large-v3-turbo-q5_0.bin` a `modelos/`.
6. Firmware: `pio run -t upload` en `firmware/`.

## Seguridad

- Los hooks solo se aceptan desde la propia Mac, nunca por el túnel.
- Aprobar/rechazar exige firma HMAC; el token en texto plano solo vale en la red local.
- Por el túnel los títulos y comandos se reemplazan por la carpeta y la herramienta.
