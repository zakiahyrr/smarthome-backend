import time
import numpy as np
import cv2
import paho.mqtt.publish as publish
from tensorflow.keras.models import load_model

# ===== LOAD MODEL =====
model = load_model("model/keras_model.h5", compile=False)

# ===== LOAD LABEL =====
with open("model/labels.txt", "r") as f:
    labels = [line.strip() for line in f.readlines()]

# ===== MQTT CONFIG =====
MQTT_HOST = "127.0.0.1"
MQTT_TOPIC = "kamera/ai"

# ===== WEBCAM =====
cap = cv2.VideoCapture(0)

def predict(frame):
    img = cv2.resize(frame, (224, 224))
    img = np.asarray(img, dtype=np.float32)
    img = (img / 127.5) - 1  # normalisasi
    img = np.expand_dims(img, axis=0)

    pred = model.predict(img, verbose=0)
    idx = np.argmax(pred)
    return labels[idx]

while True:
    ret, frame = cap.read()
    if not ret:
        continue

    label = predict(frame)

    print("Hasil AI:", label)

    # kirim ke MQTT
    publish.single(MQTT_TOPIC, label, hostname=MQTT_HOST)

    # tampilkan kamera (opsional)
    cv2.putText(frame, label, (10, 40),
                cv2.FONT_HERSHEY_SIMPLEX, 1, (0,255,0), 2)
    cv2.imshow("AI Camera", frame)

    if cv2.waitKey(1) & 0xFF == 27:  # ESC untuk keluar
        break

    time.sleep(2)

cap.release()
cv2.destroyAllWindows()