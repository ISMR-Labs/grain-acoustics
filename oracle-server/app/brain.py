"""The Oracle's brain: Claude with a few tools (time, Home Assistant). Spoken-style answers."""
import json, os, time
from datetime import datetime
from zoneinfo import ZoneInfo

import httpx

MODEL = os.environ.get("CLAUDE_MODEL", "claude-opus-5")
EFFORT = os.environ.get("CLAUDE_EFFORT", "low")
TZ = os.environ.get("ASSISTANT_TZ", "Asia/Colombo")
LOCATION = os.environ.get("ASSISTANT_LOCATION", "")
HA_URL = os.environ.get("HA_URL", "").rstrip("/")
HA_TOKEN = os.environ.get("HA_TOKEN", "")
HOOKS = {}            # filled by main: play(device, name), stop(device), library()
HISTORY_TTL = 600      # forget the conversation after 10 min of silence
MAX_TURNS = 12

SYSTEM = f"""You are The Oracle, a voice assistant that lives in a small speaker in {LOCATION or "the user's home"}.
Everything you say is converted to speech, so:
- Answer in one to three short sentences. No markdown, no lists, no headings, no emojis, no URLs.
- Be direct and warm. Numbers and times in plain words a listener can follow.
- If you use a tool, do it silently and then just give the result.
- If asked to control a device and the tool succeeds, confirm in a few words ("Okay, the kitchen light is off").
- If you genuinely cannot do something, say so briefly.
- When asked to play a song, call play_music and then reply with at most a few words (the music starts right after you speak).
The user speaks to you through a microphone; transcripts may contain small errors, so interpret charitably."""


def _tools():
    tools = [{
        "name": "get_current_time",
        "description": "Get the current local date and time for the user.",
        "input_schema": {"type": "object", "properties": {}, "additionalProperties": False},
        "strict": True,
    }]
    tools += [
        {"name": "play_music", "description": "Play a song or sound from the local library on the speaker. Use when the user asks to play music or a named song.",
         "input_schema": {"type": "object", "properties": {"name": {"type": "string", "description": "song name as the user said it"}}, "required": ["name"], "additionalProperties": False}, "strict": True},
        {"name": "stop_music", "description": "Stop whatever is playing on the speaker.", "input_schema": {"type": "object", "properties": {}, "additionalProperties": False}, "strict": True},
        {"name": "list_music", "description": "List the songs available in the local library.", "input_schema": {"type": "object", "properties": {}, "additionalProperties": False}, "strict": True},
    ]
    if HA_URL and HA_TOKEN:
        tools += [
            {"name": "home_find_entities", "description": "Search Home Assistant for devices/entities by name (lights, switches, sensors, climate, media players). Returns entity ids with current state.",
             "input_schema": {"type": "object", "properties": {"query": {"type": "string", "description": "words from the device name, e.g. 'kitchen light'"}}, "required": ["query"], "additionalProperties": False}, "strict": True},
            {"name": "home_get_state", "description": "Get the state and attributes of one Home Assistant entity.",
             "input_schema": {"type": "object", "properties": {"entity_id": {"type": "string"}}, "required": ["entity_id"], "additionalProperties": False}, "strict": True},
            {"name": "home_call_service", "description": "Control a Home Assistant entity, e.g. domain 'light' service 'turn_on', domain 'switch' service 'toggle', domain 'media_player' service 'media_pause'. Optional data such as {\"brightness_pct\": 50}.",
             "input_schema": {"type": "object", "properties": {"domain": {"type": "string"}, "service": {"type": "string"}, "entity_id": {"type": "string"},
                              "data": {"type": "object", "additionalProperties": True}}, "required": ["domain", "service", "entity_id"], "additionalProperties": False}},
        ]
    return tools


def _ha(method, path, **kw):
    r = httpx.request(method, HA_URL + path, headers={"Authorization": f"Bearer {HA_TOKEN}"}, timeout=10, **kw)
    r.raise_for_status(); return r.json() if r.content else {}


