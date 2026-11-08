"""Crear sesiones de Claude Code por voz o texto.

Flujo de la placa: audio PCM (16 kHz, mono, 16 bits) -> whisper.cpp local -> texto + carpeta
-> la placa muestra el texto -> confirmación -> se abre Terminal con `claude "<texto>"`.

El audio y el texto no salen de la Mac salvo por el túnel (TLS de ngrok) entre placa/iPhone y Mac.
La transcripción es local: no se usa ningún servicio externo de voz.
"""

import difflib
import os
import re
import subprocess
import tempfile
import threading
import time
import unicodedata
import uuid
import wave
from pathlib import Path

AQUI = Path(__file__).resolve().parent
# large-v3-turbo entiende mejor los términos en inglés dentro del español (behavior, recovery…)
# y en esta Mac tarda <1 s por frase; small queda de respaldo.
_TURBO = AQUI / "modelos" / "ggml-large-v3-turbo-q5_0.bin"
MODELO = Path(os.environ.get("PANEL_MODELO_WHISPER",
                             _TURBO if _TURBO.exists() else AQUI / "modelos" / "ggml-small.bin"))
# Términos en inglés frecuentes en las instrucciones; se pasan a whisper como pista.
TERMINOS = ("behavior, recovery, backtest, score, scorecard, deciles, KS, PSI, AUC, roll rate, "
            "vintage, pipeline, dashboard, commit, branch, pull request, tests, T-Conecta, RecomTC")
WHISPER = os.environ.get("PANEL_WHISPER", "/opt/homebrew/bin/whisper-cli")
CLAUDE = os.environ.get("PANEL_CLAUDE", str(Path.home() / ".local" / "bin" / "claude"))
RAIZ_PROYECTOS = Path.home() / "proyectos"
# ~/proyectos y no ~: Claude Code nunca guarda la confianza del home y preguntaría en cada sesión.
CARPETA_DEFECTO = Path(os.environ.get("PANEL_CARPETA_DEFECTO", str(RAIZ_PROYECTOS)))
LANZAMIENTOS = AQUI / "lanzamientos"
VIGENCIA_PENDIENTE = 120  # segundos para confirmar

_pendientes = {}  # id -> {"texto", "carpeta", "ts"}
_lock = threading.Lock()


# ------------------------------------------------------------- carpetas ----

def _normalizar(txt):
    txt = unicodedata.normalize("NFKD", txt.lower())
    txt = "".join(c for c in txt if not unicodedata.combining(c))
    return re.sub(r"[^a-z0-9]+", " ", txt).strip()


def proyectos():
    try:
        return sorted(p.name for p in RAIZ_PROYECTOS.iterdir() if p.is_dir() and not p.name.startswith("."))
    except OSError:
        return []


def separar_carpeta(texto):
    """'En calculadora tconecta app, revisa los tests' -> (~/proyectos/calculadora_tconecta_app, 'revisa los tests').
    Solo reconoce carpetas de ~/proyectos; si no hay coincidencia, usa la carpeta por defecto."""
    limpio = texto.strip()
    m = re.match(r"^\s*(?:en el proyecto|en la carpeta|en el|proyecto|en)\s+(.+)$", limpio, flags=re.I)
    if m:
        resto = m.group(1)
        palabras = resto.split()
        mejor = None
        # Probar prefijos de 1 a 5 palabras contra los nombres de carpeta normalizados.
        for n in range(min(5, len(palabras)), 0, -1):
            candidato = _normalizar(" ".join(palabras[:n]))
            for nombre in proyectos():
                norm = _normalizar(nombre)
                # Exacto, prefijo, o parecido (whisper escribe "recompt" por "recomtc").
                parecido = len(candidato) >= 8 and difflib.SequenceMatcher(None, candidato, norm).ratio() >= 0.85
                if candidato and (candidato == norm or (len(candidato) >= 5 and norm.startswith(candidato)) or parecido):
                    mejor = (nombre, n)
                    break
            if mejor:
                break
        if mejor:
            nombre, n = mejor
            instruccion = " ".join(palabras[n:]).lstrip(" ,.:;-")
            return RAIZ_PROYECTOS / nombre, instruccion or limpio
    return CARPETA_DEFECTO, limpio


def _ruta_corta(carpeta):
    home = str(Path.home())
    s = str(carpeta)
    return "~" + s[len(home):] if s.startswith(home) else s


# --------------------------------------------------------- transcripción ----

