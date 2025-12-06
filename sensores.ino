#include "BluetoothSerial.h"
#include <WiFi.h>
#include <PubSubClient.h>      // Librería para MQTT
#include <time.h>              // Para la sincronización NTP y manejo de tiempo
#include <Wire.h>
#include "MAX30105.h"
#include "heartRate.h"         // Librería para el algoritmo de detección de pulso

// ====================================================================
// === 1. CONFIGURACIÓN DE PINES Y CONSTANTES ===
// Pines MAX30102
const int SDA_PIN = 21;
const int SCL_PIN = 22;
const int INT_PIN = 4;        
#define PIN_KY013_SENAL 34    // Pin para el Termistor
#define PIN_BAT_READ 35       // NUEVO: Pin ADC para lectura de Batería

// NUEVO: Pines LED RGB (Cátodo Común)
#define PIN_LED_R 12
#define PIN_LED_G 14
#define PIN_LED_B 27
// NUEVO: Pin Buzzer
#define PIN_BUZZER 26

// Constantes de Pulso
const int BPM_OFFSET = 50;    // Constante para sumar 50 BPM al resultado final (Shifted value)
long lastBeat = 0;            
float bpmFiltered = 0.0;      
const float alpha = 0.85;     

// Umbrales de Riesgo (Usando valores con el OFFSET de +50 BPM)
const int BPM_RISK_HIGH_SHIFTED = 150; // 100 BPM real
const int BPM_RISK_LOW_SHIFTED = 100;  // 50 BPM real
const float TEMP_RISK_HIGH = 38.5; 
const float TEMP_RISK_LOW = 35.0;  

// Constantes del Termistor (KY-013)
const float R_NOMINAL = 10000;
const float T_NOMINAL = 298.15; // 25°C en Kelvin
const float B_COEFICIENTE = 3950;
const float R_SERIE = 10000;

// NUEVO: Constantes de Batería y Geofence
const float MAX_BATTERY_VOLTAGE = 4.2; // 100% de la batería (LiPo)
const float MIN_BATTERY_VOLTAGE = 3.3; // 0% de la batería
const float ADC_RATIO = 2.0;           // Factor de divisor de voltaje (si se usa 10k/10k)

const float LAT_FENCE = 20.734503;
const float LNG_FENCE = -103.455896;
const float RADIUS_FENCE_KM = 0.2;     // 0.2 km = 200 metros
bool isOutGeofence = false;            // Estado actual del Geofence

// === 2. CONFIGURACIÓN DE RED Y DISPOSITIVO ===
const char* ssid = "Paul'sS25";
const char* password = "paulsb25";
const char* mqtt_server = "broker.mqtt.cool";
const int mqtt_port = 1883;
const char* mqtt_topic = "vaca/telemetria";
const char* mqtt_client_id = "ESP32_MooCollar_101";
const char* DEVICE_ID = "V1"; 

#define BT_DEVICE_NAME "MOO_COLLAR_MONITOR" 

// === NTP CONFIGURACIÓN DE TIEMPO ===
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 0; 
const int   daylightOffset_sec = 0;

// === 3. VARIABLES DE ESTADO Y TIMING ===
BluetoothSerial SerialBT; 
MAX30105 particleSensor;
WiFiClient espClient; 
PubSubClient mqttClient(espClient);

// Lecturas de sensores
int bpm_reading = 0;      
float temp_reading = 0.0; 
float gpsLat = 0.0;
float gpsLng = 0.0;
float battery_percent = 0.0; // NUEVO
String gpsData = "Lat: 0.0 Lon: 0.0"; 

// Timing (NON-BLOCKING)
unsigned long lastPublish = 0;
const long PUBLISH_INTERVAL = 15000; // 15 segundos
const long SENSOR_READ_DELAY = 50;   // 50ms

// ====================================================================
// === DEFINICIÓN DE FUNCIONES DE CONTROL ===

// NUEVO: Control RGB
void setRGB(int r, int g, int b) {
    analogWrite(PIN_LED_R, r);
    analogWrite(PIN_LED_G, g);
    analogWrite(PIN_LED_B, b);
}

