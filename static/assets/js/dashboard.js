// ----------------------------------------------------------------------
// 1. CONFIGURACIÓN DEL BACKEND (Inyectada por Jinja)
// ----------------------------------------------------------------------
const API_BASE_URL = window.APP_CONFIG.API_BASE_URL;
const API_KEY = window.APP_CONFIG.API_KEY;

const API_HEADERS = {
    'X-API-Key': API_KEY,
    'Content-Type': 'application/json'
};

// Contenedores de elementos del DOM
const cowTableBody = document.getElementById('cow-table-body');

// Objetos globales de Leaflet
let map = null;
let geofenceCircle = null;
const cowMarkers = {}; // Almacena los marcadores de las vacas por ID

// ----------------------------------------------------------------------
// 2. INICIALIZACIÓN DEL MAPA
// ----------------------------------------------------------------------



const alertsContainer = document.getElementById('alerts-list-content');

// 2. Función para obtener y mostrar alertas
async function updateAlerts() {
    // Llama al endpoint de la API
    const alerts = await fetchData('/api/v1/alertas');

    if (!alerts || alerts.error) {
        alertsContainer.innerHTML = '<div class="alert alert-warning">No hay conexión o alertas disponibles.</div>';
        return;
    }

    alertsContainer.innerHTML = ''; // <-- Limpia todos los placeholders de alerta

    alerts.forEach(alert => {
        // Lógica para asignar estilos basados en el tipo de alerta
        const isCritical = alert.tipo_alerta === 'Geocerca' || alert.tipo_alerta.includes('Alta') || alert.tipo_alerta.includes('Bajo');
        const alertClass = isCritical ? 'alert-danger' : 'alert-warning';

        const alertHtml = `
            <div class="alert ${alertClass} py-2 mb-2" role="alert">
                <b class="d-block">${alert.tipo_alerta} en Vaca ${alert.id_vaca}</b>
                <small>${alert.mensaje}</small>
                <small class="d-block text-muted">${alert.ts}</small>
            </div>
        `;
        // Agrega el nuevo HTML al contenedor
        alertsContainer.insertAdjacentHTML('beforeend', alertHtml);
    });
}

// Función genérica de fetch con manejo de errores y API Key
async function fetchData(endpoint) {
    try {
        const response = await fetch(`${API_BASE_URL}${endpoint}`, { headers: API_HEADERS });
        if (!response.ok) {
            throw new Error(`HTTP error! status: ${response.status}`);
        }
        return await response.json();
    } catch (error) {
        console.error(`Error fetching data from ${endpoint}:`, error);
        // Mostrar una alerta en el dashboard si la conexión falla
        return null;
    }
}

async function initializeMap() {
    const geofenceData = await fetchData('/api/v1/geofence');

    if (!geofenceData || geofenceData.error) {
        // Usar coordenadas de prueba si la API falla
        const pastureCenter = [20.11, -99.22];
        map = L.map('cow-tracking-map').setView(pastureCenter, 16);

        // Ocultar mapa o mostrar error si la geocerca es nula
        console.error("No se pudo obtener la configuración de geocerca.");
        return;
    }

    const center = [geofenceData.center.lat, geofenceData.center.lng];
    const radiusMeters = geofenceData.radius_km * 1000;

    map = L.map('cow-tracking-map').setView(center, 16);

    // Añadir la capa de Tiles (OpenStreetMap)
    L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png', {
        maxZoom: 19,
        attribution: '&copy; <a href="http://www.openstreetmap.org/copyright">OpenStreetMap</a>'
    }).addTo(map);

    // Dibujar la Geocerca (Círculo)
    geofenceCircle = L.circle(center, {
        color: 'red',
        fillColor: '#f03',
        fillOpacity: 0.15,
        radius: radiusMeters
    }).addTo(map).bindPopup(`Geocerca: ${geofenceData.radius_km} km`);
}

// ----------------------------------------------------------------------
// 3. ACTUALIZACIÓN DE DATOS (Vacas y Alertas)
// ----------------------------------------------------------------------

function createCowMarker(vacaId, lat, lng, temp, pulso, riesgo) {
    const iconColor = riesgo ? '#FF0000' : '#00AA00'; // Rojo para riesgo, verde para normal
    const marker = L.circleMarker([lat, lng], {
        radius: 8,
        fillColor: iconColor,
        color: iconColor,
        weight: 1,
        opacity: 1,
        fillOpacity: 0.8
    });

    marker.bindPopup(`
        <b>ID: ${vacaId}</b><br>
        Temp: ${temp}°C<br>
        BPM: ${pulso}<br>
        Riesgo: ${riesgo ? 'ALTO 🔴' : 'Normal 🟢'}
    `);
    return marker;
}

