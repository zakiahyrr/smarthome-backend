/*
 * Smart Home ESP32 — MQTT + WiFiManager v3.0
 *
 * Library (Arduino Library Manager):
 *   WiFiManager         | tzapu/WiFiManager
 *   PubSubClient        | knolleary/pubsubclient
 *   DHT sensor library  | Adafruit
 *   Adafruit Unified Sensor | Adafruit
 *   MFRC522             | miguelbalboa/rfid
 *   ESP32Servo          | madhephaestus/ESP32Servo
 *   ArduinoJson         | bblanchon
 */

#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <Preferences.h>

// ============================================================
// PIN — ANALOG INPUT (ADC1, aman saat WiFi aktif)
// ============================================================
#define PIN_TANAH        32
#define PIN_TEGANGAN     33
#define PIN_LDR          34   // input only
#define PIN_MQ2          35   // input only

// ============================================================
// PIN — DIGITAL INPUT
// ============================================================
#define PIN_DHT          13
#define PIN_PIR          14
#define PIN_API          36   // VP — KY-026 DO (LOW = api terdeteksi)
#define PIN_HUJAN        39   // VN — Rain sensor DO (LOW = hujan)

// ============================================================
// PIN — RFID RC522 (SPI VSPI)
// ============================================================
#define PIN_RFID_SS      5
#define PIN_RFID_RST     22
// SCK=18, MISO=19, MOSI=23 (hardware VSPI default)

// ============================================================
// PIN — OUTPUT (aktuator)
// ============================================================
#define PIN_SERVO1       25   // Servo pintu (RFID)
#define PIN_SERVO2       26   // Servo jemuran (hujan)
#define PIN_RELAY_POMPA  27   // Relay pompa — active LOW
#define PIN_RELAY_LAMPU1 4    // Relay lampu 1 — active LOW
#define PIN_RELAY_LAMPU2 15   // Relay lampu 2 — active LOW
#define PIN_KIPAS1       16   // Kipas 1 (gas MQ-2)
#define PIN_KIPAS2       17   // Kipas 2 (suhu DHT22)
#define PIN_BUZZER1      21   // Buzzer 1 (deteksi hewan AI)
#define PIN_BUZZER2      2    // Buzzer 2 (deteksi api KY-026)

#define RELAY_ON   LOW
#define RELAY_OFF  HIGH

// ============================================================
// THRESHOLD
// ============================================================
#define GAS_BAHAYA       700      // ppm — kipas 1 aktif
#define SUHU_PANAS       32.0f   // °C  — kipas 2 aktif
#define TANAH_KERING_PCT 30      // %   — pompa auto nyala
#define HUJAN_THRESHOLD  2000    // ADC — rain sensor AO, < threshold = hujan
#define VOLT_RASIO       5.0f    // faktor voltage divider
#define MQ2_WARMUP_MS    30000   // ms  — warmup sensor gas

// ============================================================
// TIMING
// ============================================================
#define INTERVAL_SENSOR    1000   // ms — publish sensor ke MQTT
#define SERVO_BUKA_DURASI  5000   // ms — pintu terbuka sebelum tutup otomatis
#define RFID_TIMEOUT       8000   // ms — timeout tunggu response whitelist
#define MQTT_RETRY_MS      5000   // ms — jeda retry koneksi MQTT
#define MQTT_PORT          1883   // listener demo; autentikasi dan ACL tetap wajib
#define MQTT_DEFAULT_HOST  "mqtt.rizkirmdan.my.id"

// ============================================================
// OBJECTS
// ============================================================
WiFiClient mqttClient;
PubSubClient mqtt(mqttClient);
DHT          dht(PIN_DHT, DHT22);
MFRC522      rfid(PIN_RFID_SS, PIN_RFID_RST);
Servo        servo1, servo2;
Preferences  prefs;

