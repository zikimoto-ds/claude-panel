// Panel de sesiones de Claude Code y Codex en la Waveshare ESP32-S3-Touch-AMOLED-1.75C.
// Recibe el estado del servidor en la Mac y dibuja:
//   - anillo delgado al borde: consumo de tokens del bloque de 5 h de Claude
//   - hasta 6 sesiones como muñequitos pixel (naranja = Claude, azul = Codex), animados según
//     su estado: trabajando (brazos), pide permiso (mano arriba y "!" ámbar), en espera (quieto)
//   - botón BOOT: empezar / terminar de grabar una instrucción de voz

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <Arduino_GFX_Library.h>
#include <mbedtls/md.h>
#include <Wire.h>
#include <ESP_I2S.h>
#include "TouchDrvCSTXXX.hpp"
#include "es8311.h"
#include "es7210.h"
#include <mbedtls/sha256.h>
#include "secrets.h"

// Pines del repositorio oficial waveshareteam/ESP32-S3-Touch-AMOLED-1.75C (board_config/pin_config.h)
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 38
#define LCD_RESET 1
#define LCD_CS 12
#define ANCHO 466
#define ALTO 466
#define CX 233
#define CY 233

Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *pantalla = new Arduino_CO5300(bus, LCD_RESET, 0, ANCHO, ALTO, 6, 0, 0, 0);
Arduino_Canvas *lienzo = new Arduino_Canvas(ANCHO, ALTO, pantalla);  // búfer completo en PSRAM, evita parpadeo

// Colores RGB565
const uint16_t C_FONDO = 0x0000;
const uint16_t C_PISTA = 0x18E6;   // gris azulado oscuro
const uint16_t C_FILA = 0x10A3;
const uint16_t C_FILA_P = 0x3940;  // ámbar oscuro
const uint16_t C_TINTA = 0xEF7D;
const uint16_t C_TENUE = 0x8C92;
const uint16_t C_VERDE = 0x3EF0;
const uint16_t C_AMBAR = 0xFD84;
const uint16_t C_ROJO = 0xFA6B;
const uint16_t C_AZUL = 0x4D1F;
const uint16_t C_GRIS = 0x4A8C;
const uint16_t C_CLAUDE = 0xF448;     // #F58A45
const uint16_t C_CLAUDE_T = 0x9285;   // naranja tenue (en espera)
const uint16_t C_CODEX = 0x5D7F;      // #58ACFF
const uint16_t C_CODEX_T = 0x3333;    // azul tenue (en espera)
const uint16_t C_PERMISO = 0xFE8C;    // #FFD166
#define MAX_SES 6

struct Sesion {
  String nombre, proyecto, herramienta, origen;
  String espera, resumen;  // permiso contestable: id y comando/archivo (vacío por el túnel)
  String sid;              // id de la sesión si acepta mensajes por voz (Claude en Terminal)
  char estado;
  char agente;  // 'c' = Claude Code, 'x' = Codex
  long salida;
  int esperaSeg, minutos;
};

struct Estado {
  bool valido = false;
  float pct = 0, psem = 0;
  String reinicia;
  long usado = 0, limite = 0, usem = 0, lsem = 0, hoy = 0;
  int hmsg = 0;
  String rsem;
  int libera = -1, np = 0, nt = 0, nr = 0, total = 0;
  long na = 0;
  int nses = 0;
  Sesion ses[MAX_SES];
} estado;

WiFiMulti redes;
String servidor;              // IP resuelta del servidor
String errorTxt = "Conectando WiFi...";
uint32_t ultimaConsulta = 0, ultimoDibujo = 0;
int fallos = 0;
bool modoTunel = false;          // true cuando la Mac no es visible en la red local
uint32_t ultimoSondeoLan = 0;
WiFiClientSecure clienteTls;

bool aplicar(JsonDocument &doc);

// ------------------------------------------------------------------ red ----

void resolverServidor() {
  IPAddress ip = MDNS.queryHost(SERVIDOR_MDNS, 1500);
  String respaldo = SERVIDOR_IP;
#ifdef SERVIDOR_IP_2
  if (WiFi.SSID() == WIFI_SSID_2) respaldo = SERVIDOR_IP_2;
#endif
#ifdef SERVIDOR_IP_3
  if (WiFi.SSID() == WIFI_SSID_3) respaldo = SERVIDOR_IP_3;
#endif
  servidor = (ip != IPAddress(0, 0, 0, 0)) ? ip.toString() : respaldo;
  Serial.printf("Servidor: %s\n", servidor.c_str());
}

String hmacHex(const String &mensaje) {
  uint8_t salida[32];
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t *)PANEL_TOKEN, strlen(PANEL_TOKEN),
                  (const uint8_t *)mensaje.c_str(), mensaje.length(), salida);
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", salida[i]);
  return String(hex);
}

// ---- Red local: una consulta HTTP por segundo -------------------------------------------
// Devuelve el código HTTP, o negativo si falla la conexión, la firma o el JSON.
int pedirLocal(JsonDocument &doc) {
  if (servidor.isEmpty()) resolverServidor();
  HTTPClient http;
  http.setConnectTimeout(800);
  http.setTimeout(1500);
  if (!http.begin("http://" + servidor + ":" + String(SERVIDOR_PUERTO) + "/api/placa")) return -100;
  // Firma HMAC con hora: el token nunca viaja. Sin hora NTP todavía no se consulta.
  time_t ts = time(nullptr);
  if (ts <= 1700000000) {
    http.end();
    return -300;
  }
  const char *cabeceras[] = {"X-Panel-Firma-Resp"};
  http.collectHeaders(cabeceras, 1);
  String tsTxt = String((long)ts);
  http.addHeader("X-Panel-Ts", tsTxt);
  http.addHeader("X-Panel-Firma", hmacHex("GET /api/placa " + tsTxt));
  int codigo = http.GET();
  if (codigo == 200) {
    String cuerpo = http.getString();
    if (!hmacHex(tsTxt + "\n" + cuerpo).equals(http.header("X-Panel-Firma-Resp"))) codigo = -400;
    else if (deserializeJson(doc, cuerpo)) codigo = -200;
  }
  http.end();
  return codigo;
}

// ---- Túnel: una sola conexión HTTPS abierta (streaming) ----------------------------------
// ngrok gratis cuenta peticiones (20,000/mes), no tiempo conectado: el servidor empuja cada
// cambio por la misma conexión y un latido cada 15 s. Cada línea: "<hmac> <seq> <json>".
// La librería TLS de la placa no logra validar la cadena de ngrok (probado con raíces ISRG y
// con el paquete integrado), así que TLS va sin verificar y la autenticidad la da el HMAC.
bool streamActivo = false;
bool streamChunked = false;
long chunkRestante = 0;
long ultimaSeq = 0;
String streamTs, lineaStream;
uint32_t ultimoDatoStream = 0, ultimoIntentoStream = 0;

void cerrarStream() {
  clienteTls.stop();
  streamActivo = false;
}

bool abrirStream() {
  cerrarStream();
  if (time(nullptr) < 1700000000) {
    errorTxt = "Esperando hora (NTP)";
    return false;
  }
  if (!clienteTls.connect(TUNEL_HOST, 443)) {
    char err[100] = "";
    clienteTls.lastError(err, sizeof(err));
    Serial.printf("Túnel: sin conexión TLS (%s)\n", err);
    errorTxt = "Sin túnel";
    return false;
  }
  streamTs = String((long)time(nullptr));
  clienteTls.print(String("GET /api/placa/stream HTTP/1.1\r\nHost: " TUNEL_HOST "\r\n") +
                   "X-Panel-Ts: " + streamTs + "\r\n" +
                   "X-Panel-Firma: " + hmacHex("GET /api/placa/stream " + streamTs) + "\r\n" +
                   "ngrok-skip-browser-warning: 1\r\nUser-Agent: claude-panel/1\r\n\r\n");
  // Cabeceras de respuesta
  clienteTls.setTimeout(8000);
  String linea = clienteTls.readStringUntil('\n');
  int codigo = linea.length() > 12 ? linea.substring(9, 12).toInt() : 0;
  streamChunked = false;
  while (clienteTls.connected()) {
    linea = clienteTls.readStringUntil('\n');
    linea.trim();
    if (linea.isEmpty()) break;
    linea.toLowerCase();
    if (linea.startsWith("transfer-encoding:") && linea.indexOf("chunked") > 0) streamChunked = true;
  }
  clienteTls.setTimeout(1000);
  if (codigo != 200) {
    Serial.printf("Túnel: código %d\n", codigo);
    errorTxt = codigo == 401 ? "Firma rechazada" : "Sin túnel";
    cerrarStream();
    return false;
  }
  Serial.println("Túnel: conexión abierta");
  chunkRestante = 0;
  ultimaSeq = 0;
  lineaStream = "";
  ultimoDatoStream = millis();
  streamActivo = true;
  return true;
}

