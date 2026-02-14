/**
 * tc_webcontrol.h — Embedded HTTP server for NDI web control
 *
 * Registers as NDI Web Control so right-click in Studio Monitor
 * opens "Paramètres Timecode" config page.
 *
 * Features:
 *   - TC framerate selector (9 broadcast standards)
 *   - LTC audio gain slider (-40 to 0 dBFS)
 *   - Mute toggle
 *   - 440Hz debug tone toggle
 *   - Live timecode display (auto-refresh)
 *   - Dark professional UI matching the TC display aesthetic
 *
 * Thread-safe: settings are shared via std::atomic with the main loop.
 */

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>

// ── Shared settings between web control and main loop ───────────────

struct TCWebSettings {
    // Controlled by web UI, read by main loop
    std::atomic<int>   tcFpsIndex{2};       // index into TC_FRAMERATES (default: 25fps)
    std::atomic<float> ltcGainDb{-3.0f};    // LTC volume in dBFS
    std::atomic<bool>  ltcMuted{false};
    std::atomic<bool>  audio440{false};     // 440Hz on channel 2
    std::atomic<bool>  changed{false};      // flag: main loop should re-apply settings

    // Written by main loop, read by web UI for live display
    std::atomic<int>   hh{0}, mm{0}, ss{0}, ff{0};
    std::atomic<int>   frameCount{0};
    std::atomic<int>   receivers{0};
    std::atomic<bool>  dropFrame{false};
};

// ── Minimal HTTP server ─────────────────────────────────────────────

class TCWebControl {
public:
    TCWebControl(TCWebSettings* settings) : m_settings(settings) {}

    ~TCWebControl() { stop(); }