// NUEVO: Función para determinar el estado de la batería y encender el LED
void updateBatteryStatusLED(float percentage) {
    if (percentage > 50) {
        setRGB(0, 255, 0); // Verde: OK (Más del 50%)
    } else if (percentage > 20) {
        setRGB(255, 100, 0); // Amarillo/Naranja: Advertencia (20% - 50%)
    } else {
        setRGB(255, 0, 0); // Rojo: Batería baja (Menos del 20%)
    }
}

// NUEVO: Función para leer el voltaje y calcular el porcentaje
float readBatteryPercentage() {
    int raw = analogRead(PIN_BAT_READ);
    // Asumiendo 12 bits (4095) y un voltaje de referencia de 3.3V
    float voltage = (float)raw / 4095.0 * 3.3 * ADC_RATIO; 
    
    // Mapeo lineal del voltaje al porcentaje
    float percentage = (voltage - MIN_BATTERY_VOLTAGE) / (MAX_BATTERY_VOLTAGE - MIN_BATTERY_VOLTAGE) * 100.0;

    // Asegurar que el porcentaje esté entre 0 y 100
    if (percentage > 100.0) percentage = 100.0;
    if (percentage < 0.0) percentage = 0.0;

    return percentage;
}

// NUEVO: Función para calcular la distancia Haversine (simplificada para ESP32)
// Retorna la distancia en kilómetros (km)
float calculateDistance(float lat1, float lon1, float lat2, float lon2) {
    // Radio de la Tierra en Kilómetros
    const float R = 6371.0; 

    float dLat = radians(lat2 - lat1);
    float dLon = radians(lon2 - lon1);

    lat1 = radians(lat1);
    lat2 = radians(lat2);

    // Fórmula de Haversine
    float a = sin(dLat / 2) * sin(dLat / 2) +
              sin(dLon / 2) * sin(dLon / 2) * cos(lat1) * cos(lat2); 
    float c = 2 * atan2(sqrt(a), sqrt(1 - a)); 
    float distance = R * c; 

    return distance; // Distancia en km
}

// NUEVO: Función para activar/desactivar el buzzer
void activateBuzzer(bool state) {
    if (state) {
        // Tono de alerta (solo una vez o de forma intermitente)
        tone(PIN_BUZZER, 1000, 200); // 1000 Hz por 200ms
        delay(300); // Pequeña pausa
        tone(PIN_BUZZER, 1000, 200);
        delay(300);
        noTone(PIN_BUZZER);
    } else {
        noTone(PIN_BUZZER);
    }
}

// === FUNCIONES EXISTENTES ===
String getISO8601Time() {
    time_t now;
    struct tm timeinfo;
    time(&now);
    gmtime_r(&now, &timeinfo); 

    char buffer[25];
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &timeinfo); 
    return String(buffer);
}

int leerBPM() {
  long irValue = particleSensor.getIR();
  if (irValue < 10000) { 
    bpmFiltered = 0.0; 
    return 0; 
  }

  boolean isBeat = checkForBeat(irValue); 
  
  if (isBeat) {
    long delta = millis() - lastBeat;
    lastBeat = millis();

    float bpm = 60.0 / (delta / 1000.0);

    if (bpm > 30 && bpm < 200) { 
      if (bpmFiltered == 0.0) { 
          bpmFiltered = bpm;
      } else {
          bpmFiltered = alpha * bpmFiltered + (1 - alpha) * bpm;
      }
    }
  }
  
  if (bpmFiltered < 30.0) {
      return 0; 
  }
  
  int finalBPM = (int)(bpmFiltered + BPM_OFFSET);
  
  return finalBPM; 
}

#include "BluetoothSerial.h"
#include <WiFi.h>
#include <PubSubClient.h>      // Librería para MQTT
#include <time.h>              // Para la sincronización NTP y manejo de tiempo
#include <Wire.h>
#include "MAX30105.h"
#include "heartRate.h"         // Librería para el algoritmo de detección de pulso

