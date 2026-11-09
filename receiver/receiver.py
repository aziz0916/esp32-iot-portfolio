import os
import uuid
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
NOCOBASE_LIST_URL = "http://host.docker.internal:13002/api/esp32_vitals:list"
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
                sync_status TEXT NOT NULL DEFAULT 'legacy',
                event_id TEXT
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
        if "event_id" not in columns:
            connection.execute(
                "ALTER TABLE vitals ADD COLUMN event_id TEXT"
            )
        pending_without_id = connection.execute(
            "SELECT id FROM vitals "
            "WHERE sync_status = ? AND event_id IS NULL",
            ("pending",),
        ).fetchall()
        for (record_id,) in pending_without_id:
            connection.execute(
                "UPDATE vitals SET event_id = ? WHERE id = ?",
                (str(uuid.uuid4()), record_id),
            )

def record_exists_in_nocobase(spo2, heart_rate, received_at, event_id):
    try:
        response = requests.get(
            NOCOBASE_LIST_URL,
            headers={"Authorization": f"Bearer {NOCOBASE_API_KEY}"},
            params={"filter": json.dumps({"event_id": event_id})},
            timeout=10,
        )
        response.raise_for_status()
        result = response.json()
        if not isinstance(result, dict):
            print("查詢既有資料失敗：回應格式不正確", flush=True)
            return "retry"
        records = result.get("data")

        if not isinstance(records, list) or len(records) != 1:
            print("無法確認既有資料：查詢結果不是一筆", flush=True)
            return "retry"

        record = records[0]
        if not isinstance(record, dict):
            print("查詢既有資料失敗：資料格式不正確", flush=True)
            return "retry"

        if not isinstance(received_at, str) or not isinstance(
            record.get("received_at"), str
        ):
            print("查詢既有資料失敗：接收時間格式不正確", flush=True)
            return "retry"
        if (
            record.get("event_id") != event_id
            or type(record.get("spo2")) is not int
            or type(record.get("heart_rate")) is not int
        ):
            print("查詢既有資料失敗：識別碼或資料格式不正確", flush=True)
            return "retry"
        # NocoBase 回傳的時間可能只保留到毫秒。
        local_time = datetime.fromisoformat(received_at.replace("Z", "+00:00"))
        remote_time = datetime.fromisoformat(
            record["received_at"].replace("Z", "+00:00")
        )
        if local_time.utcoffset() is None or remote_time.utcoffset() is None:
            raise ValueError("接收時間缺少時區")

        local_time = local_time.astimezone(timezone.utc)
        remote_time = remote_time.astimezone(timezone.utc)
        local_time = local_time.replace(
            microsecond=(local_time.microsecond // 1000) * 1000
        )
        remote_time = remote_time.replace(
            microsecond=(remote_time.microsecond // 1000) * 1000
        )

        matches = (
            record.get("event_id") == event_id
            and type(record.get("spo2")) is int
            and record["spo2"] == spo2
            and type(record.get("heart_rate")) is int
            and record["heart_rate"] == heart_rate
            and local_time == remote_time
        )

        if not matches:
            print("既有資料內容不相符", flush=True)
            return "conflict"

        return "match"
    except (requests.RequestException, ValueError, KeyError, TypeError) as error:
        print(f"查詢既有資料失敗：{error}", flush=True)
        return "retry"

def send_to_nocobase(spo2, heart_rate, received_at, event_id):
    if not NOCOBASE_API_KEY:
        print("NocoBase 傳送失敗：尚未設定 API key", flush=True)
        return "retry"

    try:
        response = requests.post(
            NOCOBASE_URL,
            headers={"Authorization": f"Bearer {NOCOBASE_API_KEY}"},
            json={
                "event_id": event_id,
                "spo2": spo2,
                "heart_rate": heart_rate,
                "received_at": received_at,
            },
            timeout=10,
        )
        if response.status_code == 400:
            result = record_exists_in_nocobase(
                spo2, heart_rate, received_at, event_id
            )
            if result == "match":
                print("既有資料核對相符，視為已傳送", flush=True)
                return "sent"
            if result == "conflict":
                return "conflict"
        response.raise_for_status()
        print("已傳送至 NocoBase", flush=True)
        return "sent"
    except requests.RequestException as error:
        print(f"NocoBase 傳送失敗：{error}", flush=True)
        return "retry"

def get_pending_records():
    with sqlite3.connect(DB_PATH) as connection:
        return connection.execute(
            "SELECT id, spo2, heart_rate, received_at, event_id "
            "FROM vitals WHERE sync_status = ? ORDER BY id LIMIT 50",
            ("pending",),
        ).fetchall()

def retry_pending_records():
    for record_id, spo2, heart_rate, received_at, event_id in get_pending_records():
        result = send_to_nocobase(spo2, heart_rate, received_at, event_id)

        if result not in ("sent", "conflict"):
            # 連線或查詢失敗時保留 pending，稍後再試。
            break

        with sqlite3.connect(DB_PATH) as connection:
            connection.execute(
                "UPDATE vitals SET sync_status = ? "
                "WHERE id = ? AND sync_status = ?",
                (result, record_id, "pending"),
            )

        if result == "conflict":
            print(f"資料 ID {record_id} 發生衝突，請檢查", flush=True)

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
        event_id = str(uuid.uuid4())

        with sqlite3.connect(DB_PATH) as connection:
            connection.execute(
                "INSERT INTO vitals "
                "(received_at, spo2, heart_rate, sync_status, event_id) "
                "VALUES (?, ?, ?, ?, ?)",
                (received_at, spo2, heart_rate, "pending", event_id),
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

stop_event = threading.Event()
worker = threading.Thread(target=retry_worker, args=(stop_event,))
worker.start()

try:
    while not stop_event.is_set():
        try:
            client.connect(BROKER_HOST, BROKER_PORT, 60)
            break
        except OSError as error:
            print(f"MQTT 連線失敗，10 秒後重試：{error}", flush=True)
            stop_event.wait(10)

    client.loop_forever()
except KeyboardInterrupt:
    print("正在停止接收程式…", flush=True)
finally:
    stop_event.set()
    worker.join()
    client.disconnect()
    print("接收程式已停止", flush=True)