def run_tool(name, inp, device=None):
    if name == "play_music": return HOOKS["play"](device, inp.get("name", ""))
    if name == "stop_music": return HOOKS["stop"](device)
    if name == "list_music": return HOOKS["library"]()
    if name == "get_current_time":
        now = datetime.now(ZoneInfo(TZ)); return now.strftime("%A %d %B %Y, %I:%M %p") + f" ({TZ})"
    if name == "home_find_entities":
        words = [w for w in inp.get("query", "").lower().split() if w]
        out = []
        for s in _ha("GET", "/api/states"):
            eid = s["entity_id"]; fname = (s.get("attributes", {}).get("friendly_name") or "").lower()
            if eid.split(".")[0] in ("light", "switch", "fan", "climate", "media_player", "cover", "lock", "sensor", "binary_sensor", "script", "scene") and all(w in fname or w in eid for w in words):
                out.append({"entity_id": eid, "name": s.get("attributes", {}).get("friendly_name"), "state": s["state"]})
        return json.dumps(out[:15]) if out else "no matching entities"
    if name == "home_get_state":
        s = _ha("GET", f"/api/states/{inp['entity_id']}"); a = s.get("attributes", {})
        return json.dumps({"state": s["state"], "unit": a.get("unit_of_measurement"), "brightness": a.get("brightness"), "temperature": a.get("temperature"), "name": a.get("friendly_name")})
    if name == "home_call_service":
        body = {"entity_id": inp["entity_id"], **(inp.get("data") or {})}
        _ha("POST", f"/api/services/{inp['domain']}/{inp['service']}", json=body); return "done"
    return f"unknown tool {name}"


class Brain:
    def __init__(self):
        self.client = None
        self.err = None
        try:
            import anthropic
            if os.environ.get("ANTHROPIC_API_KEY"):
                self.client = anthropic.Anthropic(timeout=60.0, max_retries=1)
            else:
                self.err = "ANTHROPIC_API_KEY is not set"
        except Exception as e:
            self.err = f"anthropic SDK: {e}"
        self.history = {}   # device -> {"messages": [...], "t": last_time}

    def configured(self): return self.client is not None

    def reset(self, device): self.history.pop(device, None)

    def ask(self, device, text, log=lambda kind, data: None):
        if not self.client:
            return "I am not connected to my brain yet. Please add a Claude API key on the server."
        h = self.history.get(device)
        if not h or time.time() - h["t"] > HISTORY_TTL: h = self.history[device] = {"messages": [], "t": time.time()}
        h["t"] = time.time()
        messages = h["messages"] + [{"role": "user", "content": text}]
        tools = _tools()
        try:
            for _ in range(6):
                resp = self.client.beta.messages.create(
                    model=MODEL, max_tokens=1024,
                    system=[{"type": "text", "text": SYSTEM, "cache_control": {"type": "ephemeral"}}],
                    output_config={"effort": EFFORT},
                    tools=tools, messages=messages,
                    betas=["server-side-fallback-2026-07-01"], fallbacks="default",
                )
                if resp.stop_reason == "refusal":
                    return "I would rather not answer that one."
                messages.append({"role": "assistant", "content": resp.content})
                uses = [b for b in resp.content if b.type == "tool_use"]
                if not uses or resp.stop_reason not in ("tool_use", "pause_turn"):
                    break
                results = []
                for u in uses:
                    try:
                        out = run_tool(u.name, u.input, device); log("tool", {"name": u.name, "input": u.input, "result": str(out)[:300]})
                        results.append({"type": "tool_result", "tool_use_id": u.id, "content": str(out)})
                    except Exception as e:
                        results.append({"type": "tool_result", "tool_use_id": u.id, "content": f"error: {e}", "is_error": True})
                messages.append({"role": "user", "content": results})
            reply = " ".join(b.text for b in resp.content if b.type == "text").strip() or "I have nothing to add."
        except Exception as e:
            log("error", {"where": "claude", "error": str(e)[:300]})
            return "Sorry, I could not reach my brain just now."
        h["messages"] = messages[-(MAX_TURNS * 2):]
        # never let history start with a tool_result-only user turn after trimming
        while h["messages"] and (h["messages"][0]["role"] != "user" or not isinstance(h["messages"][0]["content"], str)):
            h["messages"].pop(0)
        return reply
