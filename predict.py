import numpy as np
from PIL import Image
import os

model_path = os.path.join('model', 'model.tflite')

try:
    from ai_edge_litert import interpreter as tflite
    interpreter = tflite.Interpreter(model_path=model_path)
except ImportError:
    try:
        import tensorflow as tf
        interpreter = tf.lite.Interpreter(model_path=model_path)
    except Exception as e:
        print(f"Gagal load model: {e}")
        interpreter = None

if interpreter:
    interpreter.allocate_tensors()

# Load labels
labels_path = os.path.join('model', 'labels.txt')
with open(labels_path, "r") as f:
    labels = [line.strip().split(" ", 1)[1] 
              for line in f.readlines()]

print("Model loaded! Labels:", labels)

def prediksi(gambar_path):
    try:
        img = Image.open(gambar_path).convert('RGB')
        img = img.resize((224, 224))
        img_array = np.array(img, dtype=np.float32)
        img_array = np.expand_dims(img_array, axis=0)
        img_array = (img_array / 127.5) - 1

        input_details = interpreter.get_input_details()
        output_details = interpreter.get_output_details()
        interpreter.set_tensor(input_details[0]['index'], img_array)
        interpreter.invoke()

        output = interpreter.get_tensor(output_details[0]['index'])
        index = np.argmax(output)
        label = labels[index]
        confidence = float(output[0][index])

        return label, confidence

    except Exception as e:
        print("Error prediksi:", e)
        return "Error", 0.0