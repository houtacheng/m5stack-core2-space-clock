"""Signal -> Space Clock bridge with an admin page.

Core2 API (token required, works on the LAN and through Cloudflare Tunnel):
  GET  /api/messages?since=<id>          -> {"latest": id, "messages": [...]}
  POST /api/reply {"id": n, "text": "..."} -> reply to that chat
  Token: header "X-Token: <token>" or query "?token=<token>".

Admin (LAN only, never through the tunnel):
  GET  /            status, Signal link QR, URLs, token, recent messages
"""
import asyncio, base64, html, ipaddress, json, os, secrets, threading, time, urllib.parse, urllib.request
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import websockets

SIGNAL = os.environ.get("SIGNAL_API", "http://signal-cli:8080")
STORE = "/data/messages.json"
CONFIG = "/data/config.json"
MAX_KEEP = 500

lock = threading.Lock()
reply_dedupe = {}
reply_dedupe_lock = threading.Lock()
messages = []  # pruned by prune(): oldest READ messages go first, unread are kept
next_id = 1
groups = {}
config = {"token": "", "lan_url": "http://10.41.10.5:18081", "public_url": "", "account": "",
          "teams_tenant": "", "teams_client": "", "teams_refresh": "", "teams_user": "", "teams_user_id": ""}
status = {"connected": False, "last_event": 0}


def load():
    global next_id
    try:
        with open(STORE, encoding="utf-8") as f:
            data = json.load(f)
        messages.extend(data.get("messages", []))
        for m in messages:  # backfill conversation fields for older records
            m.setdefault("chat", m.get("_to", ""))
            m.setdefault("chat_name", m.get("group") or (m.get("_to", "") if m.get("own") else m.get("from", "")))
            m.setdefault("read", True)
            m.setdefault("_sender", None if m.get("own") or m.get("group") else m.get("_to"))
        next_id = data.get("next_id", 1)
    except FileNotFoundError:
        pass
    try:
        with open(CONFIG, encoding="utf-8") as f:
            config.update(json.load(f))
    except FileNotFoundError:
        pass
    if not config["token"]:
        config["token"] = secrets.token_urlsafe(24)
        save_config()


def send_read_receipts(items):
    """Mark messages read on Signal itself, so the phone/desktop show them read too."""
    acct = account()
    for m in items:
        if not m.get("_sender") or not m.get("ts"):
            continue
        try:
            api("/v1/receipts/" + urllib.parse.quote(acct),
                {"receipt_type": "read", "recipient": m["_sender"], "timestamp": m["ts"]})
        except Exception as e:
            print("read receipt failed:", e, flush=True)
    if items:
        print("read receipts sent:", len(items), flush=True)


def prune():
    """Keep at most MAX_KEEP messages; delete the oldest read ones, never unread."""
    while len(messages) > MAX_KEEP:
        oldest_read = next((i for i, m in enumerate(messages) if m.get("read", True)), None)
        if oldest_read is None:
            break
        del messages[oldest_read]


def save():
    tmp = STORE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump({"next_id": next_id, "messages": list(messages)}, f, ensure_ascii=False)
    os.replace(tmp, STORE)


def save_config():
    tmp = CONFIG + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(config, f, ensure_ascii=False, indent=2)
    os.replace(tmp, CONFIG)


