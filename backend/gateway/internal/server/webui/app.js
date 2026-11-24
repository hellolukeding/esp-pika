const pick = (obj, ...keys) => {
  if (!obj) return undefined;
  for (const k of keys) {
    if (obj[k] !== undefined && obj[k] !== null) return obj[k];
  }
  return undefined;
};

async function api(path, options = {}) {
  const res = await fetch(path, {
    headers: { "Content-Type": "application/json", ...(options.headers || {}) },
    ...options,
  });
  const text = await res.text();
  let data = {};
  try {
    data = text ? JSON.parse(text) : {};
  } catch {
    data = { raw: text };
  }
  if (!res.ok) {
    throw new Error(data.message || `HTTP ${res.status}: ${text}`);
  }
  return data;
}

// Tab switching
document.getElementById("tabs").addEventListener("click", (e) => {
  const btn = e.target.closest("button[data-tab]");
  if (!btn) return;
  document.querySelectorAll("#tabs button").forEach((b) => b.classList.remove("active"));
  document.querySelectorAll("main .tab").forEach((t) => t.classList.remove("active"));
  btn.classList.add("active");
  document.getElementById(`tab-${btn.dataset.tab}`).classList.add("active");
});

async function refreshHealth() {
  const pill = document.getElementById("health-pill");
  try {
    const h = await api("/healthz");
    pill.textContent = h.status === "ok" ? "Gateway 在线" : "异常";
    pill.className = "pill ok";
  } catch {
    pill.textContent = "离线";
    pill.className = "pill err";
  }
}

async function refreshDevices() {
  const box = document.getElementById("devices-container");
  const countPill = document.getElementById("device-count-pill");
  try {
    const res = await api("/api/v1/devices");
    const list = res.devices || [];
    countPill.textContent = `${list.length} 台设备`;
    if (list.length === 0) {
      box.innerHTML = `<div class="pill">暂无已上线设备。可先添加 MAC 白名单后通过终端或“WebSocket 实时调试”接入。</div>`;
      return;
    }
    box.innerHTML = list
      .map((item) => {
        const hello = item.hello || {};
        const shadow = item.shadow || {};
        const id = pick(hello, "deviceId", "device_id") || pick(shadow, "deviceId", "device_id") || "-";
        const zone = hello.zone || shadow.zone || "-";
        const group = pick(hello, "groupId", "group_id") || pick(shadow, "groupId", "group_id") || "-";
        const online = Boolean(shadow.online);
        const caps = (hello.capabilities || []).map((c) => `<span class="tag">${c.name}</span>`).join("");
        const vad = pick(shadow, "vadState", "vad_state") || "idle";
        const rssi = shadow.rssi ?? "-";
        const vol = shadow.volume ?? "-";
        const vpVer = pick(hello, "vpVersion", "vp_version") ?? 0;
        return `
          <div class="card" style="margin:0; background: var(--panel-alt);">
            <div class="row" style="justify-content:space-between; align-items:center;">
              <strong>${zone} (${id})</strong>
              <span class="tag ${online ? "online" : "offline"}">${online ? "ONLINE" : "OFFLINE"}</span>
            </div>
            <div style="font-size:12px; color:var(--muted); margin-bottom:8px;">
              group=${group} | chip=${hello.chip || "-"} | vp_ver=${vpVer} | vad=${vad} | vol=${vol} | rssi=${rssi}
            </div>
            <div>${caps || '<span class="tag">无声明能力</span>'}</div>
          </div>`;
      })
      .join("");
  } catch (err) {
    box.innerHTML = `<pre class="log">${err.message}</pre>`;
  }
}

async function refreshMacs() {
  const tbody = document.getElementById("macs-tbody");
  const res = await api("/api/v1/macs");
  const items = res.items || [];
  const firstEnabled = items.find((m) => m.enabled);
  if (firstEnabled) {
    const wsMacInput = document.getElementById("ws-mac");
    const wsZoneInput = document.getElementById("ws-zone");
    const cmdDevInput = document.getElementById("cmd-device");
    if (wsMacInput && (!wsMacInput.value.trim() || wsMacInput.value.trim() === "30:ed:a0:f3:dc:70")) {
      wsMacInput.value = firstEnabled.mac;
      if (wsZoneInput && firstEnabled.zone) {
        wsZoneInput.value = firstEnabled.zone;
      }
    }
    if (cmdDevInput && !cmdDevInput.value.trim()) {
      cmdDevInput.value = firstEnabled.mac;
    }
  }
  tbody.innerHTML = items
    .map((m) => {
      const group = pick(m, "groupId", "group_id") || "";
      return `<tr>
        <td><code>${m.mac}</code></td>
        <td>${group}</td>
        <td>${m.zone || ""}</td>
        <td>${m.label || ""}</td>
        <td>${m.enabled ? '<span class="tag online">启用</span>' : '<span class="tag">禁用</span>'}</td>
        <td><button class="btn danger" onclick="deleteMac('${m.mac}')">删除</button></td>
      </tr>`;
    })
    .join("");
}

window.deleteMac = async (mac) => {
  await api(`/api/v1/macs/${encodeURIComponent(mac)}`, { method: "DELETE" });
  await refreshMacs();
};

