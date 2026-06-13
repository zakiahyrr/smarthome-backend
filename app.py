from flask import Flask, jsonify, render_template, request, send_file, Response
from werkzeug.utils import secure_filename
import paho.mqtt.client as mqtt
import threading
import os

app = Flask(__name__)

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
    "kamera": "-"
}

ai_result = "unknown"

# ================= MQTT =================
def on_connect(client, userdata, flags, reasonCode, properties):
    print("MQTT Terhubung ke broker! ✅")
    client.subscribe("smarthome/sensor/#")
    client.subscribe("kamera/ai")

def on_message(client, userdata, msg):
    global ai_result
    topik = msg.topic
    nilai = msg.payload.decode()
    print(f"Pesan masuk → {topik}: {nilai}")

    if topik == "smarthome/sensor/suhu":
        data_sensor['suhu'] = nilai
    elif topik == "smarthome/sensor/kelembaban":
        data_sensor['kelembaban'] = nilai
    elif topik == "smarthome/sensor/gas":
        data_sensor['gas'] = nilai
    elif topik == "smarthome/sensor/api":
        data_sensor['api'] = nilai
    elif topik == "smarthome/sensor/hujan":
        data_sensor['hujan'] = nilai
    elif topik == "smarthome/sensor/tanah":
        data_sensor['tanah'] = nilai
    elif topik == "smarthome/sensor/tegangan":
        data_sensor['tegangan_baterai'] = nilai
    elif topik == "smarthome/sensor/pir":
        data_sensor['pir'] = nilai
    elif topik == "smarthome/sensor/rfid":
        data_sensor['rfid'] = nilai
    elif topik == "kamera/ai":
        ai_result = nilai

mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
mqtt_client.on_connect = on_connect
mqtt_client.on_message = on_message
mqtt_client.connect("127.0.0.1", 1883, 60)
mqtt_client.loop_start()

# ================= ROUTES =================

@app.route('/')
def home():
    return "Smart Home API berjalan!"

@app.route('/dashboard')
def dashboard():
    return render_template('index.html')

@app.route('/ai')
def ai_camera():
    return render_template('ai_camera.html')

# ================= API SENSOR =================
@app.route('/api/sensor')
def get_sensor():
    return jsonify({
        **data_sensor,
        "kamera": ai_result if ai_result != "unknown" else "-"
    })

# ================= API AI DARI WEB =================
@app.route('/api/ai', methods=['POST'])
def receive_ai():
    global ai_result
    data = request.json
    ai_result = data.get("kamera")
    print("AI UPDATE (WEB):", ai_result)

    # Kontrol buzzer otomatis via MQTT
    if ai_result and ai_result.lower() == "hewan":
        mqtt_client.publish("smarthome/aktuator/buzzer", "ON")
    else:
        mqtt_client.publish("smarthome/aktuator/buzzer", "OFF")

    return jsonify({"status": "ok"})

# ================= API KAMERA PREDIKSI =================
@app.route('/api/ai-realtime', methods=['POST'])
def ai_realtime():
    global ai_result
    try:
        # Terima gambar dari webcam browser
        if 'gambar' not in request.files:
            return jsonify({"error": "Tidak ada gambar"}), 400

        file = request.files['gambar']
        filepath = os.path.join('uploads', 'webcam.jpg')
        file.save(filepath)

        # Import predict
        from predict import prediksi
        hasil, confidence = prediksi(filepath)

        # Update ai_result global
        ai_result = hasil

        # Kontrol buzzer via MQTT
        if hasil.lower() == "hewan":
            mqtt_client.publish("smarthome/aktuator/buzzer", "ON")
            status_buzzer = "ON"
        else:
            mqtt_client.publish("smarthome/aktuator/buzzer", "OFF")
            status_buzzer = "OFF"

        return jsonify({
            "hasil": hasil,
            "confidence": f"{confidence*100:.2f}%",
            "buzzer": status_buzzer
        })

    except Exception as e:
        print("Error:", str(e))
        return jsonify({
            "hasil": "Error",
            "confidence": "0%",
            "buzzer": "OFF",
            "error": str(e)
        }), 500

# ================= API AKTUATOR =================
@app.route('/api/aktuator/<nama>', methods=['POST'])
def kontrol_aktuator(nama):
    data = request.json
    status = data.get("status")
    print(f"Perintah aktuator → {nama}: {status}")

    # Publish ke MQTT untuk hardware nanti
    mqtt_client.publish(f"smarthome/aktuator/{nama}", status)

    return jsonify({
        "pesan": f"{nama} berhasil {status}"
    })

# ================= RUN =================
if __name__ == '__main__':
    app.run(debug=True)
# ================= API STREAM KAMERA =================
from flask import send_file
import datetime

@app.route('/api/kamera/stream')
def kamera_stream():
    try:
        # Coba esp32cam.jpg dulu
        filepath = os.path.join('uploads', 'esp32cam.jpg')
        
        # Kalau tidak ada, pakai webcam.jpg
        if not os.path.exists(filepath):
            filepath = os.path.join('uploads', 'webcam.jpg')
        
        # Kalau tidak ada, pakai frame.jpg
        if not os.path.exists(filepath):
            filepath = os.path.join('uploads', 'frame.jpg')
        
        if os.path.exists(filepath):
            return send_file(filepath, mimetype='image/jpeg')
        else:
            return jsonify({"error": "Belum ada gambar"}), 404
            
    except Exception as e:
        return jsonify({"error": str(e)}), 500
@app.route('/api/kamera/status')
def kamera_status():
    filepath = os.path.join('uploads', 'esp32cam.jpg')
    ada = os.path.exists(filepath)
    waktu = None
    if ada:
        ts = os.path.getmtime(filepath)
        waktu = datetime.datetime.fromtimestamp(ts).strftime('%H:%M:%S')
    return jsonify({
        "aktif": ada,
        "terakhir_update": waktu
    })
