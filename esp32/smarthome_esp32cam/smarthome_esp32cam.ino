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
#define FLASK_PORT       5000
#define MQTT_PORT        1883
#define COOLDOWN_MS      5000   // jeda minimum antar capture (ms)
#define MQTT_RETRY_MS    5000

// ============================================================
// MQTT TOPICS
// ============================================================
#define T_PIR   "smarthome/sensor/pir"

// ============================================================
// OBJECTS
// ============================================================
WiFiClient   espClient;
PubSubClient mqtt(espClient);
Preferences  prefs;

// ============================================================
// STATE
// ============================================================
bool          pirTrigger    = false;
unsigned long lastCapture   = 0;
unsigned long lastMqttRetry = 0;
char          serverHost[40] = "";

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
    String url = String("http://") + serverHost + ":" + FLASK_PORT + "/api/kamera/prediksi";
    http.begin(url);
    http.addHeader("Content-Type", "image/jpeg");
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

    String clientId = "ESP32CAM-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    Serial.printf("MQTT konek ke %s... ", serverHost);

    if (mqtt.connect(clientId.c_str())) {
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

    // Load server host dari flash
    prefs.begin("config", false);
    String savedHost = prefs.getString("server_host", "");
    savedHost.toCharArray(serverHost, sizeof(serverHost));
    prefs.end();

    // Tombol BOOT (GPIO 0) — tahan 3 detik saat power on untuk reset
    pinMode(0, INPUT_PULLUP);
    Serial.println("Tahan BOOT 3 detik untuk reset, atau tunggu...");
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

    // WiFiManager — isi WiFi + IP server (satu IP untuk MQTT dan Flask)
    WiFiManagerParameter serverParam("server_host", "IP Server (MQTT & Flask)", serverHost, 40);
    WiFiManager wm;
    wm.addParameter(&serverParam);
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

    // Simpan server host jika diisi/berubah
    String newHost = String(serverParam.getValue());
    newHost.trim();
    if (newHost.length() > 0 && newHost != String(serverHost)) {
        newHost.toCharArray(serverHost, sizeof(serverHost));
        prefs.begin("config", false);
        prefs.putString("server_host", newHost);
        prefs.end();
        Serial.printf("Server host disimpan: %s\n", serverHost);
    }

    if (strlen(serverHost) == 0) {
        Serial.println("IP server belum diisi — buka portal...");
        wm.startConfigPortal("SmartHomeCam-Setup");
        String h = String(serverParam.getValue()); h.trim();
        if (h.length() > 0) {
            h.toCharArray(serverHost, sizeof(serverHost));
            prefs.begin("config", false);
            prefs.putString("server_host", serverHost);
            prefs.end();
        }
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
    Serial.printf("Siap! Server: %s | Cooldown: %ds\n",
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