void procesarLineaStream(const String &l) {
  int a = l.indexOf(' '), b = l.indexOf(' ', a + 1);
  if (a != 64 || b < 0) return;
  String firma = l.substring(0, a), seqTxt = l.substring(a + 1, b), json = l.substring(b + 1);
  long seq = seqTxt.toInt();
  if (seq <= ultimaSeq || !hmacHex(streamTs + "\n" + seqTxt + "\n" + json).equals(firma)) {
    Serial.println("Túnel: línea con firma o secuencia inválida, descartada");
    return;
  }
  ultimaSeq = seq;
  JsonDocument doc;
  if (deserializeJson(doc, json)) return;
  aplicar(doc);
  fallos = 0;
  ultimoDatoStream = millis();
}

void leerStream() {
  while (clienteTls.available()) {
    if (streamChunked && chunkRestante <= 0) {  // cabecera de bloque: tamaño en hexadecimal
      String t = clienteTls.readStringUntil('\n');
      t.trim();
      if (t.isEmpty()) continue;  // CRLF que cierra el bloque anterior
      chunkRestante = strtol(t.c_str(), nullptr, 16);
      if (chunkRestante == 0) {  // fin de la respuesta
        cerrarStream();
        return;
      }
      continue;
    }
    int c = clienteTls.read();
    if (c < 0) break;
    if (streamChunked) chunkRestante--;
    if (c == '\n') {
      procesarLineaStream(lineaStream);
      lineaStream = "";
    } else if (c != '\r' && lineaStream.length() < 4096) {
      lineaStream += (char)c;
    }
  }
  if (!clienteTls.connected() || millis() - ultimoDatoStream > 40000) {  // sin latido: reconectar
    Serial.println("Túnel: conexión cerrada o sin latido");
    cerrarStream();
  }
}

// ---- Orquestación ------------------------------------------------------------------------
// Red local primero; si la Mac no aparece en 5 intentos, túnel. En túnel se prueba la red
// local cada 60 s para volver a ella.
void actualizarDatos(uint32_t ahora) {
  if (WiFi.status() != WL_CONNECTED) {
    errorTxt = "Sin WiFi";
    cerrarStream();
    return;
  }
  JsonDocument doc;
  if (!modoTunel) {
    if (ahora - ultimaConsulta < 1000) return;
    ultimaConsulta = ahora;
    int codigo = pedirLocal(doc);
    if (codigo == 200) {
      fallos = 0;
      aplicar(doc);
      return;
    }
    if (codigo == -300) {   // sin hora no hay firma: esperar NTP sin contar como fallo
      errorTxt = "Sincronizando hora...";
      return;
    }
    errorTxt = codigo == 401 ? "Firma rechazada" : codigo == -400 ? "Respuesta sin firma válida" : "Sin servidor";
    Serial.printf("Consulta local fallida: %s\n", errorTxt.c_str());
    if (++fallos % 5 == 0) resolverServidor();  // la IP de la Mac pudo cambiar
    if (fallos >= 5) {
      Serial.println("Mac no visible en la red local; uso el túnel");
      modoTunel = true;
      fallos = 0;
      ultimoSondeoLan = ahora;
      ultimoIntentoStream = 0;
    }
    return;
  }
  if (ahora - ultimoSondeoLan >= 60000) {
    ultimoSondeoLan = ahora;
    if (pedirLocal(doc) == 200) {
      Serial.println("Mac visible en la red local; dejo el túnel");
      cerrarStream();
      modoTunel = false;
      fallos = 0;
      aplicar(doc);
      return;
    }
    doc.clear();
  }
  if (streamActivo) {
    leerStream();
  } else if (ultimoIntentoStream == 0 || ahora - ultimoIntentoStream >= 10000) {
    ultimoIntentoStream = ahora;
    if (!abrirStream()) fallos++;
  }
}

bool aplicar(JsonDocument &doc) {
  estado.pct = doc["pct"] | 0.0f;
  estado.psem = doc["psem"] | 0.0f;
  estado.reinicia = doc["reinicia"] | "";
  estado.usado = doc["usado"] | 0L;
  estado.limite = doc["limite"] | 0L;
  estado.libera = doc["libera"] | -1;
  estado.na = doc["na"] | 0L;
  estado.np = doc["np"] | 0;
  estado.nt = doc["nt"] | 0;
  estado.nr = doc["nr"] | 0;
  estado.total = doc["total"] | 0;
  JsonArray arr = doc["ses"].as<JsonArray>();
  estado.nses = 0;
  for (JsonObject s : arr) {
    if (estado.nses >= MAX_SES) break;
    Sesion &d = estado.ses[estado.nses++];
    d.nombre = s["n"] | "";
    d.proyecto = s["p"] | "";
    const char *e2 = s["e"] | "E";
    d.estado = e2[0];
    d.salida = s["o"] | 0L;
    d.esperaSeg = s["w"] | 0;
    d.herramienta = s["m"] | "";
    d.origen = s["og"] | "";
    d.minutos = s["min"] | 0;
    const char *a = s["a"] | "c";
    d.agente = a[0];
    d.espera = s["q"] | "";
    d.resumen = s["r"] | "";
    d.sid = s["i"] | "";
  }
  estado.usem = doc["usem"] | 0L;
  estado.lsem = doc["lsem"] | 0L;
  estado.rsem = doc["rsem"] | "";
  estado.hoy = doc["hoy"] | 0L;
  estado.hmsg = doc["hmsg"] | 0;
  estado.valido = true;
  return true;
}

// -------------------------------------------------------------- dibujo ----

String abreviar(long n) {
  if (n >= 1000000) return String(n / 1000000.0, 1) + "M";
  if (n >= 1000) return String(n / 1000) + "k";
  return String(n);
}

int anchoTexto(const String &s) {
  int16_t x1, y1;
  uint16_t w, h;
  lienzo->getTextBounds(s.c_str(), 0, 0, &x1, &y1, &w, &h);
  return w;
}

// Recorta a un ancho máximo sin partir caracteres UTF-8 (acentos).
String recortar(String s, int maxAncho) {
  if (anchoTexto(s) <= maxAncho) return s;
  while (s.length() > 0 && anchoTexto(s + "...") > maxAncho) {
    int i = s.length() - 1;
    while (i > 0 && (s[i] & 0xC0) == 0x80) i--;  // retrocede hasta el inicio del carácter
    s.remove(i);
  }
  return s + "...";
}

void centrado(const String &s, int y, uint16_t color) {
  lienzo->setTextColor(color);
  lienzo->setCursor(CX - anchoTexto(s) / 2, y);
  lienzo->print(s);
}

// Arco desde las 12 en sentido horario; Arduino_GFX mide grados desde las 3.
void arco(int r1, int r2, float desde, float grados, uint16_t color) {
  if (grados <= 0) return;
  float ini = 270 + desde, fin = ini + grados;
  if (fin <= 360) {
    lienzo->fillArc(CX, CY, r1, r2, ini, fin, color);
  } else if (ini >= 360) {
    lienzo->fillArc(CX, CY, r1, r2, ini - 360, fin - 360, color);
  } else {
    lienzo->fillArc(CX, CY, r1, r2, ini, 360, color);
    lienzo->fillArc(CX, CY, r1, r2, 0, fin - 360, color);
  }
}

// ---- Muñequitos ----------------------------------------------------------------------------
// Personajes originales en una cuadrícula de 16×16, dibujados con rectángulos y escalados por
// enteros. Claude: cuerpo cuadrado, ojos altos, cuatro patas. Codex: cabeza escalonada, antena,
// ojos cuadrados, dos patas. Cuadros: trabajando 4 (brazos alternados + rebote), permiso 2
// (mano arriba + "!" parpadeando), en espera quieto con parpadeo cada ~4 s.
struct Pincel {
  int ox, oy, s;
  void px(int x, int y, int w, int h, uint16_t c) const { lienzo->fillRect(ox + x * s, oy + y * s, w * s, h * s, c); }
};

