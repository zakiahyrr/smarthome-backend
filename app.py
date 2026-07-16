from flask import Flask, jsonify, render_template, request, send_file, Response, stream_with_context, redirect, session
from werkzeug.utils import secure_filename
from werkzeug.security import generate_password_hash, check_password_hash
from functools import wraps
import os, datetime, json, queue, threading, time, hmac

app = Flask(__name__)
app.secret_key = os.environ.get("SECRET_KEY")
if not app.secret_key:
    if os.environ.get("APP_ENV", "development") == "production":
        raise RuntimeError("SECRET_KEY wajib diatur pada production")
    app.secret_key = "development-only-secret-key"
app.config.update(
    SESSION_COOKIE_HTTPONLY=True,
    SESSION_COOKIE_SECURE=os.environ.get("SESSION_COOKIE_SECURE", "false").lower() == "true",
    SESSION_COOKIE_SAMESITE="Lax",
    MAX_CONTENT_LENGTH=int(os.environ.get("MAX_UPLOAD_BYTES", 2 * 1024 * 1024)),
)
os.makedirs("uploads", exist_ok=True)

ALLOW_REGISTRATION = os.environ.get("ALLOW_REGISTRATION", "false").lower() == "true"

def require_auth(f):
    @wraps(f)
    def decorated(*args, **kwargs):
        if not session.get("username"):
            return jsonify({"error": "Unauthorized"}), 401
        return f(*args, **kwargs)
    return decorated

def require_device_token(env_name):
    def decorator(f):
        @wraps(f)
        def decorated(*args, **kwargs):
            expected = os.environ.get(env_name, "")
            supplied = request.headers.get("X-Device-Token", "")
            if not expected or not supplied or not hmac.compare_digest(supplied, expected):
                return jsonify({"error": "Unauthorized device"}), 401
            return f(*args, **kwargs)
        return decorated
    return decorator

