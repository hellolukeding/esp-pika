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
      return `<tr>
        <td><code>${id}</code></td>
        <td>${name}</td>
        <td>${s.role}</td>
        <td>${agent}</td>
        <td><code>${voice}</code></td>
        <td>${s.version || 0}</td>
        <td><button class="btn danger" onclick="deleteSpeaker('${id}')">删除</button></td>
      </tr>`;
    })
    .join("");
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

async function refreshAll() {
  await Promise.allSettled([refreshHealth(), refreshDevices(), refreshMacs(), refreshAcls(), refreshSpeakers()]);
}

document.getElementById("refresh-all-btn").addEventListener("click", refreshAll);
refreshAll();
setInterval(refreshDevices, 5000);
