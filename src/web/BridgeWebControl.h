/**
 * BridgeWebControl.h — Embedded HTTP server + Web UI for NDI Bridge X
 *
 * Pattern: same minimal HTTP server as tc_webcontrol.h
 * Features:
 *   - NDI source discovery
 *   - Start/stop host and join pipelines
 *   - Live stats (polling every 2s)
 *   - Dark professional theme
 *   - Cross-platform (POSIX + WinSock via Platform.h)
 */

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>

#include "../common/Platform.h"
#include "../common/Logger.h"
#include "../common/Version.h"
#include "BridgeManager.h"

#ifndef _WIN32
#include <netdb.h>
#endif

namespace ndi_bridge {

class BridgeWebControl {
public:
    BridgeWebControl(BridgeManager* manager, int port = 0, bool needsSudo = false)
        : manager_(manager), requestedPort_(port), needsSudo_(needsSudo) {}

    ~BridgeWebControl() { stop(); }

    bool start() {
        serverFd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (serverFd_ == INVALID_SOCKET_VAL) return false;

        int opt = 1;
        setsockopt(serverFd_, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&opt), sizeof(opt));

        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(static_cast<uint16_t>(requestedPort_));

        if (bind(serverFd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            platform_close_socket(serverFd_);
            serverFd_ = INVALID_SOCKET_VAL;
            return false;
        }

        // Get assigned port
        socklen_t addrLen = sizeof(addr);
        getsockname(serverFd_, (struct sockaddr*)&addr, &addrLen);
        port_ = ntohs(addr.sin_port);

        if (listen(serverFd_, 8) < 0) {
            platform_close_socket(serverFd_);
            serverFd_ = INVALID_SOCKET_VAL;
            return false;
        }

        char hostname[256] = "localhost";
        gethostname(hostname, sizeof(hostname));
        url_ = "http://" + std::string(hostname) + ":" + std::to_string(port_) + "/";

        running_ = true;
        thread_ = std::thread(&BridgeWebControl::serverLoop, this);

        Logger::instance().successf("Web UI started: %s", url_.c_str());
        return true;
    }

    void stop() {
        running_ = false;
        if (serverFd_ != INVALID_SOCKET_VAL) {
#ifdef _WIN32
            closesocket(serverFd_);
#else
            shutdown(serverFd_, SHUT_RDWR);
            close(serverFd_);
#endif
            serverFd_ = INVALID_SOCKET_VAL;
        }
        if (thread_.joinable()) thread_.join();
    }

    int port() const { return port_; }
    const std::string& url() const { return url_; }

private:
    BridgeManager* manager_;
    int requestedPort_ = 0;
    bool needsSudo_ = false;
    socket_t serverFd_ = INVALID_SOCKET_VAL;
    int port_ = 0;
    std::string url_;
    std::atomic<bool> running_{false};
    std::thread thread_;

    void serverLoop() {
        while (running_) {
#ifdef _WIN32
            WSAPOLLFD pfd = {};
            pfd.fd = serverFd_;
            pfd.events = POLLIN;
            int ret = WSAPoll(&pfd, 1, 500);
#else
            struct pollfd pfd = {serverFd_, POLLIN, 0};
            int ret = poll(&pfd, 1, 500);
#endif
            if (ret <= 0 || !running_) continue;

            struct sockaddr_in clientAddr;
            socklen_t clientLen = sizeof(clientAddr);
            socket_t client = accept(serverFd_, (struct sockaddr*)&clientAddr, &clientLen);
            if (client == INVALID_SOCKET_VAL) continue;

            // Set socket timeout
#ifdef _WIN32
            DWORD tv = 2000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
            struct timeval tv = {2, 0};
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

            handleClient(client);
            platform_close_socket(client);
        }
    }