def transcribir(pcm):
    if not MODELO.exists():
        raise RuntimeError("falta el modelo de whisper (" + MODELO.name + ")")
    if len(pcm) < 16000:  # menos de medio segundo
        raise RuntimeError("audio demasiado corto")
    with tempfile.TemporaryDirectory() as d:
        wav = Path(d) / "voz.wav"
        with wave.open(str(wav), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(16000)
            w.writeframes(pcm)
        pista = ("Instrucciones para Claude Code. Términos: " + TERMINOS + ". Proyectos: "
                 + ", ".join(p.replace("_", " ") for p in proyectos()[:40]))
        r = subprocess.run(
            [WHISPER, "-m", str(MODELO), "-f", str(wav), "-l", "es", "-nt", "-np", "-t", "6", "--prompt", pista],
            capture_output=True, text=True, timeout=90,
        )
    if r.returncode != 0:
        raise RuntimeError("whisper falló: " + r.stderr.strip()[-200:])
    texto = " ".join(r.stdout.split())
    if not texto or texto.lower() in ("[blank_audio]", "(silencio)"):
        raise RuntimeError("no se entendió nada")
    return texto


def _sin_opciones(texto):
    """Quita guiones iniciales: `claude` tomaría "--algo" como opción de línea de comandos
    (p. ej. --dangerously-skip-permissions) en vez de como instrucción."""
    return texto.lstrip("-\u2013\u2014 \t\r\n")


def _purgar():
    """Quita pendientes vencidos y deja como máximo 20 (llamar con _lock tomado)."""
    ahora = time.time()
    for k in [k for k, v in _pendientes.items() if ahora - v["ts"] > VIGENCIA_PENDIENTE]:
        _pendientes.pop(k)
    while len(_pendientes) >= 20:
        _pendientes.pop(min(_pendientes, key=lambda k: _pendientes[k]["ts"]))


def preparar(texto):
    """Registra una sesión pendiente de confirmar. Devuelve el dict que se manda a la placa."""
    carpeta, instruccion = separar_carpeta(texto)
    instruccion = _sin_opciones(instruccion)
    if not instruccion:
        raise RuntimeError("instrucción vacía")
    ident = uuid.uuid4().hex[:12]
    with _lock:
        _purgar()
        _pendientes[ident] = {"texto": instruccion, "carpeta": carpeta, "ts": time.time()}
    return {"id": ident, "texto": instruccion[:400], "carpeta": _ruta_corta(carpeta)}


# Comandos de Claude Code que se pueden dictar: "comando <palabra>" exacto, sin argumentos.
# sostener = en la placa hay que mantener Enviar 1 s, y solo con la sesión en espera.
# Solo comandos que se ejecutan directo: los que abren un panel (/status, /cost, /model...)
# dejan la sesión esperando un Esc que no se puede mandar desde aquí (probado con /status).
COMANDOS = {
    "compactar": ("/compact", False), "compacta": ("/compact", False),
    "limpiar": ("/clear", True), "limpia": ("/clear", True),
    "salir": ("/exit", True), "sal": ("/exit", True),
}
# Codex: solo salir (/quit, probado: cierra y manda SessionEnd).
COMANDOS_CODEX = {"salir": ("/quit", True), "sal": ("/quit", True)}
_PERMITIDOS = {v[0] for v in COMANDOS.values()} | {v[0] for v in COMANDOS_CODEX.values()}


def comando_dictado(texto, agente="claude"):
    """'Comando compactar.' -> ('/compact', False). None si no empieza con "comando".
    Si empieza con "comando" pero no está en la lista, error (nunca se adivina)."""
    norm = _normalizar(texto)
    tabla = COMANDOS_CODEX if agente == "codex" else COMANDOS
    m = re.fullmatch(r"comando (\w+)", norm)
    if m and m.group(1) in tabla:
        return tabla[m.group(1)]
    return None   # cualquier otra cosa es un mensaje normal ("comando de git falló…")


def preparar_mensaje(texto, tty, nombre, sid="", agente="claude", pid=0):
    """Mensaje (o comando de la lista) pendiente de confirmar para una sesión abierta en Terminal."""
    cmd = comando_dictado(texto, agente)
    if cmd:
        texto, sostener = cmd
    else:
        texto, sostener = _una_linea(texto), False
    if not texto:
        raise RuntimeError("mensaje vacío")
    ident = uuid.uuid4().hex[:12]
    with _lock:
        _purgar()
        _pendientes[ident] = {"tipo": "mensaje", "texto": texto, "tty": tty, "destino": nombre, "sid": sid,
                              "comando": bool(cmd), "sostener": sostener, "agente": agente, "pid": pid, "ts": time.time()}
    return {"id": ident, "texto": texto[:400], "carpeta": "", "destino": nombre[:60],
            "comando": 1 if cmd else 0, "sostener": 1 if sostener else 0}


def confirmar(ident, verificar=None):
    """verificar(p): lo pasa el servidor para revisar el estado actual de la sesión destino."""
    with _lock:
        p = _pendientes.pop(ident, None)
    if not p or time.time() - p["ts"] > VIGENCIA_PENDIENTE:
        raise RuntimeError("la petición venció o no existe")
    if p.get("tipo") == "mensaje":
        if verificar:
            verificar(p)
        enviar_a_terminal(p["tty"], p["texto"], comando=p.get("comando", False), agente=p.get("agente", "claude"))
        return {"ok": True, "destino": p["destino"][:60]}
    lanzar(p["texto"], p["carpeta"])
    return {"ok": True, "carpeta": _ruta_corta(p["carpeta"])}


def _una_linea(texto):
    """Una sola línea sin caracteres de control (un salto de línea mandaría el mensaje a medias)."""
    limpio = "".join(c if c.isprintable() else " " for c in str(texto))
    limpio = " ".join(limpio.split())
    # Prefijos con significado en la terminal de Claude Code: "!" ejecuta bash sin pedir permiso,
    # "/" corre comandos de la app (p. ej. /exit) y "#" escribe en la memoria.
    return limpio.lstrip("!/#\uff01 ").strip()[:2000]


# Escribe en la pestaña de Terminal cuya tty coincide y que tiene un proceso corriendo, como si se
# tecleara (+ Enter). Texto y tty van como argumentos de osascript: no se interpolan en el script.
_SCRIPT_TERMINAL = """
on run argv
  set objetivo to item 1 of argv
  set texto to item 2 of argv
  set conCR to (item 3 of argv) is "cr"
  tell application "Terminal"
    repeat with w in windows
      repeat with t in tabs of w
        if (tty of t is objetivo) and (busy of t) then
          do script texto in t
          -- Codex no envía con el salto de línea de "do script": necesita un Enter (CR) aparte.
          if conCR then do script (ASCII character 13) in t
          return "ok"
        end if
      end repeat
    end repeat
  end tell
  return "no"
end run
"""


def enviar_a_terminal(tty, texto, comando=False, agente="claude"):
    if comando:
        if texto not in _PERMITIDOS:     # solo los comandos exactos de la lista
            raise RuntimeError("comando no permitido")
    else:
        texto = _una_linea(texto)
    if not re.fullmatch(r"/dev/ttys\d+", tty or ""):
        raise RuntimeError("terminal no válida")
    r = subprocess.run(["/usr/bin/osascript", "-e", _SCRIPT_TERMINAL, tty, texto, "cr" if agente == "codex" else "lf"],
                       capture_output=True, text=True, timeout=10)
    if r.returncode != 0:
        if "-1743" in r.stderr:
            raise RuntimeError("macOS no dio permiso para controlar Terminal (Privacidad > Automatización)")
        raise RuntimeError("osascript falló: " + r.stderr.strip()[-120:])
    if r.stdout.strip() != "ok":
        raise RuntimeError("la sesión no está en la app Terminal")
    print(f"voz: mensaje enviado a {tty}", flush=True)


# ------------------------------------------------------------- lanzar ----

def lanzar(instruccion, carpeta):
    """Abre una ventana de Terminal con `claude "<instrucción>"` en la carpeta indicada.
    La instrucción va en un archivo aparte y se lee con $(cat ...): nunca se interpola en el
    script, así que no hay inyección de comandos. La carpeta solo puede ser ~ o una de ~/proyectos."""
    instruccion = _sin_opciones(instruccion)
    if not instruccion:
        raise RuntimeError("instrucción vacía")
    carpeta = Path(carpeta)
    if not (carpeta == CARPETA_DEFECTO or carpeta.parent == RAIZ_PROYECTOS) or not carpeta.is_dir():
        raise RuntimeError("carpeta no permitida")
    LANZAMIENTOS.mkdir(exist_ok=True)
    os.chmod(LANZAMIENTOS, 0o700)
    ident = time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:6]
    prompt = LANZAMIENTOS / f"{ident}.txt"
    script = LANZAMIENTOS / f"{ident}.command"
    prompt.write_text(instruccion)
    script.write_text(
        "#!/bin/zsh -l\n"
        f"cd {_comillas(str(carpeta))} || exit 1\n"
        f"PROMPT=\"$(cat {_comillas(str(prompt))})\"\n"
        f"rm -f {_comillas(str(prompt))} {_comillas(str(script))}\n"
        f"{_comillas(CLAUDE)} \"$PROMPT\"\n"
        "c=$?\n"
        "if [ $c -ne 0 ]; then echo; echo \"claude terminó con código $c\"; read -k1 '?Tecla para cerrar'; fi\n"
    )
    print(f"voz: lanzando sesión en {_ruta_corta(carpeta)}", flush=True)
    os.chmod(script, 0o700)
    subprocess.run(["/usr/bin/open", "-a", "Terminal", str(script)], check=True, timeout=10)


def _comillas(s):
    return "'" + s.replace("'", "'\\''") + "'"