// ============================================================
// STATE AKTUATOR
// ============================================================
bool pompaNyala    = false;
bool pompaAuto     = true;
bool lampu1Nyala   = false;
bool lampu2Nyala   = false;
bool lampu1Auto    = true;
bool lampu2Auto    = true;
bool servo1Terbuka = false;
bool servo2Masuk   = false;  // jemuran sudah dalam posisi masuk
bool adaHujanState = false;  // state hujan terkini untuk servo2

#define SERVO2_PUTAR_MS  2000   // durasi putar servo jemuran (ms) — sesuaikan
enum Servo2Mode { S2_IDLE, S2_MASUK, S2_KELUAR };
Servo2Mode    servo2Mode    = S2_IDLE;
unsigned long servo2PutarAt = 0;

// ============================================================
// STATE RFID
// ============================================================
bool          rfidWaiting    = false;
unsigned long rfidWaitStart  = 0;
String        rfidResponse   = "";

// ============================================================
// TIMING
// ============================================================
unsigned long lastSensor    = 0;
unsigned long lastMqttRetry = 0;
unsigned long servo1OpenAt  = 0;
unsigned long lastRfidCheck = 0;
unsigned long wifiLostAt    = 0;
#define RFID_HEALTH_MS 20000  // cek RC522 tiap 20 detik

// ============================================================
// MQTT CONFIG (disimpan di flash/NVS, tidak pernah di-commit)
// ============================================================
char mqttHost[40] = MQTT_DEFAULT_HOST;
char mqttUser[40] = "";
char mqttPass[80] = "";

// ============================================================
// MQTT TOPICS — PUBLISH (sensor)
// ============================================================
#define T_SUHU        "smarthome/sensor/suhu"
#define T_KELEMBABAN  "smarthome/sensor/kelembaban"
#define T_GAS         "smarthome/sensor/gas"
#define T_API         "smarthome/sensor/api"
#define T_HUJAN       "smarthome/sensor/hujan"
#define T_TANAH       "smarthome/sensor/tanah"
#define T_TEGANGAN    "smarthome/sensor/tegangan"
#define T_PIR         "smarthome/sensor/pir"
#define T_RFID        "smarthome/sensor/rfid"
#define T_CAHAYA      "smarthome/sensor/cahaya"
#define T_L1_MODE     "smarthome/sensor/lampu1_mode"
#define T_L1_NYALA    "smarthome/sensor/lampu1_nyala"
#define T_L2_MODE     "smarthome/sensor/lampu2_mode"
#define T_L2_NYALA    "smarthome/sensor/lampu2_nyala"

// ============================================================
// MQTT TOPICS — SUBSCRIBE (aktuator dari dashboard)
// ============================================================
#define T_AKT_POMPA   "smarthome/aktuator/pompa"
#define T_AKT_LAMPU1  "smarthome/aktuator/lampu1"
#define T_AKT_LAMPU2  "smarthome/aktuator/lampu2"
#define T_RFID_RESP   "smarthome/aktuator/rfid/response"

// ============================================================
// MQTT CALLBACK — terima perintah dari Flask/dashboard
// ============================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String t = String(topic);
    String v = "";
    for (unsigned int i = 0; i < length; i++) v += (char)payload[i];

    if (t == T_AKT_POMPA) {
        if (v == "AUTO") {
            pompaAuto = true;
        } else {
            pompaAuto  = false;
            pompaNyala = (v == "ON");
            digitalWrite(PIN_RELAY_POMPA, pompaNyala ? RELAY_ON : RELAY_OFF);
        }
        Serial.printf("[POMPA] → %s\n", v.c_str());
    }
    else if (t == T_AKT_LAMPU1) {
        if (v == "AUTO") {
            lampu1Auto = true;
        } else {
            lampu1Auto  = false;
            lampu1Nyala = (v == "ON");
            digitalWrite(PIN_RELAY_LAMPU1, lampu1Nyala ? RELAY_ON : RELAY_OFF);
        }
        Serial.printf("[LAMPU1] → %s\n", v.c_str());
    }
    else if (t == T_AKT_LAMPU2) {
        if (v == "AUTO") {
            lampu2Auto = true;
        } else {
            lampu2Auto  = false;
            lampu2Nyala = (v == "ON");
            digitalWrite(PIN_RELAY_LAMPU2, lampu2Nyala ? RELAY_ON : RELAY_OFF);
        }
        Serial.printf("[LAMPU2] → %s\n", v.c_str());
    }
    else if (t == T_RFID_RESP) {
        rfidResponse = v;
        Serial.printf("[RFID] Response: %s\n", v.c_str());
    }
    else if (t == "smarthome/aktuator/buzzer1") {
        bool on = (v == "ON");
        digitalWrite(PIN_BUZZER1, on ? LOW : HIGH);
        Serial.printf("[BUZZER1] → %s\n", v.c_str());
    }
}