// ====================================================================
// === 1. CONFIGURACIÓN DE PINES Y CONSTANTES ===
// Pines MAX30102
const int SDA_PIN = 21;
const int SCL_PIN = 22;
const int INT_PIN = 4;        
#define PIN_KY013_SENAL 34    // Pin para el Termistor
#define PIN_BAT_READ 35       // NUEVO: Pin ADC para lectura de Batería

// NUEVO: Pines LED RGB (Cátodo Común)
#define PIN_LED_R 12
#define PIN_LED_G 14
#define PIN_LED_B 27
// NUEVO: Pin Buzzer
#define PIN_BUZZER 26

// Constantes de Pulso
const int BPM_OFFSET = 50;    // Constante para sumar 50 BPM al resultado final (Shifted value)
long lastBeat = 0;            
float bpmFiltered = 0.0;      
const float alpha = 0.85;     

// Umbrales de Riesgo (Usando valores con el OFFSET de +50 BPM)
const int BPM_RISK_HIGH_SHIFTED = 150; // 100 BPM real
const int BPM_RISK_LOW_SHIFTED = 100;  // 50 BPM real
const float TEMP_RISK_HIGH = 38.5; 
const float TEMP_RISK_LOW = 35.0;  

// Constantes del Termistor (KY-013)
const float R_NOMINAL = 10000;
const float T_NOMINAL = 298.15; // 25°C en Kelvin
const float B_COEFICIENTE = 3950;
const float R_SERIE = 10000;

// NUEVO: Constantes de Batería y Geofence
const float MAX_BATTERY_VOLTAGE = 4.2; // 100% de la batería (LiPo)
const float MIN_BATTERY_VOLTAGE = 3.3; // 0% de la batería
const float ADC_RATIO = 2.0;           // Factor de divisor de voltaje (si se usa 10k/10k)

const float LAT_FENCE = 20.734503;
const float LNG_FENCE = -103.455896;
const float RADIUS_FENCE_KM = 0.2;     // 0.2 km = 200 metros
bool isOutGeofence = false;            // Estado actual del Geofence

// === 2. CONFIGURACIÓN DE RED Y DISPOSITIVO ===
const char* ssid = "Paul'sS25";
const char* password = "paulsb25";
const char* mqtt_server = "broker.mqtt.cool";
const int mqtt_port = 1883;
const char* mqtt_topic = "vaca/telemetria";
const char* mqtt_client_id = "ESP32_MooCollar_101";
const char* DEVICE_ID = "V1"; 

#define BT_DEVICE_NAME "MOO_COLLAR_MONITOR" 

// === NTP CONFIGURACIÓN DE TIEMPO ===
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 0; 
const int   daylightOffset_sec = 0;

// === 3. VARIABLES DE ESTADO Y TIMING ===
BluetoothSerial SerialBT; 
MAX30105 particleSensor;
WiFiClient espClient; 
PubSubClient mqttClient(espClient);

// Lecturas de sensores
int bpm_reading = 0;      
float temp_reading = 0.0; 
float gpsLat = 0.0;
float gpsLng = 0.0;
float battery_percent = 0.0; // NUEVO
String gpsData = "Lat: 0.0 Lon: 0.0"; 

// Timing (NON-BLOCKING)
unsigned long lastPublish = 0;
const long PUBLISH_INTERVAL = 15000; // 15 segundos
const long SENSOR_READ_DELAY = 50;   // 50ms

// ====================================================================
// === DEFINICIÓN DE FUNCIONES DE CONTROL ===

// NUEVO: Control RGB
void setRGB(int r, int g, int b) {
    analogWrite(PIN_LED_R, r);
    analogWrite(PIN_LED_G, g);
    analogWrite(PIN_LED_B, b);
}

// NUEVO: Función para determinar el estado de la batería y encender el LED
void updateBatteryStatusLED(float percentage) {
    if (percentage > 50) {
        setRGB(0, 255, 0); // Verde: OK (Más del 50%)
    } else if (percentage > 20) {
        setRGB(255, 100, 0); // Amarillo/Naranja: Advertencia (20% - 50%)
    } else {
        setRGB(255, 0, 0); // Rojo: Batería baja (Menos del 20%)
    }
}

