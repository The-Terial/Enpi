#include <WiFi.h>
#include <HTTPClient.h>
#include <PZEM004Tv30.h>
#include <time.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <WiFiClientSecure.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include <math.h>

#define PZEM_RX_PIN 17
#define PZEM_TX_PIN 18
#define BOTON_RESET_CONFIG 0

HardwareSerial pzemSerial(2);
PZEM004Tv30 pzem(pzemSerial, PZEM_RX_PIN, PZEM_TX_PIN);

// ============================================================
// VERSION / ENDPOINTS POR DEFECTO
// ============================================================
const char* FW_VERSION = "1.4.0";

// URL de rescate compilada en firmware. Aunque config_url cambie remotamente,
// ENPI intentara este bootstrap si la URL configurable deja de responder.
const char* BOOTSTRAP_CONFIG_URL =
  "";

const char* DEFAULT_API_URL =
  "";

const char* AP_SSID = "";
const char* AP_PASS = "";
const char* ADMIN_PASS = "";

const long GMT_OFFSET_SEC = -4 * 3600;
const int DAYLIGHT_OFFSET_SEC = 0;

Preferences prefs;
WebServer server(80);
DNSServer dnsServer;

String empresa = "";
String codigoMedidor = "";
String wifiSsid = "";
String wifiPass = "";

unsigned long intervaloLecturaMs = 60000;
unsigned long ultimaLectura = 0;

bool modoAP = false;


// ============================================================
// CONFIGURACION REMOTA V4
// ============================================================
// Estos valores se persisten en Preferences y pueden cambiar desde config.json.
String apiMedicionesUrl = DEFAULT_API_URL;
String configRemotaUrl = BOOTSTRAP_CONFIG_URL;

unsigned long intervaloConsultaRemotaMs = 6UL * 60UL * 60UL * 1000UL; // 6 h
unsigned long checkpointFlashMs = 60UL * 60UL * 1000UL;               // 60 min
uint8_t bufferDias = 7;                                                // 1..7
bool bufferHabilitado = true;
bool envioHabilitado = true;
bool modoDiagnostico = false;
uint32_t revisionConfig = 0;

float calibVoltaje = 1.0f;
float calibCorriente = 1.0f;
float calibPotencia = 1.0f;

unsigned long ultimaConsultaConfigRemota = 0;
bool consultaInicialConfigRealizada = false;
const unsigned long ESPERA_INICIAL_CONFIG_MS = 120000UL; // 2 min

String ultimoComandoProcesado = "";
bool actualizacionOTAEnProceso = false;


// ============================================================
// BUFFER DE MEDICIONES PENDIENTES - VERSION 7 DIAS
// ============================================================
// Objetivos:
// 1) Cada lectura conserva el instante ORIGINAL de medicion (timestamp Unix).
// 2) Las caidas cortas se absorben en RAM para evitar escrituras innecesarias.
// 3) La Flash usa un buffer circular fijo de 7 dias a 1 lectura/minuto.
// 4) Al llenarse, se reemplaza primero la lectura MAS ANTIGUA.
// 5) Al volver Internet se reenvia en orden cronologico.
//
// IMPORTANTE SOBRE LA HORA:
// - Si ENPI ya sincronizo NTP y solo se cae Internet, el reloj interno sigue
//   avanzando y los timestamps son correctos.
// - Si ENPI pierde ALIMENTACION y luego arranca sin Internet, un ESP32 sin RTC
//   con bateria no puede conocer la hora real. En ese caso las lecturas se
//   marcan como timestamp=0 hasta poder sincronizar. Para cubrir tambien cortes
//   de energia con hora exacta, la solucion industrial es agregar un RTC DS3231.

struct LecturaPendiente {
  uint32_t timestamp;              // Fecha/hora ORIGINAL de la medicion
  float voltage;
  float current;
  float power;
  float energy;
  float frequency;
  float pf;
  uint32_t duracion_medicion_ms;
  uint32_t intervalo_ms;
};

// 4 horas en RAM a 1 lectura/minuto.
const size_t MAX_PENDIENTES_RAM = 240;
LecturaPendiente pendientesRAM[MAX_PENDIENTES_RAM];
size_t cantidadPendientesRAM = 0;

// 7 dias * 24 h * 60 min = 10.080 lecturas.
const uint32_t MAX_PENDIENTES_FLASH = 7UL * 24UL * 60UL;

const char* ARCHIVO_RING = "/pendientes_ring.bin";
const char* ARCHIVO_META = "/pendientes_meta.bin";

struct MetaBufferFlash {
  uint32_t magic;
  uint32_t version;
  uint32_t inicio;   // indice fisico de la lectura mas antigua
  uint32_t cantidad; // numero de lecturas validas
};

const uint32_t META_MAGIC = 0x454E5049; // "ENPI"
const uint32_t META_VERSION = 3;
MetaBufferFlash metaFlash = {META_MAGIC, META_VERSION, 0, 0};

// Checkpoint agrupado configurable remotamente (por defecto 60 min).
unsigned long ultimoFlushFlash = 0;

// Reintento de cola: una vez por minuto, tras 15 s de WiFi estable.
const unsigned long REINTENTO_PENDIENTES_MS = 60000UL;
unsigned long ultimoReintentoPendientes = 0;
unsigned long wifiConectadoDesde = 0;
bool wifiEstabaConectado = false;

bool horaValida() {
  return time(nullptr) >= 1700000000; // fecha posterior a 2023 aprox.
}

uint32_t obtenerTimestampActual() {
  if (!horaValida()) return 0;
  return (uint32_t)time(nullptr);
}

void timestampAFechaHora(uint32_t timestamp, char* buffer, size_t bufferSize) {
  if (timestamp == 0) {
    strncpy(buffer, "HORA_NO_DISPONIBLE", bufferSize - 1);
    buffer[bufferSize - 1] = '\0';
    return;
  }

  time_t t = (time_t)timestamp;
  struct tm timeinfo;
  localtime_r(&t, &timeinfo);
  strftime(buffer, bufferSize, "%Y-%m-%d %H:%M:%S", &timeinfo);
}

bool cargarMetaFlash() {
  if (!LittleFS.exists(ARCHIVO_META)) {
    metaFlash = {META_MAGIC, META_VERSION, 0, 0};
    return true;
  }

  File f = LittleFS.open(ARCHIVO_META, FILE_READ);
  if (!f) return false;

  MetaBufferFlash leida;
  size_t n = f.read(reinterpret_cast<uint8_t*>(&leida), sizeof(leida));
  f.close();

  if (n != sizeof(leida) ||
      leida.magic != META_MAGIC ||
      leida.version != META_VERSION ||
      leida.inicio >= MAX_PENDIENTES_FLASH ||
      leida.cantidad > MAX_PENDIENTES_FLASH) {
    Serial.println("ADVERTENCIA: metadata de buffer invalida. Se reinicia la cola Flash.");
    metaFlash = {META_MAGIC, META_VERSION, 0, 0};
    return guardarMetaFlash();
  }

  metaFlash = leida;
  return true;
}