# ================= DATABASE MySQL/MariaDB =================
try:
    import pymysql
    import pymysql.cursors
    MySQLError = pymysql.MySQLError

    MYSQL_HOST = os.environ.get('MYSQL_HOST', 'localhost')
    MYSQL_PORT = int(os.environ.get('MYSQL_PORT', 3306))
    MYSQL_USER = os.environ.get('MYSQL_USER', 'root')
    MYSQL_PASS = os.environ.get('MYSQL_PASS', '')
    MYSQL_DB   = os.environ.get('MYSQL_DB',   'smarthome_db')

    def get_db():
        try:
            return pymysql.connect(
                host=MYSQL_HOST, port=MYSQL_PORT,
                user=MYSQL_USER, password=MYSQL_PASS,
                database=MYSQL_DB, connect_timeout=5,
                autocommit=False
            )
        except Exception as e:
            print(f"[DB] Koneksi gagal: {e}")
            return None

    def init_db():
        conn = get_db()
        if not conn:
            return False
        cur = conn.cursor()
        cur.execute("""CREATE TABLE IF NOT EXISTS log_sensor (
            id INT AUTO_INCREMENT PRIMARY KEY,
            waktu DATETIME DEFAULT CURRENT_TIMESTAMP,
            suhu FLOAT, kelembaban FLOAT, gas FLOAT,
            api VARCHAR(20), hujan VARCHAR(20), tanah VARCHAR(20),
            tegangan FLOAT, pir VARCHAR(20), cahaya INT, rfid VARCHAR(50), kamera VARCHAR(20)
        )""")
        cur.execute("SHOW COLUMNS FROM log_sensor LIKE 'cahaya'")
        if not cur.fetchone():
            cur.execute("ALTER TABLE log_sensor ADD COLUMN cahaya INT DEFAULT 0 AFTER pir")
        cur.execute("ALTER TABLE log_sensor MODIFY COLUMN tanah VARCHAR(20)")
        cur.execute("""CREATE TABLE IF NOT EXISTS rfid_log (
            id INT AUTO_INCREMENT PRIMARY KEY,
            timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
            uid VARCHAR(20), status VARCHAR(20)
        )""")
        cur.execute("""CREATE TABLE IF NOT EXISTS ai_log (
            id INT AUTO_INCREMENT PRIMARY KEY,
            timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
            hasil VARCHAR(50), confidence VARCHAR(20)
        )""")
        cur.execute("""CREATE TABLE IF NOT EXISTS aktuator_log (
            id INT AUTO_INCREMENT PRIMARY KEY,
            timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
            nama VARCHAR(50), status_aktuator VARCHAR(20)
        )""")
        cur.execute("""CREATE TABLE IF NOT EXISTS users (
            id INT AUTO_INCREMENT PRIMARY KEY,
            username VARCHAR(50) UNIQUE NOT NULL,
            email VARCHAR(100) UNIQUE NOT NULL,
            password VARCHAR(255) NOT NULL,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP
        )""")
        conn.commit()
        cur.close()
        conn.close()
        print("[DB] MySQL siap.")
        return True

    def log_sensor():
        conn = get_db()
        if not conn: return
        try:
            cur = conn.cursor()
            cur.execute("""INSERT INTO log_sensor
                (suhu,kelembaban,gas,api,hujan,tanah,tegangan,pir,cahaya,rfid,kamera)
                VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)""", (
                data_sensor.get('suhu', 0),    data_sensor.get('kelembaban', 0),
                data_sensor.get('gas', 0),     data_sensor.get('api', ''),
                data_sensor.get('hujan', ''),  data_sensor.get('tanah', ''),
                data_sensor.get('tegangan_baterai', 0),
                data_sensor.get('pir', ''),    data_sensor.get('cahaya', 0),
                data_sensor.get('rfid', ''),   ai_result
            ))
            conn.commit()
        except Exception as e:
            print(f"[DB] log_sensor error: {e}")
        finally:
            try: cur.close(); conn.close()
            except: pass

    def log_rfid(uid, status):
        conn = get_db()
        if not conn: return
        try:
            cur = conn.cursor()
            cur.execute("INSERT INTO rfid_log (uid,status) VALUES (%s,%s)", (uid, status))
            conn.commit(); cur.close(); conn.close()
        except Exception as e:
            print(f"[DB] log_rfid error: {e}")

    def log_ai(hasil, confidence):
        conn = get_db()
        if not conn: return
        try:
            cur = conn.cursor()
            cur.execute("INSERT INTO ai_log (hasil,confidence) VALUES (%s,%s)", (hasil, confidence))
            conn.commit(); cur.close(); conn.close()
        except Exception as e:
            print(f"[DB] log_ai error: {e}")

    def log_aktuator(nama, status):
        conn = get_db()
        if not conn: return
        try:
            cur = conn.cursor()
            cur.execute("INSERT INTO aktuator_log (nama,status_aktuator) VALUES (%s,%s)", (nama, status))
            conn.commit(); cur.close(); conn.close()
        except Exception as e:
            print(f"[DB] log_aktuator error: {e}")

    def _sensor_log_loop():
        while True:
            time.sleep(60)  # simpan ke DB tiap 60 detik
            log_sensor()

    DB_AKTIF = init_db()
    if DB_AKTIF:
        threading.Thread(target=_sensor_log_loop, daemon=True).start()

except Exception as e:
    print(f"[DB] MySQL tidak aktif: {e}")
    DB_AKTIF = False
    def log_rfid(*a): pass
    def log_ai(*a): pass
    def log_aktuator(*a): pass
    def log_sensor(*a): pass

# ================= STATE =================
data_sensor = {
    "suhu": 0, "kelembaban": 0, "gas": 0, "api": "aman",
    "hujan": "tidak hujan", "tanah": "kering", "tegangan_baterai": 0,
    "pir": "tidak ada gerak", "rfid": "-", "cahaya": 0,
    "lampu1_mode": "AUTO", "lampu1_nyala": "0",
    "lampu2_mode": "AUTO", "lampu2_nyala": "0",
    "pompa_mode": "AUTO",  "pompa_nyala": "0",
    "rfid_status": "-", "rfid_pintu": "tertutup",
    "lampu_mode": "AUTO", "lampu_nyala": "0",  # backward compat
}
ai_result     = "unknown"
ai_confidence = "—"
WHITELIST_FILE = 'whitelist.json'

def load_whitelist():
    try:
        with open(WHITELIST_FILE, 'r') as f:
            return set(json.load(f))
    except (FileNotFoundError, json.JSONDecodeError):
        return set()

def save_whitelist():
    with open(WHITELIST_FILE, 'w') as f:
        json.dump(sorted(list(rfid_whitelist)), f)

