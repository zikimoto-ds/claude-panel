#!/usr/bin/env python3
"""Panel local de sesiones de Claude Code.

Solo usa la biblioteca estándar. Fuentes de datos:
  - ~/.claude/sessions/<pid>.json : registro de sesiones vivas (estado busy/idle, nombre, origen).
  - ~/.claude/projects/**/*.jsonl : transcripts; de aquí sale el conteo de tokens.
  - POST /hook                     : eventos que mandan los hooks de Claude Code (permisos, fin de turno).

Privacidad: del payload de los hooks solo se guardan el evento, el session_id y el texto
de la notificación. El contenido de herramientas y prompts se descarta al recibirlo.
El servidor escucha en 127.0.0.1 salvo que se defina PANEL_HOST.

Variables de entorno:
  PANEL_HOST        (default 127.0.0.1)
  PANEL_PORT        (default 8765)
  PANEL_TOKEN       si se define, /api/* exige ?token= o header X-Panel-Token (obligatorio fuera de localhost)
  PANEL_LIMITE_5H   tope de tokens para el medidor de la ventana de 5 h (default 2000000)
"""

import glob
import hashlib
import hmac
import ipaddress
import json
import subprocess
import sys
import os
import threading
import time
from collections import deque
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

CLAUDE_DIR = Path.home() / ".claude"
SESSIONS_DIR = CLAUDE_DIR / "sessions"
PROJECTS_DIR = CLAUDE_DIR / "projects"
AQUI = Path(__file__).resolve().parent
sys.path.insert(0, str(AQUI))  # con python -I la carpeta del script no entra sola al path
import voz  # noqa: E402

def _cargar_config_local():
    """Lee .env.local (KEY=VALOR, ignorado por git) sin pisar variables ya definidas."""
    ruta = AQUI / ".env.local"
    if not ruta.exists():
        return
    for linea in ruta.read_text().splitlines():
        linea = linea.strip()
        if linea and not linea.startswith("#") and "=" in linea:
            k, v = linea.split("=", 1)
            os.environ.setdefault(k.strip(), v.strip())


_cargar_config_local()
HOST = os.environ.get("PANEL_HOST", "127.0.0.1")
PORT = int(os.environ.get("PANEL_PORT", "8765"))
TOKEN = os.environ.get("PANEL_TOKEN", "")
# Token aparte para el Atajo de Siri: solo crea sesiones (/api/sesion). Por el túnel ngrok ve los
# encabezados en claro, así que el token principal nunca se acepta en texto plano por ahí.
TOKEN_SIRI = os.environ.get("PANEL_TOKEN_SIRI", "")
HOSTS_TUNEL = (".ngrok-free.dev", ".ngrok-free.app", ".ngrok.app", ".ngrok.io", ".ngrok.dev")
HOSTS_EXTRA = {h.strip().lower() for h in os.environ.get("PANEL_HOSTS", "").split(",") if h.strip()}


def _nombre_mdns():
    """Nombre .local de esta Mac (no se acepta cualquier .local: otro equipo de la red podría
    anunciar uno propio por mDNS y usarlo para DNS rebinding)."""
    try:
        r = subprocess.run(["/usr/sbin/scutil", "--get", "LocalHostName"], capture_output=True, text=True, timeout=3)
        nombre = r.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        nombre = ""
    if not nombre:
        import socket
        nombre = socket.gethostname().split(".")[0]
    return (nombre.lower() + ".local") if nombre else ""


HOST_MDNS = _nombre_mdns()
ESPERA_PERMISO_MAX = 590   # s que un hook PermissionRequest espera la decisión de la placa (timeout del hook: 600)
# Topes calibrados contra claude.ai → Settings → Usage (métrica: entrada + escritura de caché + salida).
LIMITE_SESION = int(os.environ.get("PANEL_LIMITE_SESION", os.environ.get("PANEL_LIMITE_5H", "15600000")))
LIMITE_SEMANA = int(os.environ.get("PANEL_LIMITE_SEMANA", "52000000"))
# Reinicio semanal "día hora" con día 0=lunes … 6=domingo. Default: miércoles 21:00.
RESET_SEMANAL = os.environ.get("PANEL_RESET_SEMANAL", "2 21:00")
LOCALES = ("127.0.0.1", "::1")


def _leer_ancla():
    """PANEL_ANCLA_BLOQUE = 'AAAA-MM-DD HH:MM': inicio de un bloque de sesión según claude.ai
    (hora de reinicio que muestra Usage menos 5 h). Corrige el desfase por uso fuera de esta Mac."""
    txt = os.environ.get("PANEL_ANCLA_BLOQUE", "").strip()
    try:
        return datetime.strptime(txt, "%Y-%m-%d %H:%M").timestamp() if txt else None
    except ValueError:
        print("PANEL_ANCLA_BLOQUE inválida, se ignora:", txt, flush=True)
        return None


ANCLA_BLOQUE = _leer_ancla()

VENTANA_5H = 5 * 3600
RETENCION = 8 * 24 * 3600   # alcanza para la semana completa

lock = threading.Lock()
TRANSCRIBIENDO = threading.Lock()   # whisper usa 6 hilos: una transcripción a la vez


# ---------------------------------------------------------------- tokens ----

