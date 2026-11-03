import os

import paho.mqtt.client as mqtt

import sqlite3
from pathlib import Path

import json
from datetime import datetime, timezone


BROKER_HOST = os.environ.get("MQTT_HOST", "host.docker.internal")
BROKER_PORT = 1883
TOPIC = "esp32/vitals"
DB_PATH = Path(__file__).resolve().parent / "vitals.db"

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
    except (UnicodeDecodeError, json.JSONDecodeError, KeyError, TypeError,
            ValueError, sqlite3.Error) as error:
        print(f"訊息處理失敗：{error}", flush=True)

init_database()
client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
client.on_connect = on_connect
client.on_message = on_message

client.connect(BROKER_HOST, BROKER_PORT, 60)
client.loop_forever()