async function refreshAcls() {
  const tbody = document.getElementById("acls-tbody");
  const res = await api("/api/v1/acls");
  const rules = res.rules || [];
  tbody.innerHTML = rules
    .map((r) => {
      const id = pick(r, "ruleId", "rule_id");
      const agent = pick(r, "agentId", "agent_id");
      const group = pick(r, "groupId", "group_id") || "*";
      const zone = pick(r, "zonePattern", "zone_pattern") || "*";
      const cap = pick(r, "capabilityPattern", "capability_pattern") || "*";
      const role = pick(r, "minSpeakerRole", "min_speaker_role") ?? 1;
      return `<tr>
        <td><code>${id}</code></td>
        <td>${agent}</td>
        <td>${group} / ${zone}</td>
        <td><code>${cap}</code></td>
        <td>${role}</td>
        <td>${r.allow ? '<span class="tag online">Allow</span>' : '<span class="tag">Deny</span>'}</td>
        <td><button class="btn danger" onclick="deleteAcl('${id}')">删除</button></td>
      </tr>`;
    })
    .join("");
}

window.deleteAcl = async (id) => {
  await api(`/api/v1/acls/${encodeURIComponent(id)}`, { method: "DELETE" });
  await refreshAcls();
};

async function refreshSpeakers() {
  const tbody = document.getElementById("speakers-tbody");
  const res = await api("/api/v1/speakers");
  const list = res.speakers || [];
  tbody.innerHTML = list
    .map((s) => {
      const id = pick(s, "speakerId", "speaker_id");
      const name = pick(s, "displayName", "display_name") || "";
      const agent = pick(s, "agentId", "agent_id") || "";
      const voice = pick(s, "ttsVoice", "tts_voice") || "";
      const rawPrompt = pick(s, "prompt") || "";
      const prompt = String(rawPrompt).replace(/[&<>"]/g, (ch) => (
        { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[ch]
      ));
      return `<tr>
        <td><code>${id}</code></td>
        <td>${name}</td>
        <td>${s.role}</td>
        <td>${agent}</td>
        <td><code>${voice}</code></td>
        <td>${prompt}</td>
        <td>${s.version || 0}</td>
        <td><button class="btn danger" onclick="deleteSpeaker('${id}')">删除</button></td>
      </tr>`;
    })
    .join("");
}

async function refreshPrompt() {
  const input = document.getElementById("global-prompt-input");
  const pill = document.getElementById("global-prompt-status");
  if (!input || !pill) return;
  try {
    const res = await api("/api/v1/settings/prompt");
    const custom = pick(res, "system_prompt") || "";
    const effective = pick(res, "effective_prompt") || pick(res, "default_prompt") || "";
    input.value = custom || effective;
    pill.textContent = custom ? "已启用自定义提示词" : "使用默认提示词";
    pill.className = custom ? "pill ok" : "pill";
  } catch (err) {
    pill.textContent = `加载失败: ${err.message}`;
    pill.className = "pill err";
  }
}

function applyDeepSeekView(res) {
  const pill = document.getElementById("deepseek-key-status");
  const keyEl = document.getElementById("deepseek-api-key");
  const modelEl = document.getElementById("deepseek-model");
  const voiceEl = document.getElementById("deepseek-voice");
  if (!pill || !keyEl || !modelEl || !voiceEl) return;

  const configured = Boolean(pick(res, "configured"));
  const masked = pick(res, "masked_key") || "";
  const source = pick(res, "source") || "none";
  const model = pick(res, "model") || "deepseek-flash";
  const voice = pick(res, "tts_voice") || "stella";

  modelEl.value = model;
  voiceEl.value = voice;
  keyEl.value = "";

  if (configured && source === "console") {
    pill.textContent = `已配置 (控制台) ${masked}`;
    pill.className = "pill ok";
  } else if (configured && source === "env") {
    pill.textContent = `已配置 (环境变量) ${masked}`;
    pill.className = "pill ok";
  } else {
    pill.textContent = "未配置";
    pill.className = "pill";
  }
}

async function refreshDeepSeekSettings() {
  const pill = document.getElementById("deepseek-key-status");
  if (!pill) return;
  try {
    const res = await api("/api/v1/settings/deepseek");
    applyDeepSeekView(res);
  } catch (err) {
    pill.textContent = `加载失败: ${err.message}`;
    pill.className = "pill err";
  }
}

window.deleteSpeaker = async (id) => {
  await api(`/api/v1/speakers/${encodeURIComponent(id)}`, { method: "DELETE" });
  await refreshSpeakers();
};

// Form handlers
document.getElementById("cmd-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  const out = document.getElementById("cmd-output");
  const img = document.getElementById("cmd-media-img");
  img.style.display = "none";
  const dev = document.getElementById("cmd-device").value.trim();
  try {
    const res = await api(`/api/v1/devices/${encodeURIComponent(dev)}/commands`, {
      method: "POST",
      body: JSON.stringify({
        agent_id: document.getElementById("cmd-agent").value.trim(),
        capability: document.getElementById("cmd-cap").value,
        action: document.getElementById("cmd-action").value.trim(),
        params_json: document.getElementById("cmd-params").value.trim(),
        timeout_ms: 5000,
      }),
    });
    out.textContent = JSON.stringify(res, null, 2);
    const mediaUrl = pick(res, "mediaUrl", "media_url");
    if (mediaUrl) {
      img.src = mediaUrl;
      img.style.display = "block";
    }
  } catch (err) {
    out.textContent = `错误: ${err.message}`;
  }
});