rfid_whitelist = load_whitelist()

# ================= SSE BROADCAST =================
sse_clients = []
sse_lock    = threading.Lock()

_broadcast_timer = None
_broadcast_lock  = threading.Lock()
_pending_msg     = None

def broadcast(data_dict):
    global _broadcast_timer, _pending_msg
    _pending_msg = json.dumps(data_dict)

    def _send():
        msg = _pending_msg
        with sse_lock:
            dead = []
            for q in sse_clients:
                try:
                    q.put_nowait(msg)
                except queue.Full:
                    pass
                except Exception:
                    dead.append(q)
            for q in dead:
                try: sse_clients.remove(q)
                except: pass

    with _broadcast_lock:
        if _broadcast_timer and _broadcast_timer.is_alive():
            _broadcast_timer.cancel()
        _broadcast_timer = threading.Timer(0.15, _send)
        _broadcast_timer.start()

# ================= MQTT =================
mqtt_client = None
try:
    import paho.mqtt.client as mqtt

    MQTT_HOST = os.environ.get('MQTT_HOST', 'localhost')
    MQTT_PORT = int(os.environ.get('MQTT_PORT', 1883))
    MQTT_USER = os.environ.get('MQTT_USER', '')
    MQTT_PASS = os.environ.get('MQTT_PASS', '')
    MQTT_TLS = os.environ.get("MQTT_TLS", "false").lower() == "true"
    MQTT_CA_CERT = os.environ.get("MQTT_CA_CERT", "")

    def on_connect(client, _userdata, _flags, _rc, _props=None):
        print(f"MQTT terhubung ke {MQTT_HOST}:{MQTT_PORT}")
        client.subscribe("smarthome/sensor/#")
        client.subscribe("smarthome/kamera/ai")
        client.subscribe("smarthome/status/aktuator/#")

    def on_message(client, _userdata, msg):
        global ai_result, ai_confidence
        t = msg.topic
        v = msg.payload.decode()

        topic_map = {
            "smarthome/sensor/suhu":         "suhu",
            "smarthome/sensor/kelembaban":   "kelembaban",
            "smarthome/sensor/gas":          "gas",
            "smarthome/sensor/api":          "api",
            "smarthome/sensor/hujan":        "hujan",
            "smarthome/sensor/tanah":        "tanah",
            "smarthome/sensor/tegangan":     "tegangan_baterai",
            "smarthome/sensor/pir":          "pir",
            "smarthome/sensor/cahaya":       "cahaya",
            "smarthome/sensor/lampu1_mode":  "lampu1_mode",
            "smarthome/sensor/lampu1_nyala": "lampu1_nyala",
            "smarthome/sensor/lampu2_mode":  "lampu2_mode",
            "smarthome/sensor/lampu2_nyala": "lampu2_nyala",
            "smarthome/sensor/pompa_mode":   "pompa_mode",
            "smarthome/sensor/pompa_nyala":  "pompa_nyala",
        }

        if t in topic_map:
            data_sensor[topic_map[t]] = v
            # backward compat lampu1 → lampu
            if t == "smarthome/sensor/lampu1_mode":
                data_sensor["lampu_mode"]  = v
            if t == "smarthome/sensor/lampu1_nyala":
                data_sensor["lampu_nyala"] = v
            if t == "smarthome/sensor/suhu":
                print(f"[MQTT] suhu={data_sensor['suhu']} | hum={data_sensor['kelembaban']} | gas={data_sensor['gas']} | cahaya={data_sensor['cahaya']} | tanah={data_sensor['tanah']} | volt={data_sensor['tegangan_baterai']}", flush=True)

        elif t == "smarthome/sensor/rfid":
            data_sensor['rfid'] = v
            authorized = v.upper() in {c.upper() for c in rfid_whitelist}
            data_sensor['rfid_status'] = 'diterima' if authorized else 'ditolak'

            if authorized:
                data_sensor['rfid_pintu'] = 'terbuka'
                client.publish('smarthome/aktuator/rfid/response', 'BUKA')
                log_rfid(v, 'diterima')
                def _reset():
                    time.sleep(6)
                    data_sensor['rfid_pintu'] = 'tertutup'
                    broadcast({**data_sensor, 'kamera': ai_result, 'kamera_confidence': ai_confidence})
                threading.Thread(target=_reset, daemon=True).start()
            else:
                client.publish('smarthome/aktuator/rfid/response', 'TOLAK')
                log_rfid(v, 'ditolak')

        elif t.startswith("smarthome/status/aktuator/"):
            nama = t.split("/")[-1]
            log_aktuator(nama, v)

        elif t == "smarthome/kamera/ai":
            ai_result = v
            isBad = v.lower() == "hewan"
            try: client.publish("smarthome/aktuator/buzzer1", "ON" if isBad else "OFF")
            except: pass

        broadcast({**data_sensor, 'kamera': ai_result, 'kamera_confidence': ai_confidence})

    def on_disconnect(client, userdata, disconnect_flags, reason_code, properties=None):
        print(f"MQTT terputus (rc={reason_code}) — reconnect otomatis...")

    mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    mqtt_client.on_connect    = on_connect
    mqtt_client.on_message    = on_message
    mqtt_client.on_disconnect = on_disconnect

    if MQTT_USER:
        mqtt_client.username_pw_set(MQTT_USER, MQTT_PASS)
    if MQTT_TLS:
        mqtt_client.tls_set(ca_certs=MQTT_CA_CERT or None)
        mqtt_client.tls_insecure_set(False)
    mqtt_client.reconnect_delay_set(min_delay=1, max_delay=10)
    mqtt_client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
    mqtt_client.loop_start()
    print(f"MQTT aktif → {MQTT_HOST}:{MQTT_PORT}")

