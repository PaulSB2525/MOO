import json
import paho.mqtt.client as mqtt
import psycopg2

conn = psycopg2.connect(
    dbname="ganaderoiot",
    user="iot",
    password="1234",
    host="localhost"
)
cur = conn.cursor()

def on_message(client, userdata, msg):
    data = json.loads(msg.payload.decode())
    id_vaca = data["id_vaca"]
    ts = data["timestamp"]
    lat = data["lat"]
    lng = data["lng"]
    area = data["area"]
    temp = data["temperatura"]
    pulso = data["pulso"]
    riesgo = data["riesgo"]

    cur.execute("""
        INSERT INTO ubicacion(id_vaca, ts, lat, lng, area)
        VALUES (%s,%s,%s,%s,%s)
    """, (id_vaca, ts, lat, lng, area))

    cur.execute("""
        INSERT INTO salud(id_vaca, ts, temperatura, pulso, riesgo)
        VALUES (%s,%s,%s,%s,%s)
    """, (id_vaca, ts, temp, pulso, riesgo))

    conn.commit()
    print(f"Datos insertados: {id_vaca} {ts}")

client = mqtt.Client()
client.on_message = on_message
client.connect("localhost")
client.subscribe("vaca/telemetria")
client.loop_forever()