bool guardarMetaFlash() {
  File f = LittleFS.open(ARCHIVO_META, FILE_WRITE);
  if (!f) {
    Serial.println("ERROR: no se pudo guardar metadata del buffer Flash.");
    return false;
  }

  size_t n = f.write(reinterpret_cast<const uint8_t*>(&metaFlash), sizeof(metaFlash));
  f.flush();
  f.close();
  return n == sizeof(metaFlash);
}

bool escribirRegistroFlashEnArchivo(File& f, uint32_t indice, const LecturaPendiente& lectura) {
  if (indice >= MAX_PENDIENTES_FLASH) return false;

  uint32_t offset = indice * sizeof(LecturaPendiente);
  if (!f.seek(offset, SeekSet)) {
    Serial.println("ERROR: seek de escritura en buffer Flash.");
    return false;
  }

  size_t n = f.write(reinterpret_cast<const uint8_t*>(&lectura), sizeof(lectura));
  return n == sizeof(lectura);
}

bool leerRegistroFlash(uint32_t indice, LecturaPendiente& lectura) {
  if (indice >= MAX_PENDIENTES_FLASH || !LittleFS.exists(ARCHIVO_RING)) return false;

  File f = LittleFS.open(ARCHIVO_RING, FILE_READ);
  if (!f) return false;

  uint32_t offset = indice * sizeof(LecturaPendiente);
  if (!f.seek(offset, SeekSet)) {
    f.close();
    return false;
  }

  size_t n = f.read(reinterpret_cast<uint8_t*>(&lectura), sizeof(lectura));
  f.close();
  return n == sizeof(lectura);
}

size_t contarPendientesFlash() {
  return metaFlash.cantidad;
}

uint32_t capacidadBufferRegistros() {
  // bufferDias representa dias reales de respaldo al intervalo ACTUAL.
  // Se limita fisicamente a 10.080 registros.
  uint64_t registros = ((uint64_t)bufferDias * 24ULL * 60ULL * 60ULL * 1000ULL
                        + intervaloLecturaMs - 1ULL) / intervaloLecturaMs;
  if (registros < 1) registros = 1;
  if (registros > MAX_PENDIENTES_FLASH) registros = MAX_PENDIENTES_FLASH;
  return (uint32_t)registros;
}

void ajustarBufferALimite() {
  uint32_t limite = capacidadBufferRegistros();
  if (metaFlash.cantidad <= limite) return;

  uint32_t eliminar = metaFlash.cantidad - limite;
  metaFlash.inicio = (metaFlash.inicio + eliminar) % MAX_PENDIENTES_FLASH;
  metaFlash.cantidad = limite;
  guardarMetaFlash();

  Serial.print("Buffer ajustado al nuevo limite. Registros descartados mas antiguos: ");
  Serial.println(eliminar);
}

bool urlHttpsValida(const String& url) {
  return url.startsWith("https://") && url.length() >= 12 && url.length() <= 300;
}


void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("======================================");
  Serial.println("ENPI CLIENTE V4 - BUFFER + CONFIG REMOTA + OTA");
  Serial.print("Firmware: ");
  Serial.println(FW_VERSION);
  Serial.println("======================================");

  pinMode(BOTON_RESET_CONFIG, INPUT_PULLUP);

  if (!LittleFS.begin(true)) {
    Serial.println("ADVERTENCIA: No se pudo montar LittleFS. El buffer persistente no estara disponible.");
  } else {
    Serial.println("LittleFS iniciado correctamente.");
    cargarMetaFlash();
    Serial.print("Pendientes persistidos al arrancar: ");
    Serial.println(contarPendientesFlash());
    Serial.print("Capacidad buffer Flash (registros): ");
    Serial.println(MAX_PENDIENTES_FLASH);
  }

  pzemSerial.begin(9600, SERIAL_8N1, PZEM_RX_PIN, PZEM_TX_PIN);

  cargarConfiguracion();

  if (digitalRead(BOTON_RESET_CONFIG) == LOW) {
    Serial.println("Boton BOOT presionado. Borrando configuracion...");
    borrarConfiguracion();
    delay(1000);
    ESP.restart();
  }

  if (wifiSsid.length() == 0 || codigoMedidor.length() == 0) {
    iniciarPortalConfiguracion();
    return;
  }

  conectarWiFi();

  // Si ya existe configuracion y la red no esta disponible al arrancar,
  // NO entramos al portal: ENPI sigue midiendo y guardando offline.
  // Para cambiar credenciales se puede mantener BOOT al reiniciar.
  if (WiFi.status() == WL_CONNECTED) {
    sincronizarHora();
    iniciarServidorWebNormal();
  } else {
    modoAP = false;
    Serial.println("Arranque offline: ENPI seguira midiendo y acumulando pendientes.");
  }

  ultimaLectura = millis() - intervaloLecturaMs;
}

void loop() {
  if (modoAP) {
    dnsServer.processNextRequest();
    server.handleClient();
    delay(10);
    return;
  }

  server.handleClient();
  verificarWiFi();

  unsigned long ahora = millis();
  bool wifiConectado = (WiFi.status() == WL_CONNECTED);

  // Detecta una reconexion real y espera unos segundos antes de vaciar cola.
  if (wifiConectado && !wifiEstabaConectado) {
    wifiConectadoDesde = ahora;
    Serial.println("WiFi recuperado. Se programara reenvio de pendientes.");

    // Si el equipo arranco sin Internet, sincroniza el reloj al recuperar red.
    if (time(nullptr) < 100000) {
      sincronizarHora();
    }

    // Si el servidor web no pudo iniciarse al arranque offline, lo iniciamos ahora.
    iniciarServidorWebNormal();
  }
  wifiEstabaConectado = wifiConectado;

  // IMPORTANTE: medir SIEMPRE, haya o no Internet.
  if (ahora - ultimaLectura >= intervaloLecturaMs) {
    ultimaLectura = ahora;
    tomarYEnviarLectura();
  }

  // Si hay datos en RAM durante una caida larga, checkpoint configurable.
  if (bufferHabilitado &&
      cantidadPendientesRAM > 0 &&
      ahora - ultimoFlushFlash >= checkpointFlashMs) {
    volcarPendientesRamAFlash();
  }

  // Al recuperar Internet, primero se envia la cola antigua de Flash y luego RAM.
  // Esperamos 15 s de WiFi estable y no repetimos mas de una vez por minuto.
  if (envioHabilitado &&
      wifiConectado &&
      ahora - wifiConectadoDesde >= 15000UL &&
      ahora - ultimoReintentoPendientes >= REINTENTO_PENDIENTES_MS) {
    ultimoReintentoPendientes = ahora;
    procesarPendientes();
  }

  // Configuracion remota: primera consulta a los 2 min y luego segun parametro remoto.
  if (wifiConectado && !actualizacionOTAEnProceso) {
    if (!consultaInicialConfigRealizada && ahora >= ESPERA_INICIAL_CONFIG_MS) {
      consultaInicialConfigRealizada = true;
      ultimaConsultaConfigRemota = ahora;
      revisarConfiguracionRemota();
    } else if (consultaInicialConfigRealizada &&
               ahora - ultimaConsultaConfigRemota >= intervaloConsultaRemotaMs) {
      ultimaConsultaConfigRemota = ahora;
      revisarConfiguracionRemota();
    }
  }

  delay(100);
}