except Exception as e:
    print(f"MQTT tidak aktif: {e}")

# ================= AUTH =================
@app.route('/api/register', methods=['POST'])
def register():
    if not ALLOW_REGISTRATION:
        return jsonify({"error": "Registrasi publik dinonaktifkan"}), 403
    data = request.json or {}
    username = data.get('username', '').strip()
    email    = data.get('email', '').strip()
    password = data.get('password', '')
    if not username or not email or not password:
        return jsonify({'error': 'Semua field wajib diisi'}), 400
    if len(password) < 12:
        return jsonify({'error': 'Password minimal 12 karakter'}), 400
    if not DB_AKTIF:
        return jsonify({'error': 'Database tidak aktif'}), 500
    conn = get_db()
    if not conn:
        return jsonify({'error': 'Koneksi database gagal'}), 500
    try:
        cur = conn.cursor()
        cur.execute("INSERT INTO users (username,email,password) VALUES (%s,%s,%s)",
                    (username, email, generate_password_hash(password)))
        conn.commit()
        return jsonify({'status': 'ok', 'message': 'Akun berhasil dibuat'})
    except Exception as e:
        if 'Duplicate' in str(e):
            return jsonify({'error': 'Username atau email sudah terdaftar'}), 409
        return jsonify({'error': str(e)}), 500
    finally:
        try: cur.close(); conn.close()
        except: pass

@app.route('/api/login', methods=['POST'])
def login():
    data     = request.json or {}
    username = data.get('username', '').strip()
    password = data.get('password', '')
    if not username or not password:
        return jsonify({'error': 'Username dan password wajib diisi'}), 400
    if not DB_AKTIF:
        return jsonify({'error': 'Database tidak aktif'}), 500
    conn = get_db()
    if not conn:
        return jsonify({'error': 'Koneksi database gagal'}), 500
    try:
        cur = conn.cursor(pymysql.cursors.DictCursor)
        cur.execute("SELECT * FROM users WHERE username=%s", (username,))
        user = cur.fetchone()
        if not user or not check_password_hash(user['password'], password):
            return jsonify({'error': 'Username atau password salah'}), 401
        session.clear()
        session["username"] = username
        return jsonify({"username": username})
    except Exception as e:
        return jsonify({'error': str(e)}), 500
    finally:
        try: cur.close(); conn.close()
        except: pass

@app.route("/api/logout", methods=["POST"])
@require_auth
def logout():
    session.clear()
    return jsonify({"status": "ok"})
# ================= ROUTES =================
@app.route("/")
def home():
    return redirect("/dashboard" if session.get("username") else "/login")

@app.route("/login")
def login_page():
    if session.get("username"):
        return redirect("/dashboard")
    return render_template("login.html")

@app.route("/dashboard")
@require_auth
def dashboard():
    return render_template("index.html", username=session["username"])