// NUEVO: Función para leer el voltaje y calcular el porcentaje
float readBatteryPercentage() {
    int raw = analogRead(PIN_BAT_READ);
    // Asumiendo 12 bits (4095) y un voltaje de referencia de 3.3V
    float voltage = (float)raw / 4095.0 * 3.3 * ADC_RATIO; 
    
    // Mapeo lineal del voltaje al porcentaje
    float percentage = (voltage - MIN_BATTERY_VOLTAGE) / (MAX_BATTERY_VOLTAGE - MIN_BATTERY_VOLTAGE) * 100.0;

    // Asegurar que el porcentaje esté entre 0 y 100
    if (percentage > 100.0) percentage = 100.0;
    if (percentage < 0.0) percentage = 0.0;

    return percentage;
}

// NUEVO: Función para calcular la distancia Haversine (simplificada para ESP32)
// Retorna la distancia en kilómetros (km)
float calculateDistance(float lat1, float lon1, float lat2, float lon2) {
    // Radio de la Tierra en Kilómetros
    const float R = 6371.0; 

    float dLat = radians(lat2 - lat1);
    float dLon = radians(lon2 - lon1);

    lat1 = radians(lat1);
    lat2 = radians(lat2);

    // Fórmula de Haversine
    float a = sin(dLat / 2) * sin(dLat / 2) +
              sin(dLon / 2) * sin(dLon / 2) * cos(lat1) * cos(lat2); 
    float c = 2 * atan2(sqrt(a), sqrt(1 - a)); 
    float distance = R * c; 

    return distance; // Distancia en km
}

// NUEVO: Función para activar/desactivar el buzzer
void activateBuzzer(bool state) {
    if (state) {
        // Tono de alerta (solo una vez o de forma intermitente)
        tone(PIN_BUZZER, 1000, 200); // 1000 Hz por 200ms
        delay(300); // Pequeña pausa
        tone(PIN_BUZZER, 1000, 200);
        delay(300);
        noTone(PIN_BUZZER);
    } else {
        noTone(PIN_BUZZER);
    }
}

// === FUNCIONES EXISTENTES ===
String getISO8601Time() {
    time_t now;
    struct tm timeinfo;
    time(&now);
    gmtime_r(&now, &timeinfo); 

    char buffer[25];
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &timeinfo); 
    return String(buffer);
}

int leerBPM() {
  long irValue = particleSensor.getIR();
  if (irValue < 10000) { 
    bpmFiltered = 0.0; 
    return 0; 
  }

  boolean isBeat = checkForBeat(irValue); 
  
  if (isBeat) {
    long delta = millis() - lastBeat;
    lastBeat = millis();

    float bpm = 60.0 / (delta / 1000.0);

    if (bpm > 30 && bpm < 200) { 
      if (bpmFiltered == 0.0) { 
          bpmFiltered = bpm;
      } else {
          bpmFiltered = alpha * bpmFiltered + (1 - alpha) * bpm;
      }
    }
  }
  
  if (bpmFiltered < 30.0) {
      return 0; 
  }
  
  int finalBPM = (int)(bpmFiltered + BPM_OFFSET);
  
  return finalBPM; 
}

float leerTemperaturaKY013() {
  int lecturaAnalogica = analogRead(PIN_KY013_SENAL);

  if (lecturaAnalogica == 0 || lecturaAnalogica == 4095) return 0.0;

  float resistenciaTermistor = R_SERIE / ((4095.0 / lecturaAnalogica) - 1);
  float temperaturaK = resistenciaTermistor / R_NOMINAL; 
  temperaturaK = log(temperaturaK);               
  temperaturaK /= B_COEFICIENTE;                
  temperaturaK += (1.0 / T_NOMINAL);            
  temperaturaK = 1.0 / temperaturaK;            

  float temperaturaC = temperaturaK - 273.15;

  return temperaturaC;
}