    void handleClient(socket_t fd) {
        char buf[8192];
#ifdef _WIN32
        int n = recv(fd, buf, sizeof(buf) - 1, 0);
#else
        int n = static_cast<int>(read(fd, buf, sizeof(buf) - 1));
#endif
        if (n <= 0) return;
        buf[n] = '\0';

        char method[8] = {}, path[512] = {};
        sscanf(buf, "%7s %511s", method, path);

        if (strcmp(method, "GET") == 0) {
            if (strcmp(path, "/") == 0) {
                sendResponse(fd, 200, "text/html; charset=utf-8", buildHTML());
            } else if (strcmp(path, "/api/sources") == 0) {
                sendResponse(fd, 200, "application/json", buildSourcesJson());
            } else if (strcmp(path, "/api/pipelines") == 0) {
                sendResponse(fd, 200, "application/json", buildPipelinesJson());
            } else {
                sendResponse(fd, 404, "text/plain", "Not Found");
            }
        } else if (strcmp(method, "POST") == 0) {
            // Find body after \r\n\r\n
            char* body = strstr(buf, "\r\n\r\n");
            std::string bodyStr;
            if (body) bodyStr = body + 4;

            if (strcmp(path, "/api/host/add") == 0) {
                sendResponse(fd, 200, "application/json", handleAddHost(bodyStr));
            } else if (strcmp(path, "/api/join/add") == 0) {
                sendResponse(fd, 200, "application/json", handleAddJoin(bodyStr));
            } else if (strncmp(path, "/api/stop/", 10) == 0) {
                int id = atoi(path + 10);
                sendResponse(fd, 200, "application/json", handleStop(id));
            } else {
                sendResponse(fd, 404, "text/plain", "Not Found");
            }
        } else if (strcmp(method, "OPTIONS") == 0) {
            sendResponse(fd, 200, "text/plain", "");
        }
    }

    void sendResponse(socket_t fd, int code, const char* contentType, const std::string& body) {
        const char* status = (code == 200) ? "OK" : "Not Found";
        char header[512];
        snprintf(header, sizeof(header),
                 "HTTP/1.1 %d %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                 "Access-Control-Allow-Headers: Content-Type\r\n"
                 "\r\n",
                 code, status, contentType, body.size());
#ifdef _WIN32
        send(fd, header, (int)strlen(header), 0);
        send(fd, body.data(), (int)body.size(), 0);
#else
        write(fd, header, strlen(header));
        write(fd, body.data(), body.size());
#endif
    }

    // ── JSON helpers ─────────────────────────────────────────────