def api(path, body=None, raw=False, timeout=20):
    req = urllib.request.Request(SIGNAL + path, method="POST" if body is not None else "GET",
                                 data=json.dumps(body).encode() if body is not None else None,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        data = r.read()
        if raw:
            return data
        return json.loads(data) if data else None


def account():
    """The linked Signal number (auto-detected from signal-cli)."""
    try:
        accounts = api("/v1/accounts", timeout=5) or []
        if accounts and accounts[0] != config.get("account"):
            config["account"] = accounts[0]
            save_config()
    except Exception:
        pass
    return config.get("account") or ""


def refresh_groups():
    acct = account()
    if not acct:
        return
    try:
        for g in api("/v1/groups/" + urllib.parse.quote(acct), timeout=30) or []:
            groups[g.get("internal_id")] = {"id": g.get("id"), "name": g.get("name") or "群組"}
    except Exception as e:
        print("group refresh failed:", e, flush=True)


contacts = {}  # number -> display name (from signal-cli and incoming messages)


def refresh_contacts():
    acct = account()
    if not acct:
        return
    try:
        for c in api("/v1/contacts/" + urllib.parse.quote(acct), timeout=30) or []:
            name = c.get("name") or c.get("profile_name") or (c.get("profile") or {}).get("given_name")
            if c.get("number") and name:
                contacts[c["number"]] = name
    except Exception as e:
        print("contact refresh failed:", e, flush=True)


def contact_name(number):
    if number and number not in contacts:
        refresh_contacts()
    return contacts.get(number) or number or "未知"


def group_of(info):
    if not info:
        return None
    gid = info.get("groupId")
    if gid not in groups:
        refresh_groups()
    return groups.get(gid, {"id": None, "name": "群組"}) | {"internal": gid}


def add(envelope):
    global next_id
    status["last_event"] = int(time.time())
    read_sync = (envelope.get("syncMessage") or {}).get("readMessages")
    if read_sync:
        # Read on the phone or desktop: mirror it so every Core2 shows it read.
        keys = {((r.get("senderNumber") or r.get("sender")), r.get("timestamp")) for r in read_sync}
        stamps = {r.get("timestamp") for r in read_sync}
        changed = 0
        with lock:
            for m in messages:
                if not m.get("read") and ((m.get("_sender"), m.get("ts")) in keys or m.get("ts") in stamps):
                    m["read"] = True
                    changed += 1
            if changed:
                save()
        if changed:
            print("marked read from other device:", changed, flush=True)
        return
    data = envelope.get("dataMessage")
    sent = (envelope.get("syncMessage") or {}).get("sentMessage")
    msg = data or sent
    if not msg:
        return
    text = (msg.get("message") or "").strip()
    attachments = len(msg.get("attachments") or [])
    if not text and not attachments:
        return  # reactions, receipts, typing, etc.
    group = group_of(msg.get("groupInfo"))
    own = sent is not None
    peer_number = (sent.get("destinationNumber") or sent.get("destination")) if own else \
        (envelope.get("sourceNumber") or envelope.get("source"))
    if not own and envelope.get("sourceName") and peer_number:
        contacts[peer_number] = envelope["sourceName"]
    chat = (group or {}).get("id") or (group or {}).get("internal") or peer_number or ""
    item = {
        "chat": chat,
        "chat_name": group["name"] if group else contact_name(peer_number),
        "ts": msg.get("timestamp") or envelope.get("timestamp") or int(time.time() * 1000),
        "from": "我" if own else (envelope.get("sourceName") or envelope.get("sourceNumber") or ""),
        "own": own,
        "group": group["name"] if group else "",
        "text": text[:600],
        "attachments": attachments,
        "read": own,  # own messages are read; incoming stay unread until opened
        "_to": (group or {}).get("id") or peer_number,
        "_sender": None if own else peer_number,  # for read receipts
    }
    with lock:
        item["id"] = next_id
        next_id += 1
        messages.append(item)
        prune()
        save()
    print("message", item["id"], "group" if group else "direct", "own" if own else "in", flush=True)


async def receive_loop():
    while True:
        acct = account()
        if not acct:
            status["connected"] = False
            await asyncio.sleep(5)
            continue
        url = SIGNAL.replace("http", "ws", 1) + "/v1/receive/" + urllib.parse.quote(acct)
        try:
            async with websockets.connect(url, ping_interval=30, max_size=None) as ws:
                status["connected"] = True
                print("connected to signal-cli", flush=True)
                async for raw in ws:
                    try:
                        add(json.loads(raw).get("envelope") or {})
                    except Exception as e:
                        print("bad envelope:", e, flush=True)
        except Exception as e:
            print("websocket error, retrying:", e, flush=True)
        status["connected"] = False
        await asyncio.sleep(5)




# ---------------------------------------------------------------------------
# Microsoft Teams (work/school) via Microsoft Graph, delegated device-code login
# ---------------------------------------------------------------------------
GRAPH = "https://graph.microsoft.com/v1.0"
TEAMS_SCOPES = "offline_access User.Read Chat.ReadWrite"
teams_state = {"access": "", "expires": 0, "device": None, "error": "", "last_poll": 0,
               "chat_seen": {}, "chat_names": {}, "baseline": 0}
TEAMS_SEEN = "/data/teams_seen.json"
teams_wake = threading.Event()  # set by /api/sync to poll Teams right away
teams_poll_lock = threading.Lock()  # the loop and /api/sync must not poll at once


def load_teams_seen():
    try:
        with open(TEAMS_SEEN, encoding="utf-8") as f:
            teams_state["chat_seen"] = json.load(f)
        teams_state["baseline"] = 1  # resume where we stopped; no messages lost across restarts
    except (FileNotFoundError, ValueError):
        pass


def save_teams_seen():
    tmp = TEAMS_SEEN + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(teams_state["chat_seen"], f)
    os.replace(tmp, TEAMS_SEEN)


def http_json(url, data=None, headers=None, method=None, form=False, timeout=20):
    body = None
    hdrs = dict(headers or {})
    if data is not None:
        if form:
            body = urllib.parse.urlencode(data).encode()
            hdrs["Content-Type"] = "application/x-www-form-urlencoded"
        else:
            body = json.dumps(data).encode()
            hdrs["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=body, headers=hdrs, method=method or ("POST" if body else "GET"))
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            raw = r.read()
            return r.status, (json.loads(raw) if raw else {})
    except urllib.error.HTTPError as e:
        raw = e.read()
        try:
            return e.code, json.loads(raw)
        except Exception:
            return e.code, {"error": raw.decode(errors="replace")[:300]}


def teams_token_url():
    return f"https://login.microsoftonline.com/{config['teams_tenant']}/oauth2/v2.0/token"


def teams_start_login():
    """Begin device-code login; the admin page shows the code to enter."""
    code, res = http_json(f"https://login.microsoftonline.com/{config['teams_tenant']}/oauth2/v2.0/devicecode",
                          {"client_id": config["teams_client"], "scope": TEAMS_SCOPES}, form=True)
    if code != 200:
        teams_state["error"] = res.get("error_description") or str(res)
        return
    teams_state["device"] = {"code": res["device_code"], "user_code": res["user_code"],
                             "url": res["verification_uri"], "interval": res.get("interval", 5),
                             "expires": time.time() + res.get("expires_in", 900)}
    teams_state["error"] = ""
    threading.Thread(target=teams_wait_login, daemon=True).start()


def teams_store_tokens(res):
    teams_state["access"] = res["access_token"]
    teams_state["expires"] = time.time() + int(res.get("expires_in", 3600)) - 120
    if res.get("refresh_token"):
        config["teams_refresh"] = res["refresh_token"]
        save_config()


def teams_wait_login():
    dev = teams_state["device"]
    while dev and time.time() < dev["expires"]:
        time.sleep(dev["interval"])
        code, res = http_json(teams_token_url(), {"grant_type": "urn:ietf:params:oauth:grant-type:device_code",
                                                  "client_id": config["teams_client"], "device_code": dev["code"]}, form=True)
        if code == 200:
            teams_store_tokens(res)
            teams_state["device"] = None
            me = teams_graph("GET", "/me")
            config["teams_user"] = me.get("userPrincipalName") or me.get("displayName") or ""
            config["teams_user_id"] = me.get("id", "")
            save_config()
            teams_state["baseline"] = 0  # establish a fresh baseline: only new messages from now on
            print("teams signed in:", config["teams_user"], flush=True)
            return
        if res.get("error") not in ("authorization_pending", "slow_down"):
            teams_state["error"] = res.get("error_description") or res.get("error") or "login failed"
            teams_state["device"] = None
            return
    teams_state["device"] = None


def teams_access_token():
    if teams_state["access"] and time.time() < teams_state["expires"]:
        return teams_state["access"]
    if not config.get("teams_refresh"):
        return ""
    code, res = http_json(teams_token_url(), {"grant_type": "refresh_token", "client_id": config["teams_client"],
                                              "refresh_token": config["teams_refresh"], "scope": TEAMS_SCOPES}, form=True)
    if code != 200:
        teams_state["error"] = "Teams 登入已失效，請重新登入：" + (res.get("error_description") or "")[:120]
        config["teams_refresh"] = ""
        save_config()
        return ""
    teams_store_tokens(res)
    return teams_state["access"]


def teams_graph(method, path, data=None):
    token = teams_access_token()
    if not token:
        return {}
    code, res = http_json(GRAPH + path if path.startswith("/") else path, data,
                          {"Authorization": "Bearer " + token}, method=method)
    if code >= 400:
        raise RuntimeError(f"Graph {code}: {str(res)[:200]}")
    return res


def html_to_text(content):
    import re
    text = re.sub(r"<br\s*/?>|</p>|</div>", "\n", content or "", flags=re.I)
    text = re.sub(r"<[^>]+>", "", text)
    return html.unescape(text).strip()


def teams_chat_name(chat):
    cid = chat["id"]
    if chat.get("topic"):
        return chat["topic"]
    if cid in teams_state["chat_names"]:
        return teams_state["chat_names"][cid]
    try:
        members = teams_graph("GET", f"/chats/{cid}/members").get("value", [])
        names = [m.get("displayName") for m in members if m.get("userId") != config["teams_user_id"] and m.get("displayName")]
        name = "、".join(names[:3]) or "Teams 聊天"
    except Exception:
        name = "Teams 聊天"
    teams_state["chat_names"][cid] = name
    return name


def teams_add(chat, msg):
    global next_id
    if msg.get("messageType") != "message" or msg.get("deletedDateTime"):
        return
    content = (msg.get("body") or {}).get("content") or ""
    text = html_to_text(content)
    attachments = len(msg.get("attachments") or [])
    if not text and "<img" in content.lower():
        text = "[圖片]"  # pasted/inline images carry no text and no attachment entry
    if not text and not attachments:
        return
    sender = ((msg.get("from") or {}).get("user") or {})
    own = sender.get("id") == config["teams_user_id"]
    ts = int(time.mktime(time.strptime(msg["createdDateTime"][:19], "%Y-%m-%dT%H:%M:%S")) - time.timezone) * 1000
    name = "Teams · " + teams_chat_name(chat)
    item = {"chat": "teams:" + chat["id"], "chat_name": name, "ts": ts,
            "from": "我" if own else (sender.get("displayName") or "Teams"), "own": own,
            "group": name if chat.get("chatType") != "oneOnOne" else "", "text": text[:600],
            "attachments": attachments, "read": own, "_to": "teams:" + chat["id"], "_sender": None,
            "_source": "teams", "_teams_id": msg.get("id"), "_created": msg.get("createdDateTime", "")}
    with lock:
        if any(m.get("_teams_id") == item["_teams_id"] for m in messages):
            return
        item["id"] = next_id
        next_id += 1
        messages.append(item)
        prune()
        save()
    print("teams message", item["id"], "own" if own else "in", flush=True)


def teams_poll_once():
    with teams_poll_lock:
        teams_poll_locked()


def teams_poll_locked():
    if not (config.get("teams_client") and config.get("teams_tenant") and config.get("teams_refresh")):
        return
    res = teams_graph("GET", "/me/chats?$expand=lastMessagePreview&$orderby=lastMessagePreview/createdDateTime%20desc&$top=30")
    chats = res.get("value", [])
    first = not teams_state["baseline"]
    changed = False
    for chat in chats:
        preview = chat.get("lastMessagePreview") or {}
        stamp = preview.get("createdDateTime") or ""
        read_until = (chat.get("viewpoint") or {}).get("lastMessageReadDateTime") or ""
        seen = teams_state["chat_seen"].get(chat["id"])
        if first:
            # Very first run: remember where each chat is; nothing is imported.
            teams_state["chat_seen"][chat["id"]] = stamp
            changed = True
            continue
        if stamp and stamp != seen:
            msgs = teams_graph("GET", f"/chats/{chat['id']}/messages?$top=20&$orderby=createdDateTime%20desc").get("value", [])
            for msg in reversed(msgs):
                if not seen or msg.get("createdDateTime", "") > seen:
                    teams_add(chat, msg)
            # Only now is it safe to move the marker (a failed fetch retries next time).
            teams_state["chat_seen"][chat["id"]] = stamp
            changed = True
        if read_until:
            key = "teams:" + chat["id"]
            with lock:
                marked = 0
                for m in messages:
                    if m.get("chat") == key and not m.get("read") and m.get("_created", "") and m["_created"] <= read_until:
                        m["read"] = True
                        marked += 1
                if marked:
                    save()
    if changed:
        save_teams_seen()
    teams_state["baseline"] = 1
    teams_state["last_poll"] = int(time.time())
    teams_state["error"] = ""


def teams_backfill(hours=24):
    """One-off: import messages from the last `hours` that were missed; mark them read."""
    cutoff = time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(time.time() - hours * 3600))
    chats = teams_graph("GET", "/me/chats?$expand=lastMessagePreview&$orderby=lastMessagePreview/createdDateTime%20desc&$top=30").get("value", [])
    added = 0
    for chat in chats:
        if ((chat.get("lastMessagePreview") or {}).get("createdDateTime") or "") < cutoff:
            continue
        msgs = teams_graph("GET", f"/chats/{chat['id']}/messages?$top=30&$orderby=createdDateTime%20desc").get("value", [])
        for msg in reversed(msgs):
            if msg.get("createdDateTime", "") >= cutoff and not any(m.get("_teams_id") == msg.get("id") for m in messages):
                before = len(messages)
                teams_add(chat, msg)
                if len(messages) > before:
                    messages[-1]["read"] = True  # history, not new
                    added += 1
    with lock:
        messages.sort(key=lambda m: m["ts"])  # keep chronological order after backfill
        save()
    print("teams backfill added", added, flush=True)
    return added


def teams_loop():
    load_teams_seen()
    if not os.path.exists("/data/teams_backfill_done"):
        try:
            teams_backfill()
            open("/data/teams_backfill_done", "w").close()
        except Exception as e:
            print("teams backfill failed:", e, flush=True)
    while True:
        try:
            teams_poll_once()
        except Exception as e:
            teams_state["error"] = str(e)[:200]
            print("teams poll failed:", e, flush=True)
        teams_wake.wait(10)  # every 10 s, or immediately after /api/sync
        teams_wake.clear()


def teams_send(chat_key, text):
    teams_graph("POST", f"/chats/{chat_key[6:]}/messages", {"body": {"contentType": "text", "content": text}})


def teams_mark_read(chat_key):
    try:
        teams_graph("POST", f"/chats/{chat_key[6:]}/markChatReadForUser",
                    {"user": {"id": config["teams_user_id"], "tenantId": config["teams_tenant"]}})
    except Exception as e:
        print("teams mark read failed:", e, flush=True)


PAGE_CSS = """body{font-family:system-ui,-apple-system,sans-serif;background:#08111f;color:#eef4ff;margin:0;padding:16px}
main{max-width:760px;margin:auto}h1{color:#6fb6ff;margin:.2em 0}h2{color:#9ecbff;border-bottom:1px solid #26384f;padding-bottom:4px}
.card{background:#101d31;border:1px solid #26384f;border-radius:12px;padding:14px 16px;margin:12px 0}
label{display:block;margin:10px 0 4px;color:#b8c7da}input{box-sizing:border-box;width:100%;padding:10px;border-radius:8px;border:1px solid #3a4f69;background:#0b1626;color:#fff;font-size:15px}
button{background:#1688e5;color:#fff;border:0;border-radius:9px;padding:10px 16px;font-size:15px;margin-top:10px;cursor:pointer}
button.secondary{background:#26384f}.ok{color:#70e39a}.bad{color:#ff8a80}.muted{color:#8aa0b8;font-size:14px}
code{background:#0b1626;padding:3px 6px;border-radius:6px;word-break:break-all}.msg{border-top:1px solid #1d2c40;padding:8px 0}
.qr{background:#fff;padding:10px;border-radius:10px;display:inline-block;margin-top:10px}"""


def render_admin(message=""):
    acct = account()
    esc = html.escape
    rows = []
    with lock:
        recent = list(messages)[-15:][::-1]
    for m in recent:
        when = time.strftime("%m/%d %H:%M", time.localtime(m["ts"] / 1000))
        who = esc(m["from"]) + (" · " + esc(m["group"]) if m["group"] else "")
        body = esc(m["text"]) + (f" [附件 {m['attachments']}]" if m["attachments"] else "")
        rows.append(f"<div class='msg'><span class='muted'>{when} · {who}</span><br>{body}</div>")
    linked = f"<span class='ok'>已連結：{esc(acct)}</span>" if acct else "<span class='bad'>尚未連結 Signal 帳號</span>"
    conn = "<span class='ok'>即時接收中</span>" if status["connected"] else "<span class='bad'>未連線</span>"
    if config.get("teams_refresh"):
        last = time.strftime("%H:%M:%S", time.localtime(teams_state["last_poll"])) if teams_state["last_poll"] else "尚未"
        teams_status = f"<span class='ok'>已登入：{esc(config.get('teams_user', ''))}</span> · 最近檢查 {last}"
    else:
        teams_status = "<span class='bad'>尚未登入</span>"
    if teams_state["error"]:
        teams_status += f"<br><span class='bad'>{esc(teams_state['error'])}</span>"
    dev = teams_state.get("device")
    teams_device = (f"<p>請在手機或電腦開啟 <a href='{esc(dev['url'])}' target='_blank'>{esc(dev['url'])}</a>，輸入代碼 "
                    f"<code style='font-size:22px'>{esc(dev['user_code'])}</code>，用公司帳號登入並同意。完成後重新整理本頁。</p>") if dev else ""
    return f"""<!doctype html><html lang="zh-Hant"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Signal 轉接後台</title><style>{PAGE_CSS}</style><main>
<h1>Signal → Space Clock 轉接後台</h1>{f"<p class='ok'>{esc(message)}</p>" if message else ""}
<div class="card"><h2>Signal 帳號</h2><p>{linked} · {conn}</p>
<p class="muted">手機 Signal → 設定 → 已連結的裝置 → 連結新裝置，然後掃描下方 QR code。QR code 幾分鐘內有效；重新連結前，請先在手機移除舊的「SpaceClock」。</p>
<form method="get" action="/"><input type="hidden" name="qr" value="1"><button>產生連結用 QR code</button></form>
{"<div class='qr'><img src='/admin/qr?t=" + str(int(time.time())) + "' width='256' height='256' alt='Signal QR'></div>" if message == "__qr__" else ""}
</div>
<div class="card"><h2>Microsoft Teams（公司/學校帳號）</h2>
<p>{teams_status}</p>
<form method="post" action="/admin/teams">
<label>目錄（租用戶）識別碼 Tenant ID</label><input name="teams_tenant" value="{esc(config['teams_tenant'])}" placeholder="xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx">
<label>應用程式（用戶端）識別碼 Client ID</label><input name="teams_client" value="{esc(config['teams_client'])}" placeholder="xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx">
<button>儲存</button></form>
<form method="post" action="/admin/teams-login"><button class="secondary">登入 Teams</button></form>
{teams_device}
<p class="muted">登入後只接收之後的新訊息。聊天會以「Teams · 名稱」出現在 Core2 的訊息列表。</p></div>
<div class="card"><h2>Core2 連線設定</h2>
<form method="post" action="/admin/save">
<label>內網網址（Core2 在家時使用）</label><input name="lan_url" value="{esc(config['lan_url'])}" placeholder="http://10.41.10.5:18081">
<label>外網網址（Cloudflare Tunnel，Core2 在外面時使用）</label><input name="public_url" value="{esc(config['public_url'])}" placeholder="https://signal.theoakhouse.org">
<button>儲存</button></form>
<label>存取權杖（貼到 Core2 網頁後台「Signal」分頁）</label><code>{esc(config['token'])}</code>
<form method="post" action="/admin/token" onsubmit="return confirm('重新產生後，所有 Core2 都要貼上新的權杖，確定嗎？')"><button class="secondary">重新產生權杖</button></form>
<p class="muted">Core2 會先試內網網址，連不到時改用外網網址。外網請求一律需要權杖；後台頁面只能從區網開啟。</p></div>
<div class="card"><h2>最近訊息（{len(messages)} 則已保存）</h2>{"".join(rows) or "<p class='muted'>尚無訊息</p>"}</div>
</main></html>"""


class Handler(BaseHTTPRequestHandler):
    def send(self, code, body, ctype="application/json; charset=utf-8"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body, ensure_ascii=False)
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def is_local_admin(self):
        # Requests relayed by Cloudflare carry CF headers; admin stays LAN-only.
        if self.headers.get("Cf-Connecting-Ip") or self.headers.get("Cf-Ray"):
            return False
        try:
            return ipaddress.ip_address(self.client_address[0]).is_private
        except ValueError:
            return False

    def token_ok(self, query):
        given = self.headers.get("X-Token") or (query.get("token") or [""])[0]
        return secrets.compare_digest(given, config["token"])

    def read_form(self):
        raw = self.rfile.read(int(self.headers.get("Content-Length") or 0)).decode()
        return {k: v[0] for k, v in urllib.parse.parse_qs(raw).items()}

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        if u.path == "/api/messages" or u.path == "/messages":
            if not self.token_ok(q):
                return self.send(401, {"error": "invalid token"})
            since = int((q.get("since") or ["0"])[0] or 0)
            with lock:
                unread = [m["id"] for m in messages if not m.get("read")]
                latest = messages[-1]["id"] if messages else 0
                out = [{k: v for k, v in m.items() if not k.startswith("_")} for m in messages if m["id"] > since][:100]
            return self.send(200, {"latest": latest, "messages": out, "unread": unread})
        if not self.is_local_admin():
            return self.send(403, {"error": "admin is LAN only"})
        if u.path == "/":
            return self.send(200, render_admin("__qr__" if q.get("qr") else ""), "text/html; charset=utf-8")
        if u.path == "/admin/qr":
            try:
                png = api("/v1/qrcodelink?device_name=SpaceClock", raw=True, timeout=60)
                return self.send(200, png, "image/png")
            except Exception as e:
                return self.send(502, {"error": str(e)})
        return self.send(404, {"error": "not found"})

    def do_POST(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        if u.path == "/api/sync":
            if not self.token_ok(q):
                return self.send(401, {"error": "invalid token"})
            try:
                teams_poll_once()  # check Teams right now (Signal is already live)
            except Exception as e:
                print("teams sync failed:", e, flush=True)
            return self.send(200, {"ok": True})
        if u.path == "/api/read":
            if not self.token_ok(q):
                return self.send(401, {"error": "invalid token"})
            req = json.loads(self.rfile.read(int(self.headers.get("Content-Length") or 0)) or b"{}")
            chat = req.get("chat") or ""
            newly_read = []
            with lock:
                for m in messages:
                    if m.get("chat") == chat and not m.get("read"):
                        m["read"] = True
                        newly_read.append(dict(m))
                if newly_read:
                    prune()
                    save()
            # Receipts go out in the background so the Core2 is not kept waiting.
            if chat.startswith("teams:"):
                if newly_read:
                    threading.Thread(target=teams_mark_read, args=(chat,), daemon=True).start()
            else:
                threading.Thread(target=send_read_receipts, args=(newly_read,), daemon=True).start()
            return self.send(200, {"ok": True, "marked": len(newly_read)})
        if u.path == "/api/reply" or u.path == "/reply":
            if not self.token_ok(q):
                return self.send(401, {"error": "invalid token"})
            try:
                req = json.loads(self.rfile.read(int(self.headers.get("Content-Length") or 0)) or b"{}")
                with lock:
                    if req.get("chat"):
                        target = next((m for m in reversed(messages) if m.get("chat") == req["chat"]), None)
                    else:
                        target = next((m for m in messages if m["id"] == int(req.get("id", -1))), None)
                text = str(req.get("text") or "").strip()[:200]
                # A retry (lost answer, failover, double tap) must not send twice:
                # drop the same request id, or the same text to the same chat
                # within a few seconds.
                now_s = time.time()
                key_rid = str(req.get("rid") or "")
                key_txt = (str(req.get("chat") or req.get("id")), text)
                with reply_dedupe_lock:
                    for k in [k for k, t0 in reply_dedupe.items() if now_s - t0 > 60]:
                        del reply_dedupe[k]
                    dup = (key_rid and ("rid", key_rid) in reply_dedupe) or \
                          (("txt",) + key_txt in reply_dedupe and now_s - reply_dedupe[("txt",) + key_txt] < 8)
                    if not dup:
                        if key_rid: reply_dedupe[("rid", key_rid)] = now_s
                        reply_dedupe[("txt",) + key_txt] = now_s
                if dup:
                    print("duplicate reply ignored:", text[:20], flush=True)
                    return self.send(200, {"ok": True, "duplicate": True})
                if not target or not target.get("_to") or not text:
                    with reply_dedupe_lock:
                        reply_dedupe.pop(("rid", key_rid), None); reply_dedupe.pop(("txt",) + key_txt, None)
                    print("reply rejected: unknown chat", str(req.get("chat") or req.get("id"))[:40], flush=True)
                    return self.send(400, {"error": "unknown message or empty text"})
                if str(target["_to"]).startswith("teams:"):
                    teams_send(target["_to"], text)
                else:
                    api("/v2/send", {"number": account(), "recipients": [target["_to"]], "message": text})
                # signal-cli does not echo messages sent by this linked device,
                # so record the reply here to show it in the conversation.
                global next_id
                with lock:
                    item = {"ts": int(time.time() * 1000), "from": "我", "own": True,
                            "group": target["group"], "text": text, "attachments": 0,
                            "chat": target.get("chat", ""), "chat_name": target.get("chat_name", ""), "read": True,
                            "_to": target["_to"], "id": next_id}
                    next_id += 1
                    replied_unread = []
                    for m in messages:  # replying means the conversation was read
                        if m.get("chat") == target.get("chat"):
                            if not m.get("read"):
                                replied_unread.append(dict(m))
                            m["read"] = True
                    messages.append(item)
                    prune()
                    save()
                threading.Thread(target=send_read_receipts, args=(replied_unread,), daemon=True).start()
                print("reply sent to message", target["id"], flush=True)
                return self.send(200, {"ok": True})
            except Exception as e:
                # Sending failed, so a retry must be allowed through.
                with reply_dedupe_lock:
                    for k in (("rid", locals().get("key_rid")), ("txt",) + tuple(locals().get("key_txt") or ())):
                        reply_dedupe.pop(k, None)
                print("reply failed:", e, flush=True)
                return self.send(502, {"error": str(e)})
        if not self.is_local_admin():
            return self.send(403, {"error": "admin is LAN only"})
        if u.path == "/admin/save":
            form = self.read_form()
            config["lan_url"] = form.get("lan_url", "").strip().rstrip("/")
            config["public_url"] = form.get("public_url", "").strip().rstrip("/")
            save_config()
            return self.send(200, render_admin("已儲存網址設定。"), "text/html; charset=utf-8")
        if u.path == "/admin/teams":
            form = self.read_form()
            config["teams_tenant"] = form.get("teams_tenant", "").strip()
            config["teams_client"] = form.get("teams_client", "").strip()
            save_config()
            return self.send(200, render_admin("已儲存 Teams 設定，請按「登入 Teams」。"), "text/html; charset=utf-8")
        if u.path == "/admin/teams-login":
            if not (config["teams_tenant"] and config["teams_client"]):
                return self.send(200, render_admin("請先填入 Tenant ID 與 Client ID。"), "text/html; charset=utf-8")
            teams_start_login()
            return self.send(200, render_admin("已產生登入代碼。" if teams_state.get("device") else "登入失敗：" + teams_state["error"]), "text/html; charset=utf-8")
        if u.path == "/admin/token":
            config["token"] = secrets.token_urlsafe(24)
            save_config()
            return self.send(200, render_admin("已產生新的存取權杖，請更新每台 Core2。"), "text/html; charset=utf-8")
        return self.send(404, {"error": "not found"})

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    load()
    threading.Thread(target=refresh_groups, daemon=True).start()
    threading.Thread(target=teams_loop, daemon=True).start()
    threading.Thread(target=ThreadingHTTPServer(("0.0.0.0", 8081), Handler).serve_forever, daemon=True).start()
    asyncio.run(receive_loop())