void cargarConfiguracion() {
  prefs.begin("enpi", true);

  empresa = prefs.getString("empresa", "");
  codigoMedidor = prefs.getString("codigo", "");
  wifiSsid = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");
  intervaloLecturaMs = prefs.getULong("intervalo", 60000);

  apiMedicionesUrl = prefs.getString("api_url", DEFAULT_API_URL);
  configRemotaUrl = prefs.getString("cfg_url", BOOTSTRAP_CONFIG_URL);
  intervaloConsultaRemotaMs = prefs.getULong("cfg_check", 6UL * 60UL * 60UL * 1000UL);
  checkpointFlashMs = prefs.getULong("checkpoint", 60UL * 60UL * 1000UL);
  bufferDias = prefs.getUChar("buf_dias", 7);
  bufferHabilitado = prefs.getBool("buf_on", true);
  envioHabilitado = prefs.getBool("envio_on", true);
  modoDiagnostico = prefs.getBool("diag", false);
  revisionConfig = prefs.getULong("cfg_rev", 0);
  calibVoltaje = prefs.getFloat("cal_v", 1.0f);
  calibCorriente = prefs.getFloat("cal_i", 1.0f);
  calibPotencia = prefs.getFloat("cal_p", 1.0f);
  ultimoComandoProcesado = prefs.getString("ultimo_cmd", "");

  prefs.end();

  if (intervaloLecturaMs < 1000) {
    intervaloLecturaMs = 1000;
  }

  Serial.println("Configuracion cargada:");
  Serial.print("Empresa: ");
  Serial.println(empresa);
  Serial.print("Dispositivo: ");
  Serial.println(codigoMedidor);
  Serial.print("WiFi SSID: ");
  Serial.println(wifiSsid);
  if (intervaloConsultaRemotaMs < 300000UL) intervaloConsultaRemotaMs = 300000UL;
  if (checkpointFlashMs < 300000UL) checkpointFlashMs = 300000UL;
  if (bufferDias < 1) bufferDias = 1;
  if (bufferDias > 7) bufferDias = 7;
  if (!urlHttpsValida(apiMedicionesUrl)) apiMedicionesUrl = DEFAULT_API_URL;
  if (!urlHttpsValida(configRemotaUrl)) configRemotaUrl = BOOTSTRAP_CONFIG_URL;

  Serial.print("Intervalo lectura ms: ");
  Serial.println(intervaloLecturaMs);
  Serial.print("API mediciones: ");
  Serial.println(apiMedicionesUrl);
  Serial.print("Config remota: ");
  Serial.println(configRemotaUrl);
  Serial.print("Buffer dias: ");
  Serial.println(bufferDias);
  Serial.print("Checkpoint ms: ");
  Serial.println(checkpointFlashMs);
  Serial.print("Calibracion V/I/P: ");
  Serial.print(calibVoltaje, 6);
  Serial.print(" / ");
  Serial.print(calibCorriente, 6);
  Serial.print(" / ");
  Serial.println(calibPotencia, 6);
}

void guardarConfiguracion(String nuevaEmpresa, String nuevoCodigo, String nuevoSsid, String nuevoPass) {
  prefs.begin("enpi", false);

  prefs.putString("empresa", nuevaEmpresa);
  prefs.putString("codigo", nuevoCodigo);
  prefs.putString("ssid", nuevoSsid);
  prefs.putString("pass", nuevoPass);
  prefs.putULong("intervalo", intervaloLecturaMs);

  prefs.end();
}

void guardarIntervalo(unsigned long nuevoIntervaloMs) {
  if (nuevoIntervaloMs < 1000) {
    nuevoIntervaloMs = 1000;
  }

  intervaloLecturaMs = nuevoIntervaloMs;

  prefs.begin("enpi", false);
  prefs.putULong("intervalo", intervaloLecturaMs);
  prefs.end();

  ultimaLectura = millis() - intervaloLecturaMs;
}

void borrarConfiguracion() {
  prefs.begin("enpi", false);
  prefs.clear();
  prefs.end();
}

void iniciarPortalConfiguracion() {
  modoAP = true;

  Serial.println();
  Serial.println("======================================");
  Serial.println("MODO CONFIGURACION ENPI");
  Serial.println("======================================");
  Serial.print("Red WiFi creada: ");
  Serial.println(AP_SSID);
  Serial.print("Clave: ");
  Serial.println(AP_PASS);
  Serial.println("Abrir navegador en: http://192.168.4.1");
  Serial.println();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  IPAddress ip = WiFi.softAPIP();

  dnsServer.start(53, "*", ip);

  configurarRutasWeb();
  server.begin();
}

void iniciarServidorWebNormal() {
  modoAP = false;
  configurarRutasWeb();
  server.begin();

  Serial.println("Servidor web interno iniciado.");
  Serial.print("Abrir panel en: http://");
  Serial.println(WiFi.localIP());
}

void configurarRutasWeb() {
  server.on("/", HTTP_GET, mostrarPanel);
  server.on("/save", HTTP_POST, guardarDesdeFormulario);
  server.on("/admin", HTTP_GET, mostrarAdmin);
  server.on("/admin_intervalo", HTTP_POST, guardarIntervaloAdmin);
  server.on("/admin_reset_medidor", HTTP_POST, resetMedidorAdmin);
  server.on("/borrar_config", HTTP_POST, borrarConfigAdmin);
  server.onNotFound(mostrarPanel);
}

void mostrarPanel() {
  String html = "";
  html += "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>ENPI</title>";
  html += estiloHtml();
  html += "</head><body><div class='box'>";
  html += "<h1>ENPI</h1>";

  if (modoAP) {
    html += "<p>Configuracion inicial del dispositivo.</p>";
  } else {
    html += "<p>Dispositivo conectado y operando.</p>";
    html += "<div class='card'><b>Empresa:</b> " + htmlEscape(empresa) + "<br>";
    html += "<b>Dispositivo:</b> " + htmlEscape(codigoMedidor) + "<br>";
    html += "<b>WiFi:</b> " + htmlEscape(WiFi.SSID()) + "<br>";
    html += "<b>IP:</b> " + WiFi.localIP().toString() + "<br>";
    html += "<b>RSSI:</b> " + String(WiFi.RSSI()) + " dBm<br>";
    html += "<b>Intervalo:</b> " + String(intervaloLecturaMs) + " ms</div>";
  }

  html += "<form method='POST' action='/save'>";
  html += "<label>Nombre empresa</label>";
  html += "<input name='empresa' value='" + htmlEscape(empresa) + "' placeholder='Ej: Pasteleria Amelie' required>";

  html += "<label>Nombre dispositivo / codigo medidor</label>";
  html += "<input name='codigo' value='" + htmlEscape(codigoMedidor) + "' placeholder='Ej: Amelie_1' required>";

  html += "<label>Nombre red WiFi</label>";
  html += "<input name='ssid' value='" + htmlEscape(wifiSsid) + "' placeholder='SSID WiFi' required>";

  html += "<label>Contrasena WiFi</label>";
  html += "<input name='pass' type='password' value='" + htmlEscape(wifiPass) + "' placeholder='Clave WiFi'>";

  html += "<button type='submit'>Guardar y reiniciar ENPI</button>";
  html += "</form>";

  html += "<a class='link' href='/admin'>Panel administrador</a>";
  html += "</div></body></html>";

  server.send(200, "text/html", html);
}