void parseGPS() {
    int latIndex = gpsData.indexOf("Lat:");
    int lonIndex = gpsData.indexOf("Lon:");
    if (latIndex != -1 && lonIndex != -1) {
        int latStart = latIndex + 4; 
        int latEnd = lonIndex;       
        int lonStart = lonIndex + 4; 
        if (latEnd > latStart) {
            String latStr = gpsData.substring(latStart, latEnd);
            latStr.trim(); 
            gpsLat = latStr.toFloat();

            String lonStr = gpsData.substring(lonStart);
            lonStr.trim(); 
            gpsLng = lonStr.toFloat();
            
            Serial.println("--- ¡PARSEADO GPS EXITOSO! ---");
            Serial.print("Latitud: ");
            Serial.print(gpsLat, 6);
            Serial.print(", Longitud: ");
            Serial.println(gpsLng, 6);
        }
    } else {
        // Serial.println("Error: Formato GPS no encontrado."); // Desactivado para no llenar el log
    }
}

void setup_wifi() {
    Serial.print("Conectando a ");
    Serial.println(ssid);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi conectado!");
    Serial.print("Dirección IP: ");
    Serial.println(WiFi.localIP());
}

void setup_ntp() {
    Serial.print("Sincronizando tiempo con NTP...");
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    
    time_t now = time(nullptr);
    int attempts = 0;
    while (now < 1000000000 && attempts < 20) { 
        delay(500);
        Serial.print(".");
        now = time(nullptr);
        attempts++;
    }
    if (now > 1000000000) {
      Serial.println("\nTiempo sincronizado exitosamente!");
    } else {
      Serial.println("\nFALLO: No se pudo obtener el tiempo NTP.");
    }
}

void reconnect_mqtt() {
    while (!mqttClient.connected()) {
        Serial.print("Intentando conexión MQTT...");
        if (mqttClient.connect(mqtt_client_id)) {
            Serial.println("Conectado al Broker!");
        } else {
            Serial.print("Falló, rc=");
            Serial.print(mqttClient.state());
            Serial.println(". Reintentando en 5 segundos...");
            delay(5000);
        }
    }
}

// ====================================================================
// === SETUP ===
void setup() {
    Serial.begin(115200);
    delay(100);

    // Configuración de pines (NUEVO)
    pinMode(PIN_LED_R, OUTPUT);
    pinMode(PIN_LED_G, OUTPUT);
    pinMode(PIN_LED_B, OUTPUT);
    pinMode(PIN_BUZZER, OUTPUT);
    pinMode(PIN_BAT_READ, INPUT);
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db); // Rango completo de 0 a 3.3V

    // 1. Inicializar WiFi y NTP
    setup_wifi();
    setup_ntp();

    // 2. Inicializar Sensores y I2C
    Wire.begin(SDA_PIN, SCL_PIN); 
    pinMode(INT_PIN, INPUT); 
    if (particleSensor.begin(Wire, I2C_SPEED_FAST)) {
        Serial.println("MAX30102 detectado y configurando.");
        particleSensor.setup(0x1F, 4, 2, 100, 411, 4096); 
        particleSensor.clearFIFO();
    } else {
        Serial.println("ERROR: MAX30102 no encontrado.");
    }

    // 3. Configurar MQTT y Termistor
    mqttClient.setServer(mqtt_server, mqtt_port);
    pinMode(PIN_KY013_SENAL, INPUT);

    // 4. Inicializar Bluetooth
    if (!SerialBT.begin(BT_DEVICE_NAME)) {
        Serial.println("!!! Error al inicializar Bluetooth !!!");
    } else {
        Serial.println("Bluetooth SPP iniciado.");
    }
    
    // Al inicio, mostrar LED Verde (asumiendo batería OK hasta la primera lectura)
    setRGB(0, 255, 0); 
}