class ContadorTokens:
    """Lee transcripts de forma incremental y acumula el uso por mensaje (deduplicado por id)."""

    def __init__(self):
        self.offsets = {}          # ruta -> (bytes leídos, inode)
        self.restos = {}           # ruta -> línea incompleta pendiente
        self.vistos = set()        # ids de mensaje ya contados
        self.eventos = deque()     # (ts, session_id, entrada, cache_escritura, cache_lectura, salida)
        self.por_sesion = {}       # session_id -> dict de totales
        self.titulos = {}          # session_id -> {"custom": str, "ai": str} (el último visto de cada tipo)

    _recientes = (0.0, set())   # (cuándo se listó, rutas modificadas en la retención)

    def _archivos(self, ids_activos):
        corte = time.time() - RETENCION
        t, recientes = self._recientes
        if time.time() - t > 30:   # el glob recursivo es caro: cada 30 s; lo activo se agrega abajo
            recientes = set()
            for ruta in glob.glob(str(PROJECTS_DIR / "**" / "*.jsonl"), recursive=True):
                try:
                    if os.path.getmtime(ruta) >= corte:
                        recientes.add(ruta)
                except OSError:
                    pass
            self._recientes = (time.time(), recientes)
        rutas = set(recientes)
        for sid in ids_activos:
            rutas.update(glob.glob(str(PROJECTS_DIR / "*" / f"{sid}.jsonl")))
        return rutas

    def actualizar(self, ids_activos):
        for ruta in self._archivos(ids_activos):
            self._leer(ruta)
        limite = time.time() - RETENCION
        while self.eventos and self.eventos[0][0] < limite:
            self.eventos.popleft()

    def _leer(self, ruta):
        try:
            st = os.stat(ruta)
        except OSError:
            return
        leido, inode = self.offsets.get(ruta, (0, st.st_ino))
        if inode != st.st_ino or st.st_size < leido:   # archivo reemplazado o truncado
            leido, self.restos[ruta] = 0, ""
        if st.st_size == leido:
            return
        with open(ruta, "rb") as f:
            f.seek(leido)
            bloque = f.read()
        self.offsets[ruta] = (leido + len(bloque), st.st_ino)
        texto = self.restos.get(ruta, "") + bloque.decode("utf-8", errors="replace")
        lineas = texto.split("\n")
        self.restos[ruta] = lineas.pop()               # la última puede venir incompleta
        for linea in lineas:
            if '"usage"' in linea and '"assistant"' in linea:
                self._procesar(linea)
            elif '"type":"custom-title"' in linea or '"type":"ai-title"' in linea:
                self._titulo(linea)

    def _titulo(self, linea):
        try:
            d = json.loads(linea)
        except ValueError:
            return
        sid = d.get("sessionId", "")
        if d.get("type") == "custom-title" and d.get("customTitle"):
            self.titulos.setdefault(sid, {})["custom"] = str(d["customTitle"])[:80]
        elif d.get("type") == "ai-title" and d.get("aiTitle"):
            self.titulos.setdefault(sid, {})["ai"] = str(d["aiTitle"])[:80]

    def _procesar(self, linea):
        try:
            d = json.loads(linea)
        except ValueError:
            return
        if d.get("type") != "assistant":
            return
        msg = d.get("message") or {}
        uso = msg.get("usage") or {}
        mid = msg.get("id")
        if not uso or not mid or mid in self.vistos:
            return
        self.vistos.add(mid)
        ts = _ts(d.get("timestamp"))
        sid = d.get("sessionId", "")
        e = int(uso.get("input_tokens") or 0)
        cw = int(uso.get("cache_creation_input_tokens") or 0)
        cr = int(uso.get("cache_read_input_tokens") or 0)
        s = int(uso.get("output_tokens") or 0)
        if ts >= time.time() - RETENCION:
            self.eventos.append((ts, sid, e, cw, cr, s))
        t = self.por_sesion.setdefault(sid, {"entrada": 0, "cache_escritura": 0, "cache_lectura": 0, "salida": 0})
        t["entrada"] += e
        t["cache_escritura"] += cw
        t["cache_lectura"] += cr
        t["salida"] += s

    def _bloque_actual(self, ahora):
        """Bloques fijos de 5 h como los cuenta Claude: el bloque empieza con el primer mensaje
        posterior al fin del anterior (redondeado hacia abajo a 10 min, que coincide con lo que
        muestra claude.ai) y dura 5 h. Devuelve (inicio, fin) o None si no hay bloque activo."""
        inicio = fin = None
        if ANCLA_BLOQUE:   # inicio de bloque conocido (copiado de claude.ai): encadena desde ahí
            inicio, fin = ANCLA_BLOQUE, ANCLA_BLOQUE + VENTANA_5H
        for ts in sorted(ev[0] for ev in self.eventos):
            if ANCLA_BLOQUE and ts < ANCLA_BLOQUE:
                continue
            if fin is None or ts >= fin:
                inicio = ts - (ts % 600)
                fin = inicio + VENTANA_5H
        if fin is None or ahora >= fin:
            return None
        return inicio, fin

    def _inicio_semana(self, ahora):
        try:
            dia_txt, hora_txt = RESET_SEMANAL.split()
            dia, (h, m) = int(dia_txt), map(int, hora_txt.split(":"))
        except ValueError:
            dia, h, m = 2, 21, 0
        ref = datetime.fromtimestamp(ahora).replace(hour=h, minute=m, second=0, microsecond=0)
        dias_atras = (ref.weekday() - dia) % 7
        inicio = ref.timestamp() - dias_atras * 86400
        if inicio > ahora:
            inicio -= 7 * 86400
        return inicio

    @staticmethod
    def _medidor(t, limite, fin, ahora):
        usado = t["entrada"] + t["cache_escritura"] + t["salida"]
        return {
            "usado": usado,
            "limite": limite,
            "pct": round(100 * usado / limite, 1) if limite else None,
            "libera_en_min": round((fin - ahora) / 60) if fin else None,
            "reinicia": datetime.fromtimestamp(fin).strftime("%a %H:%M") if fin else None,
        }

    def resumen(self):
        ahora = time.time()
        hoy = datetime.now().replace(hour=0, minute=0, second=0, microsecond=0).timestamp()
        bloque = self._bloque_actual(ahora)
        if bloque:
            ini, fin = bloque
            ses = _suma(ev for ev in self.eventos if ini <= ev[0] < fin)
        else:
            fin, ses = None, _suma([])
        ini_sem = self._inicio_semana(ahora)
        sem = _suma(ev for ev in self.eventos if ev[0] >= ini_sem)
        dia = _suma(ev for ev in self.eventos if ev[0] >= hoy)
        return {
            "ventana_5h": ses,     # bloque de sesión actual
            "hoy": dia,
            "semana": sem,
            "medidor": self._medidor(ses, LIMITE_SESION, fin, ahora),
            "medidor_semana": self._medidor(sem, LIMITE_SEMANA, ini_sem + 7 * 86400, ahora),
            "nota": "métrica = entrada + escritura de caché + salida; topes calibrados con claude.ai, aproximados",
        }