document.getElementById("mac-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  const mac = document.getElementById("mac-addr").value.trim();
  await api(`/api/v1/macs/${encodeURIComponent(mac)}`, {
    method: "PUT",
    body: JSON.stringify({
      group_id: document.getElementById("mac-group").value.trim(),
      zone: document.getElementById("mac-zone").value.trim(),
      label: document.getElementById("mac-label").value.trim(),
      enabled: document.getElementById("mac-enabled").value === "true",
    }),
  });
  await refreshMacs();
});

document.getElementById("acl-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  const id = document.getElementById("acl-id").value.trim();
  await api(`/api/v1/acls/${encodeURIComponent(id)}`, {
    method: "PUT",
    body: JSON.stringify({
      agent_id: document.getElementById("acl-agent").value.trim(),
      group_id: document.getElementById("acl-group").value.trim(),
      zone_pattern: document.getElementById("acl-zone").value.trim(),
      capability_pattern: document.getElementById("acl-cap").value.trim(),
      min_speaker_role: Number(document.getElementById("acl-role").value),
      allow: document.getElementById("acl-allow").value === "true",
    }),
  });
  await refreshAcls();
});

document.getElementById("speaker-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  const id = document.getElementById("sp-id").value.trim();
  await api(`/api/v1/speakers/${encodeURIComponent(id)}`, {
    method: "PUT",
    body: JSON.stringify({
      display_name: document.getElementById("sp-name").value.trim(),
      uid: document.getElementById("sp-uid").value.trim(),
      agent_id: document.getElementById("sp-agent").value.trim(),
      group_id: document.getElementById("sp-group").value.trim(),
      role: Number(document.getElementById("sp-role").value),
      tts_voice: document.getElementById("sp-voice").value.trim(),
      prompt: document.getElementById("sp-prompt").value.trim(),
    }),
  });
  await refreshSpeakers();
});

// Browser Virtual Device WebSocket Debugger
let ws = null;
const appendWSLog = (line) => {
  const el = document.getElementById("ws-log");
  el.textContent += `\n[${new Date().toLocaleTimeString()}] ${line}`;
  el.scrollTop = el.scrollHeight;
};

document.getElementById("ws-connect-btn").addEventListener("click", () => {
  if (ws) ws.close();
  const mac = document.getElementById("ws-mac").value.trim();
  const zone = document.getElementById("ws-zone").value.trim();
  const proto = location.protocol === "https:" ? "wss" : "ws";
  const statusPill = document.getElementById("ws-status");
  ws = new WebSocket(`${proto}://${location.host}/ws/device?mac=${encodeURIComponent(mac)}`);
  ws.binaryType = "arraybuffer";
  ws.onopen = () => {
    statusPill.textContent = `已连接 (${mac})`;
    statusPill.className = "pill ok";
    const hello = {
      type: "device.hello",
      mac,
      group_id: "home-default",
      zone,
      chip: "web-sim",
      capabilities: [
        { name: "device.display_show", type: "tool" },
        { name: "device.camera_capture", type: "tool", upload_channel: "http_post" },
      ],
    };
    ws.send(JSON.stringify(hello));
    appendWSLog("-> " + JSON.stringify(hello));
  };
  ws.onmessage = (evt) => {
    if (evt.data instanceof ArrayBuffer) {
      const txt = new TextDecoder().decode(evt.data);
      appendWSLog(`<- [BINARY ${evt.data.byteLength}B] ${txt}`);
    } else {
      appendWSLog("<- " + evt.data);
    }
  };
  ws.onclose = (evt) => {
    statusPill.textContent = `已断开 (${evt.code})`;
    statusPill.className = "pill err";
    appendWSLog(`WS closed code=${evt.code}`);
  };
});

document.getElementById("ws-disconnect-btn").addEventListener("click", () => {
  if (ws) ws.close();
});

document.getElementById("ws-wake-btn").addEventListener("click", () => {
  if (!ws || ws.readyState !== WebSocket.OPEN) return;
  const msg = {
    type: "wake.detected",
    group_id: "home-default",
    zone: document.getElementById("ws-zone").value.trim(),
    wake_word: "hi_esp",
    wake_score: 0.95,
    snr_db: 21.0,
    detected_at_ms: Date.now(),
  };
  ws.send(JSON.stringify(msg));
  appendWSLog("-> " + JSON.stringify(msg));
});

document.getElementById("ws-send-turn-btn").addEventListener("click", () => {
  if (!ws || ws.readyState !== WebSocket.OPEN) return;
  const msg = {
    type: "text.turn",
    text: document.getElementById("ws-turn-text").value,
    speaker_id: document.getElementById("ws-speaker").value.trim(),
    speaker_role: 1,
    agent_id: document.getElementById("ws-agent").value.trim(),
  };
  ws.send(JSON.stringify(msg));
  appendWSLog("-> " + JSON.stringify(msg));
});

