from flask import Flask, request, jsonify, render_template
import psycopg2
from dotenv import load_dotenv
import os
from functools import wraps

app = Flask(__name__)

# --- Carga de Configuración ---
load_dotenv()

# Variables de conexión y constantes
CONNECTION_STRING = os.getenv("CONNECTION_STRING")
API_KEY_SECRETA = os.getenv("API_KEY_SECRETA")  # Asumimos que la llave secreta se pasa aquí
GEOFENCE_CENTRO_LAT = float(os.getenv("GEOFENCE_CENTRO_LAT"))
GEOFENCE_CENTRO_LNG = float(os.getenv("GEOFENCE_CENTRO_LNG"))
GEOFENCE_RADIO_KM = float(os.getenv("GEOFENCE_RADIO_KM"))


# --- Funciones de Utilidad y Seguridad ---

def get_connection():
    """Establishes a connection to the PostgreSQL database."""
    return psycopg2.connect(CONNECTION_STRING)


def get_all_vaca_ids():  # Renombrada para claridad
    """Fetches all unique VACA IDs from the database (table: ubicacion)."""
    conn = None
    vaca_ids = []
    try:
        conn = get_connection()
        cur = conn.cursor()
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


def require_api_key(view_function):
    """
    Decorator para verificar la API Key en el header 'X-API-Key' para seguridad.
    """

    @wraps(view_function)
    def decorated_function(*args, **kwargs):
        key_from_request = request.headers.get('X-API-Key')
        if key_from_request and key_from_request == API_KEY_SECRETA:
            return view_function(*args, **kwargs)
        else:
            return jsonify({"error": "Acceso no autorizado",
                            "message": "Falta o la llave 'X-API-Key' es inválida."}), 401

    return decorated_function


# --- Funciones de Acceso a Datos (sin rutas) ---

def get_data_from_api(id_vaca):
    """Obtiene los datos más recientes de telemetría unificada de una vaca."""
    conn = None
    try:
        conn = get_connection()
        cur = conn.cursor()

        # 1. Obtener la última lectura
        cur.execute("""
                    SELECT full_data, created_at, temp_value
                    FROM telemetria_vaca
                    WHERE id_vaca = %s
                    ORDER BY created_at DESC LIMIT 1;
                    """, (id_vaca,))
        row = cur.fetchone()

        # 2. Obtener el conteo total de lecturas
        cur.execute("""
                    SELECT count(*)
                    FROM telemetria_vaca
                    WHERE id_vaca = %s;
                    """, (id_vaca,))
        total_readings = cur.fetchone()[0]

        cur.close()

        if not row:
            return {"error": f"No readings found in VACA ID: {id_vaca}"}

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


# --- Rutas del API (Endpoints Protegidos) ---

@app.route('/api/v1/geofence')
@require_api_key
def get_geofence_params():
    """Devuelve el centro y radio de la geocerca para dibujar en el mapa."""
    return jsonify({
        "center": {
            "lat": GEOFENCE_CENTRO_LAT,
            "lng": GEOFENCE_CENTRO_LNG
        },
        "radius_km": GEOFENCE_RADIO_KM
    })


@app.route('/api/v1/vaca_ids')
@require_api_key
def get_vaca_ids_endpoint():
    """Endpoint para devolver solo la lista de IDs de vaca (para iterar en JS)."""
    vaca_ids = get_all_vaca_ids()
    if not vaca_ids:
        return jsonify([]), 200

    return jsonify(vaca_ids)


@app.route('/api/v1/vaca/<string:vaca_id>/latest')  # Endpoint necesario para el JS de la tabla
@require_api_key
def get_vaca_latest_data(vaca_id):
    """Devuelve el último estado de telemetría unificada para una vaca."""
    data = get_data_from_api(vaca_id)
    if 'error' in data:
        return jsonify(data), 404
    return jsonify(data)


@app.route("/api/v1/vaca/<string:vaca_id>/ruta")
@require_api_key
def get_vaca_route(vaca_id):
    """Obtiene todos los puntos históricos de ubicación (ruta)."""
    conn = None
    try:
        conn = get_connection()
        cur = conn.cursor()

        cur.execute("""
                    SELECT lat, lng, ts
                    FROM ubicacion
                    WHERE id_vaca = %s
                    ORDER BY ts ASC;
                    """, (vaca_id,))

        column_names = [desc[0] for desc in cur.description]

        ruta_data = []
        for row in cur.fetchall():
            row_dict = dict(zip(column_names, row))
            row_dict['ts'] = row_dict['ts'].strftime('%Y-%m-%d %H:%M:%S')
            ruta_data.append(row_dict)

        return jsonify({"vaca_id": vaca_id, "route": ruta_data})

    except Exception as e:
        return jsonify({"error": f"Error al obtener la ruta: {str(e)}"}), 500

    finally:
        if conn:
            conn.close()


@app.route('/api/v1/alertas')
@require_api_key
def get_active_alerts():
    """Obtiene las 20 alertas más recientes (salud o geocerca)."""
    conn = None
    try:
        conn = get_connection()
        cur = conn.cursor()

        cur.execute("""
                    SELECT id_vaca, ts, tipo_alerta, mensaje, lat, lng
                    FROM alertas
                    ORDER BY ts DESC LIMIT 6;
                    """)

        column_names = [desc[0] for desc in cur.description]
        alerts = []
        for row in cur.fetchall():
            row_dict = dict(zip(column_names, row))
            row_dict['ts'] = row_dict['ts'].strftime('%Y-%m-%d %H:%M:%S')
            alerts.append(row_dict)

        return jsonify(alerts)
    except Exception as e:
        return jsonify({"error": f"Error al obtener alertas: {str(e)}"}), 500
    finally:
        if conn:
            conn.close()


# --- RUTA DE RENDERIZADO DEL DASHBOARD (HTML) ---

def get_base_url():
    # Helper para construir la URL base para el frontend
    host = "10.199.122.56"
    port = os.environ.get('FLASK_RUN_PORT', '5000')
    return f"http://{host}:{port}"


@app.route("/dashboard")
def dashboard():
    """Ruta que renderiza el template HTML y pasa la configuración."""

    # Aquí puedes obtener todos los IDs de vaca si el frontend los necesita estáticamente.
    all_vaca_ids = get_all_vaca_ids()

    return render_template(
        "dashboard.html",
        API_BASE_URL=get_base_url(),
        API_KEY=API_KEY_SECRETA,
        all_vaca_ids=all_vaca_ids  # Pasar IDs a Jinja si se necesitan en el HTML
    )


# --- MECANISMO DE EJECUCIÓN (MAIN) ---

if __name__ == '__main__':
    # Flask debe escuchar en 0.0.0.0 para ser accesible en la red de la RPi
    print("Servidor API Flask iniciado...")
    print(f"API Key necesaria: {API_KEY_SECRETA}")
    app.run(host='0.0.0.0', port=5000, debug=True)
