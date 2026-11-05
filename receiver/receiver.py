import os
import threading

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
                heart_rate INTEGER NOT NULL,
                sync_status TEXT NOT NULL DEFAULT 'legacy'
            )
            """
        )
        columns = {
            row[1] for row in connection.execute("PRAGMA table_info(vitals)")
        }
        if "sync_status" not in columns:
            connection.execute(
                "ALTER TABLE vitals "
                "ADD COLUMN sync_status TEXT NOT NULL DEFAULT 'legacy'"
            )

def send_to_nocobase(spo2, heart_rate, received_at):
    if not NOCOBASE_API_KEY:
        print("NocoBase 傳送失敗：尚未設定 API key", flush=True)
        return False

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
        return True
    except requests.RequestException as error:
        print(f"NocoBase 傳送失敗：{error}", flush=True)
        return False

def get_pending_records():
    with sqlite3.connect(DB_PATH) as connection:
        return connection.execute(
            "SELECT id, spo2, heart_rate, received_at "
            "FROM vitals WHERE sync_status = ? ORDER BY id LIMIT 50",
            ("pending",),
        ).fetchall()

def retry_pending_records():
    for record_id, spo2, heart_rate, received_at in get_pending_records():
        if not send_to_nocobase(spo2, heart_rate, received_at):
            break

        with sqlite3.connect(DB_PATH) as connection:
            connection.execute(
                "UPDATE vitals SET sync_status = ? WHERE id = ?",
                ("sent", record_id),
            )

def retry_worker(stop_event):
    while not stop_event.is_set():
        try:
            retry_pending_records()
        except sqlite3.Error as error:
            print(f"補傳資料庫錯誤：{error}", flush=True)

        stop_event.wait(10)

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
                "INSERT INTO vitals (received_at, spo2, heart_rate, sync_status) "
                "VALUES (?, ?, ?, ?)",
                (received_at, spo2, heart_rate, "pending"),
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

stop_event = threading.Event()
worker = threading.Thread(target=retry_worker, args=(stop_event,))
worker.start()

try:
    client.loop_forever()
except KeyboardInterrupt:
    print("正在停止接收程式…", flush=True)
finally:
    stop_event.set()
    worker.join()
    client.disconnect()
    print("接收程式已停止", flush=True)