document.getElementById("ws-barge-btn").addEventListener("click", () => {
  if (!ws || ws.readyState !== WebSocket.OPEN) return;
  const msg = { type: "speech.barge_in", reason: "user_interrupt" };
  ws.send(JSON.stringify(msg));
  appendWSLog("-> " + JSON.stringify(msg));
});

// Browser 16kHz Mono PCM / WAV Recorder & Voiceprint Debugger
let recAudioCtx = null;
let recStream = null;
let recProc = null;
let recChunks = [];
let recordedWavBytes = null;

function downsampleLinear(input, srcRate, dstRate) {
  if (srcRate === dstRate) return input;
  const ratio = srcRate / dstRate;
  const outLen = Math.floor(input.length / ratio);
  const out = new Float32Array(outLen);
  for (let i = 0; i < outLen; i++) {
    const pos = i * ratio;
    const idx = Math.floor(pos);
    const frac = pos - idx;
    const s0 = input[idx] || 0;
    const s1 = input[idx + 1] !== undefined ? input[idx + 1] : s0;
    out[i] = s0 + frac * (s1 - s0);
  }
  return out;
}

function encodeWav16kMono(samplesFloat) {
  const pcmLen = samplesFloat.length * 2;
  const buf = new ArrayBuffer(44 + pcmLen);
  const view = new DataView(buf);
  const writeStr = (off, s) => {
    for (let i = 0; i < s.length; i++) view.setUint8(off + i, s.charCodeAt(i));
  };
  writeStr(0, "RIFF");
  view.setUint32(4, 36 + pcmLen, true);
  writeStr(8, "WAVE");
  writeStr(12, "fmt ");
  view.setUint32(16, 16, true);
  view.setUint16(20, 1, true);
  view.setUint16(22, 1, true);
  view.setUint32(24, 16000, true);
  view.setUint32(28, 32000, true);
  view.setUint16(32, 2, true);
  view.setUint16(34, 16, true);
  writeStr(36, "data");
  view.setUint32(40, pcmLen, true);
  for (let i = 0; i < samplesFloat.length; i++) {
    const clamped = Math.max(-1, Math.min(1, samplesFloat[i]));
    view.setInt16(44 + i * 2, clamped < 0 ? clamped * 0x8000 : clamped * 0x7fff, true);
  }
  return new Uint8Array(buf);
}

function bytesToBase64(u8) {
  let bin = "";
  const chunk = 0x8000;
  for (let i = 0; i < u8.length; i += chunk) {
    bin += String.fromCharCode(...u8.subarray(i, i + chunk));
  }
  return btoa(bin);
}

function setRecordedAudio(float16k, label) {
  recordedWavBytes = encodeWav16kMono(float16k);
  const blob = new Blob([recordedWavBytes], { type: "audio/wav" });
  const player = document.getElementById("rec-audio-player");
  player.src = URL.createObjectURL(blob);
  player.style.display = "block";
  const sec = (float16k.length / 16000).toFixed(2);
  const pill = document.getElementById("rec-status");
  pill.textContent = `${label} (${sec}s / ${(recordedWavBytes.length / 1024).toFixed(1)} KB)`;
  pill.className = "pill ok";
}

document.getElementById("rec-start-btn").addEventListener("click", async () => {
  const pill = document.getElementById("rec-status");
  try {
    recStream = await navigator.mediaDevices.getUserMedia({ audio: true });
    recAudioCtx = new (window.AudioContext || window.webkitAudioContext)();
    const src = recAudioCtx.createMediaStreamSource(recStream);
    recProc = recAudioCtx.createScriptProcessor(4096, 1, 1);
    recChunks = [];
    recProc.onaudioprocess = (ev) => {
      const ch = ev.inputBuffer.getChannelData(0);
      recChunks.push(new Float32Array(ch));
      let peak = 0;
      for (let i = 0; i < ch.length; i++) peak = Math.max(peak, Math.abs(ch[i]));
      document.getElementById("rec-level").value = Math.min(100, Math.round(peak * 180));
    };
    src.connect(recProc);
    recProc.connect(recAudioCtx.destination);
    document.getElementById("rec-start-btn").disabled = true;
    document.getElementById("rec-stop-btn").disabled = false;
    pill.textContent = "录音中...";
    pill.className = "pill ok";
  } catch (err) {
    pill.textContent = `麦克风受限: ${err.message}`;
    pill.className = "pill err";
  }
});

document.getElementById("rec-stop-btn").addEventListener("click", () => {
  if (recProc) {
    recProc.disconnect();
    recProc = null;
  }
  if (recStream) {
    recStream.getTracks().forEach((t) => t.stop());
    recStream = null;
  }
  const srcRate = recAudioCtx ? recAudioCtx.sampleRate : 16000;
  if (recAudioCtx) {
    recAudioCtx.close();
    recAudioCtx = null;
  }
  document.getElementById("rec-start-btn").disabled = false;
  document.getElementById("rec-stop-btn").disabled = true;
  document.getElementById("rec-level").value = 0;

  let total = 0;
  for (const c of recChunks) total += c.length;
  const merged = new Float32Array(total);
  let off = 0;
  for (const c of recChunks) {
    merged.set(c, off);
    off += c.length;
  }
  const resampled = downsampleLinear(merged, srcRate, 16000);
  setRecordedAudio(resampled, "录音就绪");
});

