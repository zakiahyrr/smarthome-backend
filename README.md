# Smart Home IoT System

Sistem rumah pintar berbasis ESP32 dengan monitoring sensor real-time, kendali aktuator otomatis, deteksi AI via kamera, dan akses RFID. Backend Flask + MQTT, frontend dapat di-deploy ke Netlify.

---

## Arsitektur Sistem

```
Sensor/RFID/PIR
    └─► ESP32 (firmware utama)
            │
            ├─► MQTT Publish ──► Mosquitto Broker ──► Flask (app.py)
            │                                               │
            │                                               ├─► Database (MariaDB)
            │                                               ├─► SSE ──► Browser Dashboard
            │                                               └─► MQTT Publish (balik ke ESP32)
            │
            └─► PIR trigger ──► ESP32-CAM
                                    └─► HTTP POST foto ──► Flask /api/kamera/prediksi
                                                                └─► Model AI (TFLite)
                                                                      └─► Hasil: manusia/hewan/kosong
                                                                            └─► MQTT ──► ESP32 ──► Buzzer
```

---

## Fitur

- **Monitoring real-time** — data sensor dikirim via SSE ke dashboard tanpa reload
- **Deteksi AI** — ESP32-CAM hanya aktif saat PIR mendeteksi gerakan, hasil diklasifikasi model Teachable Machine (manusia / hewan / kosong)
- **Akses RFID** — servo pintu hanya terbuka untuk UID yang terdaftar di `whitelist.json`
- **Aktuator otomatis** — kipas, buzzer, pompa, servo jemuran aktif berdasarkan threshold sensor
- **Kendali lampu manual** — relay lampu 1 & 2 dapat dikontrol dari dashboard
- **Login & Register** — auth berbasis token, mendukung banyak pengguna
- **Riwayat log** — semua data sensor, RFID, AI, dan aktuator tersimpan di database

---

## Hardware

### Sensor
| Sensor | GPIO | Keterangan |
|---|---|---|
| DHT22 (Suhu & Lembab) | 13 | Digital |
| MQ-2 (Gas) | 35 | ADC1 |
| LDR (Cahaya) | 34 | ADC1, input only |
| Flame Sensor KY-026 | 36 (VP) | Digital, LOW = api |
| Rain Sensor | 39 (VN) | Digital, LOW = hujan |
| Soil Moisture | 32 | ADC1 |
| Sensor Tegangan | 33 | ADC1 |
| PIR | 14 | Digital |
| RFID RC522 | SS=5, RST=22 | SPI VSPI (SCK=18, MISO=19, MOSI=23) |

### Aktuator
| Aktuator | GPIO | Trigger |
|---|---|---|
| Servo 1 (Pintu) | 25 | RFID whitelist |
| Servo 2 (Jemuran) | 26 | Sensor hujan |
| Relay Pompa | 27 | Soil moisture kering |
| Relay Lampu 1 | 4 | Manual (dashboard) |
| Relay Lampu 2 | 15 | Manual (dashboard) |
| Kipas 1 | 16 | Gas MQ-2 berbahaya |
| Kipas 2 | 17 | Suhu tinggi |
| Buzzer 1 | 21 | Deteksi hewan (AI) |
| Buzzer 2 | 2 | Deteksi api |

### ESP32-CAM (AI Thinker)
Terhubung terpisah via WiFi + MQTT. Mengambil foto hanya saat PIR aktif, lalu mengirim ke Flask untuk inferensi AI.

---

## Threshold Sensor

| Sensor | Kondisi Bahaya | Aksi |
|---|---|---|
| DHT22 Suhu | > 32°C | Kipas 2 ON |
| DHT22 Lembab | < 30% | — |
| MQ-2 Gas | > 700 ADC | Kipas 1 ON |
| LDR Cahaya | < 300 ADC | Lampu relay ON |
| Rain Sensor | < 2000 ADC | Servo jemuran tutup |
| Soil Moisture | < 30% | Pompa air ON |
| Flame KY-026 | LOW (terdeteksi) | Buzzer 2 ON |
| PIR | HIGH (ada gerak) | Trigger ESP32-CAM |

---

## Model AI

- **Platform:** Google Teachable Machine
- **Format:** TFLite (server, `model/model.tflite`) + TF.js (browser, `public/model/`)
- **Kelas:** `manusia`, `hewan`, `kosong`
- **Input:** gambar 224×224 px dari ESP32-CAM

---

## Stack Teknologi

| Layer | Teknologi |
|---|---|
| Backend | Python 3, Flask 3.0, Gunicorn |
| Database | MariaDB / MySQL (via PyMySQL) |
| MQTT Broker | Mosquitto |
| Real-time | Server-Sent Events (SSE) |
| AI Server | TFLite (NumPy + Pillow) |
| AI Browser | TensorFlow.js (Teachable Machine) |
| Frontend | HTML/CSS/JS, Chart.js |
| Firmware | Arduino IDE (C++), PubSubClient, WiFiManager |
| Deploy Backend | VPS (Gunicorn) |
| Deploy Frontend | Netlify |

---

## Struktur Proyek