def _suma(evs):
    t = {"entrada": 0, "cache_escritura": 0, "cache_lectura": 0, "salida": 0, "mensajes": 0}
    for _, _, e, cw, cr, s in evs:
        t["entrada"] += e
        t["cache_escritura"] += cw
        t["cache_lectura"] += cr
        t["salida"] += s
        t["mensajes"] += 1
    return t


def _ts(valor):
    if not valor:
        return time.time()
    try:
        return datetime.fromisoformat(str(valor).replace("Z", "+00:00")).timestamp()
    except ValueError:
        return time.time()


# -------------------------------------------------------------- sesiones ----

def _vivo(pid):
    if not isinstance(pid, int) or not 0 < pid < 10_000_000:
        return False
    try:
        os.kill(pid, 0)
        return True
    except PermissionError:
        return True
    except (OSError, ValueError, OverflowError):
        return False


def leer_registro():
    sesiones = []
    for ruta in glob.glob(str(SESSIONS_DIR / "*.json")):
        try:
            with open(ruta) as f:
                d = json.load(f)
        except (OSError, ValueError):
            continue
        pid = d.get("pid")
        if not isinstance(pid, int) or not _vivo(pid):
            continue
        sesiones.append({
            "id": d.get("sessionId", ""),
            "pid": pid,
            "nombre": d.get("name") or "",
            "nombre_origen": d.get("nameSource", ""),
            "proyecto": os.path.basename((d.get("cwd") or "").rstrip("/")) or "~",
            "origen": "escritorio" if d.get("entrypoint") == "claude-desktop" else d.get("entrypoint", ""),
            "estado_registro": d.get("status", ""),
            "inicio": (d.get("startedAt") or 0) / 1000,
            "cambio_estado": (d.get("statusUpdatedAt") or d.get("updatedAt") or 0) / 1000,
        })
    return sesiones


# ----------------------------------------------------------------- hooks ----

class EstadoHooks:
    def __init__(self):
        self.permiso = {}                 # session_id -> {"desde": ts, "mensaje": str}
        self.ultimo_evento = {}           # session_id -> (evento, ts)
        self.alertas = deque(maxlen=30)   # historial corto para el panel
        self.n_alerta = 0                 # contador que sube con cada alerta nueva (para que la placa vibre/suene una vez)
        self.conteo_eventos = {}          # diagnóstico: cuántos eventos de cada tipo han llegado
        self.avisadas_codex = set()       # sesiones de Codex cuyo permiso pendiente ya sumó una alerta
        self.pregunta = set()             # sesiones con AskUserQuestion abierto

    def recibir(self, d):
        evento = d.get("hook_event_name", "")
        sid = d.get("session_id", "")
        ahora = time.time()
        self.ultimo_evento[sid] = (evento, ahora)
        self.conteo_eventos[evento] = self.conteo_eventos.get(evento, 0) + 1
        if evento == "Notification":
            mensaje = str(d.get("message", ""))[:160]
            tipo = d.get("notification_type", "")
            pide_permiso = tipo == "permission_prompt" or "permission" in mensaje.lower()
            if pide_permiso:
                self.permiso[sid] = {"desde": ahora, "mensaje": mensaje}
            self.n_alerta += 1
            self.alertas.appendleft({
                "ts": ahora, "session_id": sid,
                "tipo": "permiso" if pide_permiso else (tipo or "aviso"),
                "mensaje": mensaje,
            })
        elif evento in ("PostToolUse", "UserPromptSubmit", "Stop", "SessionEnd", "SessionStart"):
            self.permiso.pop(sid, None)
        if evento in ("UserPromptSubmit", "SessionStart", "SessionEnd", "Stop"):
            self.pregunta.discard(sid)
        if evento == "PreToolUse" and d.get("tool_name") == "AskUserQuestion":
            self.pregunta.add(sid)
        elif evento == "PostToolUse" and d.get("tool_name") == "AskUserQuestion":
            self.pregunta.discard(sid)


CODEX_DIR = Path.home() / ".codex"
CODEX_ESTADO = AQUI / "estado_codex.json"
CODEX_INACTIVA_MAX = 6 * 3600   # una sesión de Codex sin eventos en 6 h se da por cerrada