const synthTestVoice = (f0) => {
  const n = 24000; // 1.5s @ 16kHz
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    const t = i / 16000;
    out[i] = 0.45 * Math.sin(2 * Math.PI * f0 * t) + 0.25 * Math.sin(2 * Math.PI * f0 * 3 * t);
  }
  setRecordedAudio(out, `合成样本 ${f0}Hz`);
};
document.getElementById("rec-synth-a-btn").addEventListener("click", () => synthTestVoice(150));
document.getElementById("rec-synth-b-btn").addEventListener("click", () => synthTestVoice(420));

document.getElementById("vp-enroll-btn").addEventListener("click", async () => {
  const logEl = document.getElementById("vp-enroll-log");
  if (!recordedWavBytes) {
    logEl.textContent = "请先录制人声或点击生成合成样本。";
    return;
  }
  try {
    const res = await api("/api/v1/voice/enroll", {
      method: "POST",
      body: JSON.stringify({
        speaker_id: document.getElementById("vp-speaker-id").value.trim(),
        display_name: document.getElementById("vp-display-name").value.trim(),
        uid: document.getElementById("vp-uid").value.trim(),
        agent_id: document.getElementById("vp-agent").value.trim(),
        group_id: document.getElementById("vp-group").value.trim(),
        role: Number(document.getElementById("vp-role").value),
        tts_voice: document.getElementById("vp-tts-voice").value.trim(),
        audio_base64: bytesToBase64(recordedWavBytes),
      }),
    });
    logEl.textContent = JSON.stringify(res, null, 2);
    await refreshSpeakers();
  } catch (err) {
    logEl.textContent = `注册失败: ${err.message}`;
  }
});

document.getElementById("vp-identify-btn").addEventListener("click", async () => {
  const logEl = document.getElementById("vp-identify-log");
  const bar = document.getElementById("vp-score-bar");
  const pill = document.getElementById("vp-tier-pill");
  if (!recordedWavBytes) {
    logEl.textContent = "请先录制人声或生成合成样本。";
    return;
  }
  try {
    const res = await api("/api/v1/voice/identify", {
      method: "POST",
      body: JSON.stringify({
        uid: document.getElementById("vp-uid").value.trim(),
        agent_id: document.getElementById("vp-agent").value.trim(),
        audio_base64: bytesToBase64(recordedWavBytes),
      }),
    });
    const conf = Number(res.confidence || 0);
    bar.value = Math.max(0, Math.min(100, Math.round(conf * 100)));
    if (conf >= 0.85) {
      pill.textContent = `${res.speaker_id} (本地高可信)`;
      pill.className = "pill ok";
    } else if (conf >= 0.65 && res.speaker_id) {
      pill.textContent = `${res.speaker_id} (灰区复核)`;
      pill.className = "pill";
    } else {
      pill.textContent = "未匹配 / Guest";
      pill.className = "pill err";
    }
    logEl.textContent = JSON.stringify(res, null, 2);
  } catch (err) {
    logEl.textContent = `识别失败: ${err.message}`;
  }
});

document.getElementById("vp-asr-btn").addEventListener("click", async () => {
  const logEl = document.getElementById("vp-identify-log");
  if (!recordedWavBytes) {
    logEl.textContent = "请先录制人声或生成合成样本。";
    return;
  }
  try {
    const res = await api("/api/v1/voice/transcribe", {
      method: "POST",
      body: JSON.stringify({
        device_id: document.getElementById("ws-mac").value.trim(),
        audio_base64: bytesToBase64(recordedWavBytes),
      }),
    });
    logEl.textContent = JSON.stringify(res, null, 2);
  } catch (err) {
    logEl.textContent = `转写失败: ${err.message}`;
  }
});

document.getElementById("vp-ws-send-btn").addEventListener("click", () => {
  const logEl = document.getElementById("vp-identify-log");
  if (!recordedWavBytes) {
    logEl.textContent = "请先录制人声或生成合成样本。";
    return;
  }
  if (!ws || ws.readyState !== WebSocket.OPEN) {
    logEl.textContent = "请先前往“WebSocket 实时调试”页连接虚拟终端。";
    return;
  }
  const startMsg = {
    type: "speech.start",
    speaker_id: document.getElementById("ws-speaker").value.trim(),
    speaker_role: 1,
    confidence: 0.92,
    agent_id: document.getElementById("ws-agent").value.trim(),
  };
  ws.send(JSON.stringify(startMsg));
  appendWSLog("-> " + JSON.stringify(startMsg));
  ws.send(recordedWavBytes);
  appendWSLog(`-> [BINARY ${recordedWavBytes.length}B WAV/PCM]`);
  const stopMsg = { type: "speech.stop" };
  ws.send(JSON.stringify(stopMsg));
  appendWSLog("-> " + JSON.stringify(stopMsg));
  logEl.textContent = "已通过 WebSocket 发送 speech.start → 音频帧 → speech.stop，请在“WebSocket 实时调试”查看响应。";
});

