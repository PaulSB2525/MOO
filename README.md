# Ganadería IoT RPi - Sistema de Telemetría y Geocerca

Este repositorio contiene el *backend* completo para un sistema de monitoreo de ganado. El sistema recibe datos de telemetría (ubicación, temperatura, pulso) vía MQTT, los procesa, detecta alertas de geocerca y salud crítica, y los almacena en una base de datos PostgreSQL, sirviéndolos a través de una API Flask.

## 1. Arquitectura del Proyecto

El sistema está dividido en tres capas principales que residen en la Raspberry Pi (RPi), con la excepción del dispositivo IoT físico (ESP32/Simulador).

1.  **Capa de Dispositivo (PRODUCER):** El dispositivo (ESP32) envía datos de telemetría.
2.  **Capa de Servidor/Persistencia (RPi):** El Broker (Mosquitto) recibe los datos y el Consumidor los guarda y procesa.
3.  **Capa de Presentación (API):** La API Flask lee los datos procesados y sirve JSON/HTML al *frontend*.



---

## 2. Requisitos y Dependencias

### Hardware
* Raspberry Pi (Recomendado Pi 3B+ o superior).
* Dispositivo IoT (ESP32 / ESP8266) o usar el simulador (`publicador.py`).

### Software Base (En la Raspberry Pi)
* **Python 3.x**
* **PostgreSQL** (Servidor de Base de Datos).
* **Mosquitto** (Broker MQTT).

### Dependencias de Python
Se requieren las siguientes librerías. Instálalas usando `requirements.txt`:

```bash
pip install -r requirements.txt