// ====================================================================
// === LOOP PRINCIPAL ===
void loop() {
    
    // --- PASO 1: LECTURA RÁPIDA DE SENSORES Y COMUNICACIÓN BT (NO BLOQUEANTE) ---
    if (SerialBT.available()) {
        gpsData = SerialBT.readStringUntil('\n'); 
        gpsData.trim(); 
        if (gpsData.length() > 0) {
            parseGPS(); 
        }
    }
    
    temp_reading = leerTemperaturaKY013();
    int current_bpm = leerBPM();
    if (current_bpm > 0) {
        bpm_reading = current_bpm;
    }
    
    // NUEVO: Lectura de Batería y actualización de LED
    battery_percent = readBatteryPercentage();
    updateBatteryStatusLED(battery_percent);
    
    // NUEVO: Lógica de Geofence y Buzzer (Se ejecuta si hay datos GPS válidos)
    if (gpsLat != 0.0 && gpsLng != 0.0) {
        float distance_km = calculateDistance(gpsLat, gpsLng, LAT_FENCE, LNG_FENCE);
        
        if (distance_km > RADIUS_FENCE_KM) {
            if (!isOutGeofence) {
                isOutGeofence = true;
                Serial.println("!!! ALERTA: FUERA DE CERCADO VIRTUAL !!!");
            }
            // El buzzer solo sonará una vez por ciclo si está fuera.
            // Para evitar un tono constante, lo movemos al bloque de publicación.
        } else {
            isOutGeofence = false;
        }
    }
    
    delay(SENSOR_READ_DELAY); 
    
    // -------------------------------------------------------------------
    // --- PASO 2: LÓGICA DE PUBLICACIÓN (solo cada 15 segundos) ---
    if (millis() - lastPublish >= PUBLISH_INTERVAL) {
        lastPublish = millis();
        
        if (!mqttClient.connected()) {
            reconnect_mqtt();
        }
        mqttClient.loop(); 

        // 3. Control del Buzzer (se activa solo si está fuera del cercado)
        activateBuzzer(isOutGeofence);

        // 4. Detección de Riesgo (combinando variables originales) 
        bool riesgo = false;
        
        if(temp_reading > TEMP_RISK_HIGH || temp_reading < TEMP_RISK_LOW){
            riesgo = true;
        }
        else if(bpm_reading > BPM_RISK_HIGH_SHIFTED || bpm_reading < BPM_RISK_LOW_SHIFTED){
            if (bpm_reading > 0) { 
                riesgo = true;
            }
        }
        
        // Agregar alerta de batería baja al riesgo
        if (battery_percent < 20.0) {
             riesgo = true;
             Serial.println("!!! ALERTA: BATERÍA BAJA !!!");
        }
        
        // 5. Construir JSON Payload
        String isoTimestamp = getISO8601Time();
        const char* riesgo_json = riesgo ? "true" : "false";

        String payload = "{";
        payload += "\"id_vaca\": \"" + String(DEVICE_ID) + "\","; 
        payload += "\"timestamp\": \"" + isoTimestamp + "\","; 
        payload += "\"lat\": " + String(gpsLat, 6) + ","; 
        payload += "\"lng\": " + String(gpsLng, 6) + ",";  
        payload += "\"temperatura\": " + String(temp_reading, 1) + ","; 
        payload += "\"pulso\": " + String(bpm_reading) + ","; 
        payload += "\"bateria_porcentaje\": " + String(battery_percent, 1) + ","; // NUEVO
        payload += "\"riesgo\": " + String(riesgo_json) + ",";
        payload += "\"fuera_cerca\": " + String(isOutGeofence ? "true" : "false") + ","; // NUEVO
        payload += "\"area\":" "\"TEC\"";
        payload += "}";

        Serial.println("----------------------------------------");
        Serial.println("[PAQUETE_FINAL_JSON]:");
        Serial.println(payload);
        
        // 6. ENVIAR DATOS POR MQTT
        if (mqttClient.publish(mqtt_topic, payload.c_str())) {
            Serial.println("¡EXITO en la publicación!");
        } else {
            Serial.println("¡FALLO en la publicación!");
        }
        Serial.println("----------------------------------------");
    }
}