document.getElementById("global-prompt-form")?.addEventListener("submit", async (e) => {
  e.preventDefault();
  const input = document.getElementById("global-prompt-input");
  const pill = document.getElementById("global-prompt-status");
  try {
    const res = await api("/api/v1/settings/prompt", {
      method: "PUT",
      body: JSON.stringify({ system_prompt: input.value }),
    });
    const custom = pick(res, "system_prompt") || "";
    const effective = pick(res, "effective_prompt") || pick(res, "default_prompt") || "";
    input.value = custom || effective;
    pill.textContent = custom ? "已保存自定义提示词" : "已恢复默认提示词";
    pill.className = "pill ok";
  } catch (err) {
    pill.textContent = `保存失败: ${err.message}`;
    pill.className = "pill err";
  }
});

document.getElementById("global-prompt-reset-btn")?.addEventListener("click", async () => {
  const input = document.getElementById("global-prompt-input");
  const pill = document.getElementById("global-prompt-status");
  try {
    const res = await api("/api/v1/settings/prompt", {
      method: "PUT",
      body: JSON.stringify({ system_prompt: "" }),
    });
    input.value = pick(res, "effective_prompt") || pick(res, "default_prompt") || "";
    pill.textContent = "已恢复默认提示词";
    pill.className = "pill";
  } catch (err) {
    pill.textContent = `重置失败: ${err.message}`;
    pill.className = "pill err";
  }
});

document.getElementById("deepseek-settings-form")?.addEventListener("submit", async (e) => {
  e.preventDefault();
  const pill = document.getElementById("deepseek-key-status");
  const apiKey = document.getElementById("deepseek-api-key").value.trim();
  const model = document.getElementById("deepseek-model").value.trim();
  const ttsVoice = document.getElementById("deepseek-voice").value;
  try {
    const res = await api("/api/v1/settings/deepseek", {
      method: "PUT",
      body: JSON.stringify({
        api_key: apiKey,
        model,
        tts_voice: ttsVoice,
      }),
    });
    applyDeepSeekView(res);
  } catch (err) {
    if (pill) {
      pill.textContent = `保存失败: ${err.message}`;
      pill.className = "pill err";
    }
  }
});

document.getElementById("deepseek-clear-key-btn")?.addEventListener("click", async () => {
  const pill = document.getElementById("deepseek-key-status");
  try {
    const res = await api("/api/v1/settings/deepseek", {
      method: "PUT",
      body: JSON.stringify({
        clear_key: true,
        model: document.getElementById("deepseek-model").value.trim(),
        tts_voice: document.getElementById("deepseek-voice").value,
      }),
    });
    applyDeepSeekView(res);
  } catch (err) {
    if (pill) {
      pill.textContent = `清除失败: ${err.message}`;
      pill.className = "pill err";
    }
  }
});

let ezPollTimer = null;

function escHtml(s) {
  return String(s ?? "")
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");
}