class SesionesCodex:
    """Codex no tiene registro de sesiones vivas: se reconstruye con sus hooks.
    De cada evento solo se guardan id, carpeta, PID, estado y hora (nunca prompts ni herramientas)."""

    def __init__(self):
        self.ses = {}   # session_id -> {"cwd", "pid", "estado", "inicio", "cambio", "evento"}
        try:
            self.ses = json.loads(CODEX_ESTADO.read_text())
        except (OSError, ValueError):
            pass
        self._titulos = {}
        self._titulos_mtime = 0

    def _guardar(self):
        try:
            CODEX_ESTADO.write_text(json.dumps(self.ses))
            os.chmod(CODEX_ESTADO, 0o600)
        except OSError:
            pass

    def recibir(self, d, pid):
        evento = d.get("hook_event_name", "")
        sid = str(d.get("session_id", ""))
        if not sid:
            return
        ahora = time.time()
        if evento == "SessionEnd":
            if self.ses.pop(sid, None) is not None:
                self._guardar()
            return
        s = self.ses.setdefault(sid, {"cwd": "", "pid": 0, "estado": "en_espera", "inicio": ahora})
        s["cwd"] = str(d.get("cwd") or s["cwd"])
        if 1 < pid < 10_000_000:
            s["pid"] = pid
        s["cambio"] = ahora
        s["evento"] = evento
        if evento == "PermissionRequest":
            s["estado"] = "permiso"
            s["desde"] = ahora
            s["herramienta"] = str(d.get("tool_name", ""))[:24]
        elif evento in ("UserPromptSubmit", "PostToolUse", "PreToolUse"):
            s["estado"] = "trabajando"
        elif evento in ("Stop", "SessionStart"):
            s["estado"] = "en_espera"
        self._guardar()

    def _cargar_titulos(self):
        ruta = CODEX_DIR / "session_index.jsonl"
        try:
            mt = ruta.stat().st_mtime
        except OSError:
            return
        if mt == self._titulos_mtime:
            return
        self._titulos_mtime = mt
        titulos = {}
        try:
            with open(ruta) as f:
                for linea in f:
                    try:
                        j = json.loads(linea)
                    except ValueError:
                        continue
                    if j.get("id") and j.get("thread_name"):
                        titulos[j["id"]] = j["thread_name"]
        except OSError:
            return
        self._titulos = titulos

    _ps_cache = (0.0, [])

    @classmethod
    def _procesos(cls):
        """Líneas 'pid comando' de ps, en caché 3 s."""
        t, lineas = cls._ps_cache
        if time.time() - t > 3:
            try:
                r = subprocess.run(["/bin/ps", "-ax", "-o", "pid=,command="], capture_output=True, text=True, timeout=3)
                lineas = r.stdout.splitlines()
            except (OSError, subprocess.SubprocessError):
                lineas = []
            cls._ps_cache = (time.time(), lineas)
        return lineas

    def _viva(self, s):
        """Los hooks de las sesiones de terminal los corre el daemon de Codex (--managed-daemon), que
        sigue vivo al cerrar la ventana: para esas, la sesión vive mientras haya algún `codex` de terminal."""
        pid = s.get("pid") or 0
        if not pid:
            return True
        if not _vivo(pid):
            return False
        lineas = self._procesos()
        cmd = next((l.split(None, 1)[1] for l in lineas if l.split(None, 1)[0] == str(pid) and " " in l.strip()), "")
        if "--managed-daemon" not in cmd:
            return True
        for l in lineas:
            partes = l.split(None, 1)
            if len(partes) < 2:
                continue
            c = partes[1]
            if "app-server" in c or "code-mode-host" in c:
                continue
            exe = c.split()[0] if not c.startswith("node ") else (c.split() + [""])[1]
            if exe.endswith("/bin/codex") or exe.endswith("/codex"):
                return True
        return False

    def lista(self):
        """Sesiones vivas con la misma forma que leer_registro()."""
        ahora = time.time()
        self._cargar_titulos()
        muertas = [k for k, s in self.ses.items()
                   if not self._viva(s) or ahora - s.get("cambio", 0) > CODEX_INACTIVA_MAX]
        for k in muertas:
            self.ses.pop(k)
        if muertas:
            self._guardar()
        salida = []
        for sid, s in self.ses.items():
            estado_vis = s["estado"]
            item = {
                "id": sid, "pid": s.get("pid", 0),
                "nombre": self._titulos.get(sid, ""),
                "proyecto": os.path.basename(s.get("cwd", "").rstrip("/")) or "~",
                "origen": "codex", "agente": "codex",
                "estado": estado_vis,
                "inicio": s.get("inicio", 0), "cambio_estado": s.get("cambio", 0),
                "tokens": {"entrada": 0, "cache_escritura": 0, "cache_lectura": 0, "salida": 0},
                "ultimo_hook": {"evento": s.get("evento", ""), "hace_seg": round(ahora - s.get("cambio", ahora))},
            }
            if s["estado"] == "permiso":
                item["esperando_seg"] = round(ahora - s.get("desde", ahora))
                item["permiso_mensaje"] = "permission to use " + s.get("herramienta", "")
            salida.append(item)
        return salida


def _es_interactiva(sid):
    """La sesión está en el registro como interactiva (CLI o app de escritorio)."""
    for ruta in glob.glob(str(SESSIONS_DIR / "*.json")):
        try:
            with open(ruta) as f:
                d = json.load(f)
        except (OSError, ValueError):
            continue
        if d.get("sessionId") == sid:
            return d.get("kind", "interactive") == "interactive" and _vivo(d.get("pid"))
    return False


def resumen_permiso(tool, entrada):
    """Lo que la placa muestra de un permiso: comando de shell o nombre del archivo. Corto."""
    entrada = entrada if isinstance(entrada, dict) else {}
    cmd = entrada.get("command") or entrada.get("cmd")
    if isinstance(cmd, list):
        cmd = " ".join(str(c) for c in cmd)
    if cmd:
        return " ".join(str(cmd).split())[:140]
    ruta = entrada.get("file_path") or entrada.get("path") or entrada.get("notebook_path")
    if ruta:
        return os.path.basename(str(ruta))[:80]
    return ""


class Esperas:
    """Permisos que esperan respuesta desde la placa. Cada uno es una petición POST /permiso
    del hook, bloqueada hasta que la placa decide, se contesta en la Mac o vence."""

    def __init__(self):
        self.d = {}   # id -> {"sid", "agente", "herr", "resumen", "desde", "evento", "decision"}

    def crear(self, sid, agente, herr, resumen):
        ident = hashlib.sha256(os.urandom(16)).hexdigest()[:16]
        self.d[ident] = {"sid": sid, "agente": agente, "herr": herr, "resumen": resumen,
                         "desde": time.time(), "evento": threading.Event(), "decision": None}
        return ident

    def de_sesion(self, sid):
        pend = [(v["desde"], k, v) for k, v in self.d.items() if v["sid"] == sid and v["decision"] is None]
        return max(pend)[1:] if pend else (None, None)

    def decidir(self, ident, decision):
        e = self.d.get(ident)
        if not e or e["decision"] is not None:
            return False
        e["decision"] = decision
        e["evento"].set()
        return True


contador = ContadorTokens()
hooks = EstadoHooks()
codex = SesionesCodex()
esperas = Esperas()
cache_estado = {"json": b"{}", "placa": b"{}", "json_red": b"{}", "placa_red": b"{}"}


def redactar(estado):
    """Copia del estado sin títulos de sesión (se reemplazan por el nombre de la carpeta)."""
    copia = json.loads(json.dumps(estado))
    for s in copia["sesiones"]:
        s["nombre"] = s["proyecto"]
        if "espera_resumen" in s:   # el comando puede traer datos: por el túnel solo la herramienta
            s["espera_resumen"] = ""
    return copia

