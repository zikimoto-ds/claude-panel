#!/usr/bin/env python3
"""Capturas de la pantalla de la placa para el README (por el USB serial).

Pone la placa en modo demo (datos ficticios: no muestra sesiones, carpetas ni comandos reales),
pide cada cuadro tal cual está en su framebuffer (RGB565) y lo guarda como PNG redondo; también
arma un GIF de la vista principal. Al terminar devuelve la placa a los datos reales.

Uso (con el .venv del proyecto, que tiene pyserial y Pillow):
    .venv/bin/python herramientas/captura.py [/dev/cu.usbmodem101] [carpeta_salida]
"""

import sys
import time
from pathlib import Path

import serial
from PIL import Image, ImageDraw

PUERTO = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem101"
SALIDA = Path(sys.argv[2] if len(sys.argv) > 2 else Path(__file__).resolve().parent.parent / "docs" / "img")
ESCENAS = ["principal", "seis", "permiso", "detalle", "tokens", "grabando", "comando", "mensaje"]


def leer_linea(s, prefijo, plazo=10):
    fin = time.time() + plazo
    while time.time() < fin:
        l = s.readline().decode(errors="replace").strip()
        if l.startswith(prefijo):
            return l
    raise TimeoutError(f"no llegó '{prefijo}'")


def capturar(s, t_ms):
    s.reset_input_buffer()
    s.write(f"CAPTURA {t_ms}\n".encode())
    _, ancho, alto, n, suma = leer_linea(s, "CAPTURA ").split()
    ancho, alto, n, suma = int(ancho), int(alto), int(n), int(suma)
    datos = bytearray()
    fin = time.time() + 20
    while len(datos) < n and time.time() < fin:
        datos += s.read(n - len(datos))
    if len(datos) != n or (sum(datos) & 0xFFFFFFFF) != suma:
        raise IOError(f"captura incompleta o corrupta ({len(datos)}/{n} bytes)")
    leer_linea(s, "FIN")
    # RGB565 little-endian -> RGB888
    px = bytearray(ancho * alto * 3)
    for i in range(ancho * alto):
        v = datos[2 * i] | (datos[2 * i + 1] << 8)
        r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
        px[3 * i:3 * i + 3] = bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
    img = Image.frombytes("RGB", (ancho, alto), bytes(px))
    # Máscara circular: la pantalla es redonda
    mascara = Image.new("L", (ancho * 4, alto * 4), 0)
    ImageDraw.Draw(mascara).ellipse((0, 0, ancho * 4 - 1, alto * 4 - 1), fill=255)
    redonda = img.convert("RGBA")
    redonda.putalpha(mascara.resize((ancho, alto), Image.LANCZOS))
    return redonda


def main():
    SALIDA.mkdir(parents=True, exist_ok=True)
    with serial.Serial(PUERTO, 115200, timeout=1) as s:
        time.sleep(0.5)
        try:
            for escena in ESCENAS:
                s.write(f"DEMO {escena}\n".encode())
                leer_linea(s, "DEMO ")
                t0 = time.perf_counter()
                img = capturar(s, 10_000)
                img.save(SALIDA / f"{escena}.png", optimize=True)
                print(f"{escena}.png  ({time.perf_counter() - t0:.1f} s)")
            # GIF de la vista principal: 16 cuadros a 4 fps con reloj simulado (animación fiel)
            s.write(b"DEMO principal\n")
            leer_linea(s, "DEMO ")
            cuadros = []
            for k in range(16):
                fondo = Image.new("RGB", (466, 466), (13, 17, 23))   # fondo oscuro tipo GitHub
                c = capturar(s, 20_000 + k * 250)
                fondo.paste(c, (0, 0), c)
                cuadros.append(fondo.resize((360, 360), Image.LANCZOS).convert("P", palette=Image.ADAPTIVE, colors=64))
            cuadros[0].save(SALIDA / "demo.gif", save_all=True, append_images=cuadros[1:], duration=250, loop=0, optimize=True)
            print("demo.gif")
        finally:
            s.write(b"DEMO 0\n")


if __name__ == "__main__":
    main()