function applyEZDeviceStatus(res) {
  const st = res?.status || res || {};
  const mcpEl = document.getElementById("ezdevice-mcp-url");
  if (mcpEl && res?.mcp_endpoint) {
    mcpEl.textContent = res.mcp_endpoint;
  } else if (mcpEl && window.location?.origin) {
    mcpEl.textContent = `${window.location.origin}/mcp`;
  }

  const modeEl = document.getElementById("ezdevice-mode");
  if (modeEl && st.mode && document.activeElement !== modeEl) {
    modeEl.value = st.mode;
  }

  const backendPill = document.getElementById("ezdevice-backend-pill");
  if (backendPill) {
    backendPill.textContent = `当前通道: ${st.active_backend || "-"}`;
    backendPill.className = st.active_backend === "ez_device" ? "pill ok" : "pill";
  }

  const statusPill = document.getElementById("ezdevice-status-pill");
  const stateLabels = {
    unpaired: ["未配对", "pill"],
    pairing: ["正在连接桌面端...", "pill"],
    awaiting_code: ["等待桌面端输入配对码", "pill"],
    awaiting_confirm: ["等待桌面端点击确认", "pill"],
    connecting: ["正在重连桌面端...", "pill"],
    online: ["EZ Assistant 已在线", "pill ok"],
    offline: ["已停用（仅 gRPC 模式）", "pill"],
    revoked: ["已被桌面端撤销，请重新配对", "pill err"],
    auth_error: ["认证失败，请重置配对", "pill err"],
  };
  if (statusPill) {
    const [label, cls] = stateLabels[st.state] || [st.state || "未知", "pill"];
    statusPill.textContent = label;
    statusPill.className = cls;
  }

  const addrEl = document.getElementById("ezdevice-addr");
  const idEl = document.getElementById("ezdevice-install-id");
  const fpEl = document.getElementById("ezdevice-cert-fp");
  const nameEl = document.getElementById("ezdevice-display-name");
  if (addrEl && (!addrEl.value || st.state === "pairing" || st.state === "awaiting_code" || st.state === "awaiting_confirm") && st.host_address && document.activeElement !== addrEl) {
    addrEl.value = st.host_address;
  }
  if (idEl && !idEl.value && st.installation_id) idEl.value = st.installation_id;
  if (fpEl && !fpEl.value && st.certificate_fingerprint) fpEl.value = st.certificate_fingerprint;
  if (nameEl && !nameEl.value && st.display_name) nameEl.value = st.display_name;

  const codeBox = document.getElementById("ezdevice-code-box");
  const codePrompt = document.getElementById("ezdevice-code-prompt");
  const codeVal = document.getElementById("ezdevice-code-value");
  const codeMeta = document.getElementById("ezdevice-code-meta");
  if (codeBox && codeVal && codeMeta) {
    if ((st.pairing_mode === "direct_confirm" || st.state === "awaiting_confirm") && (st.state === "pairing" || st.state === "awaiting_confirm")) {
      codeBox.style.display = "block";
      if (codePrompt) {
        codePrompt.textContent = "免码确认模式：请在 Mac EZ Assistant 桌面端 → 设置 → 智能终端 中核对下方密钥指纹并点击「确认配对」：";
      }
      codeVal.textContent = st.key_fingerprint || "生成中...";
      codeMeta.textContent = st.state === "awaiting_confirm"
        ? "Host 已将网关加入候选列表，请点击桌面端「确认配对」（无需输入配对码）"
        : "正在向桌面端发送免码配对申请...";
    } else if (st.pairing_code && (st.state === "pairing" || st.state === "awaiting_code")) {
      codeBox.style.display = "block";
      if (codePrompt) {
        codePrompt.textContent = "请在 Mac EZ Assistant 桌面端 → 设置 → 智能终端 → 开启「设备接入」并点击「添加设备」，然后在候选设备中输入下方 6 位数字配对码：";
      }
      codeVal.textContent = st.pairing_code;
      const expText = st.pairing_expires_at_ms
        ? `有效期至 ${new Date(st.pairing_expires_at_ms).toLocaleTimeString()}`
        : "等待桌面端确认...";
      codeMeta.textContent = st.state === "awaiting_code" ? `Host 已就绪（${expText}）` : "正在发送 pairing_hello...";
    } else {
      codeBox.style.display = "none";
      codeVal.textContent = "------";
    }
  }

  const infoLine = document.getElementById("ezdevice-info-line");
  if (infoLine) {
    const parts = [];
    if (st.device_id) parts.push(`device_id=${st.device_id}`);
    if (st.identity_state) parts.push(`identity=${st.identity_state}`);
    if (st.last_error) parts.push(`状态提示: ${st.last_error}`);
    infoLine.textContent = parts.join(" | ");
    infoLine.style.color = st.last_error && st.state === "unpaired" ? "var(--err)" : "var(--muted)";
  }

  const shouldPoll = st.state === "pairing" || st.state === "awaiting_code" || st.state === "awaiting_confirm" || st.state === "connecting";
  if (shouldPoll && !ezPollTimer) {
    ezPollTimer = setInterval(refreshEZDeviceStatus, 2000);
  } else if (!shouldPoll && ezPollTimer) {
    clearInterval(ezPollTimer);
    ezPollTimer = null;
  }
}

async function refreshEZDeviceStatus() {
  try {
    const res = await api("/api/v1/ezdevice/status");
    applyEZDeviceStatus(res);
  } catch (err) {
    const pill = document.getElementById("ezdevice-status-pill");
    if (pill) {
      pill.textContent = `状态获取失败: ${err.message}`;
      pill.className = "pill err";
    }
  }
}

let selectedEZPairingMode = "auto";
function inferEZPairingMode(modes) {
  if (!Array.isArray(modes) || modes.length === 0) return "auto";
  if (modes.includes("direct_confirm")) return "direct_confirm";
  if (modes.includes("spake2")) return "spake2";
  return "auto";
}

function selectDiscoveredEZHost(addr, installID, certFP, mode) {
  const addrEl = document.getElementById("ezdevice-addr");
  const idEl = document.getElementById("ezdevice-install-id");
  const fpEl = document.getElementById("ezdevice-cert-fp");
  if (addrEl) addrEl.value = addr;
  if (idEl) idEl.value = installID;
  if (fpEl) fpEl.value = certFP;
  selectedEZPairingMode = mode || "auto";
}
window.selectDiscoveredEZHost = selectDiscoveredEZHost;
["ezdevice-addr", "ezdevice-install-id", "ezdevice-cert-fp"].forEach((id) => {
  document.getElementById(id)?.addEventListener("input", () => {
    selectedEZPairingMode = "auto";
  });
});