// ============================================================
// MQTT RECONNECT — non-blocking, coba tiap 2 detik
// ============================================================
void mqttReconnect() {
    if (mqtt.connected()) return;
    if (millis() - lastMqttRetry < 2000) return;  // lebih cepat reconnect
    lastMqttRetry = millis();

    if (strlen(mqttUser) == 0 || strlen(mqttPass) == 0) {
        Serial.println("MQTT belum dikonfigurasi: isi username dan password melalui portal.");
        return;
    }

    String clientId = "SmartHome-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    Serial.printf("MQTT konek ke %s... ", mqttHost);

    if (mqtt.connect(clientId.c_str(), mqttUser, mqttPass)) {
        Serial.println("OK!");
        mqtt.subscribe(T_AKT_POMPA);
        mqtt.subscribe(T_AKT_LAMPU1);
        mqtt.subscribe(T_AKT_LAMPU2);
        mqtt.subscribe(T_RFID_RESP);
        mqtt.subscribe("smarthome/aktuator/buzzer1");
    } else {
        Serial.printf("Gagal (rc=%d)\n", mqtt.state());
    }
}

// ============================================================
// BACA & PUBLISH SEMUA SENSOR
// ============================================================
void publishSensor() {
    char buf[16];

    // --- DHT22 ---
    float suhu = dht.readTemperature();
    float hum  = dht.readHumidity();
    if (isnan(suhu)) suhu = 0;
    if (isnan(hum))  hum  = 0;

    // --- MQ-2 (gas) — abaikan selama warmup 30 detik pertama ---
    bool mq2Ready  = (millis() > MQ2_WARMUP_MS);
    int  gasPpm    = mq2Ready ? map(analogRead(PIN_MQ2), 0, 4095, 0, 1000) : 0;
    bool gasBahaya = mq2Ready && (gasPpm > GAS_BAHAYA);

    // --- LDR (cahaya) — DO pin: LOW = gelap, HIGH = terang ---
    bool gelap  = (digitalRead(PIN_LDR) == HIGH);
    int  cahaya = gelap ? 0 : 100;

    // --- Sensor tanah ---
    int tanahPct = map(analogRead(PIN_TANAH), 4095, 0, 0, 100);
    String tanahStatus;
    if      (tanahPct < TANAH_KERING_PCT) tanahStatus = "kering";
    else if (tanahPct < 70)               tanahStatus = "lembab";
    else                                  tanahStatus = "basah";

    // --- Sensor tegangan panel surya ---
    float volt = (analogRead(PIN_TEGANGAN) / 4095.0f) * 3.3f * VOLT_RASIO;

    // --- KY-026 (api) — DO pin: LOW = api terdeteksi ---
    // Debounce: butuh 2 pembacaan berturut-turut agar tidak false trigger
    static bool apiSebelum = false;
    bool rawApi  = (digitalRead(PIN_API) == LOW);
    bool adaApi  = rawApi && apiSebelum;
    apiSebelum   = rawApi;

    // --- Sensor hujan — AO pin: nilai kecil = basah ---
    bool adaHujan = (analogRead(PIN_HUJAN) < HUJAN_THRESHOLD);

    // --- PIR (gerak) ---
    bool adaGerak = (digitalRead(PIN_PIR)   == HIGH);

    // ---- LOGIKA OTOMATIS ----

    static int  prevBuzzer2 = -1;
    static int  prevKipas1  = -1;
    static int  prevKipas2  = -1;
    static int  prevLampu1  = -1;
    static int  prevLampu2  = -1;
    bool kipas2Nyala = (suhu > SUHU_PANAS);

    // Buzzer 2 → api terdeteksi
    digitalWrite(PIN_BUZZER2, adaApi ? LOW : HIGH);
    if ((int)adaApi != prevBuzzer2) {
        prevBuzzer2 = (int)adaApi;
        mqtt.publish("smarthome/status/aktuator/buzzer2", adaApi ? "ON" : "OFF");
    }

    // Kipas 1 → gas berbahaya
    digitalWrite(PIN_KIPAS1, gasBahaya ? LOW : HIGH);
    if ((int)gasBahaya != prevKipas1) {
        prevKipas1 = (int)gasBahaya;
        mqtt.publish("smarthome/status/aktuator/kipas1", gasBahaya ? "ON" : "OFF");
    }

    // Kipas 2 → suhu tinggi
    digitalWrite(PIN_KIPAS2, kipas2Nyala ? LOW : HIGH);
    if ((int)kipas2Nyala != prevKipas2) {
        prevKipas2 = (int)kipas2Nyala;
        mqtt.publish("smarthome/status/aktuator/kipas2", kipas2Nyala ? "ON" : "OFF");
    }

    // Lampu AUTO via LDR
    if (lampu1Auto) {
        lampu1Nyala = gelap;
        digitalWrite(PIN_RELAY_LAMPU1, lampu1Nyala ? RELAY_ON : RELAY_OFF);
        if ((int)lampu1Nyala != prevLampu1) {
            prevLampu1 = (int)lampu1Nyala;
            mqtt.publish("smarthome/status/aktuator/lampu1", lampu1Nyala ? "ON" : "OFF");
        }
    }
    if (lampu2Auto) {
        lampu2Nyala = gelap;
        digitalWrite(PIN_RELAY_LAMPU2, lampu2Nyala ? RELAY_ON : RELAY_OFF);
        if ((int)lampu2Nyala != prevLampu2) {
            prevLampu2 = (int)lampu2Nyala;
            mqtt.publish("smarthome/status/aktuator/lampu2", lampu2Nyala ? "ON" : "OFF");
        }
    }

    // Pompa AUTO → tanah kering nyala, lembab/basah mati
    if (pompaAuto) {
        if (tanahStatus == "kering" && !pompaNyala) {
            pompaNyala = true;
            digitalWrite(PIN_RELAY_POMPA, RELAY_ON);
            mqtt.publish("smarthome/status/aktuator/pompa", "ON");
        } else if (tanahStatus != "kering" && pompaNyala) {
            pompaNyala = false;
            digitalWrite(PIN_RELAY_POMPA, RELAY_OFF);
            mqtt.publish("smarthome/status/aktuator/pompa", "OFF");
        }
    }

    // Update state hujan — logika servo ditangani handleServo2()
    adaHujanState = adaHujan;

    // ---- PUBLISH MQTT ----
    dtostrf(suhu, 5, 1, buf); mqtt.publish(T_SUHU,       buf);
    dtostrf(hum,  5, 1, buf); mqtt.publish(T_KELEMBABAN, buf);
    mqtt.publish(T_GAS,      String(gasPpm).c_str());
    mqtt.publish(T_API,      adaApi   ? "terdeteksi"    : "aman");
    mqtt.publish(T_HUJAN,    adaHujan ? "hujan"         : "tidak hujan");
    mqtt.publish(T_TANAH,    tanahStatus.c_str());
    dtostrf(volt, 5, 2, buf); mqtt.publish(T_TEGANGAN,   buf);
    mqtt.publish(T_PIR,      adaGerak ? "ada gerak"     : "tidak ada gerak"); 
    mqtt.publish(T_CAHAYA,   String(cahaya).c_str());
    mqtt.publish(T_L1_MODE,  lampu1Auto ? "AUTO" : "MANUAL");
    mqtt.publish(T_L1_NYALA, lampu1Nyala ? "1" : "0");
    mqtt.publish(T_L2_MODE,  lampu2Auto ? "AUTO" : "MANUAL");
    mqtt.publish(T_L2_NYALA, lampu2Nyala ? "1" : "0");
    mqtt.publish("smarthome/sensor/pompa_mode",  pompaAuto ? "AUTO" : "MANUAL");
    mqtt.publish("smarthome/sensor/pompa_nyala", pompaNyala ? "1" : "0");

    Serial.printf("[SENSOR] suhu=%.1f hum=%.1f gas=%d cahaya=%d tanah=%s volt=%.2f pir=%s api=%s hujan=%s\n",
        suhu, hum, gasPpm, cahaya, tanahStatus.c_str(), volt,
        adaGerak ? "Y" : "N", adaApi ? "Y" : "N", adaHujan ? "Y" : "N");
}

