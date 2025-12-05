import json
import paho.mqtt.client as mqtt
import psycopg2
import math # ¡Necesario para la función Haversine!

# --- 1. CONFIGURACIÓN DE UMBRALES Y GEOFENCE ---
CENTRO_LAT = 20.734482
CENTRO_LNG = -103.455893
RADIO_MAX_KM = 0.5 
TEMP_CRITICA_ALTA = 39.5
TEMP_CRITICA_BAJA = 36.0
PULSO_CRITICO_ALTO = 85
PULSO_CRITICO_BAJO = 50

# --- 2. FUNCIONES AUXILIARES PARA DETECCIÓN ---

def haversine(lat1, lon1, lat2, lon2):
    """Calcula la distancia Haversine entre dos puntos (en km)."""
    R = 6371 
    phi1 = math.radians(lat1)
    phi2 = math.radians(lat2)
    delta_phi = math.radians(lat2 - lat1)
    delta_lambda = math.radians(lon2 - lon1)
    
    a = math.sin(delta_phi / 2)**2 + math.cos(phi1) * math.cos(phi2) * math.sin(delta_lambda / 2)**2
    c = 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))
    
    return R * c

def check_geofence(lat, lng):
    """Devuelve True si la vaca está fuera de la cerca."""
    distancia = haversine(CENTRO_LAT, CENTRO_LNG, lat, lng)
    return distancia > RADIO_MAX_KM

# --- 3. CONEXIÓN A LA BASE DE DATOS (Asegúrate de que la conexión sea externa a la función) ---
try:
    conn = psycopg2.connect(
        dbname="ganaderoiot",
        user="iot",
        password="1234",
        host="localhost"
    )
    cur = conn.cursor()
    print("✅ Conexión a DB establecida.")
except Exception as e:
    print(f"❌ ERROR CRÍTICO: Falló la conexión a PostgreSQL: {e}")
    exit()

# --- 4. FUNCIÓN PRINCIPAL DE MANEJO DE MENSAJES (on_message) ---
def on_message(client, userdata, msg):
    try:
        data = json.loads(msg.payload.decode())
        
        # Extracción de datos
        id_vaca = data["id_vaca"]
        ts = data["timestamp"]
        lat = data["lat"]
        lng = data["lng"]
        area = "TEC"
        temp = data["temperatura"]
        pulso = data["pulso"]
        riesgo = data["riesgo"]

        # --- INSERCIÓN DE DATOS BRUTOS ---
        cur.execute("""
            INSERT INTO ubicacion(id_vaca, ts, lat, lng, area)
            VALUES (%s,%s,%s,%s,%s)
        """, (id_vaca, ts, lat, lng, area))

        cur.execute("""
            INSERT INTO salud(id_vaca, ts, temperatura, pulso, riesgo)
            VALUES (%s,%s,%s,%s,%s)
        """, (id_vaca, ts, temp, pulso, riesgo))

        
        # --- LÓGICA DE DETECCIÓN DE ALERTAS (AQUÍ ESTÁ EL CAMBIO) ---
        alertas_detectadas = []
        
        # 1. Alerta de Geocerca
        if check_geofence(lat, lng):
            dist = haversine(CENTRO_LAT, CENTRO_LNG, lat, lng)
            alertas_detectadas.append({
                "tipo": "Geocerca",
                "mensaje": f"Salió de cerca digital. Distancia: {dist:.2f} km.",
                "lat": lat, "lng": lng
            })

        # 2. Alerta de Temperatura
        if temp > TEMP_CRITICA_ALTA:
            alertas_detectadas.append({"tipo": "TemperaturaAlta", "mensaje": f"Temp. Crítica: {temp}°C", "lat": lat, "lng": lng})
        elif temp < TEMP_CRITICA_BAJA:
            alertas_detectadas.append({"tipo": "TemperaturaBaja", "mensaje": f"Temp. Baja: {temp}°C", "lat": lat, "lng": lng})
            
        # 3. Alerta de Pulso
        if pulso > PULSO_CRITICO_ALTO:
            alertas_detectadas.append({"tipo": "PulsoAlto", "mensaje": f"Pulso Crítico Alto: {pulso} bpm", "lat": lat, "lng": lng})
        elif pulso < PULSO_CRITICO_BAJO:
            alertas_detectadas.append({"tipo": "PulsoBajo", "mensaje": f"Pulso Crítico Bajo: {pulso} bpm", "lat": lat, "lng": lng})

        # --- INSERCIÓN DE ALERTAS ---
        for alerta in alertas_detectadas:
            cur.execute("""
                INSERT INTO alertas(id_vaca, ts, tipo_alerta, mensaje, lat, lng)
                VALUES (%s, %s, %s, %s, %s, %s);
            """, (id_vaca, ts, alerta['tipo'], alerta['mensaje'], alerta['lat'], alerta['lng']))
        
        # Aplicar todos los cambios (ubicacion, salud, alertas)
        conn.commit()
        
        # Mensaje de confirmación de ALERTAS
        print(f"✅ Datos insertados y {len(alertas_detectadas)} alertas procesadas para {id_vaca}")

    except Exception as e:
        print(f"❌ ERROR en on_message: {e}")
        conn.rollback() # Revierte los cambios si falla algo

# --- 5. Configuración y Bucle MQTT ---
client = mqtt.Client()
client.on_message = on_message

try:
    client.connect("broker.mqtt.cool", 1883, 60)
    client.subscribe("vaca/telemetria")
    print("✅ Suscrito a vaca/telemetria. Escuchando mensajes...")
    client.loop_forever()
except Exception as e:
    print(f"❌ ERROR MQTT: {e}")