document.getElementById("ezdevice-scan-btn")?.addEventListener("click", async () => {
  const listEl = document.getElementById("ezdevice-hosts-list");
  if (listEl) listEl.innerHTML = `<div class="pill">正在扫描局域网 _ez-assistant._tcp.local. ...</div>`;
  try {
    const res = await api("/api/v1/ezdevice/discover", { method: "POST" });
    const hosts = res.hosts || [];
    if (hosts.length === 0) {
      listEl.innerHTML = `<div class="pill">未扫描到开启设备接入的 EZ Assistant 桌面端（请确认桌面端已启动且与网关同处局域网，也可手动填写上方参数）</div>`;
      return;
    }
    listEl.innerHTML = hosts
      .map((h) => {
        const primaryAddr = `${(h.addresses || [])[0] || h.host_name}:${h.port}`;
        const hostMode = inferEZPairingMode(h.pairing_modes);
        const modeLabel = (h.pairing_modes || []).includes("direct_confirm")
          ? "免码确认"
          : (h.pairing_modes || []).includes("spake2")
            ? "6位配对码"
            : "";
        return `
          <div class="row" style="background:var(--panel-alt); border:1px solid var(--border); border-radius:6px; padding:8px 10px; align-items:center; justify-content:space-between; margin-bottom:6px;">
            <div style="font-size:12px;">
              <strong>${escHtml(h.instance_name || h.host_name)}</strong>
              <span class="tag ${h.pairing_available ? "online" : ""}">${h.pairing_available ? "开放配对" : "仅重连"}</span>
              ${modeLabel ? `<span class="tag">${escHtml(modeLabel)}</span>` : ""}
              <code>${escHtml(primaryAddr)}</code>
              <span style="color:var(--muted);">install=${escHtml(h.installation_id.slice(0, 12))}...</span>
            </div>
            <button class="btn sec" type="button" onclick="selectDiscoveredEZHost('${escHtml(primaryAddr)}','${escHtml(h.installation_id)}','${escHtml(h.certificate_fingerprint)}','${escHtml(hostMode)}')">选中此主机</button>
          </div>`;
      })
      .join("");
    if (hosts.length >= 1) {
      const first = hosts[0];
      const firstAddr = `${(first.addresses || [])[0] || first.host_name}:${first.port}`;
      selectDiscoveredEZHost(firstAddr, first.installation_id, first.certificate_fingerprint, inferEZPairingMode(first.pairing_modes));
    }
  } catch (err) {
    if (listEl) listEl.innerHTML = `<div class="pill err">扫描失败: ${escHtml(err.message)}</div>`;
  }
});

document.getElementById("ezdevice-mode-save-btn")?.addEventListener("click", async () => {
  const mode = document.getElementById("ezdevice-mode")?.value || "auto";
  try {
    const res = await api("/api/v1/ezdevice/mode", {
      method: "PUT",
      body: JSON.stringify({ mode }),
    });
    applyEZDeviceStatus(res);
  } catch (err) {
    const infoLine = document.getElementById("ezdevice-info-line");
    if (infoLine) infoLine.textContent = `切换模式失败: ${err.message}`;
  }
});

document.getElementById("ezdevice-pair-form")?.addEventListener("submit", async (e) => {
  e.preventDefault();
  const infoLine = document.getElementById("ezdevice-info-line");
  try {
    const res = await api("/api/v1/ezdevice/pair", {
      method: "POST",
      body: JSON.stringify({
        address: document.getElementById("ezdevice-addr").value.trim(),
        path: "/device",
        installation_id: document.getElementById("ezdevice-install-id").value.trim(),
        certificate_fingerprint: document.getElementById("ezdevice-cert-fp").value.trim(),
        display_name: document.getElementById("ezdevice-display-name").value.trim() || "ESP-Pika Gateway",
        pairing_mode: selectedEZPairingMode || "auto",
      }),
    });
    applyEZDeviceStatus(res);
  } catch (err) {
    if (infoLine) infoLine.textContent = `发起配对失败: ${err.message}`;
  }
});

document.getElementById("ezdevice-reset-btn")?.addEventListener("click", async () => {
  const infoLine = document.getElementById("ezdevice-info-line");
  try {
    const res = await api("/api/v1/ezdevice/reset", { method: "POST" });
    applyEZDeviceStatus(res);
  } catch (err) {
    if (infoLine) infoLine.textContent = `重置配对失败: ${err.message}`;
  }
});

document.getElementById("ezdevice-test-btn")?.addEventListener("click", async () => {
  const infoLine = document.getElementById("ezdevice-info-line");
  if (infoLine) {
    infoLine.style.color = "var(--muted)";
    infoLine.textContent = "正在向 EZ Assistant 主控会话发送测试消息并合成语音...";
  }
  try {
    const res = await api("/api/v1/ezdevice/test", {
      method: "POST",
      body: JSON.stringify({}),
    });
    if (infoLine) {
      infoLine.style.color = "var(--ok)";
      infoLine.textContent = `测试成功（TTS ${res.tts_frames_sent || 0} 帧）: ${res.reply_text || ""}`;
    }
  } catch (err) {
    if (infoLine) {
      infoLine.style.color = "var(--err)";
      infoLine.textContent = `测试发送失败: ${err.message}`;
    }
  }
});

async function refreshAll() {
  await Promise.allSettled([refreshHealth(), refreshPrompt(), refreshDeepSeekSettings(), refreshEZDeviceStatus(), refreshDevices(), refreshMacs(), refreshAcls(), refreshSpeakers()]);
}

document.getElementById("refresh-all-btn").addEventListener("click", refreshAll);
refreshAll();
setInterval(refreshDevices, 5000);