// ============================================================
// BACA RFID — publish UID ke MQTT, tunggu response
// ============================================================
void cekRFID() {
    if (rfidWaiting) return;

    // Cooldown antar scan: 300ms normal, 2000ms setelah kartu baru dibaca
    static unsigned long lastAttempt    = 0;
    static unsigned long postReadUntil  = 0;
    if (millis() < postReadUntil)         return;   // jeda pasca-baca
    if (millis() - lastAttempt < 300)    return;
    lastAttempt = millis();

    // Reinit periodik tiap 30 detik (fix RC522 clone yang stuck)
    static unsigned long lastReinit = 0;
    if (millis() - lastReinit > 30000) {
        lastReinit = millis();
        rfid.PCD_Init();
        rfid.PCD_SetAntennaGain(rfid.RxGain_max);
    }

    // Stop crypto saja — JANGAN HaltA sebelum scan (membuat kartu HALT tak terdeteksi REQA)
    rfid.PCD_StopCrypto1();

    // Coba REQA (kartu IDLE), fallback WUPA (kartu HALT / KTP tertentu)
    bool cardPresent = rfid.PICC_IsNewCardPresent();
    if (!cardPresent) {
        byte atqa[2]; byte atqaSize = sizeof(atqa);
        MFRC522::StatusCode s = rfid.PICC_WakeupA(atqa, &atqaSize);
        cardPresent = (s == MFRC522::STATUS_OK || s == MFRC522::STATUS_COLLISION);
    }
    if (!cardPresent) return;
    if (!rfid.PICC_ReadCardSerial()) return;

    // Set cooldown 2 detik agar kartu yang baru di-HALT tidak langsung dibaca ulang
    postReadUntil = millis() + 2000;

    String uid = "";
    for (byte i = 0; i < rfid.uid.size; i++) {
        if (rfid.uid.uidByte[i] < 0x10) uid += "0";
        uid += String(rfid.uid.uidByte[i], HEX);
    }
    uid.toUpperCase();

    Serial.printf("[RFID] Kartu: %s — menunggu verifikasi...\n", uid.c_str());
    mqtt.publish(T_RFID, uid.c_str());

    rfidWaiting   = true;
    rfidWaitStart = millis();
    rfidResponse  = "";

    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
}

