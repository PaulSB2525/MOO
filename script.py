from flask import Flask, request, jsonify, render_template
import psycopg2
from dotenv import load_dotenv
import os

app = Flask(__name__)

load_dotenv()

CONNECTION_STRING = os.getenv("CONNECTION_STRING")
GEOFENCE_CENTRO_LAT = float(os.getenv("GEOFENCE_CENTRO_LAT"))
GEOFENCE_CENTRO_LNG = float(os.getenv("GEOFENCE_CENTRO_LNG"))
GEOFENCE_RADIO_KM = float(os.getenv("GEOFENCE_RADIO_KM"))


def get_connection():
    """Establishes a connection to the PostgreSQL database."""
    return psycopg2.connect(CONNECTION_STRING)



def get_all_device_ids():
    conn = None
    vaca_ids = []
    try:
        conn = get_connection()
        cur = conn.cursor()
        # Query para obtener todos los IDs únicos de la tabla sensors
        cur.execute("SELECT DISTINCT id_vaca FROM ubicacion ORDER BY id_vaca;")
        vaca_ids = [row[0] for row in cur.fetchall()]
        cur.close()
    except psycopg2.Error as e:
        print(f"Database error fetching IDs: {e}")
        return []
    finally:
        if conn:
            conn.close()
    return vaca_ids

@app.route('/api/v1/geofence')
def get_geofence_params():
    """Devuelve el centro y radio de la geocerca para dibujar en el mapa."""
    return jsonify({
        "center": {
            "lat": GEOFENCE_CENTRO_LAT,
            "lng": GEOFENCE_CENTRO_LNG
        },
        "radius_km": GEOFENCE_RADIO_KM
    })

def get_data_from_api(id_vaca):
    """
    Función modificada para OBTENER DATOS DIRECTAMENTE DE LA DB.
    Elimina la dependencia de hacer llamadas HTTP a tu propia URL externa.
    """
    conn = None
    try:
        conn = get_connection()
        cur = conn.cursor()

        # 1. Obtener la última lectura para el resumen del dashboard
        cur.execute("""
            SELECT full_data, created_at, temp_value
            FROM telemetria_vaca
            WHERE id_vaca = %s
            ORDER BY created_at DESC
            LIMIT 1;
        """, (id_vaca,))
        row = cur.fetchone()

        # 2. (Opcional) Obtener el conteo total de lecturas
        # Esto es más eficiente que contar todas las filas en la tabla
        cur.execute("""
            SELECT count(*) FROM telemetria_vaca WHERE id_vaca = %s;
        """, (id_vaca,))
        total_readings = cur.fetchone()[0]

        cur.close()

        if not row:
            return {"error": f"No readings found in VACA ID: {id_vaca}"}

        # Devolvemos el formato JSON esperado por el dashboard.html
        return {
            "id_vaca": id_vaca,
            "latest_data": row[0],
            "latest_temperature": row[2],
            "latest_timestamp": row[1].strftime('%Y-%m-%d %H:%M:%S'),
            "total_readings": total_readings
        }

    except Exception as e:
        return {"error": f"DB access error for vaca {id_vaca}: {str(e)}"}

    finally:
        if conn:
            conn.close()


@app.route("/api/v1/vaca/<string:vaca_id>/ruta")
def get_vaca_route(vaca_id):

    conn = None
    try:
        conn = get_connection()
        cur = conn.cursor()

        # Consulta SQL para obtener todos los puntos de la ruta
        cur.execute("""
                    SELECT lat,
                           lng,
                           ts
                    FROM ubicacion
                    WHERE id_vaca = %s
                    ORDER BY ts ASC;
                    """, (vaca_id,))

        # Obtener los nombres de las columnas para crear un diccionario (JSON)
        column_names = [desc[0] for desc in cur.description]

        # Crear una lista de diccionarios (puntos)
        ruta_data = []
        for row in cur.fetchall():
            # Formatear la marca de tiempo a string
            row_dict = dict(zip(column_names, row))
            row_dict['ts'] = row_dict['ts'].strftime('%Y-%m-%d %H:%M:%S')
            ruta_data.append(row_dict)

        return jsonify({
            "vaca_id": vaca_id,
            "route": ruta_data
        })

    except Exception as e:
        return jsonify({"error": f"Error al obtener la ruta: {str(e)}"}), 500

    finally:
        if conn:
            conn.close()


@app.route('/api/v1/alertas')
def get_active_alerts():
    """Obtiene las 20 alertas más recientes (salud o geocerca) de la base de datos."""
    conn = None
    try:
        conn = get_connection()
        cur = conn.cursor()

        # Seleccionamos los datos de la alerta, incluyendo las coordenadas para el mapa
        cur.execute("""
                    SELECT id_vaca, ts, tipo_alerta, mensaje, lat, lng
                    FROM alertas
                    ORDER BY ts DESC LIMIT 20;
                    """)

        column_names = [desc[0] for desc in cur.description]
        alerts = []
        for row in cur.fetchall():
            row_dict = dict(zip(column_names, row))
            # Formatear el timestamp a string para JSON
            row_dict['ts'] = row_dict['ts'].strftime('%Y-%m-%d %H:%M:%S')
            alerts.append(row_dict)

        return jsonify(alerts)
    except Exception as e:
        return jsonify({"error": f"Error al obtener alertas: {str(e)}"}), 500
    finally:
        if conn:
            conn.close()