# ================= SSE STREAM =================
@app.route('/api/stream')
@require_auth
def stream():
    q = queue.Queue(maxsize=30)
    with sse_lock:
        sse_clients.append(q)

    def generate():
        try:
            # Kirim state saat ini langsung saat client konek
            yield f"data: {json.dumps({**data_sensor, 'kamera': ai_result, 'kamera_confidence': ai_confidence})}\n\n"
            while True:
                try:
                    msg = q.get(timeout=25)
                    yield f"data: {msg}\n\n"
                except queue.Empty:
                    yield ": ping\n\n"  # keepalive
        finally:
            with sse_lock:
                try: sse_clients.remove(q)
                except: pass

    return Response(
        stream_with_context(generate()),
        mimetype='text/event-stream',
        headers={
            'Cache-Control':    'no-cache',
            'X-Accel-Buffering':'no',
            'Connection':       'keep-alive'
        }
    )

# ================= API SENSOR =================
@app.route('/api/sensor')
@require_auth
def get_sensor():
    return jsonify({**data_sensor, 'kamera': ai_result, 'kamera_confidence': ai_confidence})

@app.route('/api/sensor/raw')
@require_auth
def get_sensor_raw():
    return jsonify({**data_sensor, 'kamera': ai_result})

@app.route('/api/sensor/update', methods=['POST'])
@require_device_token("CONTROLLER_API_TOKEN")
def update_sensor():
    data = request.json or {}
    mapping = {
        "suhu": "suhu", "kelembaban": "kelembaban", "gas": "gas",
        "api": "api", "hujan": "hujan", "tanah": "tanah",
        "tegangan": "tegangan_baterai", "tegangan_baterai": "tegangan_baterai",
        "pir": "pir", "rfid": "rfid", "cahaya": "cahaya",
        "lampu1_mode": "lampu1_mode", "lampu1_nyala": "lampu1_nyala",
        "lampu2_mode": "lampu2_mode", "lampu2_nyala": "lampu2_nyala",
        "lampu_mode": "lampu_mode",   "lampu_nyala":  "lampu_nyala",
    }
    for k, v in data.items():
        if k in mapping:
            data_sensor[mapping[k]] = v
    broadcast({**data_sensor, 'kamera': ai_result, 'kamera_confidence': ai_confidence})
    return jsonify({"status": "ok"})

# ================= API AKTUATOR =================
@app.route('/api/aktuator/<nama>', methods=['POST'])
@require_auth
def kontrol_aktuator(nama):
    status = (request.json or {}).get("status")
    if mqtt_client:
        try: mqtt_client.publish(f"smarthome/aktuator/{nama}", status)
        except: pass
    log_aktuator(nama, status)
    return jsonify({"pesan": f"{nama} → {status}"})

@app.route('/api/aktuator/pending')
@require_auth
def get_aktuator_pending():
    return jsonify({})

# ================= RFID WHITELIST =================
@app.route('/api/rfid/whitelist', methods=['GET'])
@require_auth
def get_whitelist():
    return jsonify(sorted(list(rfid_whitelist)))

@app.route('/api/rfid/whitelist', methods=['POST'])
@require_auth
def add_whitelist():
    card_id = (request.json or {}).get('card_id', '').strip().upper()
    if card_id:
        rfid_whitelist.add(card_id)
        save_whitelist()
    return jsonify({'status': 'ok', 'whitelist': sorted(list(rfid_whitelist))})

@app.route('/api/rfid/whitelist/<card_id>', methods=['DELETE'])
@require_auth
def remove_whitelist(card_id):
    rfid_whitelist.discard(card_id.upper())
    save_whitelist()
    return jsonify({'status': 'ok', 'whitelist': sorted(list(rfid_whitelist))})

# ================= AI & KAMERA =================
@app.route('/api/ai', methods=['POST'])
@require_device_token("CAMERA_API_TOKEN")
def receive_ai():
    global ai_result
    ai_result = (request.json or {}).get("kamera", "unknown")
    broadcast({**data_sensor, 'kamera': ai_result, 'kamera_confidence': ai_confidence})
    return jsonify({"status": "ok"})