// ============================================================
// HANDLE RFID RESPONSE (non-blocking)
// ============================================================
void handleRFIDResponse() {
    if (!rfidWaiting) return;

    if (rfidResponse == "BUKA") {
        servo1.write(90);
        servo1Terbuka = true;
        servo1OpenAt  = millis();
        Serial.println("[RFID] Akses DITERIMA — pintu BUKA");
        mqtt.publish("smarthome/status/aktuator/servo1", "BUKA");
        rfidWaiting  = false;
        rfidResponse = "";
    }
    else if (rfidResponse == "TOLAK") {
        Serial.println("[RFID] Akses DITOLAK");
        rfidWaiting  = false;
        rfidResponse = "";
    }
    else if (millis() - rfidWaitStart > RFID_TIMEOUT) {
        Serial.println("[RFID] Timeout — server tidak merespons");
        rfidWaiting  = false;
        rfidResponse = "";
    }
}

// ============================================================
// TUTUP PINTU OTOMATIS setelah SERVO_BUKA_DURASI ms
// ============================================================
void handleServo1Timer() {
    if (!servo1Terbuka) return;
    if (millis() - servo1OpenAt > SERVO_BUKA_DURASI) {
        servo1.write(0);
        servo1Terbuka = false;
        Serial.println("[SERVO1] Pintu TUTUP otomatis");
        mqtt.publish("smarthome/status/aktuator/servo1", "TUTUP");
    }
}