ESTADO_CORTO = {"permiso": "P", "trabajando": "T", "en_espera": "E", "respuesta": "R"}


DIAS_ES = ["lun", "mar", "mié", "jue", "vie", "sáb", "dom"]


def _dia_hora_es(minutos):
    if minutos is None:
        return ""
    t = datetime.fromtimestamp(time.time() + minutos * 60)
    return f"{DIAS_ES[t.weekday()]} {t:%H:%M}"


def _herramienta(mensaje):
    """'Claude needs your permission to use Bash' -> 'Bash'. Solo el nombre de la herramienta."""
    marca = "permission to use "
    i = mensaje.find(marca)
    return mensaje[i + len(marca):].strip()[:24] if i >= 0 else ""


def resumen_placa(estado):
    """Versión compacta para el ESP32: pocos campos, nombres recortados."""
    m = estado["tokens"]["medidor"]
    ms = estado["tokens"]["medidor_semana"]
    hoy = estado["tokens"]["hoy"]
    return {
        "usem": ms["usado"],
        "lsem": ms["limite"],
        "rsem": _dia_hora_es(ms["libera_en_min"]),
        "hoy": hoy["entrada"] + hoy["cache_escritura"] + hoy["salida"],
        "hmsg": hoy["mensajes"],
        "pct": m["pct"] or 0,
        "usado": m["usado"],
        "limite": m["limite"],
        "libera": m["libera_en_min"] if m["libera_en_min"] is not None else -1,
        "reinicia": (m["reinicia"] or "").split(" ")[-1],   # solo HH:MM
        "psem": ms["pct"] or 0,
        "na": estado["n_alerta"],
        "np": estado["conteo"]["permiso"],
        "nt": estado["conteo"]["trabajando"],
        "nr": estado["conteo"]["respuesta"],
        "total": estado["conteo"]["total"],
        "ses": [
            {"n": s["nombre"][:60] or s["proyecto"][:60], "p": s["proyecto"][:24],
             "e": ESTADO_CORTO.get(s["estado"], "E"), "o": s["tokens"]["salida"],
             "w": s.get("esperando_seg", 0),                    # segundos esperando permiso
             "m": _herramienta(s.get("permiso_mensaje", "")),   # p. ej. "Bash"
             "og": s["origen"][:10],
             "min": round((time.time() - s["inicio"]) / 60) if s["inicio"] else 0,
             "a": "x" if s.get("agente") == "codex" else "c",   # c = Claude, x = Codex
             "k": hashlib.sha256(str(s["id"]).encode()).hexdigest()[:10],   # id estable (los nombres se repiten)
             "i": s["id"] if s.get("agente") != "codex" and s.get("origen") == "cli" else "",   # acepta mensajes por voz
             "q": s.get("espera_id", ""),                       # permiso contestable desde la placa
             "r": s.get("espera_resumen", "")[:140]}
            for s in _seis_estables(estado["sesiones"])
        ],
    }


def _seis_estables(sesiones):
    """Hasta 6 sesiones en orden de inicio (cada muñequito conserva su lugar en la pantalla).
    Si hay más de 6, entran primero las que piden permiso y luego las más recientes."""
    if len(sesiones) > 6:
        orden = {"permiso": 0, "respuesta": 1, "trabajando": 2, "en_espera": 3}
        sesiones = sorted(sesiones, key=lambda s: (orden.get(s["estado"], 3), -s["inicio"]))[:6]
    return sorted(sesiones, key=lambda s: (s["inicio"], s["id"]))


def construir_estado():
    sesiones = leer_registro()
    contador.actualizar([s["id"] for s in sesiones])
    ahora = time.time()
    for s in sesiones:
        # Prioridad del nombre: el que puso el usuario > título personalizado > título generado por IA > nombre derivado.
        t = contador.titulos.get(s["id"], {})
        if s.pop("nombre_origen") != "user" or not s["nombre"]:
            s["nombre"] = t.get("custom") or t.get("ai") or s["nombre"]
        p = hooks.permiso.get(s["id"])
        if p:
            s["estado"] = "permiso"
            s["permiso_mensaje"] = p["mensaje"]
            s["esperando_seg"] = round(ahora - p["desde"])
        elif s["id"] in hooks.pregunta:
            s["estado"] = "respuesta"
        elif s["estado_registro"] == "busy":
            s["estado"] = "trabajando"
        else:
            s["estado"] = "en_espera"
        s["tokens"] = contador.por_sesion.get(s["id"], {"entrada": 0, "cache_escritura": 0, "cache_lectura": 0, "salida": 0})
        ev = hooks.ultimo_evento.get(s["id"])
        s["ultimo_hook"] = {"evento": ev[0], "hace_seg": round(ahora - ev[1])} if ev else None
        s["agente"] = "claude"
    for s in codex.lista():
        if s["estado"] == "permiso" and s["id"] not in hooks.avisadas_codex:
            hooks.avisadas_codex.add(s["id"])   # alerta nueva: la placa suena una vez
            hooks.n_alerta += 1
        elif s["estado"] != "permiso":
            hooks.avisadas_codex.discard(s["id"])
        sesiones.append(s)
    for s in sesiones:   # permiso que se puede contestar desde la placa
        ident, e = esperas.de_sesion(s["id"])
        if ident and s["estado"] == "permiso":
            s["espera_id"], s["espera_resumen"], s["espera_herr"] = ident, e["resumen"], e["herr"]
    orden = {"permiso": 0, "respuesta": 1, "trabajando": 2, "en_espera": 3}
    sesiones.sort(key=lambda s: (orden.get(s["estado"], 3), -s["cambio_estado"]))
    vivos = {s["id"] for s in sesiones}
    for sid in list(hooks.permiso):        # limpia permisos de sesiones que ya cerraron
        if sid not in vivos:
            hooks.permiso.pop(sid, None)
    return {
        "generado": ahora,
        "conteo": {
            "total": len(sesiones),
            "trabajando": sum(s["estado"] == "trabajando" for s in sesiones),
            "permiso": sum(s["estado"] == "permiso" for s in sesiones),
            "respuesta": sum(s["estado"] == "respuesta" for s in sesiones),
        },
        "n_alerta": hooks.n_alerta,
        "eventos_hook": hooks.conteo_eventos,
        "alertas": list(hooks.alertas)[:10],
        "tokens": contador.resumen(),
        "sesiones": sesiones,
    }