@app.route('/api/ai-realtime', methods=['POST'])
@require_auth
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
            try: mqtt_client.publish("smarthome/aktuator/buzzer1", "ON" if isBad else "OFF")
            except: pass
        broadcast({**data_sensor, 'kamera': ai_result, 'kamera_confidence': ai_confidence})
        return jsonify({"hasil": hasil, "confidence": f"{confidence*100:.2f}%", "buzzer": "ON" if isBad else "OFF"})
    except Exception as e:
        return jsonify({"hasil": "Error", "confidence": "0%", "buzzer": "OFF"}), 500

@app.route('/api/kamera/prediksi', methods=['POST'])
@require_device_token("CAMERA_API_TOKEN")
def kamera_prediksi():
    global ai_result, ai_confidence
    try:
        if request.content_type == "image/jpeg":
            if not request.data.startswith(b"\xff\xd8"):
                return jsonify({"error": "Payload harus JPEG"}), 400
            filepath = os.path.join("uploads", "esp32cam.jpg")
            with open(filepath, "wb") as f:
                f.write(request.data)
        elif 'gambar' in request.files:
            file = request.files['gambar']
            filepath = os.path.join('uploads', secure_filename(file.filename))
            file.save(filepath)
        else:
            return jsonify({"error": "Tidak ada gambar"}), 400
        from predict import prediksi
        hasil, confidence = prediksi(filepath)
        ai_result     = hasil
        ai_confidence = f"{confidence*100:.2f}%"
        isBad = hasil.lower() == "hewan"
        if mqtt_client:
            try: mqtt_client.publish("smarthome/kamera/ai", hasil)
            except: pass
        log_ai(hasil, ai_confidence)
        broadcast({**data_sensor, 'kamera': ai_result, 'kamera_confidence': ai_confidence})
        print(f"[AI] Hasil: {hasil} ({confidence*100:.1f}%)", flush=True)
        return jsonify({"hasil": hasil, "confidence": ai_confidence, "buzzer": "ON" if isBad else "OFF"})
    except Exception as e:
        print(f"[AI] Error: {e}", flush=True)
        return jsonify({"error": str(e)}), 500

@app.route('/api/kamera/stream')
@require_auth
def kamera_stream():
    try:
        for fname in ['esp32cam.jpg', 'webcam.jpg', 'frame.jpg']:
            fp = os.path.join('uploads', fname)
            if os.path.exists(fp):
                return send_file(fp, mimetype='image/jpeg')
        return jsonify({"error": "Belum ada gambar"}), 404
    except Exception as e:
        return jsonify({"error": str(e)}), 500

@app.route('/api/kamera/status')
@require_auth
def kamera_status():
    fp  = os.path.join('uploads', 'esp32cam.jpg')
    ada = os.path.exists(fp)
    waktu = datetime.datetime.fromtimestamp(os.path.getmtime(fp)).strftime('%H:%M:%S') if ada else None
    return jsonify({"aktif": ada, "terakhir_update": waktu})

# ================= HISTORY (MySQL) =================
def _query_history(table, limit=100):
    columns = {
        "log_sensor": "waktu",
        "rfid_log": "timestamp",
        "ai_log": "timestamp",
        "aktuator_log": "timestamp",
    }
    order_column = columns.get(table)
    if not DB_AKTIF or not order_column:
        return []
    conn = get_db()
    if not conn:
        return []
    cur = None
    try:
        cur = conn.cursor(pymysql.cursors.DictCursor)
        cur.execute(f"SELECT * FROM {table} ORDER BY {order_column} DESC LIMIT %s", (limit,))
        rows = cur.fetchall()
        for row in rows:
            row[order_column] = str(row[order_column])
        return rows
    except Exception as e:
        print(f"[DB] query error: {e}")
        return []
    finally:
        if cur:
            cur.close()
        conn.close()

@app.route('/api/history/sensor')
@require_auth
def history_sensor():
    return jsonify(_query_history('log_sensor'))

@app.route('/api/history/rfid')
@require_auth
def history_rfid():
    return jsonify(_query_history('rfid_log', 50))

@app.route('/api/history/ai')
@require_auth
def history_ai():
    return jsonify(_query_history('ai_log', 50))

@app.route('/api/history/aktuator')
@require_auth
def history_aktuator():
    return jsonify(_query_history('aktuator_log', 50))

# ================= RUN =================
if __name__ == '__main__':
    port = int(os.environ.get('PORT', 5000))
    app.run(host='0.0.0.0', port=port, debug=False, threaded=True)