// ============================================================
// SERVO 2 JEMURAN — continuous rotation, timer-based
// ============================================================
void handleServo2() {
    // Trigger masuk saat hujan mulai
    if (adaHujanState && !servo2Masuk && servo2Mode == S2_IDLE) {
        servo2.write(0);            // putar ke arah masuk
        servo2Mode    = S2_MASUK;
        servo2PutarAt = millis();
        Serial.println("[SERVO2] Hujan — jemuran MASUK (putar)");
        mqtt.publish("smarthome/status/aktuator/servo2", "MASUK");
    }
    // Trigger keluar saat hujan berhenti
    else if (!adaHujanState && servo2Masuk && servo2Mode == S2_IDLE) {
        servo2.write(180);          // putar ke arah keluar
        servo2Mode    = S2_KELUAR;
        servo2PutarAt = millis();
        Serial.println("[SERVO2] Kering — jemuran KELUAR (putar)");
        mqtt.publish("smarthome/status/aktuator/servo2", "KELUAR");
    }

    // Berhenti setelah durasi selesai
    if (servo2Mode == S2_MASUK && millis() - servo2PutarAt >= SERVO2_PUTAR_MS) {
        servo2.write(90);           // stop
        servo2Mode  = S2_IDLE;
        servo2Masuk = true;
        Serial.println("[SERVO2] Jemuran MASUK — berhenti");
    }
    else if (servo2Mode == S2_KELUAR && millis() - servo2PutarAt >= SERVO2_PUTAR_MS) {
        servo2.write(90);           // stop
        servo2Mode  = S2_IDLE;
        servo2Masuk = false;
        Serial.println("[SERVO2] Jemuran KELUAR — berhenti");
    }
}