function updateCowMarker(vacaId, lat, lng, temp, pulso, riesgo) {
    const newLatLng = [lat, lng];
    const iconColor = riesgo ? '#FF0000' : '#00AA00'; // Rojo para riesgo, verde para normal

    if (cowMarkers[vacaId]) {
        // Marcador existente: Actualizar posición y estilo
        cowMarkers[vacaId].setLatLng(newLatLng);
        cowMarkers[vacaId].setStyle({ fillColor: iconColor, color: iconColor });
    } else {
        // Marcador nuevo: Crear y añadir al mapa
        const marker = L.circleMarker(newLatLng, {
            radius: 8,
            fillColor: iconColor,
            color: iconColor,
            weight: 1,
            opacity: 1,
            fillOpacity: 0.8
        });

        marker.bindPopup(`
            <b>ID: ${vacaId}</b><br>
            Temp: ${temp}°C<br>
            BPM: ${pulso}
        `);
        marker.addTo(map);
        cowMarkers[vacaId] = marker;
    }
}

function createCowTableRow(vacaId, lat, lng, temp, pulso, riesgo) {
    // Estilos basados en riesgo (geocerca o salud)
    const statusClass = riesgo ? 'bg-danger' : 'bg-success';
    const statusText = riesgo ? 'ALTO' : 'Normal';

    return `
        <tr>
          <td class="px-0">
            <h6 class="mb-0 fw-bolder">${vacaId}</h6>
          </td>
          <td class="px-0">${lat.toFixed(5)}, ${lng.toFixed(5)}</td>
          <td class="px-0">
            <span class="badge ${statusClass}">${temp.toFixed(1)}°C</span>
          </td>
          <td class="px-0 text-dark fw-medium text-end">${pulso} bpm</td>
        </tr>
    `;
}

// --- Función para Llenar la Tabla y el Mapa ---
async function updateAllCowsAndTable() {
    // 1. Obtener la lista de IDs disponibles
    // Asumiendo que el endpoint /api/v1/vaca_ids devuelve ['V1', 'V2', ...]
    const vacaIds = await fetchData('/api/v1/vaca_ids');
    if (!vacaIds || vacaIds.error) return;

    // 2. Limpiar la tabla de todos los datos de ejemplo
    cowTableBody.innerHTML = '';

    let newTableHTML = '';

    for (const vacaId of vacaIds) {
        // Llama a la API para obtener el resumen de la última lectura (desde telemetria_vaca)
        const data = await fetchData(`/api/v1/vaca/${vacaId}/latest`);

        if (data && !data.error && data.latest_data) {
            const latestData = data.latest_data;
            const lat = latestData.lat;
            const lng = latestData.lng;
            const temp = latestData.temp;
            const pulso = latestData.pulso;
            const riesgo = latestData.riesgo; // True/False

            // 3. Generar el HTML de la fila
            newTableHTML += createCowTableRow(vacaId, lat, lng, temp, pulso, riesgo);

            // 4. Actualizar/Crear Marcador en el Mapa (lógica que ya creamos en el JS)
            updateCowMarker(vacaId, lat, lng, temp, pulso, riesgo);
        }
    }

    // 5. Inserción final en el DOM (reemplazando todas las filas de ejemplo)
    cowTableBody.innerHTML = newTableHTML;
}


// ----------------------------------------------------------------------
// 4. BUCLE DE ACTUALIZACIÓN
// ----------------------------------------------------------------------

async function mainLoop() {
    if (!map) {
        await initializeMap();

        // --- AÑADIR ESTA VERIFICACIÓN ---
        setTimeout(async() => {
            if (map) {
                map.invalidateSize(); // Obliga a Leaflet a recalcular el tamaño del contenedor
                // Centrar el mapa en la geocerca inicial
                const geofenceData = await fetchData('/api/v1/geofence');
                if (geofenceData && !geofenceData.error) {
                    map.setView([geofenceData.center.lat, geofenceData.center.lng], 16);
                }
            }
        }, 300);
    }

    updateAllCowsAndTable();
    updateAlerts();
}

// Iniciar el bucle de actualización cada 10 segundos
document.addEventListener('DOMContentLoaded', () => {
// Asegurarse de que la configuración exista antes de iniciar
if (window.APP_CONFIG) {
    mainLoop(); // Primera llamada inmediata
    setInterval(mainLoop, 10000); // Luego cada 10 segundos
}
});