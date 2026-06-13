from flask import Flask, jsonify, render_template, request, send_file, Response
from werkzeug.utils import secure_filename
import os
import datetime

app = Flask(__name__)

os.makedirs('uploads', exist_ok=True)

# ================= DATA SENSOR =================
data_sensor = {
    "suhu": 0,
    "kelembaban": 0,
    "gas": "aman",
    "api": "aman",
    "hujan": "tidak hujan",
    "tanah": "kering",
    "tegangan_baterai": 0,
    "pir": "tidak ada gerak",
    "rfid": "-",
    "cahaya": 0,
    "lampu_mode": "AUTO",
    "lampu_nyala": "0"
}

ai_result = "unknown"

# Antrian perintah aktuator untuk ESP32
aktuator_pending = {}

# ================= MQTT (opsional, untuk lokal) =================
mqtt_client = None
try:
    import paho.mqtt.client as mqtt

    MQTT_HOST = os.environ.get('MQTT_HOST', '')
    MQTT_PORT = int(os.environ.get('MQTT_PORT', 1883))

    if MQTT_HOST:
        def on_connect(client, userdata, flags, reasonCode, properties):
            print(f"MQTT terhubung ke {MQTT_HOST}")
            client.subscribe("smarthome/sensor/#")
            client.subscribe("kamera/ai")

        def on_message(client, userdata, msg):
            global ai_result
            topik = msg.topic
            nilai = msg.payload.decode()
            if topik == "smarthome/sensor/suhu":        data_sensor['suhu'] = nilai
            elif topik == "smarthome/sensor/kelembaban": data_sensor['kelembaban'] = nilai
            elif topik == "smarthome/sensor/gas":        data_sensor['gas'] = nilai
            elif topik == "smarthome/sensor/api":        data_sensor['api'] = nilai
            elif topik == "smarthome/sensor/hujan":      data_sensor['hujan'] = nilai
            elif topik == "smarthome/sensor/tanah":      data_sensor['tanah'] = nilai
            elif topik == "smarthome/sensor/tegangan":   data_sensor['tegangan_baterai'] = nilai
            elif topik == "smarthome/sensor/pir":        data_sensor['pir'] = nilai
            elif topik == "smarthome/sensor/rfid":       data_sensor['rfid'] = nilai
            elif topik == "smarthome/sensor/cahaya":     data_sensor['cahaya'] = nilai
            elif topik == "smarthome/sensor/lampu_mode": data_sensor['lampu_mode'] = nilai
            elif topik == "smarthome/sensor/lampu_nyala":data_sensor['lampu_nyala'] = nilai
            elif topik == "kamera/ai":                   ai_result = nilai

        mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        mqtt_client.on_connect = on_connect
        mqtt_client.on_message = on_message
        MQTT_USER = os.environ.get('MQTT_USER', '')
        MQTT_PASS = os.environ.get('MQTT_PASS', '')
        if MQTT_USER:
            mqtt_client.username_pw_set(MQTT_USER, MQTT_PASS)
        mqtt_client.connect(MQTT_HOST, MQTT_PORT, 60)
        mqtt_client.loop_start()
        print(f"MQTT aktif → {MQTT_HOST}:{MQTT_PORT}")
    else:
        print("MQTT tidak dikonfigurasi — pakai HTTP mode")

except Exception as e:
    print(f"MQTT tidak aktif: {e}")

# ================= ROUTES =================
@app.route('/')
def home():
    return "Smart Home API berjalan!"

@app.route('/dashboard')
def dashboard():
    return render_template('index.html')

# ================= API SENSOR (GET) =================
@app.route('/api/sensor')
def get_sensor():
    return jsonify({
        **data_sensor,
        "kamera": ai_result if ai_result != "unknown" else "-"
    })

# ================= API SENSOR UPDATE (POST dari ESP32 via HTTP) =================
@app.route('/api/sensor/update', methods=['POST'])
def update_sensor():
    data = request.json or {}
    mapping = {
        "suhu":            "suhu",
        "kelembaban":      "kelembaban",
        "gas":             "gas",
        "api":             "api",
        "hujan":           "hujan",
        "tanah":           "tanah",
        "tegangan":        "tegangan_baterai",
        "tegangan_baterai":"tegangan_baterai",
        "pir":             "pir",
        "rfid":            "rfid",
        "cahaya":          "cahaya",
        "lampu_mode":      "lampu_mode",
        "lampu_nyala":     "lampu_nyala",
    }
    for k, v in data.items():
        if k in mapping:
            data_sensor[mapping[k]] = v
    return jsonify({"status": "ok"})

