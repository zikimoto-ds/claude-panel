#pragma once
// Plantilla: copia este archivo a secrets.h (que no se sube a git) y llénalo.

// Red principal (2.4 GHz: la placa no ve redes de 5 GHz)
#define WIFI_SSID     "mi-red"
#define WIFI_PASSWORD "mi-contraseña"

// Nombre mDNS de tu Mac (sin .local) e IP de respaldo si mDNS falla
#define SERVIDOR_MDNS "MiMac"
#define SERVIDOR_IP   "192.168.1.10"
#define SERVIDOR_PUERTO 8765

// Mismo valor que PANEL_TOKEN en .env.local (genera uno largo y aleatorio)
#define PANEL_TOKEN   "cambia-esto"

// Segunda red opcional (p. ej. hotspot del iPhone con "Maximizar compatibilidad")
// #define WIFI_SSID_2     "mi-hotspot"
// #define WIFI_PASSWORD_2 "contraseña"
// #define SERVIDOR_IP_2   "172.20.10.2"

// Tercera red opcional
// #define WIFI_SSID_3     "otra-red"
// #define WIFI_PASSWORD_3 "contraseña"
// #define SERVIDOR_IP_3   "192.168.0.10"

// Túnel opcional (ngrok u otro) para cuando la placa no ve la Mac en la red local
// #define TUNEL_HOST "mi-dominio.ngrok-free.dev"
