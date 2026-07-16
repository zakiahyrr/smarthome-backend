import os
import unittest
os.environ.setdefault("SECRET_KEY", "test-secret")
os.environ.setdefault("CAMERA_API_TOKEN", "camera-token")
os.environ.setdefault("CONTROLLER_API_TOKEN", "controller-token")
from app import app

class SecurityTests(unittest.TestCase):
    def setUp(self):
        app.config.update(TESTING=True, SECRET_KEY="test-secret")
        self.client = app.test_client()
    def test_dashboard_requires_session(self):
        self.assertEqual(self.client.get("/dashboard").status_code, 401)
    def test_registration_is_disabled_by_default(self):
        self.assertEqual(self.client.post("/api/register", json={}).status_code, 403)
    def test_camera_requires_device_token(self):
        response = self.client.post("/api/kamera/prediksi", data=b"\xff\xd8test", content_type="image/jpeg")
        self.assertEqual(response.status_code, 401)
    def test_authenticated_sensor_access(self):
        with self.client.session_transaction() as sess:
            sess["username"] = "admin"
        self.assertEqual(self.client.get("/api/sensor").status_code, 200)

if __name__ == "__main__":
    unittest.main()