def bucle():
    while True:
        try:
            with lock:
                estado = construir_estado()
                cache_estado["json"] = json.dumps(estado, ensure_ascii=False).encode()
                cache_estado["placa"] = json.dumps(resumen_placa(estado), ensure_ascii=False).encode()
                red = redactar(estado)
                cache_estado["json_red"] = json.dumps(red, ensure_ascii=False).encode()
                cache_estado["placa_red"] = json.dumps(resumen_placa(red), ensure_ascii=False).encode()
        except Exception as exc:  # el panel no debe caerse por un archivo raro
            print("error al construir estado:", exc, flush=True)
        time.sleep(1)


# ------------------------------------------------------------------ HTTP ----

class Manejador(BaseHTTPRequestHandler):
    timeout = 30   # s por operación de socket (lectura o escritura)

    def log_message(self, *a):
        pass

    def _por_tunel(self):
        """ngrok (u otro proxy) entrega desde 127.0.0.1 pero agrega encabezados de reenvío."""
        return any(k.lower().startswith(("x-forwarded", "forwarded", "ngrok", "x-original"))
                   for k in self.headers.keys())

    def _es_local(self):
        return self.client_address[0] in LOCALES and not self._por_tunel()

    def _firma_valida(self, cuerpo=None):
        """La placa no manda el token: firma '<MÉTODO> <ruta> <ts>[ <sha256 del cuerpo>]' con
        HMAC-SHA256(token). Vale 120 s, así la firma no se puede reutilizar después."""
        firma = self.headers.get("X-Panel-Firma", "")
        ts = self.headers.get("X-Panel-Ts", "")
        if not (TOKEN and firma and ts.isdigit()) or abs(time.time() - int(ts)) > 120:
            return False
        ruta = self.path   # ruta con query: la sesión destino (?s=) también queda firmada
        mensaje = f"{self.command} {ruta} {ts}"
        if cuerpo is not None:
            mensaje += " " + hashlib.sha256(cuerpo).hexdigest()
        esperada = hmac.new(TOKEN.encode(), mensaje.encode(), "sha256").hexdigest()
        return hmac.compare_digest(firma, esperada)

    def _host_valido(self):
        """Contra DNS rebinding: una página web que resuelva su dominio a 127.0.0.1 llega con su
        propio Host. Solo se aceptan IPs, localhost, el nombre .local de esta Mac, el túnel y PANEL_HOSTS."""
        host = (self.headers.get("Host") or "").strip().lower()
        if host.startswith("["):
            host = host[1:].split("]")[0]
        else:
            host = host.rsplit(":", 1)[0] if host.count(":") == 1 else host
        if not host:
            return False
        try:
            ipaddress.ip_address(host)
            return True
        except ValueError:
            pass
        return host == "localhost" or (HOST_MDNS and host == HOST_MDNS) or host.endswith(HOSTS_TUNEL) or host in HOSTS_EXTRA

    def _autorizado(self, qs, cuerpo=None, solo_firma=False, sin_local=False):
        """solo_firma: exige HMAC (la placa) aunque se esté en la red local; lo usan las aprobaciones.
        sin_local: la petición local también necesita firma o token (rutas que lanzan sesiones)."""
        if self._es_local() and not solo_firma and not sin_local:
            return True
        if not TOKEN:
            return False
        if self._firma_valida(cuerpo):
            return True
        if solo_firma:
            return False
        dado = self.headers.get("X-Panel-Token") or qs.get("token", [""])[0]
        if not self._por_tunel():
            return hmac.compare_digest(dado.encode(), TOKEN.encode())
        # Por el túnel solo el token de Siri, y solo para crear sesiones.
        return bool(TOKEN_SIRI) and urlparse(self.path).path == "/api/sesion" and \
            hmac.compare_digest(dado.encode(), TOKEN_SIRI.encode())

    def _cache(self, clave):
        # Por el túnel el tráfico pasa por un tercero: se mandan versiones sin títulos de sesión.
        return cache_estado[clave + "_red"] if self._por_tunel() else cache_estado[clave]

    def _enviar(self, codigo, cuerpo, tipo="application/json; charset=utf-8"):
        self.send_response(codigo)
        self.send_header("Content-Type", tipo)
        self.send_header("Content-Length", str(len(cuerpo)))
        self.send_header("Cache-Control", "no-store")
        ts = self.headers.get("X-Panel-Ts", "")
        if codigo == 200 and TOKEN and ts.isdigit():
            # Firma de la respuesta: la placa descarta datos que no vengan de este servidor.
            firma = hmac.new(TOKEN.encode(), ts.encode() + b"\n" + cuerpo, "sha256").hexdigest()
            self.send_header("X-Panel-Firma-Resp", firma)
        self.end_headers()
        self.wfile.write(cuerpo)

    def do_GET(self):
        u = urlparse(self.path)
        if not self._host_valido():
            return self._enviar(403, b'{"error":"host no permitido"}')
        qs = parse_qs(u.query)
        if u.path in ("/", "/index.html"):
            if not self._autorizado(qs):
                return self._enviar(401, b"token requerido", "text/plain; charset=utf-8")
            self._enviar(200, (AQUI / "panel.html").read_bytes(), "text/html; charset=utf-8")
        elif u.path == "/api/placa":
            if not self._autorizado(qs):
                return self._enviar(401, b'{"error":"token"}')
            with lock:
                cuerpo = self._cache("placa")
            self._enviar(200, cuerpo)
        elif u.path == "/api/placa/stream":
            if not self._autorizado(qs):
                return self._enviar(401, b'{"error":"token"}')
            self._stream_placa()
        elif u.path == "/api/estado":
            if not self._autorizado(qs):
                return self._enviar(401, b'{"error":"token"}')
            with lock:
                cuerpo = self._cache("json")
            self._enviar(200, cuerpo)
        else:
            self._enviar(404, b'{"error":"no existe"}')

    def _stream_placa(self):
        """Una sola petición larga: manda el estado cuando cambia y un latido cada 15 s.
        Pensado para el túnel (ngrok gratis cuenta peticiones, no tiempo de conexión).
        Cada línea: '<hmac> <seq> <json>' con hmac = HMAC(token, ts + '\\n' + seq + '\\n' + json).
        Se cierra a la hora para que la placa renueve la firma."""
        ts = self.headers.get("X-Panel-Ts", "0")
        self.send_response(200)
        self.send_header("Content-Type", "application/x-ndjson")
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Accel-Buffering", "no")
        self.end_headers()
        clave = "placa_red" if self._por_tunel() else "placa"
        ultimo_envio = 0.0
        anterior = None
        seq = 0
        inicio = time.time()
        try:
            while time.time() - inicio < 3600:
                with lock:
                    actual = cache_estado[clave]
                ahora = time.time()
                if actual != anterior or ahora - ultimo_envio >= 15:
                    seq += 1
                    firma = hmac.new(TOKEN.encode(), f"{ts}\n{seq}\n".encode() + actual, "sha256").hexdigest()
                    self.wfile.write(f"{firma} {seq} ".encode() + actual + b"\n")
                    self.wfile.flush()
                    anterior, ultimo_envio = actual, ahora
                time.sleep(0.5)
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass  # la placa se desconectó

    def _leer_cuerpo(self, maximo):
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            raise ValueError("Content-Length inválido")
        if n < 0 or n > maximo:
            raise ValueError("cuerpo demasiado grande o longitud inválida")
        return self.rfile.read(n) if n else b""

    def _json(self, codigo, datos):
        self._enviar(codigo, json.dumps(datos, ensure_ascii=False).encode())

    def _destino(self, sid):
        """tty y nombre de una sesión de Claude Code abierta en la CLI (para escribirle por voz)."""
        with lock:
            estado = json.loads(cache_estado["json"])
        for s in estado.get("sesiones", []):
            if s.get("id") == sid and s.get("agente") != "codex":
                if s.get("origen") != "cli":
                    raise RuntimeError("esa sesión no corre en una terminal")
                if s.get("estado") in ("permiso", "respuesta"):
                    # lo tecleado caería en el diálogo de permiso o en las opciones de la pregunta
                    raise RuntimeError("primero contesta el permiso o la pregunta de esa sesión")
                try:
                    r = subprocess.run(["/bin/ps", "-o", "tty=", "-p", str(int(s["pid"]))],
                                       capture_output=True, text=True, timeout=3)
                except (OSError, subprocess.SubprocessError, ValueError):
                    raise RuntimeError("no se pudo ubicar la terminal")
                tty = r.stdout.strip()
                if not tty or tty == "??":
                    raise RuntimeError("la sesión no tiene terminal")
                return "/dev/" + tty, s.get("nombre") or s.get("proyecto") or "sesión"
        raise RuntimeError("la sesión ya no está abierta")

    def _verificar_destino(self, p):
        """Al confirmar, revisa el estado actual: pudo cambiar desde que se dictó."""
        if not p.get("sid"):
            return
        with lock:
            estado = json.loads(cache_estado["json"])
        s = next((x for x in estado.get("sesiones", []) if x.get("id") == p["sid"]), None)
        if not s:
            raise RuntimeError("la sesión ya no está abierta")
        if s.get("estado") in ("permiso", "respuesta"):
            raise RuntimeError("primero contesta el permiso o la pregunta de esa sesión")
        try:
            r = subprocess.run(["/bin/ps", "-o", "tty=", "-p", str(int(s["pid"]))], capture_output=True, text=True, timeout=3)
            tty_actual = "/dev/" + r.stdout.strip()
        except (OSError, subprocess.SubprocessError, ValueError):
            tty_actual = ""
        if tty_actual != p.get("tty"):
            raise RuntimeError("la terminal de esa sesión cambió; vuelve a dictar")
        if p.get("sostener") and s.get("estado") != "en_espera":
            raise RuntimeError(p["texto"] + " solo con la sesión en espera (ahora está trabajando)")

    def _post_voz(self, ruta):
        try:
            cuerpo = self._leer_cuerpo(2_000_000)   # ~60 s de audio a 16 kHz mono
        except ValueError as e:
            return self._json(413, {"error": str(e)})
        if not self._autorizado({}, cuerpo, sin_local=True):
            return self._json(401, {"error": "firma"})
        try:
            if ruta == "/api/voz":                 # placa: audio PCM -> texto pendiente de confirmar
                if not TRANSCRIBIENDO.acquire(blocking=False):
                    return self._json(429, {"error": "ya hay una transcripción en curso"})
                try:
                    texto = voz.transcribir(cuerpo)
                finally:
                    TRANSCRIBIENDO.release()
                destino = parse_qs(urlparse(self.path).query).get("s", [""])[0]
                if destino:                        # mensaje para una sesión abierta en Terminal
                    tty, nombre = self._destino(destino)
                    return self._json(200, voz.preparar_mensaje(texto, tty, nombre, destino))
                return self._json(200, voz.preparar(texto))
            datos = json.loads(cuerpo or b"{}")
            if ruta == "/api/voz/confirmar":       # placa: confirma la sesión pendiente
                return self._json(200, voz.confirmar(str(datos.get("id", "")), verificar=self._verificar_destino))
            if ruta == "/api/sesion":              # Atajo de Siri: texto ya revisado en el iPhone
                texto = str(datos.get("texto", "")).strip()
                if not texto:
                    return self._json(400, {"error": "texto vacío"})
                p = voz.preparar(texto)
                return self._json(200, voz.confirmar(p["id"]) | {"texto": p["texto"]})
        except (RuntimeError, ValueError, OSError, subprocess.SubprocessError) as e:
            print("voz:", e, flush=True)
            return self._json(422, {"error": str(e)[:200]})
        return self._json(404, {"error": "no existe"})

    def _post_permiso_hook(self, qs):
        """Hook PermissionRequest (Claude o Codex): espera la decisión de la placa.
        Responde la decisión en el formato de hook, o 204 sin cuerpo si se contestó en la Mac,
        la sesión siguió o venció (el hook no decide y queda el diálogo normal)."""
        try:
            d = json.loads(self._leer_cuerpo(1_000_000) or b"{}")
        except ValueError:
            d = {}
        sid = str(d.get("session_id", ""))
        herr = str(d.get("tool_name", ""))[:24]
        resumen = resumen_permiso(herr, d.get("tool_input"))
        es_codex = qs.get("src", [""])[0] == "codex"
        if not es_codex and not _es_interactiva(sid):
            return self._enviar(204, b"")   # -p / SDK: nadie mira la placa; no frenar la corrida
        with lock:
            if es_codex:
                try:
                    pid = int(qs.get("pid", ["0"])[0])
                except ValueError:
                    pid = 0
                codex.recibir(d, pid)
            else:
                hooks.permiso[sid] = {"desde": time.time(), "mensaje": "permission to use " + herr}
            clave = ("codex:" if es_codex else "") + "PermissionRequest"
            hooks.conteo_eventos[clave] = hooks.conteo_eventos.get(clave, 0) + 1
            ident = esperas.crear(sid, "codex" if es_codex else "claude", herr, resumen)
            e = esperas.d[ident]
        del d
        inicio = time.time()
        try:
            while not e["evento"].wait(1):
                with lock:   # ¿se contestó en la Mac o venció? Revisar y retirar juntos, bajo el lock,
                    # para que una decisión de la placa no llegue entre ambos pasos.
                    sigue = (codex.ses.get(sid, {}).get("estado") == "permiso") if es_codex else (sid in hooks.permiso)
                    if not sigue or time.time() - inicio > ESPERA_PERMISO_MAX:
                        if e["decision"] is None:
                            esperas.d.pop(ident, None)
                            break
        finally:
            with lock:
                esperas.d.pop(ident, None)
                otra, _ = esperas.de_sesion(sid)   # ¿llegó otro permiso de la misma sesión mientras tanto?
        if e["decision"] is None:
            return self._enviar(204, b"")
        decision = {"behavior": "allow"} if e["decision"] == "a" else \
            {"behavior": "deny", "message": "Rechazado desde el panel físico."}
        with lock:   # la placa ya contestó: quitar el aviso (salvo que ya haya otro permiso pendiente)
            if otra is None:
                if es_codex and sid in codex.ses:
                    codex.ses[sid]["estado"] = "trabajando" if e["decision"] == "a" else "en_espera"
                else:
                    hooks.permiso.pop(sid, None)
        print(f"permiso: {'aprobado' if e['decision'] == 'a' else 'rechazado'} desde la placa ({herr})", flush=True)
        cuerpo = json.dumps({"hookSpecificOutput": {"hookEventName": "PermissionRequest", "decision": decision}})
        self._enviar(200, cuerpo.encode())

    def _post_permiso_placa(self):
        """La placa aprueba (a) o rechaza (r) un permiso pendiente. Solo con firma HMAC."""
        try:
            cuerpo = self._leer_cuerpo(4096)
        except ValueError as e:
            return self._json(413, {"error": str(e)})
        if not self._autorizado({}, cuerpo, solo_firma=True):
            return self._json(401, {"error": "firma"})
        try:
            datos = json.loads(cuerpo or b"{}")
        except ValueError:
            return self._json(400, {"error": "json"})
        d = datos.get("d")
        if d not in ("a", "r"):
            return self._json(400, {"error": "decisión"})
        with lock:
            ok = esperas.decidir(str(datos.get("id", "")), d)
        if not ok:
            return self._json(422, {"error": "el permiso ya se contestó o venció"})
        return self._json(200, {"ok": True})

    def do_POST(self):
        u = urlparse(self.path)
        # Ningún cliente legítimo (placa, hooks, Siri) manda Origin: solo los navegadores.
        # Así una página web abierta en la Mac no puede lanzar sesiones ni falsear hooks.
        if self.headers.get("Origin") is not None or not self._host_valido():
            return self._enviar(403, b'{"error":"origen no permitido"}')
        if u.path in ("/api/voz", "/api/voz/confirmar", "/api/sesion"):
            return self._post_voz(u.path)
        if u.path == "/api/permiso":
            return self._post_permiso_placa()
        if u.path == "/permiso":
            if not self._es_local():
                return self._enviar(403, b'{"error":"solo local"}')
            return self._post_permiso_hook(parse_qs(u.query))
        if u.path != "/hook":
            return self._enviar(404, b'{"error":"no existe"}')
        # Los hooks solo se aceptan desde la propia Mac (nunca por el túnel).
        if not self._es_local():
            return self._enviar(403, b'{"error":"solo local"}')
        try:
            crudo = self._leer_cuerpo(8_000_000)   # PostToolUse trae la salida completa de la herramienta
        except ValueError:
            return self._enviar(413, b'{"error":"cuerpo"}')
        try:
            d = json.loads(crudo or b"{}")
        except ValueError:
            d = {}
        qs = parse_qs(u.query)
        with lock:
            if qs.get("src", [""])[0] == "codex":
                try:
                    pid = int(qs.get("pid", ["0"])[0])
                except ValueError:
                    pid = 0
                hooks.conteo_eventos["codex:" + str(d.get("hook_event_name", ""))] = \
                    hooks.conteo_eventos.get("codex:" + str(d.get("hook_event_name", "")), 0) + 1
                codex.recibir(d, pid)
            else:
                hooks.recibir(d)
        del crudo, d   # no se conserva el payload completo
        self._enviar(204, b"")


def main():
    if HOST not in ("127.0.0.1", "localhost", "::1") and not TOKEN:
        raise SystemExit("PANEL_TOKEN es obligatorio si el servidor escucha fuera de localhost")
    threading.Thread(target=bucle, daemon=True).start()
    srv = ThreadingHTTPServer((HOST, PORT), Manejador)
    print(f"Panel en http://{HOST}:{PORT}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