void guardarDesdeFormulario() {
  String nuevaEmpresa = server.arg("empresa");
  String nuevoCodigo = server.arg("codigo");
  String nuevoSsid = server.arg("ssid");
  String nuevoPass = server.arg("pass");

  nuevaEmpresa.trim();
  nuevoCodigo.trim();
  nuevoSsid.trim();

  if (nuevaEmpresa.length() == 0 || nuevoCodigo.length() == 0 || nuevoSsid.length() == 0) {
    server.send(400, "text/plain", "Faltan datos obligatorios.");
    return;
  }

  guardarConfiguracion(nuevaEmpresa, nuevoCodigo, nuevoSsid, nuevoPass);

  server.send(200, "text/html", paginaMensaje("Configuracion guardada", "ENPI se reiniciara en unos segundos."));
  delay(2000);
  ESP.restart();
}

void mostrarAdmin() {
  String html = "";
  html += "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Admin ENPI</title>";
  html += estiloHtml();
  html += "</head><body><div class='box'>";
  html += "<h1>Admin ENPI</h1>";
  html += "<p>Funciones protegidas para instalador/administrador.</p>";

  html += "<div class='card'>";
  html += "<b>Intervalo actual:</b> " + String(intervaloLecturaMs) + " ms<br>";
  html += "<b>Pendientes RAM:</b> " + String(cantidadPendientesRAM) + "<br>";
  html += "<b>Pendientes Flash:</b> " + String(contarPendientesFlash()) + " / " + String(MAX_PENDIENTES_FLASH) + "<br>";
  html += "<b>Buffer configurado:</b> " + String(bufferDias) + " dias (" + String(capacidadBufferRegistros()) + " registros max.)<br>";
  html += "<b>API:</b> " + htmlEscape(apiMedicionesUrl) + "<br>";
  html += "<b>Config remota:</b> " + htmlEscape(configRemotaUrl) + "<br>";
  html += "<b>Envio habilitado:</b> " + String(envioHabilitado ? "SI" : "NO") + "<br>";
  html += "<b>Buffer habilitado:</b> " + String(bufferHabilitado ? "SI" : "NO") + "<br>";
  html += "<b>Diagnostico:</b> " + String(modoDiagnostico ? "SI" : "NO") + "<br>";
  html += "<b>Revision config:</b> " + String(revisionConfig) + "<br>";
  html += "<b>Firmware:</b> " + String(FW_VERSION);
  html += "</div>";

  html += "<form method='POST' action='/admin_intervalo'>";
  html += "<label>Clave administrador</label>";
  html += "<input name='clave' type='password' placeholder='Clave admin' required>";
  html += "<label>Intervalo lectura/envio en ms</label>";
  html += "<input name='intervalo' type='number' min='1000' step='1000' value='" + String(intervaloLecturaMs) + "' required>";
  html += "<button type='submit'>Guardar intervalo</button>";
  html += "</form>";

  html += "<form method='POST' action='/admin_reset_medidor'>";
  html += "<label>Clave administrador</label>";
  html += "<input name='clave' type='password' placeholder='Clave admin' required>";
  html += "<button class='danger' type='submit'>Reiniciar energia acumulada del medidor</button>";
  html += "</form>";

  html += "<form method='POST' action='/borrar_config'>";
  html += "<label>Clave administrador</label>";
  html += "<input name='clave' type='password' placeholder='Clave admin' required>";
  html += "<button class='danger' type='submit'>Borrar configuracion WiFi/cliente</button>";
  html += "</form>";

  html += "<a class='link' href='/'>Volver</a>";
  html += "</div></body></html>";

  server.send(200, "text/html", html);
}

bool claveAdminCorrecta() {
  return server.arg("clave") == String(ADMIN_PASS);
}

void guardarIntervaloAdmin() {
  if (!claveAdminCorrecta()) {
    server.send(403, "text/html", paginaMensaje("Clave incorrecta", "No autorizado."));
    return;
  }

  unsigned long nuevoIntervalo = server.arg("intervalo").toInt();

  if (nuevoIntervalo < 1000) {
    server.send(400, "text/html", paginaMensaje("Intervalo invalido", "El minimo permitido es 1000 ms."));
    return;
  }

  guardarIntervalo(nuevoIntervalo);

  server.send(200, "text/html", paginaMensaje("Intervalo guardado", "Nuevo intervalo: " + String(intervaloLecturaMs) + " ms."));
}

void resetMedidorAdmin() {
  if (!claveAdminCorrecta()) {
    server.send(403, "text/html", paginaMensaje("Clave incorrecta", "No autorizado."));
    return;
  }

  bool ok = pzem.resetEnergy();

  if (ok) {
    server.send(200, "text/html", paginaMensaje("Medidor reiniciado", "Energia acumulada del PZEM reiniciada correctamente."));
  } else {
    server.send(500, "text/html", paginaMensaje("Error", "No se pudo reiniciar energia acumulada del PZEM."));
  }
}

void borrarConfigAdmin() {
  if (!claveAdminCorrecta()) {
    server.send(403, "text/html", paginaMensaje("Clave incorrecta", "No autorizado."));
    return;
  }

  borrarConfiguracion();

  server.send(200, "text/html", paginaMensaje("Configuracion borrada", "ENPI se reiniciara en modo configuracion."));
  delay(2000);
  ESP.restart();
}

void conectarWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

  Serial.print("Conectando a WiFi: ");
  Serial.println(wifiSsid);

  unsigned long inicio = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 30000) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi conectado correctamente");
    Serial.print("SSID: ");
    Serial.println(WiFi.SSID());
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("Gateway: ");
    Serial.println(WiFi.gatewayIP());
    Serial.print("DNS: ");
    Serial.println(WiFi.dnsIP());
    Serial.print("RSSI: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.println("ERROR: No se pudo conectar a WiFi.");
  }
}

void verificarWiFi() {
  static unsigned long ultimoIntento = 0;

  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  if (millis() - ultimoIntento < 15000) {
    return;
  }

  ultimoIntento = millis();

  Serial.println("WiFi desconectado. Reintentando...");
  WiFi.disconnect();
  delay(500);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
}

void sincronizarHora() {
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");

  Serial.print("Sincronizando hora NTP");

  unsigned long inicio = millis();

  while (time(nullptr) < 100000 && millis() - inicio < 30000) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (time(nullptr) >= 100000) {
    Serial.println("Hora sincronizada correctamente");
    Serial.print("Hora actual: ");
    imprimirTiempo(time(nullptr));
  } else {
    Serial.println("ADVERTENCIA: No se pudo sincronizar hora NTP");
  }
}

