# ESP32 Smart Home — Panduan Setup

## Struktur
```
esp32/
├── README.md
└── smarthome_esp32/
    └── smarthome_esp32.ino
```

---

## Library yang harus diinstall
Buka **Arduino IDE → Tools → Manage Libraries**, cari dan install:

| Library | Author |
|---------|--------|
| WiFiManager | tzapu / tablatronix |
| PubSubClient | Nick O'Leary |
| DHT sensor library | Adafruit |
| Adafruit Unified Sensor | Adafruit |

---

## Cara kerja WiFiManager

### Pertama kali (atau setelah reset konfigurasi)
1. ESP32 **tidak menemukan** kredensial WiFi tersimpan
2. ESP32 membuat WiFi AP bernama **`SmartHome-Setup`** (password: `12345678`)
3. Hubungkan HP/laptop ke AP tersebut
4. Browser otomatis membuka halaman konfigurasi — jika tidak, buka manual: **`192.168.4.1`**
5. Isi:
   - **SSID** WiFi rumah
   - **Password** WiFi
   - **IP MQTT Broker** — IP komputer yang menjalankan server Flask
     *(cek dengan `ipconfig` di CMD Windows)*
6. Klik **Save** → ESP32 restart dan terhubung ke WiFi yang diisi
7. Kredensial & IP MQTT disimpan ke flash — tidak perlu diisi ulang setelah restart

### Startup normal
- ESP32 mencoba connect ke WiFi tersimpan selama **5 detik**
- Jika berhasil → lanjut normal
- Jika gagal (misalnya WiFi tidak ada) → restart dan coba lagi

---

## Reset & konfigurasi ulang

### Via browser (direkomendasikan)
Setelah ESP32 terhubung, buka browser dan masuk ke:

| URL | Fungsi |
|-----|--------|
| `http://<ip-esp32>/` | Halaman status (IP, WiFi, MQTT, pompa) |
| `http://<ip-esp32>/reset` | Restart ESP32 (kredensial tetap) |
| `http://<ip-esp32>/config` | Reset konfigurasi WiFi → masuk AP mode |

> Cek IP ESP32 di Serial Monitor Arduino IDE (baud: 115200)

### Via Serial Monitor
Contoh output saat boot berhasil:
```
=============================
 Smart Home ESP32  v1.0
=============================
📋 MQTT IP dari flash: 192.168.1.50
📶 Mencoba WiFi tersimpan (timeout: 5 detik)...
✅ WiFi terhubung! IP: 192.168.1.105
🌐 Buka browser: http://192.168.1.105
🔄 Reset ESP32 : http://192.168.1.105/reset
⚙️  Konfigurasi : http://192.168.1.105/config
🔌 Menghubungkan MQTT... terhubung!
📌 Subscribe: smarthome/aktuator/pompa
📤 Suhu: 28.5°C  Kelembaban: 65.2%  Pompa: OFF
```

---

## Rangkaian

### DHT22
```
DHT22          ESP32
-----          -----
VCC    →       3.3V
GND    →       GND
DATA   →       GPIO 4
```
> **Wajib**: pasang resistor **10kΩ** antara pin DATA dan VCC (pull-up).

### Relay Pompa
```
Relay Module   ESP32
------------   -----
VCC    →       5V (pin VIN)
GND    →       GND
IN     →       GPIO 26
```
> Sketch default: relay aktif **LOW** (`RELAY_ON = LOW`).
> Jika relay kamu aktif HIGH, ganti di baris:
> ```cpp
> #define RELAY_ON   HIGH
> #define RELAY_OFF  LOW
> ```

---

## Topik MQTT

| Arah | Topik | Contoh payload |
|------|-------|----------------|
| ESP32 → Server | `smarthome/sensor/suhu` | `28.5` |
| ESP32 → Server | `smarthome/sensor/kelembaban` | `65.2` |
| Server → ESP32 | `smarthome/aktuator/pompa` | `ON` atau `OFF` |

---

## Urutan menjalankan

1. Jalankan MQTT broker di komputer:
   ```
   mosquitto
   ```
2. Jalankan server Flask:
   ```
   python app.py
   ```
3. Upload sketch ke ESP32 via Arduino IDE
4. Buka **Serial Monitor** (baud 115200) untuk melihat IP
5. Buka dashboard: `http://localhost:5000/dashboard`
6. Klik toggle **Pompa Air** di dashboard → pompa menyala/mati

---

## Catatan pin default

| Fungsi | GPIO |
|--------|------|
| DHT22 DATA | 4 |
| Relay pompa | 26 |

Ganti sesuai rangkaian kamu di bagian atas file `.ino`:
```cpp
#define PIN_DHT    4
#define PIN_RELAY  26
```
