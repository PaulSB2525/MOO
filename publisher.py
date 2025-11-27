import json
import paho.mqtt.client as mqtt
import random
import time
from datetime import datetime

# --- 1. CONFIGURACIÓN ---
BROKER = "localhost"
TOPIC = "vaca/telemetria"
# Lista de IDs de vacas para simular
VACAS = ["V1", "V2", "V3", "V4"]

# Coordenadas y Umbrales Base (Debe coincidir con la configuración del Consumidor)
CENTRO_LAT = 20.734503
CENTRO_LNG = -103.455896
OFFSET_NORMAL = 0.001  # Pequeña variación (aprox. 100m)
OFFSET_RIESGO_GEOFENCE = 0.5  # Gran variación (aprox. 50km) para forzar alerta

# --- 2. SETUP DE CONEXIÓN MQTT ---
client = mqtt.Client()

try:
    client.connect(BROKER, 1883, 60)
    print(f"✅ Conexión MQTT establecida con el broker en {BROKER}.")
except Exception as e:
    print(f"❌ ERROR: No se pudo conectar al broker MQTT. Asegúrate de que Mosquitto esté corriendo. {e}")
    exit()


# --- 3. LÓGICA DE SIMULACIÓN Y PUBLICACIÓN ---

def generate_payload(vaca_id):
    """Genera un diccionario de datos simulados, forzando alertas en casos específicos."""

    # Valores base normales
    lat = CENTRO_LAT + random.uniform(-OFFSET_NORMAL, OFFSET_NORMAL)
    lng = CENTRO_LNG + random.uniform(-OFFSET_NORMAL, OFFSET_NORMAL)
    temp = 38.5 + random.uniform(-0.3, 0.3)
    pulso = random.randint(65, 75)
    riesgo = False
    area = "Potrero Norte"

    # --- Forzar Alertas para la Prueba ---
    if vaca_id == "V2":
        # Simular Fiebre Crítica (Debe activar Alerta: TemperaturaAlta)
        temp = 39.8 + random.uniform(0.1, 0.2)
        pulso = 88  # Pulso alto por fiebre
        riesgo = True

    elif vaca_id == "V3":
        # Simular Salida de Geocerca (Debe activar Alerta: Geocerca)
        lat = CENTRO_LAT + OFFSET_RIESGO_GEOFENCE
        lng = CENTRO_LNG - OFFSET_RIESGO_GEOFENCE
        area = "Fuera de Limites"

    elif vaca_id == "V4":
        # Simular Pulso Crítico Bajo (Debe activar Alerta: PulsoBajo)
        pulso = 45
        riesgo = True

    # Construir el JSON
    payload = {
        "id_vaca": vaca_id,
        "timestamp": datetime.utcnow().isoformat(),
        "lat": lat,
        "lng": lng,
        "area": area,
        "temperatura": round(temp, 2),
        "pulso": pulso,
        "riesgo": riesgo
    }

    return payload


# --- Bucle Principal de Envío ---
print(f"Iniciando simulación. Reportando {len(VACAS)} vacas cada 15 segundos.")
print("------------------------------------------------------------------")

try:
    while True:
        for vaca_id in VACAS:
            data = generate_payload(vaca_id)

            # 1. Serializar el diccionario a una cadena JSON
            payload_json = json.dumps(data)

            # 2. Publicar en el tópico
            client.publish(TOPIC, payload_json)

            # 3. Mostrar el estado
            alerta_msg = "🚨 ALERTA FORZADA" if data["riesgo"] or data["area"] != "Potrero Norte" else "🟢 OK"
            print(
                f"[{alerta_msg}] Vaca {vaca_id}: Temp={data['temperatura']} | Pulso={data['pulso']} | Ubicación={data['lat']:.4f}")

        # Esperar 15 segundos antes del próximo ciclo de reporte
        time.sleep(15)

except KeyboardInterrupt:
    print("\n🛑 Simulación detenida por el usuario.")
    client.disconnect()
except Exception as e:
    print(f"\n❌ ERROR inesperado en el bucle: {e}")
