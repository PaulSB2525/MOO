import json
import paho.mqtt.client as mqtt
import psycopg2
import math
from datetime import datetime, timedelta
import os
from dotenv import load_dotenv

# Cargar variables de entorno desde el archivo .env
load_dotenv()

# --- 1. CONFIGURACIÓN ---
MQTT_BROKER = os.getenv("MQTT_BROKER")
MQTT_PORT = int(os.getenv("MQTT_PORT"))
MQTT_TOPIC = os.getenv("MQTT_TOPIC")

DB_HOST = os.getenv("DB_HOST")
DB_NAME = os.getenv("DB_NAME")
DB_USER = os.getenv("DB_USER")
DB_PASSWORD = os.getenv("DB_PASSWORD")
DB_PORT = os.getenv("DB_PORT")

# Umbrales (Ejemplos)
TEMP_THRESHOLD_HIGH = 39.5
PULSE_THRESHOLD_HIGH = 85
PULSE_THRESHOLD_LOW = 45
GEOFENCE_LAT_CENTER = 20.734503
GEOFENCE_LNG_CENTER = -103.455896
GEOFENCE_RADIUS_KM = 0.5 # 500 metros

# Inicialización de conexión a la base de datos
conn = None
cur = None

def get_db_connection():
    """Establece la conexión a la base de datos PostgreSQL."""
    global conn, cur
    try:
        conn = psycopg2.connect(
            host=DB_HOST,
            database=DB_NAME,
            user=DB_USER,
            password=DB_PASSWORD,
            port=DB_PORT
        )
        cur = conn.cursor()
        print("Conexión a PostgreSQL establecida con éxito.")
    except Exception as e:
        print(f"Error al conectar a PostgreSQL: {e}")
        conn = None
        cur = None

# --- 2. LÓGICA DE DETECCIÓN DE ALERTAS ---

def haversine(lat1, lon1, lat2, lon2):
    """Calcula la distancia Haversine entre dos puntos (en km)."""
    R = 6371  # Radio de la Tierra en kilómetros

    lat1_rad = math.radians(lat1)
    lon1_rad = math.radians(lon1)
    lat2_rad = math.radians(lat2)
    lon2_rad = math.radians(lon2)

    dlon = lon2_rad - lon1_rad
    dlat = lat2_rad - lat1_rad

    a = math.sin(dlat / 2)**2 + math.cos(lat1_rad) * math.cos(lat2_rad) * math.sin(dlon / 2)**2
    c = 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))

    distance = R * c
    return distance

def check_for_alerts(id_vaca, temp, pulso, lat, lng, riesgo):
    """Evalúa los datos y devuelve una lista de alertas detectadas."""
    alertas = []
    
    # 1. Alerta de Temperatura
    if temp > TEMP_THRESHOLD_HIGH:
        alertas.append({
            'tipo': 'Temperatura Alta',
            'mensaje': f"Temperatura corporal crítica ({temp}°C).",
            'lat': lat,
            'lng': lng
        })
        
    # 2. Alerta de Pulso
    if pulso > PULSE_THRESHOLD_HIGH:
        alertas.append({
            'tipo': 'Taquicardia',
            'mensaje': f"Pulso cardiaco anormalmente alto ({pulso} ppm).",
            'lat': lat,
            'lng': lng
        })
    elif pulso < PULSE_THRESHOLD_LOW:
        alertas.append({
            'tipo': 'Bradicardia',
            'mensaje': f"Pulso cardiaco anormalmente bajo ({pulso} ppm).",
            'lat': lat,
            'lng': lng
        })
        
    # 3. Alerta de Geocerca (Geofence)
    distance = haversine(lat, lng, GEOFENCE_LAT_CENTER, GEOFENCE_LNG_CENTER)
    if distance > GEOFENCE_RADIUS_KM:
        alertas.append({
            'tipo': 'Fuera de Geocerca',
            'mensaje': f"Vaca fuera del perímetro de pastoreo. Distancia: {distance:.2f} km.",
            'lat': lat,
            'lng': lng
        })

    # 4. Alerta de Riesgo (si el dispositivo la envía)
    if riesgo == 1:
        alertas.append({
            'tipo': 'Riesgo Alto Detectado',
            'mensaje': "El dispositivo ha reportado un estado de riesgo alto.",
            'lat': lat,
            'lng': lng
        })
        
    return alertas

# --- 3. FUNCIONES DE CONEXIÓN MQTT ---

def on_connect(client, userdata, flags, rc):
    """Se llama cuando el cliente recibe una respuesta CONNACK del servidor."""
    if rc == 0:
        print("Conectado al broker MQTT. Suscribiéndose al tema...")
        client.subscribe(MQTT_TOPIC)
    else:
        print(f"Fallo de conexión. Código: {rc}")

def on_disconnect(client, userdata, rc):
    """Se llama cuando el cliente se desconecta del broker MQTT."""
    print(f"Desconectado del broker MQTT con código: {rc}")
    global conn
    if conn:
        try:
            conn.close()
            print("Conexión a PostgreSQL cerrada.")
        except Exception as e:
            print(f"Error al cerrar la conexión a PostgreSQL: {e}")

# --- 4. FUNCIÓN PRINCIPAL DE MANEJO DE MENSAJES (on_message) ---

def on_message(client, userdata, msg):
    if not cur:
        print("Error: No hay conexión a la base de datos.")
        return

    try:
        data = json.loads(msg.payload.decode())
