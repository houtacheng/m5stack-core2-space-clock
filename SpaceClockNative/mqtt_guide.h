#pragma once

static const char MQTT_GUIDE_HTML[] PROGMEM = R"MQTTGUIDE(
<!doctype html><html lang="zh-Hant"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Core2 MQTT 使用指南</title><style>
:root{color-scheme:dark}body{font-family:system-ui,-apple-system,"Noto Sans TC",sans-serif;background:#08111f;color:#eef4ff;max-width:820px;margin:auto;padding:20px;line-height:1.65}h1,h2{color:#65b9ff}h2{margin-top:34px;border-bottom:1px solid #344b63;padding-bottom:6px}h3{color:#9cd2ff}code,pre{font-family:ui-monospace,SFMono-Regular,Menlo,monospace}code{background:#17263a;padding:2px 5px;border-radius:4px}pre{background:#101d2e;border:1px solid #344b63;border-radius:10px;padding:14px;overflow:auto;white-space:pre}table{width:100%;border-collapse:collapse;margin:12px 0}th,td{border:1px solid #344b63;padding:8px;text-align:left;vertical-align:top}th{background:#17263a}.note{background:#17263a;border-left:4px solid #f0c75e;padding:12px}.button{box-sizing:border-box;display:block;width:100%;padding:13px;margin:22px 0;border-radius:9px;background:#1688e5;color:white;text-align:center;text-decoration:none;font-size:17px}
</style></head><body>
<a class="button" href="/">返回設定頁</a><h1>Core2 MQTT 完整設定指南</h1>
<p>本機使用一般 MQTT TCP、QoS 0，預設連接埠為 <code>1883</code>，目前未使用 TLS。JSON 一律採 UTF-8。以下以預設 Base Topic <code>spaceclock/core2</code> 示範；若您修改 Base Topic，請同步替換所有 Topic 前綴。</p>

<h2>1. 啟用 MQTT</h2><ol><li>返回設定頁並找到 MQTT。</li><li>勾選 Enable MQTT。</li><li>輸入 Broker IP／主機名、Port、Username、Password 和 Base Topic。</li><li>按 Save settings，Core2 會自動連線。</li></ol>
<p class="note">Broker 必須允許 Core2 所在網路連線，防火牆需開放 TCP 1883。請勿將未加密的 1883 直接暴露至網際網路，建議只在區域網路或 VPN 中使用。</p>

<h2>2. Topic 一覽</h2><table><tr><th>Topic</th><th>方向</th><th>功能</th></tr>
<tr><td><code>spaceclock/core2/state</code></td><td>Core2 → Broker</td><td>每秒發布時間、靜心、電池及螢幕狀態；保留訊息</td></tr>
<tr><td><code>spaceclock/core2/status</code></td><td>Core2 → Broker</td><td>與 state 相同，供舊版整合使用；保留訊息</td></tr>
<tr><td><code>spaceclock/core2/settings</code></td><td>Core2 → Broker</td><td>完整設定，不含密碼；保留訊息</td></tr>
<tr><td><code>spaceclock/core2/set</code></td><td>Broker → Core2</td><td>以部分或完整 JSON 修改設定</td></tr>
<tr><td><code>spaceclock/core2/ack</code></td><td>Core2 → Broker</td><td>設定成功或錯誤回報</td></tr>
<tr><td><code>spaceclock/core2/command/#</code></td><td>Broker → Core2</td><td>即時操作指令</td></tr></table>

<h2>Home Assistant 自動加入</h2><p>啟用 MQTT 後，Core2 會自動發布 <a href="https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery">Home Assistant MQTT Discovery</a> 設定。Home Assistant 的 MQTT 整合啟用 Discovery 後，會自動出現 Space Clock Core2 裝置，以及電池、充電、螢幕、亮度、鬧鐘音量、小夜燈、鬧鐘燈、表盤、靜心開始與停止鬧鐘等實體。</p>
<p>Core2 本身沒有內建環境光感測器；因此「Ambient light」照度實體會被正確標示為不可用，而不是回報虛假的 lux 值。日後接上外接光感測器時可啟用該欄位。</p>

<h2>3. 即時狀態</h2><pre>{
  "time":"2026-09-13T08:35:42",
  "date":"2026-09-13",
  "weekday":"星期日 (SUN)",
  "clock_time":"08:35:42",
  "timezone":"Taipei",
  "time_format":24,
  "meditation":{
    "state":"running",
    "duration_seconds":300,
    "duration_text":"05:00",
    "elapsed_seconds":72,
    "elapsed_text":"01:12",
    "remaining_seconds":228,
    "remaining_text":"03:48"
  },
  "device":{
    "ip":"10.41.10.223",
    "battery_percent":83,
    "charging":true,
    "screen_on":true
  }
}</pre>
<p>靜心狀態：<code>ready</code> 尚未開始、<code>running</code> 計時中、<code>paused</code> 已暫停、<code>done</code> 時間到。</p>
<p>日期、星期、時間、時區可分別使用 JSON Path <code>date</code>、<code>weekday</code>、<code>clock_time</code>、<code>timezone</code>。舊的 <code>time</code> ISO 日期時間欄位會繼續保留。</p>
<p>靜心時間同時提供秒數與已格式化文字。<code>meditation.remaining_text</code> 可直接顯示成 <code>03:48</code>，超過一小時會顯示成 <code>01:03:48</code>。</p>

<h3>在 Companion 按鈕顯示剩餘時間</h3><ol>
<li>編輯按鈕並進入 Feedbacks，新增 MQTT 的 <b>Update variable with value from MQTT topic</b>；若有新版 <b>Get MQTT topic value</b>，也可使用新版。</li>
<li>Topic 填入 <code>spaceclock/core2/state</code>。</li>
<li>JSON Path 填入 <code>meditation.remaining_text</code>。</li>
<li>舊版 Feedback 的 Variable 填入 <code>remaining_text</code>，並保持右上角開關啟用。</li>
<li>不必編輯 Layered Styles Overrides。切換到按鈕上方的 <b>Style</b> 頁籤，在 Button text string 填入：</li></ol>
<pre>剩餘時間
$(mqtt:remaining_text)</pre>
<p><code>mqtt</code> 是 Generic MQTT 連線名稱；若您的連線使用其他名稱，請換成實際名稱。日期、星期、時間與時區也能用相同方式建立變數。</p>

<h2>4. 讀取與修改設定</h2><p>訂閱 <code>spaceclock/core2/settings</code> 可取得完整設定。也可發送 <code>get</code> 到 <code>spaceclock/core2/command/settings</code> 要求重新發布。</p>
<p>將 JSON 發至 <code>spaceclock/core2/set</code> 即可修改。只需提供要變更的欄位：</p><pre>{"time_format":12,"adaptive_brightness":true,"day_brightness":70}</pre>
<p>成功回覆：<code>{"ok":true}</code>；錯誤回覆：<code>{"ok":false,"error":"原因"}</code>。Wi-Fi 與 MQTT 密碼只能寫入，不會在 settings 中回傳。</p>

<h2>5. 時鐘與螢幕設定</h2><pre>{
  "timezone_index":18,
  "clock_face":1,
  "time_format":24,
  "flat_virtual_buttons":false,
  "adaptive_brightness":true,
  "day_brightness":80,
  "night_brightness":20,
  "screen_off_seconds":300
}</pre>
<table><tr><th>欄位</th><th>有效值</th></tr><tr><td>clock_face</td><td>0 太空表盤；1 翻頁表盤；2 Matrix code rain</td></tr><tr><td>time_format</td><td>12 或 24</td></tr><tr><td>day_brightness</td><td>10–100</td></tr><tr><td>night_brightness</td><td>5–100</td></tr><tr><td>screen_off_seconds</td><td>0–1800；0 表示永不關閉</td></tr></table>
<h3>時區 Index</h3><p>0 UTC、1 New York、2 Chicago、3 Denver、4 Los Angeles、5 Honolulu、6 Mexico City、7 Toronto、8 São Paulo、9 London、10 Paris、11 Berlin、12 Johannesburg、13 Dubai、14 Delhi、15 Bangkok、16 Singapore、17 Hong Kong、18 Taipei、19 Tokyo、20 Sydney、21 Auckland。</p>

<h2>6. 鬧鐘與音效</h2><pre>{"alarm_volume":80,"alarm_sound":1}</pre><p>音效代碼：0 打版、1 磬聲、2 流水聲、3 水滴聲。alarm_volume 範圍 10–100。</p>
<pre>{"alarms":[
  {"index":0,"hour":7,"minute":30,"enabled":true,"weekdays":62},
  {"index":1,"hour":9,"minute":0,"enabled":false,"weekdays":65}
]}</pre>
<p>鬧鐘 index 為 0–19。weekdays 是位元加總：星期日 1、星期一 2、星期二 4、星期三 8、星期四 16、星期五 32、星期六 64。週一至週五為 62、週末為 65、每天為 127、單次為 0。</p>

<h2>7. 鬧鐘燈光</h2><pre>{"alarm_light":{"enabled":true,"color":"#FFFFFF","brightness":35,"mode":0}}</pre>
<p>brightness 為 1–100。mode：0 持續呼吸、1 持續快閃、2 三輪呼吸、3 三輪快閃。</p>

<h2>8. 小夜燈</h2><pre>{"night_light":{"enabled":true,"color":"#FFF0C8","brightness":18,"mode":2,"seconds":60}}</pre>
<p>mode：0 螢幕關閉期間常亮、1 指定時間後關閉、2 指定時間後漸暗。brightness 為 1–100，seconds 為 5–3600。</p>

<h2>9. 靜心時鐘</h2><pre>{"meditation":{
  "preset_minutes":[5,15],
  "sound_enabled":true,
  "start_sound":1,"start_volume":55,
  "end_sound":1,"end_volume":70,
  "light_enabled":true,
  "noise_enabled":true,"noise":0,"noise_volume":25
}}</pre>
<p>preset_minutes 各為 1–180 分鐘；提示音 0 打版、1 磬聲、2 流水、3 水滴；start/end_volume 為 5–100。背景音 noise：0 流水、1 雨聲、2 夏夜蟲鳴；noise_volume 為 5–80。</p>

<h2>10. Companion 四頁</h2><pre>{"companion":[
  {"page":1,"host":"10.41.10.5","port":16622},
  {"page":2,"host":"10.41.10.6","port":16622},
  {"page":3,"host":"","port":16622},
  {"page":4,"host":"","port":16622}
]}</pre><p>page 為 1–4。設定改變後會自動重新連接 Companion。</p>

<h2>11. 修改 MQTT／Wi-Fi</h2><pre>{"mqtt":{
  "enabled":true,"host":"10.41.10.10","port":1883,
  "username":"core2","password":"密碼","base_topic":"spaceclock/core2"
}}</pre><p>修改 Broker 或 Base Topic 後，Core2 會使用新設定重新連線。</p>
<pre>{"wifi":{"ssid":"your-ssid","password":"Wi-Fi密碼"}}</pre><p class="note">修改 Wi-Fi 後舊 IP 可能失效，MQTT 也會暫時離線。建議 Wi-Fi 優先從設定網頁修改。</p>

<h2>12. 即時控制</h2><table><tr><th>Topic</th><th>Payload</th><th>功能</th></tr>
<tr><td>command/screen</td><td>on、wake、off</td><td>喚醒／關閉螢幕</td></tr>
<tr><td>command/brightness</td><td>10–100</td><td>關閉自動亮度並設定亮度</td></tr>
<tr><td>command/page</td><td>clock、meditation、companion1–4</td><td>切換畫面</td></tr>
<tr><td>command/meditation</td><td>start1、start2、pause、resume、reset、stop</td><td>控制靜心時鐘</td></tr>
<tr><td>command/alarm</td><td>stop、dismiss、snooze</td><td>停止或貪睡正在響的鬧鐘</td></tr>
<tr><td>command/settings</td><td>get</td><td>重新發布完整設定</td></tr></table><p>表中的 Topic 前方均需加上 Base Topic，例如 <code>spaceclock/core2/command/screen</code>。</p>

<h2>13. Mosquitto 範例</h2><pre>mosquitto_sub -h 10.41.10.10 -u core2 -P '密碼' \
  -t 'spaceclock/core2/#' -v</pre>
<pre>mosquitto_pub -h 10.41.10.10 -u core2 -P '密碼' \
  -t 'spaceclock/core2/command/meditation' -m 'start1'</pre>
<pre>mosquitto_pub -h 10.41.10.10 -u core2 -P '密碼' \
  -t 'spaceclock/core2/set' \
  -m '{"alarms":[{"index":0,"hour":7,"minute":30,"enabled":true,"weekdays":62}]}'</pre>

<h2>14. 完整設定範例</h2><pre>{
  "timezone_index":18,"clock_face":1,"time_format":24,
  "flat_virtual_buttons":false,"adaptive_brightness":true,
  "day_brightness":80,"night_brightness":20,"screen_off_seconds":300,
  "alarm_volume":80,"alarm_sound":1,
  "night_light":{"enabled":true,"color":"#FFF0C8","brightness":18,"mode":2,"seconds":60},
  "alarm_light":{"enabled":true,"color":"#FFFFFF","brightness":35,"mode":0},
  "meditation":{"preset_minutes":[5,15],"sound_enabled":true,"start_sound":1,"start_volume":55,"end_sound":1,"end_volume":70,"light_enabled":true,"noise_enabled":false,"noise":0,"noise_volume":25},
  "alarms":[{"index":0,"hour":7,"minute":30,"enabled":true,"weekdays":62}],
  "companion":[{"page":1,"host":"10.41.10.5","port":16622}]
}</pre>
<a class="button" href="/">返回設定頁</a></body></html>
)MQTTGUIDE";