void parseGPS() {
    int latIndex = gpsData.indexOf("Lat:");
    int lonIndex = gpsData.indexOf("Lon:");
    if (latIndex != -1 && lonIndex != -1) {
        int latStart = latIndex + 4; 
        int latEnd = lonIndex;       
        int lonStart = lonIndex + 4; 
        if (latEnd > latStart) {
            String latStr = gpsData.substring(latStart, latEnd);
            latStr.trim(); 
            gpsLat = latStr.toFloat();

            String lonStr = gpsData.substring(lonStart);
            lonStr.trim(); 
            gpsLng = lonStr.toFloat();
            
            Serial.println("--- ¡PARSEADO GPS EXITOSO! ---");
            Serial.print("Latitud: ");
            Serial.print(gpsLat, 6);
            Serial.print(", Longitud: ");
            Serial.println(gpsLng, 6);
        }
    } else {
        // Serial.println("Error: Formato GPS no encontrado."); // Desactivado para no llenar el log
    }
}

void setup_wifi() {
    Serial.print("Conectando a ");
    Serial.println(ssid);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi conectado!");
    Serial.print("Dirección IP: ");
    Serial.println(WiFi.localIP());
}

void setup_ntp() {
    Serial.print("Sincronizando tiempo con NTP...");
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    
    time_t now = time(nullptr);
    int attempts = 0;
    while (now < 1000000000 && attempts < 20) { 
        delay(500);
        Serial.print(".");
        now = time(nullptr);
        attempts++;
    }
    if (now > 1000000000) {
      Serial.println("\nTiempo sincronizado exitosamente!");
    } else {
      Serial.println("\nFALLO: No se pudo obtener el tiempo NTP.");
    }
}

void reconnect_mqtt() {
    while (!mqttClient.connected()) {
        Serial.print("Intentando conexión MQTT...");
        if (mqttClient.connect(mqtt_client_id)) {
            Serial.println("Conectado al Broker!");
        } else {
            Serial.print("Falló, rc=");
            Serial.print(mqttClient.state());
            Serial.println(". Reintentando en 5 segundos...");
            delay(5000);
        }
    }
}

// ====================================================================
// === SETUP ===
void setup() {
    Serial.begin(115200);
    delay(100);

    // Configuración de pines (NUEVO)
    pinMode(PIN_LED_R, OUTPUT);
    pinMode(PIN_LED_G, OUTPUT);
    pinMode(PIN_LED_B, OUTPUT);
    pinMode(PIN_BUZZER, OUTPUT);
    pinMode(PIN_BAT_READ, INPUT);
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db); // Rango completo de 0 a 3.3V

    // 1. Inicializar WiFi y NTP
    setup_wifi();
    setup_ntp();

    // 2. Inicializar Sensores y I2C
    Wire.begin(SDA_PIN, SCL_PIN); 
    pinMode(INT_PIN, INPUT); 
    if (particleSensor.begin(Wire, I2C_SPEED_FAST)) {
        Serial.println("MAX30102 detectado y configurando.");
        particleSensor.setup(0x1F, 4, 2, 100, 411, 4096); 
        particleSensor.clearFIFO();
    } else {
        Serial.println("ERROR: MAX30102 no encontrado.");
    }

    // 3. Configurar MQTT y Termistor
    mqttClient.setServer(mqtt_server, mqtt_port);
    pinMode(PIN_KY013_SENAL, INPUT);

    // 4. Inicializar Bluetooth
    if (!SerialBT.begin(BT_DEVICE_NAME)) {
        Serial.println("!!! Error al inicializar Bluetooth !!!");
    } else {
        Serial.println("Bluetooth SPP iniciado.");
    }
    
    // Al inicio, mostrar LED Verde (asumiendo batería OK hasta la primera lectura)
    setRGB(0, 255, 0); 
}