# ================= API AKTUATOR PENDING (GET dari ESP32) =================
@app.route('/api/aktuator/pending')
def get_aktuator_pending():
    global aktuator_pending
    result = dict(aktuator_pending)
    aktuator_pending = {}
    return jsonify(result)

# ================= API AI DARI WEB =================
@app.route('/api/ai', methods=['POST'])
def receive_ai():
    global ai_result
    data = request.json
    ai_result = data.get("kamera", "unknown")
    if mqtt_client and ai_result and ai_result.lower() == "hewan":
        try: mqtt_client.publish("smarthome/aktuator/buzzer", "ON")
        except: pass
    elif mqtt_client:
        try: mqtt_client.publish("smarthome/aktuator/buzzer", "OFF")
        except: pass
    else:
        aktuator_pending["buzzer"] = "ON" if (ai_result and ai_result.lower() == "hewan") else "OFF"
    return jsonify({"status": "ok"})

# ================= API AI REALTIME =================
@app.route('/api/ai-realtime', methods=['POST'])
def ai_realtime():
    global ai_result
    try:
        if 'gambar' not in request.files:
            return jsonify({"error": "Tidak ada gambar"}), 400
        file = request.files['gambar']
        filepath = os.path.join('uploads', 'webcam.jpg')
        file.save(filepath)
        from predict import prediksi
        hasil, confidence = prediksi(filepath)
        ai_result = hasil
        isBad = hasil.lower() == "hewan"
        if mqtt_client:
            try: mqtt_client.publish("smarthome/aktuator/buzzer", "ON" if isBad else "OFF")
            except: pass
        else:
            aktuator_pending["buzzer"] = "ON" if isBad else "OFF"
        return jsonify({
            "hasil": hasil,
            "confidence": f"{confidence*100:.2f}%",
            "buzzer": "ON" if isBad else "OFF"
        })
    except Exception as e:
        return jsonify({"hasil": "Error", "confidence": "0%", "buzzer": "OFF"}), 500

# ================= API AKTUATOR (POST dari dashboard) =================
@app.route('/api/aktuator/<nama>', methods=['POST'])
def kontrol_aktuator(nama):
    data = request.json
    status = data.get("status")
    if mqtt_client:
        try: mqtt_client.publish(f"smarthome/aktuator/{nama}", status)
        except: aktuator_pending[nama] = status
    else:
        aktuator_pending[nama] = status
    return jsonify({"pesan": f"{nama} berhasil {status}"})

# ================= API KAMERA =================
@app.route('/api/kamera/stream')
def kamera_stream():
    try:
        for fname in ['esp32cam.jpg', 'webcam.jpg', 'frame.jpg']:
            fp = os.path.join('uploads', fname)
            if os.path.exists(fp):
                return send_file(fp, mimetype='image/jpeg')
        return jsonify({"error": "Belum ada gambar"}), 404
    except Exception as e:
        return jsonify({"error": str(e)}), 500

@app.route('/api/kamera/prediksi', methods=['POST'])
def kamera_prediksi():
    try:
        if request.content_type == 'image/jpeg':
            filepath = os.path.join('uploads', 'esp32cam.jpg')
            with open(filepath, 'wb') as f:
                f.write(request.data)
        elif 'gambar' in request.files:
            file = request.files['gambar']
            filepath = os.path.join('uploads', secure_filename(file.filename))
            file.save(filepath)
        else:
            return jsonify({"error": "Tidak ada gambar"}), 400
        from predict import prediksi
        hasil, confidence = prediksi(filepath)
        global ai_result
        ai_result = hasil
        isBad = hasil.lower() == "hewan"
        aktuator_pending["buzzer"] = "ON" if isBad else "OFF"
        return jsonify({
            "hasil": hasil,
            "confidence": f"{confidence*100:.2f}%",
            "buzzer": "ON" if isBad else "OFF"
        })
    except Exception as e:
        return jsonify({"error": str(e)}), 500

@app.route('/api/kamera/status')
def kamera_status():
    fp = os.path.join('uploads', 'esp32cam.jpg')
    ada = os.path.exists(fp)
    waktu = None
    if ada:
        waktu = datetime.datetime.fromtimestamp(os.path.getmtime(fp)).strftime('%H:%M:%S')
    return jsonify({"aktif": ada, "terakhir_update": waktu})

# ================= RUN =================
if __name__ == '__main__':
    port = int(os.environ.get('PORT', 5000))
    debug = os.environ.get('FLASK_DEBUG', 'false').lower() == 'true'
    app.run(host='0.0.0.0', port=port, debug=debug)

