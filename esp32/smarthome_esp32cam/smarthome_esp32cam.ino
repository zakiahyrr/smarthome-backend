/*
 * Smart Home ESP32-CAM — PIR Trigger Mode
 *
 * Kamera HANYA aktif saat PIR mendeteksi gerakan (via MQTT).
 * Tidak streaming terus-menerus → tidak panas, tidak patah-patah.
 *
 * Alur:
 *   ESP32 utama deteksi PIR
 *     → publish smarthome/sensor/pir = "ada gerak"
 *       → ESP32-CAM subscribe, ambil foto
 *         → HTTP POST foto ke Flask /api/kamera/prediksi
 *           → Flask jalankan AI, publish smarthome/kamera/ai
 *             → ESP32 utama subscribe, kontrol Buzzer 1
 *
 * Library (Arduino Library Manager):
 *   WiFiManager  | tzapu/WiFiManager
 *   PubSubClient | knolleary/pubsubclient
 *   ESP32 Camera | built-in (ESP32 Arduino Core >= 2.x)
 */

#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include "esp_camera.h"
#include <Preferences.h>

// ============================================================
// PIN KAMERA — AI Thinker ESP32-CAM
// ============================================================
#define PWDN_GPIO_NUM    32
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM     0
#define SIOD_GPIO_NUM    26
#define SIOC_GPIO_NUM    27
#define Y9_GPIO_NUM      35
#define Y8_GPIO_NUM      34
#define Y7_GPIO_NUM      39
#define Y6_GPIO_NUM      36
#define Y5_GPIO_NUM      21
#define Y4_GPIO_NUM      19
#define Y3_GPIO_NUM      18
#define Y2_GPIO_NUM       5
#define VSYNC_GPIO_NUM   25
#define HREF_GPIO_NUM    23
#define PCLK_GPIO_NUM    22
#define LED_FLASH_PIN     4   // LED putih onboard

// ============================================================
// CONFIG
// ============================================================
#define APP_HOST          "zacky.rizkirmdan.my.id"
#define MQTT_PORT         1883  // listener demo; autentikasi dan ACL tetap wajib
#define MQTT_DEFAULT_HOST "mqtt.rizkirmdan.my.id"
#define COOLDOWN_MS       5000

// GTS Root R4 memvalidasi sertifikat dashboard yang sedang aktif.
static const char APP_TLS_ROOT_CA[] = R"EOF(
-----BEGIN CERTIFICATE-----
MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD
VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG
A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw
WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz
IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi
AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi
QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR
HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW
BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D
9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8
p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD
-----END CERTIFICATE-----
)EOF";
#define MQTT_RETRY_MS    5000

// ============================================================
// MQTT TOPICS
// ============================================================
#define T_PIR "smarthome/sensor/pir"

// OBJECTS
WiFiClient mqttClient;
WiFiClientSecure httpsClient;
PubSubClient mqtt(mqttClient);
Preferences prefs;

// ============================================================
// STATE
// ============================================================
bool          pirTrigger    = false;
unsigned long lastCapture   = 0;
unsigned long lastMqttRetry = 0;
char          serverHost[40] = MQTT_DEFAULT_HOST;
char          mqttUser[40] = "";
char          mqttPass[80] = "";
char          cameraApiToken[96] = "";

// ============================================================
// INISIALISASI KAMERA
// ============================================================
bool useJpegNative = true;  // false jika sensor tidak support JPEG (OV7670)

bool initKamera() {
    camera_config_t cfg;
    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.ledc_timer   = LEDC_TIMER_0;
    cfg.pin_d0       = Y2_GPIO_NUM;
    cfg.pin_d1       = Y3_GPIO_NUM;
    cfg.pin_d2       = Y4_GPIO_NUM;
    cfg.pin_d3       = Y5_GPIO_NUM;
    cfg.pin_d4       = Y6_GPIO_NUM;
    cfg.pin_d5       = Y7_GPIO_NUM;
    cfg.pin_d6       = Y8_GPIO_NUM;
    cfg.pin_d7       = Y9_GPIO_NUM;
    cfg.pin_xclk     = XCLK_GPIO_NUM;
    cfg.pin_pclk     = PCLK_GPIO_NUM;
    cfg.pin_vsync    = VSYNC_GPIO_NUM;
    cfg.pin_href     = HREF_GPIO_NUM;
    cfg.pin_sscb_sda = SIOD_GPIO_NUM;
    cfg.pin_sscb_scl = SIOC_GPIO_NUM;
    cfg.pin_pwdn     = PWDN_GPIO_NUM;
    cfg.pin_reset    = RESET_GPIO_NUM;
    cfg.xclk_freq_hz = 20000000;
    cfg.pixel_format = PIXFORMAT_JPEG;

    if (psramFound()) {
        cfg.frame_size   = FRAMESIZE_VGA;
        cfg.jpeg_quality = 10;
        cfg.fb_count     = 2;
    } else {
        cfg.frame_size   = FRAMESIZE_CIF;
        cfg.jpeg_quality = 12;
        cfg.fb_count     = 1;
    }

    esp_err_t err = esp_camera_init(&cfg);

    // Sensor tidak support JPEG (OV7670) → fallback ke YUV422
    if (err != ESP_OK) {
        Serial.printf("JPEG gagal (0x%x) — coba YUV422...\n", err);
        cfg.pixel_format = PIXFORMAT_YUV422;
        cfg.frame_size   = FRAMESIZE_QVGA;  // 320x240
        cfg.fb_count     = 1;
        cfg.xclk_freq_hz = 10000000;        // OV7670 lebih stabil di 10MHz
        err = esp_camera_init(&cfg);
        if (err != ESP_OK) {
            Serial.printf("Kamera gagal init: 0x%x\n", err);
            return false;
        }
        useJpegNative = false;
        Serial.println("Mode YUV422 (OV7670) — akan dikonvert ke JPEG saat capture");
    } else {
        useJpegNative = true;
        Serial.println(psramFound() ? "PSRAM — VGA JPEG" : "Tanpa PSRAM — CIF JPEG");
    }

    // Buang frame pertama (sering gelap/blur)
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);

    Serial.println("Kamera: OK");
    return true;
}