// ====================================================================
// === LOOP PRINCIPAL ===
void loop() {
    
    // --- PASO 1: LECTURA RÁPIDA DE SENSORES Y COMUNICACIÓN BT (NO BLOQUEANTE) ---
    if (SerialBT.available()) {
        gpsData = SerialBT.readStringUntil('\n'); 
        gpsData.trim(); 
        if (gpsData.length() > 0) {
            parseGPS(); 
        }
    }
    
    temp_reading = leerTemperaturaKY013();
    int current_bpm = leerBPM();
    if (current_bpm > 0) {
        bpm_reading = current_bpm;
    }
    
    // NUEVO: Lectura de Batería y actualización de LED
    battery_percent = readBatteryPercentage();
    updateBatteryStatusLED(battery_percent);
    
    // NUEVO: Lógica de Geofence y Buzzer (Se ejecuta si hay datos GPS válidos)
    if (gpsLat != 0.0 && gpsLng != 0.0) {
        float distance_km = calculateDistance(gpsLat, gpsLng, LAT_FENCE, LNG_FENCE);
        
        if (distance_km > RADIUS_FENCE_KM) {
            if (!isOutGeofence) {
                isOutGeofence = true;
                Serial.println("!!! ALERTA: FUERA DE CERCADO VIRTUAL !!!");
            }
            // El buzzer solo sonará una vez por ciclo si está fuera.
            // Para evitar un tono constante, lo movemos al bloque de publicación.
        } else {
            isOutGeofence = false;
        }
    }
    
    delay(SENSOR_READ_DELAY); 
    
    // -------------------------------------------------------------------
    // --- PASO 2: LÓGICA DE PUBLICACIÓN (solo cada 15 segundos) ---
    if (millis() - lastPublish >= PUBLISH_INTERVAL) {
        lastPublish = millis();
        
        if (!mqttClient.connected()) {
            reconnect_mqtt();
        }
        mqttClient.loop(); 

        // 3. Control del Buzzer (se activa solo si está fuera del cercado)
        activateBuzzer(isOutGeofence);

        // 4. Detección de Riesgo (combinando variables originales) 
        bool riesgo = false;
        
        if(temp_reading > TEMP_RISK_HIGH || temp_reading < TEMP_RISK_LOW){
            riesgo = true;
        }
        else if(bpm_reading > BPM_RISK_HIGH_SHIFTED || bpm_reading < BPM_RISK_LOW_SHIFTED){
            if (bpm_reading > 0) { 
                riesgo = true;
            }
        }
        
        // Agregar alerta de batería baja al riesgo
        if (battery_percent < 20.0) {
             riesgo = true;
             Serial.println("!!! ALERTA: BATERÍA BAJA !!!");
        }
        
        // 5. Construir JSON Payload
        String isoTimestamp = getISO8601Time();
        const char* riesgo_json = riesgo ? "true" : "false";

        String payload = "{";
        payload += "\"id_vaca\": \"" + String(DEVICE_ID) + "\","; 
        payload += "\"timestamp\": \"" + isoTimestamp + "\","; 
        payload += "\"lat\": " + String(gpsLat, 6) + ","; 
        payload += "\"lng\": " + String(gpsLng, 6) + ",";  
        payload += "\"temperatura\": " + String(temp_reading, 1) + ","; 
        payload += "\"pulso\": " + String(bpm_reading) + ","; 
        payload += "\"bateria_porcentaje\": " + String(battery_percent, 1) + ","; // NUEVO
        payload += "\"riesgo\": " + String(riesgo_json) + ",";
        payload += "\"fuera_cerca\": " + String(isOutGeofence ? "true" : "false") + ","; // NUEVO
        payload += "\"area\":" "\"TEC\"";
        payload += "}";

        Serial.println("----------------------------------------");
        Serial.println("[PAQUETE_FINAL_JSON]:");
        Serial.println(payload);
        
        // 6. ENVIAR DATOS POR MQTT
        if (mqttClient.publish(mqtt_topic, payload.c_str())) {
            Serial.println("¡EXITO en la publicación!");
        } else {
            Serial.println("¡FALLO en la publicación!");
        }
        Serial.println("----------------------------------------");
    }
}
