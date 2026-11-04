import os

import paho.mqtt.client as mqtt
import requests

import sqlite3
from pathlib import Path

import json
from datetime import datetime, timezone


BROKER_HOST = os.environ.get("MQTT_HOST", "host.docker.internal")
BROKER_PORT = 1883
TOPIC = "esp32/vitals"
DB_PATH = Path(__file__).resolve().parent / "vitals.db"
NOCOBASE_URL = "http://host.docker.internal:13002/api/esp32_vitals:create"
NOCOBASE_API_KEY = os.environ.get("NOCOBASE_API_KEY", "")

def init_database():
    with sqlite3.connect(DB_PATH) as connection:
        connection.execute(
            """
            CREATE TABLE IF NOT EXISTS vitals (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                received_at TEXT NOT NULL,
                spo2 INTEGER NOT NULL,
                heart_rate INTEGER NOT NULL
            )
            """
        )

def send_to_nocobase(spo2, heart_rate, received_at):
    if not NOCOBASE_API_KEY:
        print("NocoBase 傳送失敗：尚未設定 API key", flush=True)
        return

    try:
        response = requests.post(
            NOCOBASE_URL,
            headers={"Authorization": f"Bearer {NOCOBASE_API_KEY}"},
            json={
                "spo2": spo2,
                "heart_rate": heart_rate,
                "received_at": received_at,
            },
            timeout=10,
        )
        response.raise_for_status()
        print("已傳送至 NocoBase", flush=True)
    except requests.RequestException as error:
        print(f"NocoBase 傳送失敗：{error}", flush=True)

def on_connect(client, userdata, flags, reason_code, properties):
    if reason_code.is_failure:
        print(f"MQTT 連線失敗：{reason_code}", flush=True)
        return

    print("MQTT 已連線，開始訂閱", TOPIC, flush=True)
    client.subscribe(TOPIC)


def on_message(client, userdata, message):
    try:
        data = json.loads(message.payload.decode("utf-8"))
        spo2 = data["spo2"]
        heart_rate = data["heart_rate"]

        if type(spo2) is not int or type(heart_rate) is not int:
            raise ValueError("spo2 和 heart_rate 必須是整數")

        received_at = datetime.now(timezone.utc).isoformat()

        with sqlite3.connect(DB_PATH) as connection:
            connection.execute(
                "INSERT INTO vitals (received_at, spo2, heart_rate) "
                "VALUES (?, ?, ?)",
                (received_at, spo2, heart_rate),
            )

        print(
            f"已儲存：spo2={spo2}, heart_rate={heart_rate}, "
            f"received_at={received_at}",
            flush=True,
        )
        send_to_nocobase(spo2, heart_rate, received_at)
    except (UnicodeDecodeError, json.JSONDecodeError, KeyError, TypeError,
            ValueError, sqlite3.Error) as error:
        print(f"訊息處理失敗：{error}", flush=True)

init_database()
client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
client.on_connect = on_connect
client.on_message = on_message

client.connect(BROKER_HOST, BROKER_PORT, 60)
client.loop_forever()