void tomarYEnviarLectura() {
  // El timestamp se toma INMEDIATAMENTE al comenzar la medicion.
  uint32_t timestampMedicion = obtenerTimestampActual();
  char fecha_hora[24];
  timestampAFechaHora(timestampMedicion, fecha_hora, sizeof(fecha_hora));

  unsigned long inicioMedicion = millis();

  float voltage   = pzem.voltage();
  float current   = pzem.current();
  float power     = pzem.power();
  float energy    = pzem.energy();
  float frequency = pzem.frequency();
  float pf        = pzem.pf();

  // Calibracion remota. Se aplica en el instante de la medicion para que
  // una lectura almacenada conserve los valores corregidos de ese momento.
  if (!isnan(voltage)) voltage *= calibVoltaje;
  if (!isnan(current)) current *= calibCorriente;
  if (!isnan(power)) power *= calibPotencia;

  unsigned long duracion_medicion_ms = millis() - inicioMedicion;

  Serial.println();
  Serial.println("---- MEDICION ----");
  Serial.print("Empresa: ");
  Serial.println(empresa);
  Serial.print("Dispositivo: ");
  Serial.println(codigoMedidor);
  Serial.print("Intervalo ms: ");
  Serial.println(intervaloLecturaMs);
  Serial.print("Fecha/hora ORIGINAL: ");
  Serial.println(fecha_hora);
  Serial.print("Timestamp Unix: ");
  Serial.println(timestampMedicion);
  Serial.print("Voltaje: ");
  Serial.println(voltage, 2);
  Serial.print("Corriente: ");
  Serial.println(current, 3);
  Serial.print("Potencia W: ");
  Serial.println(power, 2);
  Serial.print("Energia kWh: ");
  Serial.println(energy, 6);
  Serial.print("Frecuencia Hz: ");
  Serial.println(frequency, 2);
  Serial.print("Factor potencia: ");
  Serial.println(pf, 3);

  if (isnan(voltage) || isnan(current) || isnan(power)) {
    Serial.println("ERROR: Lectura invalida del PZEM. No se envia ni almacena.");
    return;
  }

  LecturaPendiente lectura;
  memset(&lectura, 0, sizeof(lectura));
  lectura.timestamp = timestampMedicion;
  lectura.voltage = voltage;
  lectura.current = current;
  lectura.power = power;
  lectura.energy = energy;
  lectura.frequency = frequency;
  lectura.pf = pf;
  lectura.duracion_medicion_ms = duracion_medicion_ms;
  lectura.intervalo_ms = intervaloLecturaMs;

  if (!enviarLectura(lectura)) {
    if (bufferHabilitado) {
      agregarPendiente(lectura);
    } else {
      Serial.println("ADVERTENCIA: envio fallo y buffer remoto esta deshabilitado; lectura descartada.");
    }
  }
}

bool enviarLectura(const LecturaPendiente& lectura) {
  if (!envioHabilitado) {
    Serial.println("Envio remoto deshabilitado: lectura se conserva localmente.");
    return false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Sin WiFi: lectura pasa al buffer local.");
    return false;
  }

  char fecha_hora[24];
  timestampAFechaHora(lectura.timestamp, fecha_hora, sizeof(fecha_hora));

  // No enviar una lectura con hora desconocida como si fuera valida.
  // Se conserva en el buffer hasta que el sistema pueda resolverla o se
  // incorpore un RTC con bateria para arranques totalmente offline.
  if (lectura.timestamp == 0) {
    Serial.println("Pendiente sin hora valida: se conserva localmente.");
    return false;
  }

  String url = apiMedicionesUrl +
    "?codigo_medidor=" + urlencode(codigoMedidor) +
    "&empresa=" + urlencode(empresa) +
    "&fecha_hora=" + urlencode(String(fecha_hora)) +
    "&voltaje=" + String(lectura.voltage, 2) +
    "&corriente=" + String(lectura.current, 3) +
    "&potencia_kw=" + String(lectura.power / 1000.0, 4) +
    "&energia_kwh=" + String(lectura.energy, 6) +
    "&frecuencia=" + String(lectura.frequency, 2) +
    "&factor_potencia=" + String(lectura.pf, 3) +
    "&duracion_medicion_ms=" + String(lectura.duracion_medicion_ms) +
    "&intervalo_ms=" + String(lectura.intervalo_ms);

  Serial.println("---- ENVIO SERVIDOR ----");
  Serial.print("Fecha: ");
  Serial.println(fecha_hora);

  HTTPClient http;
  http.setTimeout(10000);
  http.begin(url);

  unsigned long inicioEnvio = millis();
  int httpCode = http.GET();
  unsigned long demoraEnvio = millis() - inicioEnvio;

  Serial.print("HTTP Code: ");
  Serial.println(httpCode);
  Serial.print("Demora envio ms: ");
  Serial.println(demoraEnvio);

  bool ok = false;

  if (httpCode >= 200 && httpCode < 300) {
    String respuesta = http.getString();
    Serial.println("Respuesta servidor:");
    Serial.println(respuesta);
    ok = true;
  } else if (httpCode > 0) {
    Serial.print("Servidor rechazo envio. HTTP: ");
    Serial.println(httpCode);
  } else {
    Serial.print("ERROR HTTP: ");
    Serial.println(http.errorToString(httpCode));
  }

  http.end();
  return ok;
}

void agregarPendiente(const LecturaPendiente& lectura) {
  if (cantidadPendientesRAM >= MAX_PENDIENTES_RAM) {
    Serial.println("RAM de pendientes llena. Volcando bloque a Flash...");
    volcarPendientesRamAFlash();
  }

  if (cantidadPendientesRAM < MAX_PENDIENTES_RAM) {
    pendientesRAM[cantidadPendientesRAM++] = lectura;
    Serial.print("Lectura guardada en RAM. Pendientes RAM: ");
    Serial.println(cantidadPendientesRAM);
  } else {
    Serial.println("ERROR CRITICO: buffer RAM lleno y no fue posible persistirlo.");
  }
}