```
smarthome/
├── app.py                  # Backend utama Flask
├── predict.py              # Inferensi TFLite (dipanggil app.py)
├── simulator_ai.py         # Simulasi AI untuk testing
├── requirements.txt        # Dependensi Python
├── Procfile                # Gunicorn entry point (VPS)
├── runtime.txt             # Versi Python
├── mosquitto.conf          # Konfigurasi MQTT broker
├── whitelist.json          # Daftar UID RFID yang diizinkan
│
├── esp32/
│   ├── smarthome_esp32/
│   │   └── smarthome_esp32.ino      # Firmware ESP32 utama
│   └── smarthome_esp32cam/
│       └── smarthome_esp32cam.ino   # Firmware ESP32-CAM
│
├── model/
│   ├── model.tflite        # Model AI server-side
│   └── labels.txt          # Label kelas AI
│
├── public/                 # Deploy ke Netlify
│   ├── index.html          # Dashboard monitoring
│   ├── login.html          # Halaman login & register
│   ├── _redirects          # Routing Netlify
│   └── model/              # Model TF.js untuk inferensi di browser
│
├── templates/              # Template Flask (lokal)
│   ├── index.html
│   └── login.html
│
├── static/
│   ├── style.css
│   ├── script.js
│   └── chart.min.js
│
└── uploads/                # Foto dari ESP32-CAM (tidak di-commit)
```

---

## Database

Lima tabel dibuat otomatis saat Flask pertama kali dijalankan:

| Tabel | Isi |
|---|---|
| `log_sensor` | Data semua sensor (berkala) |
| `rfid_log` | Riwayat scan RFID + status (allowed/denied) |
| `ai_log` | Hasil deteksi AI (kelas + confidence) |
| `aktuator_log` | Log perubahan status aktuator |
| `users` | Akun pengguna (username, email, password hash) |

---

## MQTT Topics

| Topic | Arah | Isi |
|---|---|---|
| `smarthome/sensor` | ESP32 → Flask | JSON data sensor |
| `smarthome/sensor/pir` | ESP32 → ESP32-CAM | Trigger kamera |
| `smarthome/kamera/ai` | Flask → ESP32 | Hasil AI (manusia/hewan/kosong) |
| `smarthome/aktuator/kipas1` | Flask → ESP32 | ON / OFF |
| `smarthome/aktuator/kipas2` | Flask → ESP32 | ON / OFF |
| `smarthome/aktuator/lampu1` | Flask → ESP32 | ON / OFF |
| `smarthome/aktuator/lampu2` | Flask → ESP32 | ON / OFF |
| `smarthome/aktuator/pompa` | Flask → ESP32 | ON / OFF |
| `smarthome/rfid` | ESP32 → Flask | UID kartu |

---

## Instalasi & Menjalankan

### Prasyarat

- Python 3.10+
- MariaDB / MySQL (XAMPP atau server lain)
- Mosquitto MQTT Broker
- Arduino IDE (untuk upload firmware ESP32)

### 1. Clone & Install Dependensi

```bash
git clone https://github.com/zakiahyrr/smarthome-backend.git
cd smarthome-backend
pip install -r requirements.txt
```

### 2. Konfigurasi Database

Buat database di MariaDB:
```sql
CREATE DATABASE smarthome_db;
```

Konfigurasi via environment variable (opsional, default: root tanpa password):
```bash
set MYSQL_HOST=localhost
set MYSQL_USER=root
set MYSQL_PASS=
set MYSQL_DB=smarthome_db
```

### 3. Jalankan Mosquitto

```bash
mosquitto -c mosquitto.conf
```

### 4. Jalankan Flask

```bash
python app.py
```

Dashboard tersedia di: `http://localhost:5000`

### 5. Upload Firmware ESP32

Buka Arduino IDE, install library berikut via Library Manager:
- `WiFiManager` (tzapu)
- `PubSubClient` (knolleary)
- `DHT sensor library` (Adafruit)
- `MFRC522` (miguelbalboa)
- `ESP32Servo` (madhephaestus)
- `ArduinoJson` (bblanchon)

Upload `esp32/smarthome_esp32/smarthome_esp32.ino` ke ESP32 utama, lalu `esp32/smarthome_esp32cam/smarthome_esp32cam.ino` ke ESP32-CAM.

Saat pertama menyala, ESP32 akan membuka hotspot **SmartHomeAP** — sambungkan, buka browser, masukkan SSID & password WiFi rumah.

### 6. Tambah UID RFID ke Whitelist

Edit `whitelist.json`:
```json
["ABCD1234", "04112F72816D80"]
```

---

## Deploy

### Backend (VPS)

```bash
gunicorn -w 2 -b 0.0.0.0:5000 app:app
```

Pastikan Mosquitto berjalan di VPS dan port 1883 terbuka.

### Frontend (Netlify)

Deploy folder `public/` ke Netlify. Ganti URL API di `public/index.html` dan `public/login.html` sesuai alamat VPS.

---

## API Endpoint (Ringkasan)

| Method | Endpoint | Auth | Keterangan |
|---|---|---|---|
| POST | `/api/auth/login` | — | Login, dapat token |
| POST | `/api/auth/register` | — | Daftar akun baru |
| GET | `/api/sensor/raw` | Token | Data sensor terakhir |
| GET | `/api/sensor/history` | Token | Riwayat sensor |
| GET | `/api/stream` | Token | SSE real-time stream |
| POST | `/api/aktuator/<nama>` | Token | Kendali aktuator manual |
| GET | `/api/aktuator/log` | Token | Log aktuator |
| POST | `/api/kamera/prediksi` | — | Upload foto → hasil AI |
| GET | `/api/rfid/log` | Token | Riwayat RFID |

---

## Lisensi

Proyek Tugas Akhir — Teknik Elektronika.