// ============================================================
// CAPTURE & KIRIM KE FLASK
// ============================================================
void captureAndSend() {
    Serial.println("[CAM] PIR trigger — ambil foto...");

    if (strlen(cameraApiToken) == 0) {
        Serial.println("[CAM] Token API belum dikonfigurasi melalui portal.");
        return;
    }

    digitalWrite(LED_FLASH_PIN, HIGH);
    delay(50);
    camera_fb_t* fb = esp_camera_fb_get();
    digitalWrite(LED_FLASH_PIN, LOW);

    if (!fb) {
        Serial.println("[CAM] Gagal ambil frame — skip");
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[CAM] WiFi putus — skip");
        esp_camera_fb_return(fb);
        return;
    }

    uint8_t* jpgBuf  = nullptr;
    size_t   jpgLen  = 0;
    bool     converted = false;

    if (useJpegNative) {
        jpgBuf = fb->buf;
        jpgLen = fb->len;
    } else {
        // Konvert YUV422 → JPEG
        if (!frame2jpg(fb, 80, &jpgBuf, &jpgLen)) {
            Serial.println("[CAM] Konversi JPEG gagal — skip");
            esp_camera_fb_return(fb);
            return;
        }
        converted = true;
    }

    Serial.printf("[CAM] Foto: %u bytes — kirim ke Flask...\n", jpgLen);

    HTTPClient http;
    String url = String("https://") + APP_HOST + "/api/kamera/prediksi";
    httpsClient.setCACert(APP_TLS_ROOT_CA);
    http.begin(httpsClient, url);
    http.addHeader("Content-Type", "image/jpeg");
    http.addHeader("X-Device-Token", cameraApiToken);

    http.setTimeout(8000);

    int code = http.POST(jpgBuf, jpgLen);

    if (converted) free(jpgBuf);
    esp_camera_fb_return(fb);

    if (code == 200) {
        Serial.printf("[CAM] Flask OK: %s\n", http.getString().c_str());
    } else {
        Serial.printf("[CAM] Flask error HTTP %d\n", code);
    }
    http.end();
}

// ============================================================
// MQTT CALLBACK
// ============================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String v = "";
    for (unsigned int i = 0; i < length; i++) v += (char)payload[i];

    if (String(topic) == T_PIR && v == "ada gerak") {
        pirTrigger = true;
        Serial.println("[MQTT] PIR: ada gerak → akan capture");
    }
}

// ============================================================
// MQTT RECONNECT — non-blocking
// ============================================================
void mqttReconnect() {
    if (mqtt.connected()) return;
    if (millis() - lastMqttRetry < MQTT_RETRY_MS) return;
    lastMqttRetry = millis();

    if (strlen(mqttUser) == 0 || strlen(mqttPass) == 0) {
        Serial.println("MQTT belum dikonfigurasi: isi username dan password melalui portal.");
        return;
    }

    String clientId = "ESP32CAM-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    Serial.printf("MQTT konek ke %s... ", serverHost);

    if (mqtt.connect(clientId.c_str(), mqttUser, mqttPass)) {
        Serial.println("OK!");
        mqtt.subscribe(T_PIR);
        Serial.println("Subscribe: " T_PIR);
    } else {
        Serial.printf("Gagal (rc=%d)\n", mqtt.state());
    }
}