bool volcarPendientesRamAFlash() {
  if (cantidadPendientesRAM == 0) {
    ultimoFlushFlash = millis();
    return true;
  }

  // Abrir UNA SOLA VEZ y hacer UN SOLO flush al final del bloque.
  // Esta es la parte importante para reducir desgaste de Flash.
  File f = LittleFS.open(ARCHIVO_RING, LittleFS.exists(ARCHIVO_RING) ? "r+" : "w+");
  if (!f) {
    Serial.println("ERROR: no se pudo abrir buffer circular Flash para checkpoint.");
    return false;
  }

  size_t escritos = 0;
  uint32_t inicioOriginal = metaFlash.inicio;
  uint32_t cantidadOriginal = metaFlash.cantidad;

  uint32_t limiteBuffer = capacidadBufferRegistros();

  for (size_t i = 0; i < cantidadPendientesRAM; i++) {
    uint32_t indiceEscritura;

    if (metaFlash.cantidad < limiteBuffer) {
      indiceEscritura = (metaFlash.inicio + metaFlash.cantidad) % MAX_PENDIENTES_FLASH;
      if (!escribirRegistroFlashEnArchivo(f, indiceEscritura, pendientesRAM[i])) break;
      metaFlash.cantidad++;
    } else {
      // Buffer lleno: reemplazar el mas antiguo sin agrandar el archivo.
      indiceEscritura = metaFlash.inicio;
      if (!escribirRegistroFlashEnArchivo(f, indiceEscritura, pendientesRAM[i])) break;
      metaFlash.inicio = (metaFlash.inicio + 1) % MAX_PENDIENTES_FLASH;
      Serial.println("AVISO: buffer offline lleno; se reemplazo la lectura mas antigua.");
    }

    escritos++;
  }

  f.flush();      // UNA sola consolidacion del bloque
  f.close();

  if (escritos == 0) {
    metaFlash.inicio = inicioOriginal;
    metaFlash.cantidad = cantidadOriginal;
    return false;
  }

  if (!guardarMetaFlash()) {
    Serial.println("ERROR: datos escritos pero metadata no pudo persistirse.");
    return false;
  }

  // Conservar en RAM solamente lo que no alcanzo a persistirse.
  size_t restantes = cantidadPendientesRAM - escritos;
  if (restantes > 0) {
    memmove(pendientesRAM, pendientesRAM + escritos, restantes * sizeof(LecturaPendiente));
  }
  cantidadPendientesRAM = restantes;
  ultimoFlushFlash = millis();

  Serial.print("Checkpoint Flash OK. Registros escritos en UN bloque: ");
  Serial.println(escritos);
  Serial.print("Pendientes Flash actuales: ");
  Serial.println(metaFlash.cantidad);

  return restantes == 0;
}

bool eliminarMasAntiguaFlash() {
  if (metaFlash.cantidad == 0) return true;
  metaFlash.inicio = (metaFlash.inicio + 1) % MAX_PENDIENTES_FLASH;
  metaFlash.cantidad--;
  return guardarMetaFlash();
}

bool reenviarPendientesFlash() {
  if (metaFlash.cantidad == 0) return true;

  size_t enviados = 0;

  // Enviar SIEMPRE desde el registro logico mas antiguo.
  while (metaFlash.cantidad > 0 && WiFi.status() == WL_CONNECTED) {
    LecturaPendiente lectura;
    if (!leerRegistroFlash(metaFlash.inicio, lectura)) {
      Serial.println("ERROR: no se pudo leer el pendiente mas antiguo de Flash.");
      return false;
    }

    if (!enviarLectura(lectura)) {
      break;
    }

    // Avanzar solo despues de confirmacion HTTP exitosa.
    metaFlash.inicio = (metaFlash.inicio + 1) % MAX_PENDIENTES_FLASH;
    metaFlash.cantidad--;
    enviados++;

    // Persistir metadata cada 60 confirmaciones, no por cada registro.
    if (enviados % 60 == 0) {
      if (!guardarMetaFlash()) {
        Serial.println("ERROR: no se pudo guardar avance del reenvio.");
        return false;
      }
    }

    delay(150);
  }

  if (enviados > 0) {
    if (!guardarMetaFlash()) return false;
  }

  Serial.print("Pendientes Flash enviados: ");
  Serial.print(enviados);
  Serial.print(" | Restantes Flash: ");
  Serial.println(metaFlash.cantidad);

  return metaFlash.cantidad == 0;
}

bool reenviarPendientesRAM() {
  if (cantidadPendientesRAM == 0) return true;

  size_t enviados = 0;

  while (enviados < cantidadPendientesRAM) {
    if (!enviarLectura(pendientesRAM[enviados])) {
      break;
    }
    enviados++;
    delay(150);
  }

  if (enviados == 0) return false;

  size_t restantes = cantidadPendientesRAM - enviados;
  if (restantes > 0) {
    memmove(
      pendientesRAM,
      pendientesRAM + enviados,
      restantes * sizeof(LecturaPendiente)
    );
  }
  cantidadPendientesRAM = restantes;

  Serial.print("Pendientes RAM enviados: ");
  Serial.print(enviados);
  Serial.print(" | Restantes RAM: ");
  Serial.println(cantidadPendientesRAM);

  return cantidadPendientesRAM == 0;
}

void procesarPendientes() {
  if (!envioHabilitado) return;
  if (WiFi.status() != WL_CONNECTED) return;

  size_t flashAntes = contarPendientesFlash();
  size_t ramAntes = cantidadPendientesRAM;

  if (flashAntes == 0 && ramAntes == 0) return;

  Serial.println();
  Serial.println("==== REENVIO DE PENDIENTES ====");
  Serial.print("Flash: ");
  Serial.print(flashAntes);
  Serial.print(" | RAM: ");
  Serial.println(ramAntes);

  // Orden cronologico: Flash contiene lo mas antiguo.
  if (!reenviarPendientesFlash()) {
    return;
  }

  reenviarPendientesRAM();
}


// ============================================================
// CONFIGURACION REMOTA / MIGRACION DE SERVIDOR / COMANDOS / OTA
// ============================================================

void guardarParametrosRemotos() {
  prefs.begin("enpi", false);
  prefs.putString("api_url", apiMedicionesUrl);
  prefs.putString("cfg_url", configRemotaUrl);
  prefs.putULong("cfg_check", intervaloConsultaRemotaMs);
  prefs.putULong("checkpoint", checkpointFlashMs);
  prefs.putUChar("buf_dias", bufferDias);
  prefs.putBool("buf_on", bufferHabilitado);
  prefs.putBool("envio_on", envioHabilitado);
  prefs.putBool("diag", modoDiagnostico);
  prefs.putULong("cfg_rev", revisionConfig);
  prefs.putFloat("cal_v", calibVoltaje);
  prefs.putFloat("cal_i", calibCorriente);
  prefs.putFloat("cal_p", calibPotencia);
  prefs.end();
}

bool versionMayor(const String& nueva, const String& actual) {
  int n1 = 0, n2 = 0, n3 = 0;
  int a1 = 0, a2 = 0, a3 = 0;
  if (sscanf(nueva.c_str(), "%d.%d.%d", &n1, &n2, &n3) < 2) return false;
  if (sscanf(actual.c_str(), "%d.%d.%d", &a1, &a2, &a3) < 2) return false;
  if (n1 != a1) return n1 > a1;
  if (n2 != a2) return n2 > a2;
  return n3 > a3;
}

bool arregloContieneMedidor(JsonArray arr) {
  if (arr.isNull()) return false;
  for (JsonVariant v : arr) {
    if (v.as<String>() == codigoMedidor) return true;
  }
  return false;
}

bool bloqueAutorizaEquipo(JsonObject bloque, bool valorTodosPorDefecto = true) {
  if (bloque.isNull()) return valorTodosPorDefecto;
  bool todos = bloque["todos"] | valorTodosPorDefecto;
  if (todos) return true;
  return arregloContieneMedidor(bloque["medidores"].as<JsonArray>());
}