// ============================================================
// SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    Serial.println("\n========================================");
    Serial.println("  Smart Home ESP32 — MQTT v3.0");
    Serial.println("========================================");

    // Output pins
    pinMode(PIN_RELAY_POMPA,  OUTPUT); digitalWrite(PIN_RELAY_POMPA,  RELAY_OFF);
    pinMode(PIN_RELAY_LAMPU1, OUTPUT); digitalWrite(PIN_RELAY_LAMPU1, RELAY_OFF);
    pinMode(PIN_RELAY_LAMPU2, OUTPUT); digitalWrite(PIN_RELAY_LAMPU2, RELAY_OFF);
    pinMode(PIN_KIPAS1,       OUTPUT); digitalWrite(PIN_KIPAS1,       HIGH);
    pinMode(PIN_KIPAS2,       OUTPUT); digitalWrite(PIN_KIPAS2,       HIGH);
    pinMode(PIN_BUZZER1,      OUTPUT); digitalWrite(PIN_BUZZER1,      HIGH);
    pinMode(PIN_BUZZER2,      OUTPUT); digitalWrite(PIN_BUZZER2,      HIGH);

    // Input pins
    pinMode(PIN_PIR,   INPUT);
    pinMode(PIN_LDR,   INPUT);   // LDR DO pin — digital
    pinMode(PIN_API,   INPUT);   // KY-026 DO pin — GPIO36 tidak support pullup

    // ADC attenuation (0–3.3V range)
    analogSetPinAttenuation(PIN_MQ2,      ADC_11db);
    analogSetPinAttenuation(PIN_TANAH,    ADC_11db);
    analogSetPinAttenuation(PIN_HUJAN,    ADC_11db);   // rain sensor AO
    analogSetPinAttenuation(PIN_TEGANGAN, ADC_11db);

    // Servo
    servo1.attach(PIN_SERVO1);
    servo2.attach(PIN_SERVO2);
    servo1.write(0);
    servo2.write(90);  // stop — servo 360 diam di tengah

    // DHT22
    dht.begin();

    // RFID
    SPI.begin();
    rfid.PCD_Init();
    rfid.PCD_SetAntennaGain(rfid.RxGain_max);  // gain antena maksimal
    rfid.PCD_DumpVersionToSerial();
    Serial.println("RFID: OK");

    // Load konfigurasi MQTT dari flash
    prefs.begin("config", false);
    // Host broker mengikuti deployment saat ini; nilai lama di NVS tidak dipakai.
    String savedUser = prefs.getString("mqtt_user", "");
    savedUser.toCharArray(mqttUser, sizeof(mqttUser));
    String savedPass = prefs.getString("mqtt_pass", "");
    savedPass.toCharArray(mqttPass, sizeof(mqttPass));
    prefs.end();

    // Tekan BOOT dalam 3 detik setelah sketch mulai untuk reset.
    // Jangan tahan saat power-on karena GPIO 0 dapat masuk ke mode flashing.
    pinMode(0, INPUT_PULLUP);
    Serial.println("Tekan BOOT untuk reset WiFi dan MQTT, atau tunggu...");
    bool bootDitekan = false;
    for (int i = 0; i < 30; i++) {
        if (digitalRead(0) == LOW) { bootDitekan = true; break; }
        delay(100);
    }
    if (bootDitekan) {
        Serial.println("Reset WiFi & MQTT...");
        WiFiManager wmReset;
        wmReset.resetSettings();
        prefs.begin("config", false);
        prefs.clear();
        prefs.end();
        memset(mqttHost, 0, sizeof(mqttHost));
        Serial.println("Reset selesai — restart...");
        delay(500);
        ESP.restart();
    }

    // WiFiManager — user isi WiFi dan kredensial broker melalui browser.
    WiFiManagerParameter mqttParam("mqtt_host", "Host MQTT", mqttHost, sizeof(mqttHost));
    WiFiManagerParameter mqttUserParam("mqtt_user", "Username MQTT", mqttUser, sizeof(mqttUser));
    WiFiManagerParameter mqttPassParam("mqtt_pass", "Password MQTT", mqttPass, sizeof(mqttPass), " type=\"password\"");
    WiFiManager wm;
    wm.addParameter(&mqttParam);
    wm.addParameter(&mqttUserParam);
    wm.addParameter(&mqttPassParam);
    wm.setConfigPortalTimeout(180);
    wm.setConnectTimeout(30);
    wm.setDebugOutput(false);

    Serial.println("Konek WiFi...");
    if (!wm.autoConnect("SmartHome-Setup")) {
        Serial.println("WiFi gagal — restart");
        delay(1000);
        ESP.restart();
    }
    Serial.printf("WiFi OK! IP ESP32: %s\n", WiFi.localIP().toString().c_str());

    // Perangkat lama belum punya kredensial di NVS: buka portal untuk migrasi.
    if (strlen(mqttUser) == 0 || strlen(mqttPass) == 0) {
        Serial.println("Isi kredensial MQTT di portal konfigurasi...");
        wm.startConfigPortal("SmartHome-Setup");
    }

    // Simpan konfigurasi MQTT jika diisi/berubah.
    String newHost = String(mqttParam.getValue());
    newHost.trim();
    if (newHost.length() == 0) newHost = MQTT_DEFAULT_HOST;
    String newUser = String(mqttUserParam.getValue());
    newUser.trim();
    String newPass = String(mqttPassParam.getValue());
    newPass.trim();
    if (newHost != String(mqttHost) || newUser != String(mqttUser) || newPass != String(mqttPass)) {
        newHost.toCharArray(mqttHost, sizeof(mqttHost));
        newUser.toCharArray(mqttUser, sizeof(mqttUser));
        newPass.toCharArray(mqttPass, sizeof(mqttPass));
        prefs.begin("config", false);
        prefs.putString("mqtt_host", newHost);
        prefs.putString("mqtt_user", newUser);
        prefs.putString("mqtt_pass", newPass);
        prefs.end();
        Serial.printf("Konfigurasi MQTT disimpan: %s\n", mqttHost);
    }

    // Aktifkan auto reconnect WiFi bawaan ESP32
    WiFi.setAutoReconnect(true);

    // Setup MQTT
    mqtt.setServer(mqttHost, MQTT_PORT);
    mqtt.setCallback(mqttCallback);
    mqtt.setKeepAlive(15);   // deteksi disconnect lebih cepat (default 60 terlalu lama)
    mqtt.setSocketTimeout(5);
    mqtt.setBufferSize(512);

    // Warmup MQ-2 — dicatat waktu mulai, diproses non-blocking di loop()
    Serial.println("Siap! (MQ-2 warmup berjalan di background)");
}