void brazos(const Pincel &p, char estado, int cuadro, int yMedio, uint16_t c) {
  if (estado == 'P') {                       // izquierdo normal, derecho levantado
    p.px(0, yMedio, 2, 2, c);
    p.px(14, yMedio - 6, 2, 8, c);
  } else if (estado == 'T') {                // alternados
    int f = cuadro % 4;
    int dyI = f == 0 ? -2 : f == 2 ? 2 : 0;
    p.px(0, yMedio + dyI, 2, 2, c);
    p.px(14, yMedio - dyI, 2, 2, c);
  } else {
    p.px(0, yMedio, 2, 2, c);
    p.px(14, yMedio, 2, 2, c);
  }
}

// rapido: brazos al doble (lo están azotando). dormido: ojos cerrados y "z". fijo: "!" sin parpadeo.
void dibujarMunequito(int cx, int cy, int s, char agente, char estado, uint32_t t,
                      bool rapido = false, bool dormido = false, bool fijo = false) {
  int cuadro = t / (rapido ? 125 : 250);                   // 4 cuadros por segundo (8 si lo azotan)
  bool parpadea = estado != 'T' && (t % 4000) < 150;
  int rebote = (estado == 'T' && cuadro % 2) ? -1 : 0;
  Pincel p{cx - 8 * s, cy - 8 * s + rebote * s, s};
  bool codex = agente == 'x';
  uint16_t c = codex ? (estado == 'E' ? C_CODEX_T : C_CODEX) : (estado == 'E' ? C_CLAUDE_T : C_CLAUDE);
  if (codex) {
    p.px(10, -2, 3, 1, c);  p.px(11, -1, 1, 2, c);        // antena
    p.px(4, 1, 8, 1, c);    p.px(3, 2, 10, 1, c);
    p.px(2, 3, 12, 8, c);   p.px(3, 11, 10, 1, c);
    p.px(4, 12, 2, 2, c);   p.px(10, 12, 2, 2, c);         // patas
    if (dormido || parpadea) { p.px(5, 6, 2, 1, C_FONDO); p.px(9, 6, 2, 1, C_FONDO); }
    else                     { p.px(5, 5, 2, 2, C_FONDO); p.px(9, 5, 2, 2, C_FONDO); }
    brazos(p, estado, cuadro, 7, c);
  } else {
    p.px(2, 2, 12, 9, c);
    p.px(3, 11, 1, 2, c);   p.px(5, 11, 1, 2, c);          // patas
    p.px(10, 11, 1, 2, c);  p.px(12, 11, 1, 2, c);
    if (dormido || parpadea) { p.px(5, 6, 1, 1, C_FONDO); p.px(10, 6, 1, 1, C_FONDO); }
    else                     { p.px(5, 5, 1, 2, C_FONDO); p.px(10, 5, 1, 2, C_FONDO); }
    brazos(p, estado, cuadro, 6, c);
  }
  if (rapido && (t / 250) % 2 == 0) {                      // gotas de sudor
    p.px(15, 1, 1, 2, C_AZUL);
    p.px(-1, 3, 1, 2, C_AZUL);
  }
  if (dormido) {                                           // "z" que sube y baja
    int dz = (t / 700) % 2;
    p.px(14, -3 - dz, 3, 1, C_TENUE); p.px(15, -2 - dz, 1, 1, C_TENUE); p.px(14, -1 - dz, 3, 1, C_TENUE);
  }
  if (estado == 'P' && (fijo || (t / 500) % 2 == 0)) {     // "!" ámbar a la derecha
    p.px(17, 0, 2, 5, C_PERMISO);
    p.px(17, 6, 2, 2, C_PERMISO);
  }
}

// ---- Personita y látigo (broma) ------------------------------------------------------------
// Personita 8×12: cabeza, playera gris, pantalón oscuro. Poses: 'G' golpeada (salta y alza los
// brazos), 'A' azotando (brazo derecho estirado), 'C' sentada con café humeante.
const uint16_t C_PIEL = 0xF654, C_PLAYERA = 0x8C92, C_PANTALON = 0x4A8C, C_LATIGO = 0x8AC5, C_CAFE = 0x8AC5;

void dibujarPersonita(int cx, int baseY, int sp, char pose, uint32_t t, int &manoX, int &manoY) {
  int cuadro = t / 250;
  int salto = (pose == 'G' && cuadro % 3 == 2) ? -2 : 0;
  int alto = pose == 'C' ? 10 : 12;
  Pincel p{cx - 4 * sp, baseY - alto * sp + salto * sp, sp};
  p.px(2, 0, 4, 4, C_PIEL);
  p.px(3, 1, 1, 1, C_FONDO); p.px(5, 1, 1, 1, C_FONDO);  // ojos
  if (pose == 'G' && cuadro % 3 == 2) p.px(3, 3, 2, 1, C_FONDO);   // boca de "¡auch!"
  p.px(2, 4, 4, 4, C_PLAYERA);
  if (pose == 'G') {                                       // brazos arriba al recibir el golpe
    if (cuadro % 3 == 2) { p.px(1, 1, 1, 3, C_PLAYERA); p.px(6, 1, 1, 3, C_PLAYERA); }
    else                 { p.px(1, 4, 1, 3, C_PLAYERA); p.px(6, 4, 1, 3, C_PLAYERA); }
  } else if (pose == 'A') {                                // brazo derecho hacia el muñequito
    p.px(1, 4, 1, 3, C_PLAYERA);
    p.px(6, 4, 2, 1, C_PLAYERA);
  } else {                                                 // sentada con taza
    p.px(1, 4, 1, 3, C_PLAYERA);
    p.px(6, 5, 1, 1, C_PLAYERA);
  }
  if (pose == 'C') {
    p.px(2, 8, 5, 1, C_PANTALON); p.px(6, 9, 1, 1, C_PANTALON);   // piernas dobladas
    p.px(7, 4, 2, 2, C_CAFE);                                     // taza
    p.px(7 + (t / 500) % 2, 2, 1, 1, C_TENUE);                    // vapor
  } else {
    p.px(2, 8, 1, 4, C_PANTALON); p.px(5, 8, 1, 4, C_PANTALON);
  }
  manoX = p.ox + 8 * sp;
  manoY = p.oy + 4 * sp;
}

// Látigo de (x1,y1) a (x2,y2) en 3 cuadros: arriba, a medio camino, chasquido con chispa.
void dibujarLatigo(int x1, int y1, int x2, int y2, uint32_t t, int grosor) {
  int f = (t / 250) % 3;
  float alzada[] = {-0.9f, -0.35f, 0.15f};
  float alcance[] = {0.55f, 0.85f, 1.0f};
  float ex = x1 + (x2 - x1) * alcance[f], ey = y1 + (y2 - y1) * alcance[f];
  float mx = (x1 + ex) / 2, my = (y1 + ey) / 2 + alzada[f] * abs(x2 - x1);
  int px0 = x1, py0 = y1;
  for (int i = 1; i <= 10; i++) {                          // curva cuadrática en 10 tramos
    float u = i / 10.0f;
    int qx = (1 - u) * (1 - u) * x1 + 2 * (1 - u) * u * mx + u * u * ex;
    int qy = (1 - u) * (1 - u) * y1 + 2 * (1 - u) * u * my + u * u * ey;
    for (int g = 0; g < grosor; g++) lienzo->drawLine(px0, py0 + g, qx, qy + g, C_LATIGO);
    px0 = qx; py0 = qy;
  }
  if (f == 2) {                                            // chasquido
    int r = 3 * grosor;
    lienzo->drawLine(ex - r, ey - r, ex + r, ey + r, C_PERMISO);
    lienzo->drawLine(ex - r, ey + r, ex + r, ey - r, C_PERMISO);
    lienzo->drawFastHLine(ex - r - 2, ey, 2 * r + 4, C_PERMISO);
  }
}

