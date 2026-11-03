# ESP32 IoT Portfolio

這是一個使用 ESP32 建立的物聯網練習專案，模擬生命徵象資料的接收與儲存。

## 目前資料流程

1. ESP32 發送端模擬血氧（SpO2）與心率資料，透過 BLE 提供給網關讀取。
2. ESP32-S3 網關讀取 BLE 資料，並透過 Wi-Fi 發布至 MQTT broker。
3. Python 接收程式訂閱 MQTT 資料，將結果儲存至本機 SQLite 資料庫。

目前資料為模擬數值，約每 5 秒傳送一次。

## 使用技術

- ESP-IDF：ESP32 發送端與 ESP32-S3 網關韌體
- Bluetooth Low Energy（BLE）：發送端與網關之間的通訊
- Wi-Fi 與 MQTT：網關發布資料至 broker
- Eclipse Mosquitto：本機 MQTT broker
- Python 與 SQLite：接收並儲存資料
- Docker Compose：啟動 MQTT broker

## MQTT 資料格式

網關發布至主題 `esp32/vitals`，資料範例如下：

```json
{"spo2": 98, "heart_rate": 72}
```

- `spo2`：血氧飽和度，單位為百分比（%）
- `heart_rate`：心率，單位為每分鐘心跳次數（bpm）

## 啟動 MQTT broker

先啟動 Docker Desktop，再於 WSL 的專案根目錄執行：

```bash
docker compose -f mqtt/compose.yaml up -d
```

目前 broker 使用 TCP port `1883`，並允許匿名連線，適合本機開發與測試。

## 啟動資料接收程式

在 Dev Container 的專案根目錄執行：

```bash
python3 -m venv .venv
.venv/bin/python -m pip install -r receiver/requirements.txt
.venv/bin/python receiver/receiver.py
```

預設 broker 位址為 `host.docker.internal:1883`，供 Dev Container 連線至本機 broker。若 broker 位於其他主機，可透過環境變數指定主機名稱或 IP：

```bash
MQTT_HOST=192.168.1.100 .venv/bin/python receiver/receiver.py
```

請將 `192.168.1.100` 換成實際的 broker 位址。收到資料後，程式會儲存至 `receiver/vitals.db`。

## 查看最近的資料

在 Dev Container 的專案根目錄執行：

```bash
.venv/bin/python -c "import sqlite3; c = sqlite3.connect('receiver/vitals.db'); print(*c.execute('SELECT * FROM vitals ORDER BY id DESC LIMIT 5'), sep='\n'); c.close()"
```

結果會顯示最近五筆資料，欄位依序為：編號、接收時間、血氧與心率。接收時間使用 UTC 時區。

## 設定 ESP32-S3 網關

在 `firmware/esp32s3_gateway` 目錄執行 `idf.py menuconfig`，
進入 **Gateway Wi-Fi Configuration**，設定：

- Wi-Fi SSID
- Wi-Fi password
- MQTT broker URI，例如 `mqtt://192.168.1.100:1883`

請將範例 IP 換成執行 broker 的電腦在區域網路上的實際 IP。
ESP32-S3 必須能透過網路連到該位址。設定完成後儲存，重新編譯並燒錄韌體。