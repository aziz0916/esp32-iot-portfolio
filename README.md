# ESP32 IoT Portfolio

這是一個使用 ESP32 建立的物聯網練習專案，模擬生命徵象資料的接收與儲存。

## 目前資料流程

1. ESP32 發送端模擬血氧（SpO2）與心率資料，透過 BLE 提供給網關讀取。
2. ESP32-S3 網關讀取 BLE 資料，並透過 Wi-Fi 發布至 MQTT broker。
3. Python 接收程式訂閱 MQTT 資料，將結果儲存至本機 SQLite 資料庫，並透過 API 傳送至 NocoBase。

目前資料為模擬數值，約每 5 秒傳送一次。

## 使用技術

- ESP-IDF：ESP32 發送端與 ESP32-S3 網關韌體
- Bluetooth Low Energy（BLE）：發送端與網關之間的通訊
- Wi-Fi 與 MQTT：網關發布資料至 broker
- Eclipse Mosquitto：本機 MQTT broker
- Python 與 SQLite：接收並儲存資料
- NocoBase 與 PostgreSQL：透過 API 接收並顯示生命徵象資料
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

## 啟動 NocoBase

先啟動 Docker Desktop，再於 WSL 的專案根目錄執行：

```bash
cp -n nocobase/.env.example nocobase/.env
```

編輯 `nocobase/.env`，自行設定隨機的 `APP_KEY` 和資料庫密碼 `DB_PASSWORD`。請勿將 `.env` 提交到 Git。

設定完成後，執行：

```bash
docker compose -f nocobase/compose.yaml up -d
```

啟動後，在瀏覽器開啟 `http://localhost:13002`。

## 啟動資料接收程式

先在 NocoBase 手動建立 `esp32_vitals` collection，加入以下欄位：

- `spo2`：整數，血氧百分比
- `heart_rate`：整數，心率（bpm）
- `received_at`：日期與時間
- `event_id`：單行文字，啟用 Unique（唯一）索引

接著建立 API key，並設定其角色對此 collection 的權限：

- 允許新增資料，並允許新增上述四個欄位。
- 允許查看資料，並允許查看上述四個欄位，供重試時核對既有紀錄。查看範圍可設為 Own records；此範圍依資料建立者判斷，同一使用者透過其他方式建立的資料也可能包含在內。API key 所屬使用者必須能查看先前寫入的紀錄，才能完成核對。

在 Dev Container 的專案根目錄建立虛擬環境並安裝套件：

```bash
python3 -m venv .venv
.venv/bin/python -m pip install -r receiver/requirements.txt
```

在同一個終端機安全輸入 API key：

```bash
export NOCOBASE_API_KEY="$(.venv/bin/python -c 'import getpass; print(getpass.getpass("請貼上 API key：").strip())')"
```

輸入時畫面不會顯示字元。請勿將 API key 寫入程式碼或提交到 Git。

啟動接收程式：

```bash
.venv/bin/python receiver/receiver.py
```

預設 broker 位址為 `host.docker.internal:1883`。若 broker 位於其他主機，可在啟動時指定主機名稱或 IP：

```bash
MQTT_HOST=192.168.1.100 .venv/bin/python receiver/receiver.py
```

請將範例 IP 換成實際的 broker 位址。收到資料後，程式會先儲存至 `receiver/vitals.db`，再傳送至 NocoBase。

如果 NocoBase 暫時無法連線，新資料會保留在 SQLite 中，狀態為 `pending`。接收程式執行時，背景工作會定期嘗試補傳，每次最多處理 50 筆；不需要等待新的 MQTT 訊息。傳送成功後，狀態會更新為 `sent`。

如果啟動接收程式時 MQTT broker 無法連線，程式會每 10 秒重新嘗試連線；背景補傳仍會處理 SQLite 中的待傳送資料。MQTT 恢復連線並訂閱後，程式才會繼續接收新的 MQTT 訊息。

更新功能前的舊資料標記為 `legacy`，不會自動補傳。

每筆新資料會產生並保存唯一的 `event_id`。如果 NocoBase 已寫入資料但回應遺失，程式重試時會查詢既有紀錄；只有識別碼、血氧、心率和接收時間都相符，才會將本機資料標記為 `sent`。若查詢失敗或無法確認資料，紀錄會維持 `pending`，稍後再試。若確認相同識別碼的內容不符，紀錄會標記為 `conflict` 並保留供人工檢查；程式會繼續處理後續待補傳資料。

NocoBase 必須將 `event_id` 設為唯一索引，才能防止相同識別碼重複新增。啟用此功能前已存在但缺少 `event_id` 的舊資料，無法透過此機制可靠判斷是否已傳送。

## 查看最近的資料

在 Dev Container 的專案根目錄執行：

```bash
.venv/bin/python -c "import sqlite3; c = sqlite3.connect('receiver/vitals.db'); print(*c.execute('SELECT * FROM vitals ORDER BY id DESC LIMIT 5'), sep='\n'); c.close()"
```

結果會顯示最近五筆資料，欄位依序為：編號、接收時間、血氧、心率、同步狀態與資料識別碼。接收時間使用 UTC 時區。

## 查看衝突資料

當本機資料與 NocoBase 中相同 `event_id` 的紀錄內容不符時，接收程式會將本機紀錄標記為 `conflict`。這些紀錄不會自動重試，需人工核對。

在 Dev Container 的專案根目錄執行：

```bash
.venv/bin/python - <<'PY'
import sqlite3

with sqlite3.connect("file:receiver/vitals.db?mode=ro", uri=True) as connection:
    rows = connection.execute(
        """
        SELECT id, event_id, received_at, spo2, heart_rate
        FROM vitals WHERE sync_status = 'conflict'
        ORDER BY id
        """
    ).fetchall()
    for row in rows:
        print(row)
    print("衝突筆數：", len(rows))
PY
```

欄位依序為本機編號、資料識別碼、接收時間、血氧與心率。請使用 `event_id` 到 NocoBase 核對紀錄，再依實際情況決定如何處理；不要直接將衝突紀錄視為已傳送。

處理衝突時，先保留兩邊資料並查明差異原因。如果確認本機紀錄與遠端是不同事件，應先確認資料來源，避免直接改用新識別碼造成重複。若修正原因後需要重新嘗試，也應先備份資料庫，再審慎處理該筆紀錄；本程式不會自動修改 `conflict` 紀錄。

## 設定 ESP32-S3 網關

在 `firmware/esp32s3_gateway` 目錄執行 `idf.py menuconfig`，
進入 **Gateway Wi-Fi Configuration**，設定：

- Wi-Fi SSID
- Wi-Fi password
- MQTT broker URI，例如 `mqtt://192.168.1.100:1883`

請將範例 IP 換成執行 broker 的電腦在區域網路上的實際 IP。
ESP32-S3 必須能透過網路連到該位址。設定完成後儲存，重新編譯並燒錄韌體。