// ============================================================
// SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    Serial.println("\n========================================");
    Serial.println("  Smart Home ESP32-CAM — PIR Trigger");
    Serial.println("========================================");

    pinMode(LED_FLASH_PIN, OUTPUT);
    digitalWrite(LED_FLASH_PIN, LOW);

    // Load konfigurasi perangkat dari flash
    prefs.begin("config", false);
    // Host broker mengikuti deployment saat ini; nilai lama di NVS tidak dipakai.
    String savedUser = prefs.getString("mqtt_user", "");
    savedUser.toCharArray(mqttUser, sizeof(mqttUser));
    String savedPass = prefs.getString("mqtt_pass", "");
    savedPass.toCharArray(mqttPass, sizeof(mqttPass));
    String savedToken = prefs.getString("camera_api_token", "");
    savedToken.toCharArray(cameraApiToken, sizeof(cameraApiToken));
    prefs.end();

    // Tekan BOOT dalam 3 detik setelah sketch mulai untuk reset.
    // Jangan tahan saat power-on karena GPIO 0 dapat masuk ke mode flashing.
    pinMode(0, INPUT_PULLUP);
    Serial.println("Tekan BOOT untuk reset WiFi dan konfigurasi, atau tunggu...");
    bool bootDitekan = false;
    for (int i = 0; i < 30; i++) {
        if (digitalRead(0) == LOW) { bootDitekan = true; break; }
        delay(100);
    }
    if (bootDitekan) {
        Serial.println("Reset WiFi & config...");
        WiFiManager wmReset;
        wmReset.resetSettings();
        prefs.begin("config", false);
        prefs.clear();
        prefs.end();
        memset(serverHost, 0, sizeof(serverHost));
        delay(500);
        ESP.restart();
    }

    // WiFiManager — isi WiFi, kredensial MQTT, dan token kamera melalui browser.
    WiFiManagerParameter serverParam("server_host", "Host MQTT", serverHost, sizeof(serverHost));
    WiFiManagerParameter mqttUserParam("mqtt_user", "Username MQTT", mqttUser, sizeof(mqttUser));
    WiFiManagerParameter mqttPassParam("mqtt_pass", "Password MQTT", mqttPass, sizeof(mqttPass), " type=\"password\"");
    WiFiManagerParameter cameraTokenParam("camera_api_token", "Token API Kamera", cameraApiToken, sizeof(cameraApiToken), " type=\"password\"");
    WiFiManager wm;
    wm.addParameter(&serverParam);
    wm.addParameter(&mqttUserParam);
    wm.addParameter(&mqttPassParam);
    wm.addParameter(&cameraTokenParam);
    wm.setConfigPortalTimeout(180);
    wm.setConnectTimeout(30);
    wm.setDebugOutput(false);

    Serial.println("Konek WiFi...");
    if (!wm.autoConnect("SmartHomeCam-Setup")) {
        Serial.println("WiFi gagal — restart");
        delay(1000);
        ESP.restart();
    }
    Serial.printf("WiFi OK! IP: %s\n", WiFi.localIP().toString().c_str());

    // Perangkat lama belum punya kredensial/token di NVS: buka portal untuk migrasi.
    if (strlen(mqttUser) == 0 || strlen(mqttPass) == 0 || strlen(cameraApiToken) == 0) {
        Serial.println("Isi kredensial MQTT dan token kamera di portal konfigurasi...");
        wm.startConfigPortal("SmartHomeCam-Setup");
    }

    // Simpan konfigurasi jika diisi/berubah.
    String newHost = String(serverParam.getValue());
    newHost.trim();
    if (newHost.length() == 0) newHost = MQTT_DEFAULT_HOST;
    String newUser = String(mqttUserParam.getValue());
    newUser.trim();
    String newPass = String(mqttPassParam.getValue());
    newPass.trim();
    String newToken = String(cameraTokenParam.getValue());
    newToken.trim();
    if (newHost != String(serverHost) || newUser != String(mqttUser) || newPass != String(mqttPass) || newToken != String(cameraApiToken)) {
        newHost.toCharArray(serverHost, sizeof(serverHost));
        newUser.toCharArray(mqttUser, sizeof(mqttUser));
        newPass.toCharArray(mqttPass, sizeof(mqttPass));
        newToken.toCharArray(cameraApiToken, sizeof(cameraApiToken));
        prefs.begin("config", false);
        prefs.putString("server_host", newHost);
        prefs.putString("mqtt_user", newUser);
        prefs.putString("mqtt_pass", newPass);
        prefs.putString("camera_api_token", newToken);
        prefs.end();
        Serial.printf("Konfigurasi MQTT disimpan: %s\n", serverHost);
    }

    // Init kamera
    if (!initKamera()) {
        Serial.println("FATAL: kamera gagal — restart dalam 3 detik");
        delay(3000);
        ESP.restart();
    }

    // Setup MQTT
    mqtt.setServer(serverHost, MQTT_PORT);
    mqtt.setCallback(mqttCallback);
    mqtt.setKeepAlive(60);
    mqtt.setBufferSize(256);

    Serial.println("========================================");
    Serial.printf("Siap! MQTT: %s | Cooldown: %ds\n",
        serverHost, COOLDOWN_MS / 1000);
    Serial.println("Menunggu PIR trigger via MQTT...");
    Serial.println("========================================");
}

// ============================================================
// LOOP
// ============================================================
void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi putus — restart");
        delay(1000);
        ESP.restart();
    }

    if (!mqtt.connected()) mqttReconnect();
    mqtt.loop();

    // Capture hanya jika ada PIR trigger DAN cooldown sudah lewat
    if (pirTrigger && millis() - lastCapture >= COOLDOWN_MS) {
        pirTrigger  = false;
        lastCapture = millis();
        captureAndSend();
    }
}