// ============================================================
// LOOP
// ============================================================
void loop() {
    // Cek WiFi — reconnect agresif tiap 5 detik, restart setelah 60 detik
    bool wifiOk = (WiFi.status() == WL_CONNECTED);
    if (!wifiOk) {
        if (wifiLostAt == 0) {
            wifiLostAt = millis();
            Serial.println("WiFi putus — coba reconnect...");
        }
        static unsigned long lastWifiRetry = 0;
        if (millis() - lastWifiRetry > 5000) {
            lastWifiRetry = millis();
            WiFi.reconnect();
            Serial.println("WiFi reconnect...");
        }
        if (millis() - wifiLostAt > 60000) {
            Serial.println("WiFi putus >60 detik — restart");
            ESP.restart();
        }
        // Tidak return — RFID dan servo tetap jalan saat WiFi putus
    } else {
        if (wifiLostAt != 0) {
            Serial.printf("WiFi OK lagi (%.0f dtk offline)\n", (millis() - wifiLostAt) / 1000.0f);
            wifiLostAt = 0;
        }
        // MQTT hanya diproses saat WiFi OK
        if (!mqtt.connected()) mqttReconnect();
        mqtt.loop();
    }

    // RFID health check — reinit RC522 kalau tidak merespons
    if (millis() - lastRfidCheck >= RFID_HEALTH_MS) {
        lastRfidCheck = millis();
        byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
        if (v == 0x00 || v == 0xFF) {
            Serial.println("[RFID] Tidak merespons — reinit...");
            rfid.PCD_Init();
        }
        // Reset rfidWaiting kalau stuck lebih dari 10 detik
        if (rfidWaiting && millis() - rfidWaitStart > 10000) {
            Serial.println("[RFID] rfidWaiting stuck — reset paksa");
            rfidWaiting = false;
            rfidResponse = "";
        }
    }

    // RFID — cek kartu & handle response
    cekRFID();
    handleRFIDResponse();

    // Servo 1 — tutup otomatis
    handleServo1Timer();

    // Servo 2 — jemuran (continuous rotation)
    handleServo2();

    // Publish sensor tiap INTERVAL_SENSOR ms
    unsigned long now = millis();
    if (now - lastSensor >= INTERVAL_SENSOR) {
        lastSensor = now;
        if (mqtt.connected()) publishSensor();
    }
}