bool descargarJsonDesde(const String& url, String& contenido) {
  if (!urlHttpsValida(url) || WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure cliente;
  // IMPORTANTE: funcional para pruebas/despliegue inicial.
  // Antes de produccion masiva reemplazar por validacion CA/pinning.
  cliente.setInsecure();

  HTTPClient http;
  http.setTimeout(12000);

  if (!http.begin(cliente, url)) return false;

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    if (modoDiagnostico) {
      Serial.print("Config HTTP ");
      Serial.print(code);
      Serial.print(" desde ");
      Serial.println(url);
    }
    http.end();
    return false;
  }

  contenido = http.getString();
  http.end();
  return contenido.length() > 0;
}

bool obtenerConfiguracionJson(String& contenido) {
  // 1) URL configurable actual
  if (descargarJsonDesde(configRemotaUrl, contenido)) return true;

  // 2) Bootstrap fijo de rescate
  String bootstrap = String(BOOTSTRAP_CONFIG_URL);
  if (configRemotaUrl != bootstrap) {
    Serial.println("Config principal no disponible. Intentando bootstrap de rescate...");
    if (descargarJsonDesde(bootstrap, contenido)) return true;
  }

  return false;
}

void aplicarConfiguracionRemota(JsonObject root) {
  bool huboCambio = false;

  // Permite limitar TODO el bloque de configuracion a ciertos equipos.
  JsonObject aplicarA = root["aplicar_a"].as<JsonObject>();
  if (!aplicarA.isNull() && !bloqueAutorizaEquipo(aplicarA, true)) {
    Serial.println("Esta configuracion no aplica a este medidor.");
    return;
  }

  uint32_t revisionEntrante = root["revision"] | revisionConfig;
  if (revisionEntrante < revisionConfig) {
    Serial.print("Configuracion ignorada por revision antigua: ");
    Serial.print(revisionEntrante);
    Serial.print(" < ");
    Serial.println(revisionConfig);
    return;
  }
  if (revisionEntrante > revisionConfig) {
    revisionConfig = revisionEntrante;
    huboCambio = true;
  }

  JsonObject medicion = root["medicion"].as<JsonObject>();
  if (!medicion.isNull()) {
    uint32_t segundos = medicion["intervalo_segundos"] | (intervaloLecturaMs / 1000UL);
    if (segundos >= 1 && segundos <= 86400UL) {
      unsigned long nuevo = segundos * 1000UL;
      if (nuevo != intervaloLecturaMs) {
        guardarIntervalo(nuevo);
        huboCambio = true;
        Serial.print("Config remota: intervalo medicion = ");
        Serial.print(segundos);
        Serial.println(" s");
      }
    }

    bool nuevoEnvio = medicion["envio_habilitado"] | envioHabilitado;
    if (nuevoEnvio != envioHabilitado) {
      envioHabilitado = nuevoEnvio;
      huboCambio = true;
    }
  }

  JsonObject buffer = root["buffer"].as<JsonObject>();
  if (!buffer.isNull()) {
    int dias = buffer["dias"] | bufferDias;
    if (dias < 1) dias = 1;
    if (dias > 7) dias = 7;
    if ((uint8_t)dias != bufferDias) {
      bufferDias = (uint8_t)dias;
      huboCambio = true;
    }

    uint32_t checkpointMin = buffer["checkpoint_minutos"] | (checkpointFlashMs / 60000UL);
    if (checkpointMin < 5) checkpointMin = 5;
    if (checkpointMin > 1440) checkpointMin = 1440;
    unsigned long nuevoCheckpoint = checkpointMin * 60000UL;
    if (nuevoCheckpoint != checkpointFlashMs) {
      checkpointFlashMs = nuevoCheckpoint;
      huboCambio = true;
    }

    bool nuevoBuffer = buffer["habilitado"] | bufferHabilitado;
    if (nuevoBuffer != bufferHabilitado) {
      bufferHabilitado = nuevoBuffer;
      huboCambio = true;
    }
  }

  JsonObject calibracion = root["calibracion"].as<JsonObject>();
  if (!calibracion.isNull()) {
    float v = calibracion["voltaje"] | calibVoltaje;
    float i = calibracion["corriente"] | calibCorriente;
    float p = calibracion["potencia"] | calibPotencia;

    // Limites defensivos para evitar una configuracion accidental absurda.
    if (v >= 0.5f && v <= 1.5f && fabs(v - calibVoltaje) > 0.000001f) {
      calibVoltaje = v; huboCambio = true;
    }
    if (i >= 0.5f && i <= 1.5f && fabs(i - calibCorriente) > 0.000001f) {
      calibCorriente = i; huboCambio = true;
    }
    if (p >= 0.5f && p <= 1.5f && fabs(p - calibPotencia) > 0.000001f) {
      calibPotencia = p; huboCambio = true;
    }
  }

  JsonObject sistema = root["sistema"].as<JsonObject>();
  if (!sistema.isNull()) {
    uint32_t minutos = sistema["consulta_config_minutos"] | (intervaloConsultaRemotaMs / 60000UL);
    if (minutos < 5) minutos = 5;
    if (minutos > 10080) minutos = 10080;
    unsigned long nuevoCheck = minutos * 60000UL;
    if (nuevoCheck != intervaloConsultaRemotaMs) {
      intervaloConsultaRemotaMs = nuevoCheck;
      huboCambio = true;
    }

    bool diag = sistema["modo_diagnostico"] | modoDiagnostico;
    if (diag != modoDiagnostico) {
      modoDiagnostico = diag;
      huboCambio = true;
    }
  }

  JsonObject servidor = root["servidor"].as<JsonObject>();
  if (!servidor.isNull()) {
    const char* apiCfg = servidor["api_mediciones"] | apiMedicionesUrl.c_str();
    const char* urlCfg = servidor["config_url"] | configRemotaUrl.c_str();
    String nuevaApi = String(apiCfg);
    String nuevaCfg = String(urlCfg);
    nuevaApi.trim();
    nuevaCfg.trim();

    if (nuevaApi != apiMedicionesUrl) {
      if (urlHttpsValida(nuevaApi)) {
        apiMedicionesUrl = nuevaApi;
        huboCambio = true;
        Serial.println("Config remota: API de mediciones actualizada.");
      } else {
        Serial.println("Config remota rechazo api_mediciones: debe ser HTTPS.");
      }
    }

    if (nuevaCfg != configRemotaUrl) {
      if (urlHttpsValida(nuevaCfg)) {
        configRemotaUrl = nuevaCfg;
        huboCambio = true;
        Serial.println("Config remota: URL de configuracion actualizada.");
      } else {
        Serial.println("Config remota rechazo config_url: debe ser HTTPS.");
      }
    }
  }

  if (huboCambio) {
    guardarParametrosRemotos();
    ajustarBufferALimite();
    Serial.println("Configuracion remota aplicada y persistida.");
  }
}