    bool start() {
        m_serverFd = socket(AF_INET, SOCK_STREAM, 0);
        if (m_serverFd < 0) return false;

        int opt = 1;
        setsockopt(m_serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = 0;  // auto-assign

        if (bind(m_serverFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(m_serverFd);
            m_serverFd = -1;
            return false;
        }

        // Get assigned port
        socklen_t addrLen = sizeof(addr);
        getsockname(m_serverFd, (struct sockaddr*)&addr, &addrLen);
        m_port = ntohs(addr.sin_port);

        if (listen(m_serverFd, 4) < 0) {
            close(m_serverFd);
            m_serverFd = -1;
            return false;
        }

        // Get hostname for URL
        char hostname[256] = "localhost";
        gethostname(hostname, sizeof(hostname));
        m_url = "http://" + std::string(hostname) + ":" + std::to_string(m_port) + "/";

        m_running = true;
        m_thread = std::thread(&TCWebControl::serverLoop, this);
        return true;
    }

    void stop() {
        m_running = false;
        if (m_serverFd >= 0) {
            shutdown(m_serverFd, SHUT_RDWR);
            close(m_serverFd);
            m_serverFd = -1;
        }
        if (m_thread.joinable()) m_thread.join();
    }

    int port() const { return m_port; }
    const std::string& url() const { return m_url; }

    // NDI metadata XML for web control registration
    std::string ndiMetadataXml() const {
        return "<ndi_web_control url=\"" + m_url + "\"/>";
    }

private:
    TCWebSettings* m_settings;
    int m_serverFd = -1;
    int m_port = 0;
    std::string m_url;
    std::atomic<bool> m_running{false};
    std::thread m_thread;

    void serverLoop() {
        while (m_running) {
            struct pollfd pfd = {m_serverFd, POLLIN, 0};
            int ret = poll(&pfd, 1, 500);
            if (ret <= 0 || !m_running) continue;

            struct sockaddr_in clientAddr;
            socklen_t clientLen = sizeof(clientAddr);
            int client = accept(m_serverFd, (struct sockaddr*)&clientAddr, &clientLen);
            if (client < 0) continue;

            // Set socket timeout
            struct timeval tv = {2, 0};
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            handleClient(client);
            close(client);
        }
    }

    void handleClient(int fd) {
        char buf[4096];
        int n = read(fd, buf, sizeof(buf) - 1);
        if (n <= 0) return;
        buf[n] = '\0';

        char method[8] = {}, path[256] = {};
        sscanf(buf, "%7s %255s", method, path);

        if (strcmp(method, "GET") == 0) {
            if (strcmp(path, "/") == 0) {
                std::string html = buildHTML();
                sendResponse(fd, 200, "text/html; charset=utf-8", html);
            } else if (strcmp(path, "/api/status") == 0) {
                sendResponse(fd, 200, "application/json", buildStatusJson());
            } else {
                sendResponse(fd, 404, "text/plain", "Not Found");
            }
        } else if (strcmp(method, "POST") == 0) {
            if (strncmp(path, "/api/settings", 13) == 0) {
                // Find body after \r\n\r\n
                char* body = strstr(buf, "\r\n\r\n");
                if (body) {
                    body += 4;
                    applySettings(body);
                }
                sendResponse(fd, 200, "application/json", buildStatusJson());
            } else {
                sendResponse(fd, 404, "text/plain", "Not Found");
            }
        }
    }

    void sendResponse(int fd, int code, const char* contentType, const std::string& body) {
        const char* status = (code == 200) ? "OK" : "Not Found";
        char header[512];
        snprintf(header, sizeof(header),
                 "HTTP/1.1 %d %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "\r\n",
                 code, status, contentType, body.size());
        write(fd, header, strlen(header));
        write(fd, body.data(), body.size());
    }

    // Parse URL-encoded form body: key=value&key2=value2
    void applySettings(const char* body) {
        // Parse tc_fps_index
        const char* p = strstr(body, "tc_fps=");
        if (p) {
            int idx = atoi(p + 7);
            if (idx >= 0 && idx <= 8) {
                m_settings->tcFpsIndex.store(idx);
                m_settings->changed.store(true);
            }
        }

        p = strstr(body, "gain=");
        if (p) {
            float g = static_cast<float>(atof(p + 5));
            if (g >= -40.0f && g <= 0.0f) {
                m_settings->ltcGainDb.store(g);
                m_settings->changed.store(true);
            }
        }

        p = strstr(body, "mute=");
        if (p) {
            m_settings->ltcMuted.store(p[5] == '1');
            m_settings->changed.store(true);
        }

        p = strstr(body, "tone=");
        if (p) {
            m_settings->audio440.store(p[5] == '1');
            m_settings->changed.store(true);
        }
    }

    std::string buildStatusJson() {
        char json[512];
        snprintf(json, sizeof(json),
                 "{\"tc\":\"%02d:%02d:%02d%c%02d\","
                 "\"fps_index\":%d,"
                 "\"gain\":%.1f,"
                 "\"mute\":%s,"
                 "\"tone\":%s,"
                 "\"frames\":%d,"
                 "\"receivers\":%d}",
                 m_settings->hh.load(), m_settings->mm.load(),
                 m_settings->ss.load(),
                 m_settings->dropFrame.load() ? ';' : ':',
                 m_settings->ff.load(),
                 m_settings->tcFpsIndex.load(),
                 m_settings->ltcGainDb.load(),
                 m_settings->ltcMuted.load() ? "true" : "false",
                 m_settings->audio440.load() ? "true" : "false",
                 m_settings->frameCount.load(),
                 m_settings->receivers.load());
        return json;
    }

    std::string buildHTML() {
        return R"HTML(<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Timecode Settings</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{background:#111;color:#ccc;font-family:-apple-system,system-ui,sans-serif;
  padding:20px;max-width:420px;margin:0 auto}
h1{font-size:14px;color:#666;text-transform:uppercase;letter-spacing:2px;margin-bottom:20px}
.tc-display{background:#0a0a0a;border:2px solid #2a2a2a;border-radius:8px;
  padding:16px 24px;text-align:center;margin-bottom:24px;position:relative}
.tc-value{font-family:'Courier New',monospace;font-size:42px;color:#ff8c00;
  text-shadow:0 0 12px rgba(255,140,0,0.4);letter-spacing:4px}
.tc-label{font-size:11px;color:#555;margin-top:6px}
.card{background:#1a1a1a;border:1px solid #252525;border-radius:6px;padding:16px;margin-bottom:12px}
.card-title{font-size:11px;color:#555;text-transform:uppercase;letter-spacing:1px;margin-bottom:10px}
select,input[type=range]{width:100%;margin:4px 0;accent-color:#ff8c00}
select{background:#252525;color:#ccc;border:1px solid #333;border-radius:4px;
  padding:8px 10px;font-size:14px;appearance:auto;cursor:pointer}
select:focus{outline:1px solid #ff8c00;border-color:#ff8c00}
.gain-row{display:flex;align-items:center;gap:10px}
.gain-row input{flex:1}
.gain-val{font-family:monospace;font-size:13px;color:#ff8c00;min-width:60px;text-align:right}
.toggle{display:flex;align-items:center;gap:10px;padding:6px 0;cursor:pointer}
.toggle input{accent-color:#ff8c00;width:16px;height:16px;cursor:pointer}
.toggle label{font-size:13px;cursor:pointer;user-select:none}
.status{font-size:11px;color:#444;text-align:center;margin-top:16px}
.dot{display:inline-block;width:6px;height:6px;border-radius:50%;margin-right:4px}
.dot.on{background:#ff8c00}.dot.off{background:#333}
</style>
</head>
<body>
<h1>Timecode Settings</h1>

<div class="tc-display">
  <div class="tc-value" id="tc">--:--:--:--</div>
  <div class="tc-label" id="tc-info">-- fps | -- frames</div>
</div>

<div class="card">
  <div class="card-title">Framerate timecode</div>
  <select id="fps" onchange="send()">
    <option value="0">23.976 fps &mdash; Cinema NTSC</option>
    <option value="1">24 fps &mdash; Cinema</option>
    <option value="2" selected>25 fps &mdash; PAL/SECAM</option>
    <option value="3">29.97 fps &mdash; NTSC Drop-Frame</option>
    <option value="4">29.97 fps &mdash; NTSC Non-Drop</option>
    <option value="5">30 fps &mdash; Progressive</option>
    <option value="6">50 fps &mdash; PAL HFR</option>
    <option value="7">59.94 fps &mdash; NTSC HFR</option>
    <option value="8">60 fps &mdash; Progressive HFR</option>
  </select>
</div>

<div class="card">
  <div class="card-title">Audio LTC</div>
  <div class="gain-row">
    <input type="range" id="gain" min="-40" max="0" step="1" value="-3"
           oninput="updGain()" onchange="send()">
    <span class="gain-val" id="gain-val">-3 dBFS</span>
  </div>
  <div class="toggle">
    <input type="checkbox" id="mute" onchange="send()">
    <label for="mute">Mute LTC</label>
  </div>
  <div class="toggle">
    <input type="checkbox" id="tone" onchange="send()">
    <label for="tone">440 Hz tone (ch2, debug)</label>
  </div>
</div>

<div class="status">
  <span class="dot" id="dot"></span>
  <span id="status">connecting...</span>
</div>

<script>
const fpsNames = ['23.976','24','25','29.97 DF','29.97 NDF','30','50','59.94','60'];

function updGain() {
  const v = document.getElementById('gain').value;
  document.getElementById('gain-val').textContent = v + ' dBFS';
}

function send() {
  const body = 'tc_fps=' + document.getElementById('fps').value
    + '&gain=' + document.getElementById('gain').value
    + '&mute=' + (document.getElementById('mute').checked ? '1' : '0')
    + '&tone=' + (document.getElementById('tone').checked ? '1' : '0');
  fetch('/api/settings', {method:'POST', body: body,
    headers:{'Content-Type':'application/x-www-form-urlencoded'}})
    .then(r => r.json()).then(applyStatus).catch(()=>{});
}

function applyStatus(d) {
  document.getElementById('tc').textContent = d.tc;
  document.getElementById('fps').value = d.fps_index;
  document.getElementById('gain').value = d.gain;
  document.getElementById('gain-val').textContent = d.gain + ' dBFS';
  document.getElementById('mute').checked = d.mute;
  document.getElementById('tone').checked = d.tone;
  document.getElementById('tc-info').textContent =
    fpsNames[d.fps_index] + ' | ' + d.frames + ' frames';
  document.getElementById('dot').className = 'dot ' + (d.receivers > 0 ? 'on' : 'off');
  document.getElementById('status').textContent =
    d.receivers + ' receiver' + (d.receivers !== 1 ? 's' : '') + ' connected';
}

function poll() {
  fetch('/api/status').then(r => r.json()).then(applyStatus).catch(()=>{});
}

setInterval(poll, 200);
poll();
</script>
</body>
</html>
)HTML";
    }
};