// Muñequito con su personita a la izquierda. conLatigo: escena completa (si no, solo gestos).
// Pidiendo permiso: el muñequito azota a la personita. Trabajando: al revés. En espera: café y siesta.
void dibujarEscena(int cx, int cy, int s, int sp, char agente, char estado, uint32_t t, bool conLatigo, int hueco) {
  bool trabajando = estado == 'T', permiso = estado == 'P' || estado == 'R';   // R: te espera a ti
  dibujarMunequito(cx, cy, s, agente, estado, t, trabajando, estado == 'E', true);
  int base = cy + 6 * s;                                   // pies del muñequito
  int pcx = cx - 8 * s - hueco - 4 * sp;                   // centro de la personita
  int mx, my;
  dibujarPersonita(pcx, base, sp, permiso ? 'G' : trabajando ? 'A' : 'C', t, mx, my);
  if (!conLatigo) return;
  int g = s >= 4 ? 2 : 1;
  if (trabajando) dibujarLatigo(mx, my, cx - 8 * s + 2 * s, cy - 2 * s, t, g);              // personita → muñequito
  else if (permiso) dibujarLatigo(cx - 8 * s, cy - 2 * s, pcx, base - 9 * sp, t, g);         // muñequito → personita
}

// Posición y escala de cada muñequito según cuántas sesiones hay (posiciones estables).
void lugar(int i, int n, int &x, int &y, int &s) {
  if (n <= 1)      { x = CX; y = 222; s = 6; }
  else if (n == 2) { x = i == 0 ? 152 : 314; y = 222; s = 5; }
  else if (n == 3) { const int xs[] = {152, 314, CX}, ys[] = {178, 178, 286}; x = xs[i]; y = ys[i]; s = 4; }
  else if (n == 4) { x = i % 2 ? 314 : 152; y = i < 2 ? 178 : 286; s = 4; }
  else {
    s = 3;
    y = i < 3 ? 182 : 286;
    if (i < 3) x = 123 + i * 110;
    else x = (n == 5) ? 178 + (i - 3) * 110 : 123 + (i - 3) * 110;
  }
  x += n <= 4 ? 24 : 10;   // la personita va a la izquierda: centrar el par
}

void dibujarPrincipal(bool) {
  uint32_t t = millis();
  // Anillo delgado: bloque de 5 h de Claude
  float pct = constrain(estado.pct, 0.0f, 100.0f);
  lienzo->fillArc(CX, CY, 226, 221, 0, 360, C_PISTA);
  arco(226, 221, 0, pct * 3.6f, pct >= 90 ? C_ROJO : pct >= 70 ? C_AMBAR : C_CLAUDE);

  // Cabecera
  lienzo->setFont(u8g2_font_helvR12_tf);
  if (estado.np > 0) {
    String cab = String(estado.np) + (estado.np == 1 ? " pide permiso" : " piden permiso");
    if (estado.nt > 0) cab = String(estado.nt) + " trabajando · " + cab;
    centrado(cab, 96, C_PERMISO);
  } else if (estado.nr > 0) {
    String cab = String(estado.nr) + (estado.nr == 1 ? " te espera" : " te esperan");
    if (estado.nt > 0) cab = String(estado.nt) + " trabajando · " + cab;
    centrado(cab, 96, C_TINTA);
  } else if (estado.total > 0) {
    centrado(String(estado.nt) + " trabajando · " + String(estado.total) + (estado.total == 1 ? " abierta" : " abiertas"), 96, C_TENUE);
  }

  // Muñequitos con su título debajo
  int n = estado.nses;
  for (int i = 0; i < n; i++) {
    Sesion &s = estado.ses[i];
    int x, y, esc;
    lugar(i, n, x, y, esc);
    dibujarEscena(x, y, esc, esc >= 5 ? 3 : 2, s.agente, s.estado, t, n <= 4, n <= 4 ? 20 : 6);   // látigo solo con 1–4
    lienzo->setFont(esc >= 5 ? u8g2_font_helvR12_tf : u8g2_font_helvR10_tf);
    String etiqueta = recortar(s.nombre.length() ? s.nombre : s.proyecto, n <= 2 ? 150 : n <= 4 ? 140 : 100);
    lienzo->setTextColor(s.estado == 'P' ? C_PERMISO : C_TENUE);
    lienzo->setCursor(x - anchoTexto(etiqueta) / 2, y + 8 * esc + 22);
    lienzo->print(etiqueta);
  }
  if (n == 0) {
    lienzo->setFont(u8g2_font_helvR12_tf);
    centrado("Sin sesiones abiertas", CY, C_TENUE);
  }

  // Pie: consumo
  lienzo->setFont(u8g2_font_helvR10_tf);
  String pie = "5 h " + String(estado.pct, 0) + "%  ·  semana " + String(estado.psem, 0) + "%";
  centrado(pie, 372, C_TENUE);
}

// Toque sobre un muñequito (misma geometría que dibujarPrincipal); -1 si no hay.
int sesionEn(int x0, int y0) {
  for (int i = 0; i < estado.nses; i++) {
    int x, y, s;
    lugar(i, estado.nses, x, y, s);
    int medio = 8 * s + 16;
    if (abs(x0 - x) <= medio && y0 >= y - medio && y0 <= y + medio + 24) return i;
  }
  return -1;
}

// Parte un texto en renglones que caben en maxAncho (respeta UTF-8 porque corta en espacios).
int envolver(const String &texto, int maxAncho, String *renglones, int maxRenglones) {
  int n = 0;
  String actual;
  int i = 0;
  while (i <= (int)texto.length() && n < maxRenglones) {
    int j = texto.indexOf(' ', i);
    if (j < 0) j = texto.length();
    String palabra = texto.substring(i, j);
    String prueba = actual.isEmpty() ? palabra : actual + " " + palabra;
    if (anchoTexto(prueba) <= maxAncho || actual.isEmpty()) {
      actual = prueba;
    } else {
      renglones[n++] = actual;
      actual = palabra;
    }
    i = j + 1;
  }
  if (n < maxRenglones && actual.length()) renglones[n++] = actual;
  else if (n == maxRenglones && i <= (int)texto.length()) renglones[n - 1] = recortar(renglones[n - 1] + " " + actual, maxAncho);
  return n;
}

String duracion(int minutos) {
  if (minutos < 60) return String(minutos) + " min";
  return String(minutos / 60) + " h " + String(minutos % 60) + " min";
}

// Vista 2: consumo de tokens en grande.
void dibujarTokens() {
  float pct = constrain(estado.pct, 0.0f, 100.0f), psem = constrain(estado.psem, 0.0f, 100.0f);
  auto colorPct = [](float p) { return p >= 90 ? C_ROJO : p >= 70 ? C_AMBAR : C_AZUL; };
  lienzo->fillArc(CX, CY, 224, 206, 0, 360, C_PISTA);
  arco(224, 206, 0, pct * 3.6f, colorPct(pct));
  lienzo->fillArc(CX, CY, 198, 190, 0, 360, C_PISTA);
  arco(198, 190, 0, psem * 3.6f, psem >= 70 ? colorPct(psem) : C_TENUE);

  lienzo->setFont(u8g2_font_helvR12_tf);
  centrado("Sesión", 110, C_TENUE);
  lienzo->setFont(u8g2_font_helvB24_tf);
  centrado(String(estado.pct, 1) + "%", 148, C_TINTA);
  lienzo->setFont(u8g2_font_helvR10_tf);
  centrado(abreviar(estado.usado) + " / " + abreviar(estado.limite) +
           (estado.reinicia.length() ? "  ·  reinicia " + estado.reinicia : ""), 172, C_TENUE);

  lienzo->drawFastHLine(CX - 110, 198, 220, C_PISTA);

  lienzo->setFont(u8g2_font_helvR12_tf);
  centrado("Semana", 232, C_TENUE);
  lienzo->setFont(u8g2_font_helvB24_tf);
  centrado(String(estado.psem, 1) + "%", 270, C_TINTA);
  lienzo->setFont(u8g2_font_helvR10_tf);
  centrado(abreviar(estado.usem) + " / " + abreviar(estado.lsem) +
           (estado.rsem.length() ? "  ·  reinicia " + estado.rsem : ""), 294, C_TENUE);

  lienzo->drawFastHLine(CX - 110, 320, 220, C_PISTA);
  lienzo->setFont(u8g2_font_helvR12_tf);
  centrado("Hoy: " + abreviar(estado.hoy) + "  ·  " + String(estado.hmsg) + " mensajes", 352, C_TINTA);
}

// Botones del permiso (misma geometría en dibujo y toque)
const int BTN_Y = 300, BTN_H = 52, BTN_W = 132, BTN_RX = 92, BTN_AX = 242;   // Rechazar / Aprobar
float progresoAprobar = 0;   // 0..1 mientras se mantiene presionado Aprobar
bool enBoton(int x, int y, int bx) { return x >= bx && x <= bx + BTN_W && y >= BTN_Y - 6 && y <= BTN_Y + BTN_H + 6; }