void procesarComandosRemotos(JsonObject comandos) {
  if (comandos.isNull()) return;

  // Por seguridad, los comandos deben indicar explicitamente "todos": true
  // o incluir este codigoMedidor en "medidores".
  if (!bloqueAutorizaEquipo(comandos, false)) return;

  String id = comandos["id"] | "";
  id.trim();

  // Toda orden destructiva debe tener ID unico para ejecutarse una sola vez.
  if (id.length() == 0 || id == ultimoComandoProcesado) return;

  bool resetEnergia = comandos["reset_energia"] | false;
  bool reiniciar = comandos["reiniciar"] | false;

  if (!resetEnergia && !reiniciar) return;

  // Marcar ANTES de ejecutar para evitar repeticion tras reinicio.
  prefs.begin("enpi", false);
  prefs.putString("ultimo_cmd", id);
  prefs.end();
  ultimoComandoProcesado = id;

  if (resetEnergia) {
    bool ok = pzem.resetEnergy();
    Serial.println(ok ? "COMANDO REMOTO: energia PZEM reiniciada." :
                        "COMANDO REMOTO: fallo al reiniciar energia PZEM.");
  }

  if (reiniciar) {
    Serial.println("COMANDO REMOTO: reinicio solicitado.");
    if (cantidadPendientesRAM > 0 && bufferHabilitado) {
      volcarPendientesRamAFlash();
    }
    delay(1000);
    ESP.restart();
  }
}

bool medidorAutorizadoOTA(JsonObject ota) {
  if (ota.isNull()) return false;
  bool todos = ota["todos"] | false;
  if (todos) return true;
  return arregloContieneMedidor(ota["medidores"].as<JsonArray>());
}

bool ejecutarActualizacionOTA(const String& firmwareUrl, const String& nuevaVersion) {
  if (!urlHttpsValida(firmwareUrl) || WiFi.status() != WL_CONNECTED) return false;

  if (cantidadPendientesRAM > 0 && bufferHabilitado) {
    Serial.println("OTA: protegiendo pendientes RAM en Flash...");
    if (!volcarPendientesRamAFlash()) {
      Serial.println("OTA cancelada: no fue posible persistir RAM.");
      return false;
    }
  }

  Serial.println();
  Serial.println("======================================");
  Serial.println("ACTUALIZACION OTA ENPI");
  Serial.print("Version actual: ");
  Serial.println(FW_VERSION);
  Serial.print("Nueva version: ");
  Serial.println(nuevaVersion);
  Serial.println("======================================");

  actualizacionOTAEnProceso = true;

  WiFiClientSecure cliente;
  // TODO PRODUCCION: reemplazar setInsecure por certificado CA/pinning.
  cliente.setInsecure();

  httpUpdate.rebootOnUpdate(true);

  t_httpUpdate_return resultado = httpUpdate.update(cliente, firmwareUrl);

  if (resultado == HTTP_UPDATE_FAILED) {
    Serial.print("OTA fallo: ");
    Serial.println(httpUpdate.getLastErrorString());
    actualizacionOTAEnProceso = false;
    return false;
  }

  if (resultado == HTTP_UPDATE_NO_UPDATES) {
    Serial.println("OTA: servidor indica sin actualizacion.");
    actualizacionOTAEnProceso = false;
    return false;
  }

  // HTTP_UPDATE_OK normalmente reinicia automaticamente.
  Serial.println("OTA completada.");
  return true;
}

void procesarOTA(JsonObject ota) {
  if (ota.isNull()) return;
  if (!(ota["disponible"] | false)) return;
  if (!medidorAutorizadoOTA(ota)) return;

  String version = ota["version"] | "";
  String url = ota["url"] | "";
  version.trim();
  url.trim();

  if (version.length() == 0 || !versionMayor(version, FW_VERSION)) return;
  if (!urlHttpsValida(url)) {
    Serial.println("OTA rechazada: URL no HTTPS o invalida.");
    return;
  }

  ejecutarActualizacionOTA(url, version);
}

void revisarConfiguracionRemota() {
  if (WiFi.status() != WL_CONNECTED || actualizacionOTAEnProceso) return;

  Serial.println();
  Serial.println("==== REVISION CONFIG REMOTA ====");

  String contenido;
  if (!obtenerConfiguracionJson(contenido)) {
    Serial.println("No se pudo obtener config remota.");
    return;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, contenido);
  if (error) {
    Serial.print("JSON invalido: ");
    Serial.println(error.c_str());
    return;
  }

  JsonObject root = doc.as<JsonObject>();

  JsonObject aplicarA = root["aplicar_a"].as<JsonObject>();
  if (!aplicarA.isNull() && !bloqueAutorizaEquipo(aplicarA, true)) {
    Serial.println("Documento remoto no aplica a este ENPI.");
    return;
  }

  // 1) Aplicar parametros y eventual migracion de servidor.
  aplicarConfiguracionRemota(root);

  // 2) Ejecutar comandos idempotentes.
  procesarComandosRemotos(root["comandos"].as<JsonObject>());

  // 3) OTA al final, porque puede reiniciar el dispositivo.
  procesarOTA(root["ota"].as<JsonObject>());
}


void imprimirTiempo(time_t tiempo) {
  struct tm timeinfo;
  localtime_r(&tiempo, &timeinfo);

  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);

  Serial.println(buffer);
}

String urlencode(String str) {
  String encoded = "";

  for (int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);

    if (isalnum(c)) {
      encoded += c;
    } else if (c == ' ') {
      encoded += "%20";
    } else {
      char buf[4];
      sprintf(buf, "%%%02X", (unsigned char)c);
      encoded += buf;
    }
  }

  return encoded;
}

String htmlEscape(String text) {
  text.replace("&", "&amp;");
  text.replace("<", "&lt;");
  text.replace(">", "&gt;");
  text.replace("\"", "&quot;");
  text.replace("'", "&#39;");
  return text;
}

String estiloHtml() {
  String css = "";
  css += "<style>";
  css += "body{font-family:Arial;margin:0;background:#f4f6f8;color:#111}";
  css += ".box{max-width:520px;margin:30px auto;background:white;padding:24px;border-radius:10px;box-shadow:0 4px 18px #0002}";
  css += "h1{font-size:24px;margin:0 0 8px}";
  css += "p{color:#555}";
  css += "label{display:block;font-weight:bold;margin-top:14px}";
  css += "input{width:100%;box-sizing:border-box;padding:12px;margin-top:6px;border:1px solid #bbb;border-radius:6px;font-size:16px}";
  css += "button{width:100%;padding:14px;margin-top:22px;border:0;border-radius:6px;background:#0b5ed7;color:white;font-size:17px;font-weight:bold}";
  css += "button.danger{background:#b42318}";
  css += ".card{background:#eef3f8;padding:14px;border-radius:8px;margin:16px 0;line-height:1.7}";
  css += ".link{display:block;margin-top:18px;color:#0b5ed7;text-decoration:none;font-weight:bold}";
  css += "</style>";
  return css;
}

String paginaMensaje(String titulo, String mensaje) {
  String html = "";
  html += "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>ENPI</title>";
  html += estiloHtml();
  html += "</head><body><div class='box'>";
  html += "<h1>" + htmlEscape(titulo) + "</h1>";
  html += "<p>" + htmlEscape(mensaje) + "</p>";
  html += "<a class='link' href='/'>Volver</a>";
  html += "</div></body></html>";
  return html;
}