    static std::string jsonEscape(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:   out += c;      break;
            }
        }
        return out;
    }

    // Parse URL-encoded value: returns "" if key not found
    static std::string urlParam(const std::string& body, const std::string& key) {
        std::string search = key + "=";
        size_t pos = body.find(search);
        if (pos == std::string::npos) return "";
        pos += search.size();
        size_t end = body.find('&', pos);
        std::string val = (end == std::string::npos) ? body.substr(pos) : body.substr(pos, end - pos);
        // URL-decode %xx and +
        std::string decoded;
        for (size_t i = 0; i < val.size(); i++) {
            if (val[i] == '+') {
                decoded += ' ';
            } else if (val[i] == '%' && i + 2 < val.size()) {
                char hex[3] = { val[i+1], val[i+2], 0 };
                decoded += static_cast<char>(strtol(hex, nullptr, 16));
                i += 2;
            } else {
                decoded += val[i];
            }
        }
        return decoded;
    }

    // ── API handlers ─────────────────────────────────────────────

    std::string buildSourcesJson() {
        auto sources = manager_->discoverSources(3000);
        std::string json = "[";
        for (size_t i = 0; i < sources.size(); i++) {
            if (i > 0) json += ",";
            json += "{\"name\":\"" + jsonEscape(sources[i].name) + "\""
                  + ",\"address\":\"" + jsonEscape(sources[i].address) + "\"}";
        }
        json += "]";
        return json;
    }

    std::string buildPipelinesJson() {
        auto pipelines = manager_->getStatus();
        std::string json = "[";
        for (size_t i = 0; i < pipelines.size(); i++) {
            const auto& p = pipelines[i];
            if (i > 0) json += ",";
            char buf[1024];
            snprintf(buf, sizeof(buf),
                     "{\"id\":%d,\"type\":\"%s\",\"desc\":\"%s\",\"running\":%s,"
                     "\"videoRecv\":%lu,\"videoEnc\":%lu,\"videoDrop\":%lu,"
                     "\"videoDec\":%lu,\"videoOut\":%lu,\"audioOut\":%lu,"
                     "\"bytesSent\":%lu,\"time\":%.1f}",
                     p.id, p.type.c_str(), jsonEscape(p.description).c_str(),
                     p.running ? "true" : "false",
                     (unsigned long)p.videoFramesReceived,
                     (unsigned long)p.videoFramesEncoded,
                     (unsigned long)p.videoFramesDropped,
                     (unsigned long)p.videoFramesDecoded,
                     (unsigned long)p.videoFramesOutput,
                     (unsigned long)p.audioFramesOutput,
                     (unsigned long)p.bytesSent,
                     p.runTimeSeconds);
            json += buf;
        }
        json += "]";
        return json;
    }

    std::string handleAddHost(const std::string& body) {
        std::string source = urlParam(body, "source");
        std::string target = urlParam(body, "target");
        std::string portStr = urlParam(body, "port");
        std::string bitrateStr = urlParam(body, "bitrate");
        std::string mtuStr = urlParam(body, "mtu");

        if (source.empty() || target.empty()) {
            return "{\"ok\":false,\"error\":\"source and target required\"}";
        }

        uint16_t port = portStr.empty() ? 5990 : static_cast<uint16_t>(atoi(portStr.c_str()));
        int bitrate = bitrateStr.empty() ? 8 : atoi(bitrateStr.c_str());
        size_t mtu = mtuStr.empty() ? 1400 : static_cast<size_t>(atoi(mtuStr.c_str()));

        int id = manager_->addHost(source, target, port, bitrate, mtu);
        if (id < 0) {
            return "{\"ok\":false,\"error\":\"failed to start host\"}";
        }

        char buf[128];
        snprintf(buf, sizeof(buf), "{\"ok\":true,\"id\":%d}", id);
        return buf;
    }

    std::string handleAddJoin(const std::string& body) {
        std::string name = urlParam(body, "name");
        std::string portStr = urlParam(body, "port");

        if (name.empty()) {
            return "{\"ok\":false,\"error\":\"name required\"}";
        }

        uint16_t port = portStr.empty() ? 5990 : static_cast<uint16_t>(atoi(portStr.c_str()));

        int id = manager_->addJoin(name, port);
        if (id < 0) {
            return "{\"ok\":false,\"error\":\"failed to start join\"}";
        }

        char buf[128];
        snprintf(buf, sizeof(buf), "{\"ok\":true,\"id\":%d}", id);
        return buf;
    }

    std::string handleStop(int id) {
        bool ok = manager_->stopPipeline(id);
        if (ok) {
            return "{\"ok\":true}";
        }
        return "{\"ok\":false,\"error\":\"pipeline not found\"}";
    }

    // ── HTML UI ──────────────────────────────────────────────────

    std::string buildHTML() {
        return R"HTML(<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>NDI Bridge X</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{background:#0e0e0e;color:#ccc;font-family:-apple-system,system-ui,'Segoe UI',sans-serif;padding:20px;max-width:680px;margin:0 auto}
h1{font-size:18px;color:#eee;letter-spacing:1px;display:flex;align-items:center;gap:10px}
h1 span.ver{font-size:11px;color:#555;font-weight:normal}
.header{display:flex;justify-content:space-between;align-items:center;margin-bottom:20px;padding-bottom:12px;border-bottom:1px solid #222}
.header .status{font-size:11px;color:#555}
.section{margin-bottom:16px}
.section-title{font-size:11px;color:#666;text-transform:uppercase;letter-spacing:1.5px;margin-bottom:8px;padding-bottom:4px;border-bottom:1px solid #1a1a1a}
.card{background:#161616;border:1px solid #222;border-radius:6px;padding:14px;margin-bottom:8px}
.source-list{max-height:220px;overflow-y:auto}
.source-item{display:flex;align-items:center;gap:8px;padding:7px 0;border-bottom:1px solid #1a1a1a;font-size:13px}
.source-item:last-child{border-bottom:none}
.source-item input[type=checkbox]{accent-color:#ff8c00;width:15px;height:15px;cursor:pointer}
.source-item label{cursor:pointer;flex:1;user-select:none}
.form-row{display:flex;gap:10px;margin-bottom:8px;align-items:center}
.form-row label{font-size:12px;color:#888;min-width:70px}
.form-row input,.form-row select{background:#1e1e1e;color:#ccc;border:1px solid #333;border-radius:4px;padding:6px 8px;font-size:13px;flex:1}
.form-row input:focus,.form-row select:focus{outline:1px solid #ff8c00;border-color:#ff8c00}
.form-row .unit{font-size:11px;color:#666;min-width:30px}
.btn{border:none;border-radius:4px;padding:8px 16px;font-size:13px;cursor:pointer;font-weight:500;transition:background .15s}
.btn-primary{background:#ff8c00;color:#000}
.btn-primary:hover{background:#ff9f33}
.btn-primary:disabled{background:#553300;color:#886633;cursor:not-allowed}
.btn-danger{background:#661111;color:#ff6666}
.btn-danger:hover{background:#882222}
.btn-secondary{background:#2a2a2a;color:#aaa}
.btn-secondary:hover{background:#333}
.btn-sm{padding:4px 10px;font-size:11px}
.btn-row{display:flex;gap:8px;margin-top:10px}
.pipeline{background:#161616;border:1px solid #222;border-radius:6px;padding:12px;margin-bottom:8px;position:relative}
.pipeline .pl-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:6px}
.pipeline .pl-type{font-size:10px;text-transform:uppercase;letter-spacing:1px;padding:2px 6px;border-radius:3px;font-weight:600}
.pipeline .pl-type.host{background:#1a2a1a;color:#4a4}
.pipeline .pl-type.join{background:#1a1a2a;color:#66f}
.pipeline .pl-desc{font-size:13px;color:#ddd;margin-bottom:6px}
.pipeline .pl-stats{font-family:'SF Mono',Menlo,Consolas,monospace;font-size:11px;color:#888;line-height:1.6}
.pipeline .pl-stats span{color:#aaa}
.dot{display:inline-block;width:7px;height:7px;border-radius:50%;margin-right:4px}
.dot.on{background:#4a4;box-shadow:0 0 4px #4a4}
.dot.off{background:#444}
.empty{text-align:center;color:#444;font-size:13px;padding:20px}
.spinner{display:inline-block;width:12px;height:12px;border:2px solid #444;border-top-color:#ff8c00;border-radius:50%;animation:spin .6s linear infinite;margin-right:6px;vertical-align:middle}
@keyframes spin{to{transform:rotate(360deg)}}
.hidden{display:none}
.join-section{margin-top:4px}
.warning-banner{background:#442200;border:1px solid #884400;border-radius:6px;padding:10px 14px;margin-bottom:16px;font-size:12px;color:#ffaa33}
.warning-banner code{background:#331800;padding:2px 6px;border-radius:3px}
.remote-card{background:#161616;border:1px solid #222;border-radius:6px;padding:12px 14px;margin-bottom:8px;display:flex;justify-content:space-between;align-items:center}
.remote-card a{color:#ff8c00;text-decoration:none;font-size:13px}
.remote-card a:hover{text-decoration:underline}
.remote-status{font-size:11px;color:#666;margin-top:2px}
</style>
</head>
<body>

<div class="header">
  <h1>NDI Bridge X <span class="ver">)HTML" NDI_BRIDGE_VERSION R"HTML(</span></h1>
  <div class="status" id="status">connecting...</div>
</div>

<div id="sudo-warning" class="warning-banner hidden">
  <strong>sudo requis</strong> — Join via en7/SpeedFusion ne fonctionnera pas sans privileges root.<br>
  Relancez avec : <code>sudo ./build-mac/ndi-bridge-x --web-ui</code>
</div>

<!-- NDI Sources -->
<div class="section">
  <div class="section-title">Sources NDI disponibles</div>
  <div class="card">
    <div id="sources-list" class="source-list">
      <div class="empty"><span class="spinner"></span> Recherche en cours...</div>
    </div>
    <div class="btn-row">
      <button class="btn btn-secondary btn-sm" onclick="refreshSources()">Rafraichir</button>
    </div>
  </div>
</div>

<!-- Host Configuration -->
<div class="section">
  <div class="section-title">Configuration Host</div>
  <div class="card">
    <div class="form-row">
      <label>Target IP</label>
      <input type="text" id="target-host" value="127.0.0.1" placeholder="63.181.214.196">
    </div>
    <div class="form-row">
      <label>Port</label>
      <input type="number" id="target-port" value="5990" min="1024" max="65535">
    </div>
    <div class="form-row">
      <label>Bitrate</label>
      <input type="number" id="bitrate" value="8" min="1" max="50">
      <span class="unit">Mbps</span>
    </div>
    <div class="form-row">
      <label>MTU</label>
      <input type="number" id="mtu" value="1400" min="500" max="9000">
      <span class="unit">bytes</span>
    </div>
    <div class="btn-row">
      <button class="btn btn-primary" id="btn-start-hosts" onclick="startSelectedHosts()" disabled>Demarrer les bridges selectionnes</button>
    </div>
  </div>
</div>

<!-- Active Pipelines -->
<div class="section">
  <div class="section-title">Bridges actifs</div>
  <div id="pipelines-list">
    <div class="empty">Aucun bridge actif</div>
  </div>
</div>

<!-- Join Configuration -->
<div class="section join-section">
  <div class="section-title">Join (recevoir un flux)</div>
  <div class="card">
    <div class="form-row">
      <label>Nom NDI</label>
      <input type="text" id="join-name" value="NDI Bridge" placeholder="Pierre Frankfurt">
    </div>
    <div class="form-row">
      <label>Port</label>
      <input type="number" id="join-port" value="5991" min="1024" max="65535">
    </div>
    <div class="btn-row">
      <button class="btn btn-primary" onclick="startJoin()">Demarrer join</button>
    </div>
  </div>
</div>

<!-- Machines distantes -->
<div class="section">
  <div class="section-title">Machines distantes</div>
  <div id="remote-machines"></div>
  <div class="card" style="margin-top:4px">
    <div class="form-row">
      <label>URL</label>
      <input type="text" id="remote-url" placeholder="http://63.181.214.196:8080">
      <button class="btn btn-secondary btn-sm" onclick="addRemote()">Ajouter</button>
    </div>
  </div>
</div>

<script>
const NEEDS_SUDO = )HTML" + std::string(needsSudo_ ? "true" : "false") + R"HTML(;
let sources = [];
let selectedSources = new Set();

function refreshSources() {
  const el = document.getElementById('sources-list');
  el.innerHTML = '<div class="empty"><span class="spinner"></span> Recherche en cours...</div>';
  fetch('/api/sources')
    .then(r => r.json())
    .then(data => {
      sources = data;
      renderSources();
    })
    .catch(() => {
      el.innerHTML = '<div class="empty">Erreur de connexion</div>';
    });
}

function renderSources() {
  const el = document.getElementById('sources-list');
  if (sources.length === 0) {
    el.innerHTML = '<div class="empty">Aucune source NDI trouvee</div>';
    updateStartBtn();
    return;
  }
  let html = '';
  sources.forEach((s, i) => {
    const checked = selectedSources.has(s.name) ? 'checked' : '';
    html += '<div class="source-item">'
      + '<input type="checkbox" id="src-' + i + '" ' + checked + ' onchange="toggleSource(\'' + escHtml(s.name) + '\', this.checked)">'
      + '<label for="src-' + i + '">' + escHtml(s.name) + '</label>'
      + '</div>';
  });
  el.innerHTML = html;
  updateStartBtn();
}

function escHtml(s) {
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;').replace(/'/g,'&#39;');
}

function toggleSource(name, checked) {
  if (checked) selectedSources.add(name);
  else selectedSources.delete(name);
  updateStartBtn();
}

function updateStartBtn() {
  const btn = document.getElementById('btn-start-hosts');
  btn.disabled = selectedSources.size === 0;
}

function startSelectedHosts() {
  const target = document.getElementById('target-host').value.trim();
  const basePort = parseInt(document.getElementById('target-port').value) || 5990;
  const bitrate = parseInt(document.getElementById('bitrate').value) || 8;
  const mtu = parseInt(document.getElementById('mtu').value) || 1400;

  if (!target) { alert('Target IP requis'); return; }

  let portOffset = 0;
  selectedSources.forEach(source => {
    const port = basePort + portOffset;
    const body = 'source=' + encodeURIComponent(source)
      + '&target=' + encodeURIComponent(target)
      + '&port=' + port
      + '&bitrate=' + bitrate
      + '&mtu=' + mtu;

    fetch('/api/host/add', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: body
    }).then(r => r.json()).then(d => {
      if (!d.ok) alert('Erreur host: ' + (d.error || 'unknown'));
      pollPipelines();
    }).catch(e => alert('Erreur: ' + e));

    portOffset++;
  });

  selectedSources.clear();
  renderSources();
}

function startJoin() {
  const name = document.getElementById('join-name').value.trim();
  const port = parseInt(document.getElementById('join-port').value) || 5991;

  if (!name) { alert('Nom NDI requis'); return; }

  const body = 'name=' + encodeURIComponent(name) + '&port=' + port;
  fetch('/api/join/add', {
    method: 'POST',
    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
    body: body
  }).then(r => r.json()).then(d => {
    if (!d.ok) alert('Erreur join: ' + (d.error || 'unknown'));
    pollPipelines();
  }).catch(e => alert('Erreur: ' + e));
}

function stopPipeline(id) {
  fetch('/api/stop/' + id, {method: 'POST'})
    .then(r => r.json())
    .then(() => pollPipelines())
    .catch(e => alert('Erreur: ' + e));
}

function fmtBytes(b) {
  if (b < 1024) return b + ' B';
  if (b < 1048576) return (b / 1024).toFixed(1) + ' KB';
  return (b / 1048576).toFixed(2) + ' MB';
}

function fmtTime(s) {
  if (s < 60) return s.toFixed(0) + 's';
  const m = Math.floor(s / 60);
  const sec = Math.floor(s % 60);
  return m + 'm' + (sec < 10 ? '0' : '') + sec + 's';
}

function fmtFps(frames, seconds) {
  if (seconds < 1) return '--';
  return (frames / seconds).toFixed(1);
}

function renderPipelines(pipelines) {
  const el = document.getElementById('pipelines-list');
  if (pipelines.length === 0) {
    el.innerHTML = '<div class="empty">Aucun bridge actif</div>';
    return;
  }

  let html = '';
  pipelines.forEach(p => {
    const dotClass = p.running ? 'on' : 'off';
    const typeClass = p.type;
    let stats = '';

    if (p.type === 'host') {
      const fps = fmtFps(p.videoEnc, p.time);
      stats = '<span>video=</span>' + p.videoRecv + ' <span>encoded=</span>' + p.videoEnc
        + ' <span>qdrop=</span>' + p.videoDrop + ' <span>fps=</span>' + fps
        + '<br><span>sent=</span>' + fmtBytes(p.bytesSent) + ' <span>time=</span>' + fmtTime(p.time);
    } else {
      const fps = fmtFps(p.videoOut, p.time);
      stats = '<span>recv=</span>' + p.videoRecv + ' <span>decoded=</span>' + p.videoDec
        + ' <span>output=</span>' + p.videoOut + ' <span>fps=</span>' + fps
        + '<br><span>audio=</span>' + p.audioOut + ' <span>time=</span>' + fmtTime(p.time);
    }

    html += '<div class="pipeline">'
      + '<div class="pl-header">'
      + '<div><span class="dot ' + dotClass + '"></span>'
      + '<span class="pl-type ' + typeClass + '">' + p.type + ' #' + p.id + '</span></div>'
      + '<button class="btn btn-danger btn-sm" onclick="stopPipeline(' + p.id + ')">Stop</button>'
      + '</div>'
      + '<div class="pl-desc">' + escHtml(p.desc) + '</div>'
      + '<div class="pl-stats">' + stats + '</div>'
      + '</div>';
  });

  el.innerHTML = html;
}

function pollPipelines() {
  fetch('/api/pipelines')
    .then(r => r.json())
    .then(renderPipelines)
    .catch(() => {
      document.getElementById('status').textContent = 'deconnecte';
    });
}

function pollStatus() {
  fetch('/api/pipelines')
    .then(r => r.json())
    .then(data => {
      renderPipelines(data);
      const active = data.filter(p => p.running).length;
      document.getElementById('status').textContent = active + ' bridge' + (active !== 1 ? 's' : '') + ' actif' + (active !== 1 ? 's' : '');
    })
    .catch(() => {
      document.getElementById('status').textContent = 'deconnecte';
    });
}

// Remote machines
let remotes = JSON.parse(localStorage.getItem('ndi-bridge-remotes') || '[]');
function saveRemotes() { localStorage.setItem('ndi-bridge-remotes', JSON.stringify(remotes)); }

function addRemote() {
  let url = document.getElementById('remote-url').value.trim();
  if (!url) return;
  if (!url.startsWith('http')) url = 'http://' + url;
  url = url.replace(/\/+$/, '');
  if (!remotes.includes(url)) { remotes.push(url); saveRemotes(); }
  document.getElementById('remote-url').value = '';
  renderRemotes();
}

function removeRemote(i) { remotes.splice(i, 1); saveRemotes(); renderRemotes(); }

function renderRemotes() {
  const el = document.getElementById('remote-machines');
  if (remotes.length === 0) { el.innerHTML = '<div class="empty">Aucune machine distante configuree</div>'; return; }
  let html = '';
  remotes.forEach((url, i) => {
    html += '<div class="remote-card" id="remote-' + i + '">'
      + '<div><a href="' + escHtml(url) + '" target="_blank">' + escHtml(url) + ' &#8599;</a>'
      + '<div class="remote-status" id="rstatus-' + i + '">...</div></div>'
      + '<button class="btn btn-danger btn-sm" onclick="removeRemote(' + i + ')">&#10005;</button></div>';
  });
  el.innerHTML = html;
  pollRemotes();
}

function pollRemotes() {
  remotes.forEach((url, i) => {
    const el = document.getElementById('rstatus-' + i);
    if (!el) return;
    fetch(url + '/api/pipelines', {mode:'cors'})
      .then(r => r.json())
      .then(data => {
        const n = data.filter(p => p.running).length;
        el.innerHTML = '<span class="dot on"></span> ' + n + ' bridge' + (n !== 1 ? 's' : '') + ' actif' + (n !== 1 ? 's' : '');
        el.style.color = '#4a4';
      })
      .catch(() => { el.innerHTML = '<span class="dot off"></span> hors ligne'; el.style.color = '#666'; });
  });
}

// Initial load
if (NEEDS_SUDO) document.getElementById('sudo-warning').classList.remove('hidden');
refreshSources();
renderRemotes();
pollStatus();
setInterval(pollStatus, 2000);
setInterval(pollRemotes, 5000);
</script>
</body>
</html>
)HTML";
    }
};

} // namespace ndi_bridge