// Vista 3b: permiso contestable desde la placa.
void dibujarPermiso(const Sesion &s) {
  lienzo->fillArc(CX, CY, 224, 216, 0, 360, C_PERMISO);
  static uint32_t tCongelado = 0;
  if (progresoAprobar == 0) tCongelado = millis();
  dibujarEscena(CX + 36, 80, 3, 3, s.agente, s.estado, tCongelado, true, 40);
  lienzo->setFont(u8g2_font_helvB12_tf);
  centrado(recortar(s.nombre, 300), 132, C_TINTA);
  lienzo->setFont(u8g2_font_helvB14_tf);
  String h = s.herramienta.length() ? s.herramienta : String("una herramienta");
  centrado((s.agente == 'x' ? "Codex pide: " : "Claude pide: ") + h, 168, C_PERMISO);
  lienzo->setFont(u8g2_font_helvR12_tf);
  if (s.resumen.length()) {
    String renglones[4];
    int n = envolver(s.resumen, 330, renglones, 4);
    for (int i = 0; i < n; i++) centrado(renglones[i], 200 + i * 22, C_TINTA);
  } else {
    centrado(modoTunel ? "(comando oculto: vía túnel)" : "", 214, C_TENUE);
  }
  // Rechazar
  lienzo->fillRoundRect(BTN_RX, BTN_Y, BTN_W, BTN_H, 26, C_FILA);
  lienzo->setFont(u8g2_font_helvB12_tf);
  lienzo->setTextColor(C_TINTA);
  lienzo->setCursor(BTN_RX + BTN_W / 2 - anchoTexto("Rechazar") / 2, BTN_Y + 33);
  lienzo->print("Rechazar");
  // Aprobar: se llena mientras se mantiene presionado
  lienzo->fillRoundRect(BTN_AX, BTN_Y, BTN_W, BTN_H, 26, 0x2C66);
  if (progresoAprobar > 0) lienzo->fillRoundRect(BTN_AX, BTN_Y, max(BTN_H, (int)(BTN_W * progresoAprobar)), BTN_H, 26, C_VERDE);
  lienzo->setTextColor(C_TINTA);
  lienzo->setCursor(BTN_AX + BTN_W / 2 - anchoTexto("Aprobar") / 2, BTN_Y + 33);
  lienzo->print("Aprobar");
  lienzo->setFont(u8g2_font_helvR08_tf);
  centrado("mantén Aprobar 1 s  ·  toca fuera para volver", 384, C_GRIS);
}

// Vista 3: detalle de una sesión.
void dibujarDetalle(const Sesion &s) {
  if (s.estado == 'P' && s.espera.length()) return dibujarPermiso(s);
  uint16_t cEstado = s.estado == 'T' ? C_VERDE : s.estado == 'P' ? C_AMBAR : s.estado == 'R' ? C_TINTA : C_TENUE;
  lienzo->fillArc(CX, CY, 224, 216, 0, 360, cEstado);
  dibujarEscena(CX + 36, 78, 3, 3, s.agente, s.estado, millis(), true, 40);

  lienzo->setFont(u8g2_font_helvB14_tf);
  String renglones[3];
  int n = envolver(s.nombre, 330, renglones, 3);
  int y = 130 - (n - 1) * 12;
  for (int i = 0; i < n; i++) centrado(renglones[i], y + i * 26, C_TINTA);

  lienzo->setFont(u8g2_font_helvR12_tf);
  centrado(recortar(s.proyecto, 320), 212, C_TENUE);
  centrado(s.origen + "  ·  abierta hace " + duracion(s.minutos), 236, C_TENUE);

  lienzo->setFont(u8g2_font_helvB14_tf);
  if (s.estado == 'P') {
    String h = s.herramienta.length() ? s.herramienta : String("una herramienta");
    centrado("Pide permiso: " + h, 286, C_AMBAR);
    lienzo->setFont(u8g2_font_helvR12_tf);
    centrado("esperando hace " + duracion(s.esperaSeg / 60) + (s.esperaSeg < 60 ? " (" + String(s.esperaSeg) + " s)" : ""), 310, C_AMBAR);
  } else {
    centrado(s.estado == 'T' ? "Trabajando" : s.estado == 'R' ? "Espera tu respuesta" : "En espera", 286, cEstado);
  }
  lienzo->setFont(u8g2_font_helvR12_tf);
  if (s.agente != 'x') centrado("Salida: " + abreviar(s.salida) + " tokens", 350, C_TENUE);
  lienzo->setFont(u8g2_font_helvR08_tf);
  centrado(s.sid.length() ? "BOOT: mensaje por voz  ·  toca para volver" : "toca para volver", 392, C_GRIS);
}

// ------------------------------------------------------------ vistas ----
enum Vista { PRINCIPAL, TOKENS, DETALLE, GRABANDO, PROCESANDO, CONFIRMAR, AVISO };
Vista vista = PRINCIPAL;
String procesandoTxt = "Transcribiendo...", procesandoSub = "whisper en tu Mac";
// Voz (definido más abajo)
struct Pendiente { String id, texto, carpeta, destino; } pendiente;
String destinoSid, destinoNombre;   // grabación dirigida a una sesión abierta (vacío = sesión nueva)
String avisoTxt;
uint16_t avisoColor = 0;
uint32_t avisoDesde = 0, confirmarDesde = 0;
volatile bool grabando = false;
size_t grabMuestras = 0;
uint32_t inicioGrab = 0;
void dibujarGrabando();
void dibujarConfirmar();
void dibujarAviso();
void iniciarGrabacion();
void terminarGrabacion();
void lanzarPendiente();
void mostrarAviso(const String &txt, uint16_t color);
String detalleClave;          // nombre + proyecto de la sesión abierta en detalle
uint32_t ultimoToque = 0;

int buscarDetalle() {
  for (int i = 0; i < estado.nses; i++)
    if (estado.ses[i].nombre + "|" + estado.ses[i].proyecto == detalleClave) return i;
  return -1;
}

void dibujar(bool encendidoAlerta) {
  lienzo->fillScreen(C_FONDO);
  if (!estado.valido) {
    lienzo->setFont(u8g2_font_helvB14_tf);
    centrado("Panel Claude Code", CY - 10, C_TINTA);
    lienzo->setFont(u8g2_font_helvR12_tf);
    centrado(errorTxt, CY + 22, C_TENUE);
    lienzo->flush();
    return;
  }
  int iDet = vista == DETALLE ? buscarDetalle() : -1;
  if (vista == DETALLE && iDet < 0) vista = PRINCIPAL;  // la sesión se cerró
  if (vista == PRINCIPAL) dibujarPrincipal(encendidoAlerta);
  else if (vista == TOKENS) dibujarTokens();
  else if (vista == DETALLE) dibujarDetalle(estado.ses[iDet]);
  else if (vista == GRABANDO) dibujarGrabando();
  else if (vista == PROCESANDO) {
    lienzo->fillArc(CX, CY, 224, 216, 0, 360, C_AZUL);
    lienzo->setFont(u8g2_font_helvB18_tf);
    centrado(procesandoTxt, CY, C_TINTA);
    lienzo->setFont(u8g2_font_helvR10_tf);
    centrado(procesandoSub, CY + 26, C_TENUE);
  } else if (vista == CONFIRMAR) dibujarConfirmar();
  else dibujarAviso();

  // Estado de conexión
  lienzo->setFont(u8g2_font_helvR08_tf);
  if (errorTxt.length() && fallos > 0) centrado(errorTxt, 420, C_ROJO);  // datos viejos: avisar sin borrar
  else if (modoTunel) centrado("vía túnel", 420, C_TENUE);
  // Indicador de página (principal / tokens)
  if (vista == PRINCIPAL || vista == TOKENS) {
    lienzo->fillCircle(CX - 8, 438, 3, vista == PRINCIPAL ? C_TINTA : C_GRIS);
    lienzo->fillCircle(CX + 8, 438, 3, vista == TOKENS ? C_TINTA : C_GRIS);
  }
  lienzo->flush();
}

// ------------------------------------------------------------- touch ----
// CST9217 por I2C (SDA 15, SCL 14), dirección 0x5A; INT 11, RST 2. Espejo XY como el ejemplo de Waveshare.
#define TP_INT 11
#define TP_RST 2
TouchDrvCST92xx touch;
bool hayTouch = false;

void iniciarTouch() {
  pinMode(TP_RST, OUTPUT);
  digitalWrite(TP_RST, LOW);
  delay(30);
  digitalWrite(TP_RST, HIGH);
  delay(50);
  touch.setPins(TP_RST, TP_INT);
  hayTouch = touch.begin(Wire, 0x5A, 15, 14);
  if (hayTouch) {
    touch.setMaxCoordinates(ANCHO, ALTO);
    touch.setMirrorXY(true, true);
  }
  Serial.printf("Touch: %s\n", hayTouch ? "OK" : "no responde");
}

void decidirPermiso(const String &espera, char d);

void gesto(int x0, int y0, int dx, int dy) {
  Serial.printf("Toque: x=%d y=%d dx=%d dy=%d\n", x0, y0, dx, dy);
  ultimoToque = millis();
  if (vista == CONFIRMAR) {  // botones: Cancelar (izquierda) / Lanzar (derecha)
    if (y0 >= 320 && y0 <= 395 && x0 >= 90 && x0 < CX) {
      mostrarAviso("Cancelado", C_TENUE);
    } else if (y0 >= 320 && y0 <= 395 && x0 >= CX && x0 <= 376) {
      lanzarPendiente();
    }
    return;
  }
  if (vista == AVISO) {
    vista = PRINCIPAL;
    return;
  }
  if (vista == DETALLE) {   // permiso: Rechazar con un toque; Aprobar se mantiene (leerTouch)
    int iDet = buscarDetalle();
    if (iDet >= 0 && estado.ses[iDet].estado == 'P' && estado.ses[iDet].espera.length()) {
      if (enBoton(x0, y0, BTN_AX)) return;                 // un toque corto en Aprobar no hace nada
      if (enBoton(x0, y0, BTN_RX)) { decidirPermiso(estado.ses[iDet].espera, 'r'); return; }
    }
  }
  if (abs(dx) > 70 && abs(dx) > abs(dy)) {  // deslizar: alterna principal / tokens
    vista = vista == TOKENS ? PRINCIPAL : TOKENS;
    return;
  }
  if (vista == DETALLE || vista == TOKENS) {
    vista = PRINCIPAL;
    return;
  }
  // Toque sobre un muñequito: detalle de esa sesión
  int i = sesionEn(x0, y0);
  if (i >= 0) {
    detalleClave = estado.ses[i].nombre + "|" + estado.ses[i].proyecto;
    vista = DETALLE;
  }
}

// Botón BOOT (GPIO0, activo bajo): una pulsación empieza a grabar y otra termina.
// Solo se usa después del arranque (presionado al reiniciar entra en modo de carga).
#define PIN_BOOT 0
void leerBoton() {
  static bool estable = true, ultimo = true;   // true = suelto (pull-up)
  static uint32_t tCambio = 0;
  bool lectura = digitalRead(PIN_BOOT);
  if (lectura != ultimo) { ultimo = lectura; tCambio = millis(); }
  if (millis() - tCambio < 30 || lectura == estable) return;   // antirrebote 30 ms
  estable = lectura;
  if (estable) return;                                         // solo el flanco de bajada
  ultimoToque = millis();
  if (vista == GRABANDO) terminarGrabacion();
  else if (vista == DETALLE) {
    int iDet = buscarDetalle();
    if (iDet >= 0 && (estado.ses[iDet].estado == 'P' || estado.ses[iDet].estado == 'R')) {
      mostrarAviso("Primero contesta el permiso o la pregunta de esta sesión", C_AMBAR);
    } else if (iDet >= 0 && estado.ses[iDet].sid.length()) {
      destinoSid = estado.ses[iDet].sid;
      destinoNombre = estado.ses[iDet].nombre;
      iniciarGrabacion();
    } else {
      mostrarAviso("Esta sesión no recibe mensajes por voz: solo las de Claude en la app Terminal", C_AMBAR);
    }
  } else if (vista == PRINCIPAL || vista == TOKENS || vista == AVISO) {
    destinoSid = "";
    destinoNombre = "";
    iniciarGrabacion();
  }
}

void leerTouch() {
  static bool tocando = false;
  static int16_t xi, yi, xu, yu;
  static uint32_t tInicio = 0, tUltimoPunto = 0;
  if (!hayTouch) return;
  int16_t x[1], y[1];
  uint8_t n = touch.getPoint(x, y, 1);
  // El CST9217 deja huecos sin punto aunque el dedo siga abajo: solo se da por levantado
  // tras 150 ms sin lecturas (si no, una presión larga se parte en muchos toques cortos).
  if (n) tUltimoPunto = millis();
  if (n) {
    if (!tocando) {
      tocando = true;
      tInicio = millis();
      xi = x[0];
      yi = y[0];
    }
    xu = x[0];
    yu = y[0];
    // Aprobar un permiso: mantener 1 s sobre el botón (empezó dentro de él).
    static bool aprobado = false;
    if (millis() - tInicio < 30) aprobado = false;
    int iDet = vista == DETALLE ? buscarDetalle() : -1;
    if (iDet >= 0 && estado.ses[iDet].estado == 'P' && estado.ses[iDet].espera.length() &&
        enBoton(xi, yi, BTN_AX) && !aprobado) {
      progresoAprobar = min(1.0f, (millis() - tInicio) / 1000.0f);
      if (progresoAprobar >= 1.0f) {
        aprobado = true;
        progresoAprobar = 0;
        decidirPermiso(estado.ses[iDet].espera, 'a');
        xi = -1000;   // que al soltar no cuente como otro toque
      }
    }
  } else if (tocando && millis() - tUltimoPunto > 150) {
    tocando = false;
    progresoAprobar = 0;
    if (xi == -1000) return;
    if (vista != GRABANDO) gesto(xi, yi, xu - xi, yu - yi);   // la grabación se termina con BOOT
  }
}

// ------------------------------------------------------------- audio ----
// ES8311 (DAC) por I2C + I2S: MCLK 16, BCLK 9, WS 45, DOUT 8, DIN 10 (ES7210). Amplificador en GPIO 46.
#define PIN_PA 46
I2SClass i2s;
int16_t *pitido = nullptr;
size_t pitidoBytes = 0;
uint32_t pitidoMs = 0;
volatile bool sonar = false;

void generarPitido() {  // dos tonos ascendentes, estéreo 16 bits a 16 kHz, con rampas para evitar chasquidos
  const int sr = 16000;
  struct Tramo { float hz; int ms; } tramos[] = {{880, 110}, {0, 50}, {1320, 150}};
  int total = 0;
  for (auto &t : tramos) total += sr * t.ms / 1000;
  pitidoMs = total * 1000 / sr;
  pitidoBytes = total * 2 * sizeof(int16_t);
  pitido = (int16_t *)ps_malloc(pitidoBytes);
  if (!pitido) return;
  int k = 0;
  for (auto &t : tramos) {
    int n = sr * t.ms / 1000, rampa = sr * 6 / 1000;
    for (int i = 0; i < n; i++, k++) {
      float env = min(1.0f, min(i, n - 1 - i) / (float)rampa);
      int16_t v = t.hz > 0 ? (int16_t)(7000 * env * sinf(2 * PI * t.hz * i / sr)) : 0;
      pitido[2 * k] = pitido[2 * k + 1] = v;
    }
  }
}

void tareaAudio(void *) {
  for (;;) {
    if (sonar && pitido) {
      sonar = false;
      digitalWrite(PIN_PA, HIGH);
      i2s.write((uint8_t *)pitido, pitidoBytes);
      vTaskDelay(pdMS_TO_TICKS(pitidoMs + 80));
      digitalWrite(PIN_PA, LOW);  // amplificador apagado en reposo: sin siseo
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void iniciarAudio() {
  pinMode(PIN_PA, OUTPUT);
  digitalWrite(PIN_PA, LOW);
  i2s.setPins(9, 45, 8, 10, 16);
  if (!i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.println("Audio: I2S no inició");
    return;
  }
  es8311_handle_t es = es8311_create(0, ES8311_ADDRRES_0);
  const es8311_clock_config_t reloj = {false, false, true, 16000 * 256, 16000};
  bool ok = es && es8311_init(es, &reloj, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) == ESP_OK &&
            es8311_voice_volume_set(es, 70, NULL) == ESP_OK;
  Serial.printf("Audio: ES8311 %s\n", ok ? "OK" : "no responde");
  generarPitido();
  xTaskCreatePinnedToCore(tareaAudio, "audio", 4096, nullptr, 1, nullptr, 0);
}


// --------------------------------------------------------------- voz ----
// ES7210 (dos micrófonos) por I2C; los datos llegan por el mismo I2S (DIN 10). Se guarda el canal
// izquierdo, 16 kHz mono 16 bits, hasta 20 s en PSRAM, y se manda a la Mac para transcribir.
const int MAX_SEG_VOZ = 20;
int16_t *grab = nullptr;

void tareaGrabar(void *) {
  static int16_t bloque[512];  // 256 cuadros estéreo
  for (;;) {
    if (!grabando) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    size_t n = i2s.readBytes((char *)bloque, sizeof(bloque));
    for (size_t i = 0; i < n / 4 && grabMuestras < (size_t)MAX_SEG_VOZ * 16000; i++) grab[grabMuestras++] = bloque[2 * i];
    if (grabMuestras >= (size_t)MAX_SEG_VOZ * 16000) grabando = false;  // tope: se envía solo
  }
}

void iniciarMicros() {
  audio_hal_codec_config_t cfg = {};
  cfg.adc_input = AUDIO_HAL_ADC_INPUT_ALL;
  cfg.codec_mode = AUDIO_HAL_CODEC_MODE_ENCODE;
  cfg.i2s_iface.mode = AUDIO_HAL_MODE_SLAVE;
  cfg.i2s_iface.fmt = AUDIO_HAL_I2S_NORMAL;
  cfg.i2s_iface.samples = AUDIO_HAL_16K_SAMPLES;
  cfg.i2s_iface.bits = AUDIO_HAL_BIT_LENGTH_16BITS;
  esp_err_t r = es7210_adc_init(&Wire, &cfg);
  r |= es7210_adc_config_i2s(cfg.codec_mode, &cfg.i2s_iface);
  r |= es7210_adc_set_gain((es7210_input_mics_t)(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2), GAIN_30DB);
  r |= es7210_adc_ctrl_state(cfg.codec_mode, AUDIO_HAL_CTRL_START);
  grab = (int16_t *)ps_malloc((size_t)MAX_SEG_VOZ * 16000 * sizeof(int16_t));
  Serial.printf("Micrófonos: ES7210 %s\n", r == ESP_OK && grab ? "OK" : "falla");
  if (grab) xTaskCreatePinnedToCore(tareaGrabar, "grabar", 4096, nullptr, 2, nullptr, 0);
}

String sha256Hex(const uint8_t *datos, size_t n) {
  uint8_t salida[32];
  mbedtls_sha256(datos, n, salida, 0);
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", salida[i]);
  return String(hex);
}

// POST firmado: HMAC(token, "POST <ruta> <ts> <sha256 del cuerpo>"). Por red local o por el túnel.
int postFirmado(const char *ruta, const uint8_t *cuerpo, size_t n, const char *tipo, String &respuesta) {
  time_t ts = time(nullptr);
  if (ts < 1700000000) return -300;
  String tsTxt = String((long)ts);
  String firma = hmacHex(String("POST ") + ruta + " " + tsTxt + " " + sha256Hex(cuerpo, n));
  HTTPClient http;
  WiFiClientSecure tls;
  bool ok;
  http.setConnectTimeout(8000);
  http.setTimeout(60000);  // la transcripción tarda unos segundos
  if (modoTunel) {
    tls.setInsecure();  // la autenticidad la da el HMAC (ver comentario del túnel)
    ok = http.begin(tls, String("https://" TUNEL_HOST) + ruta);
  } else {
    if (servidor.isEmpty()) resolverServidor();
    ok = http.begin("http://" + servidor + ":" + String(SERVIDOR_PUERTO) + ruta);
  }
  if (!ok) return -100;
  const char *cabeceras[] = {"X-Panel-Firma-Resp"};
  http.collectHeaders(cabeceras, 1);
  http.addHeader("X-Panel-Ts", tsTxt);
  http.addHeader("X-Panel-Firma", firma);
  http.addHeader("Content-Type", tipo);
  http.addHeader("ngrok-skip-browser-warning", "1");
  int codigo = http.POST((uint8_t *)cuerpo, n);
  if (codigo > 0) {
    respuesta = http.getString();
    if (codigo == 200 && !hmacHex(tsTxt + "\n" + respuesta).equals(http.header("X-Panel-Firma-Resp"))) codigo = -400;
  }
  http.end();
  return codigo;
}

String errorDe(int codigo, const String &resp) {
  JsonDocument d;
  if (codigo > 0 && !deserializeJson(d, resp) && d["error"].is<const char *>()) return d["error"].as<String>();
  if (codigo == -300) return "sin hora (NTP)";
  if (codigo == -400) return "respuesta sin firma válida";
  return "sin conexión (" + String(codigo) + ")";
}

void mostrarAviso(const String &txt, uint16_t color) {
  avisoTxt = txt;
  avisoColor = color;
  avisoDesde = millis();
  vista = AVISO;
}

void iniciarGrabacion() {
  if (!grab) return;
  grabMuestras = 0;
  inicioGrab = millis();
  grabando = true;
  vista = GRABANDO;
  Serial.println("Voz: grabando");
}

void terminarGrabacion() {
  grabando = false;
  delay(40);  // deja terminar la última lectura
  float seg = grabMuestras / 16000.0f;
  int32_t pico = 0;
  double energia = 0;
  for (size_t i = 0; i < grabMuestras; i++) {
    pico = max(pico, (int32_t)abs(grab[i]));
    energia += (double)grab[i] * grab[i];
  }
  Serial.printf("Voz: %.1f s, pico %ld, RMS %.0f\n", seg, (long)pico, grabMuestras ? sqrt(energia / grabMuestras) : 0.0);
  if (seg < 0.8f) {
    mostrarAviso("Muy corto: pulsa BOOT, habla y vuelve a pulsar", C_AMBAR);
    return;
  }
  procesandoTxt = "Transcribiendo...";
  procesandoSub = "whisper en tu Mac";
  vista = PROCESANDO;
  dibujar(false);
  String resp;
  String rutaVoz = destinoSid.length() ? "/api/voz?s=" + destinoSid : String("/api/voz");
  int codigo = postFirmado(rutaVoz.c_str(), (uint8_t *)grab, grabMuestras * sizeof(int16_t), "application/octet-stream", resp);
  JsonDocument d;
  if (codigo == 200 && !deserializeJson(d, resp)) {
    pendiente.id = d["id"] | "";
    pendiente.texto = d["texto"] | "";
    pendiente.carpeta = d["carpeta"] | "";
    pendiente.destino = d["destino"] | "";
    confirmarDesde = millis();
    vista = CONFIRMAR;
  } else {
    mostrarAviso("No se pudo: " + errorDe(codigo, resp), C_ROJO);
  }
}

// Aprobar ('a') o rechazar ('r') un permiso pendiente. Firmado con HMAC como la voz.
void decidirPermiso(const String &espera, char d) {
  procesandoTxt = d == 'a' ? "Aprobando..." : "Rechazando...";
  procesandoSub = "";
  vista = PROCESANDO;
  dibujar(false);
  String cuerpo = "{\"id\":\"" + espera + "\",\"d\":\"" + String(d) + "\"}";
  String resp;
  int codigo = postFirmado("/api/permiso", (const uint8_t *)cuerpo.c_str(), cuerpo.length(), "application/json", resp);
  Serial.printf("Permiso %c: HTTP %d\n", d, codigo);
  if (codigo == 200) mostrarAviso(d == 'a' ? "Aprobado" : "Rechazado", d == 'a' ? C_VERDE : C_TENUE);
  else mostrarAviso("No se pudo: " + errorDe(codigo, resp), C_ROJO);
}

void lanzarPendiente() {
  procesandoTxt = pendiente.destino.length() ? "Enviando..." : "Lanzando...";
  procesandoSub = "";
  vista = PROCESANDO;
  dibujar(false);
  String cuerpo = "{\"id\":\"" + pendiente.id + "\"}";
  String resp;
  int codigo = postFirmado("/api/voz/confirmar", (const uint8_t *)cuerpo.c_str(), cuerpo.length(), "application/json", resp);
  if (codigo == 200) mostrarAviso(pendiente.destino.length() ? "Enviado a " + pendiente.destino : "Sesión lanzada en " + pendiente.carpeta, C_VERDE);
  else mostrarAviso("No se lanzó: " + errorDe(codigo, resp), C_ROJO);
  pendiente = Pendiente();
}

void dibujarGrabando() {
  float seg = (millis() - inicioGrab) / 1000.0f;
  lienzo->fillArc(CX, CY, 224, 210, 0, 360, C_PISTA);
  arco(224, 210, 0, min(seg / MAX_SEG_VOZ, 1.0f) * 360, C_ROJO);
  lienzo->fillCircle(CX, CY - 40, 36, (millis() / 400) % 2 ? C_ROJO : 0x9000);
  lienzo->setFont(u8g2_font_helvB18_tf);
  centrado("Escuchando...", CY + 30, C_TINTA);
  lienzo->setFont(u8g2_font_helvR12_tf);
  centrado(String((int)seg) + " s de " + String(MAX_SEG_VOZ), CY + 58, C_TENUE);
  centrado("pulsa BOOT para terminar", CY + 84, C_TENUE);
  if (destinoNombre.length()) centrado("mensaje para " + recortar(destinoNombre, 260), CY + 110, C_CLAUDE);
}

void dibujarConfirmar() {
  lienzo->fillArc(CX, CY, 224, 216, 0, 360, C_AZUL);
  lienzo->setFont(u8g2_font_helvR10_tf);
  if (pendiente.destino.length()) centrado("Mensaje para " + recortar(pendiente.destino, 240), 88, C_CLAUDE);
  else centrado("Nueva sesión en " + recortar(pendiente.carpeta, 240), 88, C_TENUE);
  lienzo->setFont(u8g2_font_helvB12_tf);
  String renglones[6];
  int n = envolver(pendiente.texto, 330, renglones, 6);
  int y = 205 - (n - 1) * 11;
  for (int i = 0; i < n; i++) centrado(renglones[i], y + i * 22, C_TINTA);
  lienzo->fillRoundRect(95, 330, 128, 48, 24, C_FILA);
  lienzo->fillRoundRect(243, 330, 128, 48, 24, 0x2C66);
  lienzo->setFont(u8g2_font_helvB12_tf);
  lienzo->setTextColor(C_TENUE);
  lienzo->setCursor(159 - anchoTexto("Cancelar") / 2, 360);
  lienzo->print("Cancelar");
  lienzo->setTextColor(C_TINTA);
  const char *accion = pendiente.destino.length() ? "Enviar" : "Lanzar";
  lienzo->setCursor(307 - anchoTexto(accion) / 2, 360);
  lienzo->print(accion);
}

void dibujarAviso() {
  lienzo->fillArc(CX, CY, 224, 216, 0, 360, avisoColor);
  lienzo->setFont(u8g2_font_helvB14_tf);
  String renglones[4];
  int n = envolver(avisoTxt, 320, renglones, 4);
  int y = CY - (n - 1) * 13;
  for (int i = 0; i < n; i++) centrado(renglones[i], y + i * 26, C_TINTA);
}

// --------------------------------------------------------------- setup ----

void setup() {
  Serial.begin(115200);
  if (!lienzo->begin()) Serial.println("Fallo al iniciar pantalla/lienzo");
  pantalla->setBrightness(170);  // AMOLED: brillo moderado para limitar desgaste por imagen fija
  lienzo->setUTF8Print(true);
  dibujar(false);

  Wire.begin(15, 14);  // bus I2C compartido: touch, ES8311, PMU
  iniciarTouch();
  iniciarAudio();
  iniciarMicros();
  pinMode(PIN_BOOT, INPUT_PULLUP);

  clienteTls.setInsecure();  // ver comentario del túnel: la autenticidad la da la firma HMAC

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("claude-panel");
  redes.addAP(WIFI_SSID, WIFI_PASSWORD);
  // Diagnóstico: ¿se ven las redes configuradas? (no imprime nombres)
  int n = WiFi.scanNetworks();
  bool v1 = false, v2 = false;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == WIFI_SSID) { v1 = true; Serial.printf("Red 1 visible: RSSI %d, canal %d\n", WiFi.RSSI(i), WiFi.channel(i)); }
#ifdef WIFI_SSID_2
    if (WiFi.SSID(i) == WIFI_SSID_2) { v2 = true; Serial.printf("Red 2 visible: RSSI %d, canal %d, auth %d\n", WiFi.RSSI(i), WiFi.channel(i), WiFi.encryptionType(i)); }
#endif
  }
  Serial.printf("Escaneo: %d redes, red1 %s, red2 %s\n", n, v1 ? "SI" : "NO", v2 ? "SI" : "NO");
  WiFi.scanDelete();
#ifdef WIFI_SSID_2
  redes.addAP(WIFI_SSID_2, WIFI_PASSWORD_2);  // p. ej. hotspot del iPhone
#endif
#ifdef WIFI_SSID_3
  redes.addAP(WIFI_SSID_3, WIFI_PASSWORD_3);  // p. ej. WiFi de casa
#endif
  uint32_t t0 = millis();
  while (redes.run(5000) != WL_CONNECTED && millis() - t0 < 20000) delay(250);
  errorTxt = WiFi.status() == WL_CONNECTED ? "Buscando servidor..." : "No conecta al WiFi";
  dibujar(false);
}

void loop() {
  uint32_t ahora = millis();
  static uint32_t ultimaRevisionRed = 0, ultimoReintento = 0;
  if (ahora - ultimaRevisionRed >= 1000) {
    ultimaRevisionRed = ahora;
    if (WiFi.status() != WL_CONNECTED && ahora - ultimoReintento >= 10000) {
      ultimoReintento = ahora;
      redes.run(5000);  // elige la red conocida con mejor señal
    }
    // Si cambió de red, la IP de la Mac también: volver a resolver y probar primero la red local.
    static String ssidActual;
    if (WiFi.status() == WL_CONNECTED && WiFi.SSID() != ssidActual) {
      ssidActual = WiFi.SSID();
      Serial.printf("WiFi OK (%s), IP %s\n", ssidActual.c_str(), WiFi.localIP().toString().c_str());
      MDNS.end();
      MDNS.begin("claude-panel");
      configTime(0, 0, "pool.ntp.org", "time.google.com");  // la firma HMAC lleva la hora
      cerrarStream();
      servidor = "";
      modoTunel = false;
      fallos = 0;
    }
#ifdef WIFI_SSID_2
    // Ni red local ni túnel funcionan en esta red (6 intentos de túnel, ~1 min): probar la otra.
    if (WiFi.status() == WL_CONNECTED && modoTunel && fallos >= 6) {
      bool enPrimera = WiFi.SSID() == WIFI_SSID;
      Serial.println("Sin servidor ni túnel en esta red; cambiando de red");
      cerrarStream();
      fallos = 0;
      WiFi.disconnect();
      delay(200);
      if (enPrimera) WiFi.begin(WIFI_SSID_2, WIFI_PASSWORD_2);
      else WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      ultimoReintento = ahora;  // dar 10 s antes de que redes.run() intervenga
    }
#endif
  }
  actualizarDatos(ahora);
  leerTouch();
  leerBoton();

  // Alerta nueva con permiso pendiente: sonar y volver a la vista principal (sin interrumpir la voz).
  static long naPrevio = -1;
  bool enVoz = vista == GRABANDO || vista == PROCESANDO || vista == CONFIRMAR;
  if (estado.valido) {
    if (naPrevio >= 0 && estado.na != naPrevio && estado.np > 0) {
      sonar = true;
      if (!enVoz) vista = PRINCIPAL;
    }
    naPrevio = estado.na;
  }
  if (vista == GRABANDO && !grabando) terminarGrabacion();                    // llegó al tope de 20 s
  if (vista == CONFIRMAR && millis() - confirmarDesde > 60000) mostrarAviso("Cancelado (sin respuesta)", C_TENUE);
  if (vista == AVISO && millis() - avisoDesde > 5000) vista = PRINCIPAL;
  // Sin tocar en 20 s: regresar a la vista principal.
  // Sin tocar en 20 s: regresar a la principal (60 s si es un permiso por contestar).
  int iDetT = vista == DETALLE ? buscarDetalle() : -1;
  uint32_t limiteVista = (iDetT >= 0 && estado.ses[iDetT].espera.length()) ? 60000 : 20000;
  if ((vista == TOKENS || vista == DETALLE) && millis() - ultimoToque > limiteVista) vista = PRINCIPAL;
  // Redibuja a 4 Hz: animación de los muñequitos
  if (ahora - ultimoDibujo >= 250) {
    ultimoDibujo = ahora;
    dibujar((ahora / 500) % 2 == 0);
  }
  delay(20);
}
