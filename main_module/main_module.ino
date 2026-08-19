#include <WiFi.h>
#include <WebServer.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <PINS_JC4827W543.h>

// --- Настройки сети и безопасности ---
String current_ap_ssid = "";
String current_ap_password = "123456789";
const char* admin_password = "42493"; // Пароль игротеха

// --- Одноразовые бортовые коды для инженеров ---
const char* revive_codes[3]  = {"SYS-REPAIR-1", "SYS-REPAIR-2", "SYS-REPAIR-3"};
const char* upgrade_codes[3] = {"CORE-UPG-A", "CORE-UPG-B", "CORE-UPG-C"};

bool revive_used[3]  = {false, false, false};
bool upgrade_used[3] = {false, false, false};

// --- Глобальные объекты ---
WebServer server(80);
WiFiUDP udp;
Preferences prefs;

// --- Константы системы ---
const int udp_port = 1234;
const int sos_button_pin = 17;

// --- Игровые флаги и состояния ---
bool shield_fall = false;
bool death = false;
bool sos = false;
bool boat = false;

// --- Игровые настройки (значения по умолчанию) ---
int max_heals = 3;
int regenerate = 30;
int restore = 10;
int shield_immune_duration = 3;

// --- Структура Щита ---
struct Shield {
    int heals;
    int hits;
    bool online;
    bool discovered;
    unsigned long last_ping;
    bool health_changed;
    unsigned long hit_immune_start;
};

Shield shields[5];

// --- Таймеры ---
unsigned long last_regen = 0;
unsigned long shield_fall_start = 0;
unsigned long last_display_update = 0;

// --- Состояние кнопки SOS ---
bool sos_button_pressed = false;

// Вспомогательная функция для генерации дефолтного SSID с ID ESP32
String getDefaultSSID() {
    uint64_t mac = ESP.getEfuseMac();
    uint16_t chip_id = (uint16_t)(mac >> 32);
    char buf[32];
    snprintf(buf, sizeof(buf), "space_ship-%04X", chip_id);
    return String(buf);
}

// ==========================================
// ОСНОВНОЙ ЦИКЛ И ИНИЦИАЛИЗАЦИЯ
// ==========================================

void setup() {
    Serial.begin(115200);
    Serial.println("\n[SYSTEM] Main module starting...");

    pinMode(sos_button_pin, INPUT_PULLUP);

    initDisplay();

    prefs.begin("game_state", false);
    loadGameState();

    initShields();
    last_regen = millis();

    setupWiFi();
    setupWebServer();
    udp.begin(udp_port);

    Serial.println("[SYSTEM] Main module ready!");
    updateDisplay();
}

void loop() {
    server.handleClient();
    checkSOSButton();
    handleUDPMessages();
    handleRegeneration();
    handleShieldFall();
    checkShieldsOnline();

    if (needDisplayUpdate()) {
        updateDisplay();
    }

    delay(50);
}

// ==========================================
// ЛОГИКА ИГРЫ И ОБРАБОТКА СОБЫТИЙ
// ==========================================

void handleHit(int shield_id) {
    if (death) return;

    Serial.println("[GAME] Hit registered on shield " + String(shield_id + 1));

    shields[shield_id].hits++;
    if (shields[shield_id].heals > 0) {
        shields[shield_id].heals--;
        shields[shield_id].health_changed = true;
    }

    shields[shield_id].hit_immune_start = millis();

    if (!boat) {
        udp.beginPacket(IPAddress(192, 168, 4, 255), udp_port);
        udp.print("BLINK_RED_" + String(shield_id + 1) + "_" + String(shield_immune_duration));
        udp.endPacket();
    }

    checkGameConditions(shield_id);
    saveGameState();
}

void checkGameConditions(int hit_shield_id) {
    if (death) return;

    if (boat) {
        if (hit_shield_id == 4 && shields[4].heals == 0) {
            death = true;
            shield_fall = false;
            shield_fall_start = 0;
            Serial.println("[GAME] DEATH: Lifeboat destroyed!");
            sendCommandToAllShields("OFF");
        }
    } else {
        if (!shield_fall && shields[hit_shield_id].heals == 0) {
            shield_fall = true;
            shield_fall_start = millis();

            Serial.println("[GAME] SHIELD FALL activated! Duration: " + String(restore) + "s");
            sendShieldFallCommands();
        }
        else if (shield_fall) {
            death = true;
            shield_fall = false;
            shield_fall_start = 0;
            Serial.println("[GAME] DEATH: Ship destroyed during shield failure!");
            sendCommandToAllShields("OFF");
        }
    }
}

void handleRegeneration() {
    if (death || shield_fall || boat) {
        last_regen = millis();
        return;
    }

    unsigned long current_time = millis();
    if (current_time - last_regen >= (regenerate * 1000UL)) {
        last_regen = current_time;
        bool any_regen = false;

        for (int i = 0; i < 5; i++) {
            if (shields[i].heals < max_heals) {
                shields[i].heals++;
                shields[i].health_changed = true;
                any_regen = true;
            }
        }
        if (any_regen) {
            Serial.println("[GAME] Regeneration triggered (+1 to all valid shields)");
            saveGameState();
        }
    }
}

void handleShieldFall() {
    if (death) {
        shield_fall = false;
        shield_fall_start = 0;
        return;
    }

    if (shield_fall) {
        if (boat) {
            shield_fall = false;
            shield_fall_start = 0;
            return;
        }

        unsigned long elapsed = (millis() - shield_fall_start) / 1000;

        if (elapsed >= restore) {
            shield_fall = false;
            shield_fall_start = 0;
            Serial.println("[GAME] Shield fall ended - restoring basic functionality");

            for (int i = 0; i < 5; i++) {
                if (shields[i].heals == 0) {
                    shields[i].heals = 1;
                    shields[i].health_changed = true;
                }
            }

            sendCommandToAllShields("NORMAL");
            saveGameState();
        }
    }
}

void checkSOSButton() {
    static bool last_button_state = HIGH;
    static unsigned long last_debounce = 0;

    bool current_state = digitalRead(sos_button_pin);

    if (current_state != last_button_state) {
        last_debounce = millis();
    }

    if ((millis() - last_debounce) > 50) {
        if (current_state == LOW && !sos_button_pressed && !death && !boat) {
            sos_button_pressed = true;
            sos = true;
            boat = true;

            shields[4].heals = 1;
            shields[4].health_changed = true;

            shield_fall = false;
            shield_fall_start = 0;

            Serial.println("[GAME] SOS BUTTON PRESSED! Boat mode enabled (Lifeboat health set to 1 HP).");
            sendBoatModeCommands();
            saveGameState();
        } else if (current_state == HIGH) {
            sos_button_pressed = false;
        }
    }
    last_button_state = current_state;
}

// ==========================================
// СЕТЬ И UDP КОММУНИКАЦИЯ
// ==========================================

void handleUDPMessages() {
    int packet_size = udp.parsePacket();
    if (packet_size > 0) {
        char buffer[64];
        int len = udp.read(buffer, sizeof(buffer) - 1);
        buffer[len] = '\0';
        String msg = String(buffer);

        if (msg.startsWith("PING_")) {
            int id = msg.substring(5).toInt() - 1;
            if (id >= 0 && id < 5) {
                shields[id].online = true;
                if (!shields[id].discovered) {
                    shields[id].discovered = true;
                    shields[id].health_changed = true;
                }
                shields[id].last_ping = millis();
            }
        }
        else if (msg.startsWith("HIT_")) {
            if (death) {
                Serial.println("[GAME] Hit ignored: Ship is DESTROYED.");
                return;
            }

            int id = msg.substring(4).toInt() - 1;

            if (boat && id != 4) {
                Serial.println("[GAME] Hit ignored: Boat mode active, normal shields are OFF.");
                return;
            }

            if (id >= 0 && id < 5 && shields[id].discovered) {
                if (millis() - shields[id].hit_immune_start >= (shield_immune_duration * 1000UL)) {
                    handleHit(id);
                } else {
                    Serial.println("[GAME] Hit ignored: Shield " + String(id + 1) + " is currently immune.");
                }
            }
        }
    }
}

void checkShieldsOnline() {
    unsigned long current_time = millis();
    for (int i = 0; i < 5; i++) {
        if (shields[i].online && (current_time - shields[i].last_ping) > 10000) {
            shields[i].online = false;
            Serial.println("[NET] Shield " + String(i + 1) + " went offline");
        }
    }
}

void sendCommandToAllShields(String command) {
    udp.beginPacket(IPAddress(192, 168, 4, 255), udp_port);
    udp.print(command);
    udp.endPacket();
}

void sendBoatModeCommands() {
    sendCommandToAllShields("BOAT_MODE");
}

void sendShieldFallCommands() {
    sendCommandToAllShields("BLINK_RED_" + String(restore));
}

// ==========================================
// ВЕБ-СЕРВЕР И АДМИНИСТРИРОВАНИЕ
// ==========================================

void setupWiFi() {
    WiFi.softAPdisconnect(true);
    WiFi.softAP(current_ap_ssid.c_str(), current_ap_password.c_str());
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    Serial.println("[NET] WiFi AP created: " + current_ap_ssid);
}

void setupWebServer() {
    server.on("/main", handleMainPage);
    server.on("/settings", HTTP_POST, handleSettings);
    server.on("/submit_code", HTTP_POST, handleSubmitCode);
    server.on("/master_reset", HTTP_POST, handleMasterReset);
    server.begin();
}

void handleMainPage() {
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Ship Dashboard</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no'>";

    html += "<style>";
    html += "body { font-family: sans-serif; background: #f4f4f9; padding: 10px; margin: 0; } ";
    html += "h1, h2 { text-align: center; color: #333; } ";
    html += ".table-wrapper { overflow-x: auto; max-width: 700px; margin: 0 auto 20px; } ";
    html += "table { border-collapse: collapse; width: 100%; min-width: 500px; background: #fff; font-size: 14px; } ";
    html += "th, td { border: 1px solid #ddd; padding: 10px 5px; text-align: center; } ";
    html += "th { background: #007BFF; color: #fff; } ";
    html += ".online { color: green; font-weight: bold; } .offline { color: red; font-weight: bold; } ";
    html += ".discovered { background: #e2f0d9; } .undiscovered { background: #fbe5d6; color: #888; } ";
    html += ".card { background: #fff; padding: 15px; border-radius: 8px; border: 1px solid #ddd; max-width: 700px; margin: 0 auto 20px; box-shadow: 0 2px 4px rgba(0,0,0,0.05); } ";
    html += "label { font-weight: bold; display: block; margin-bottom: 5px; color: #555; } ";

    html += "input[type='number'], input[type='password'], input[type='text'] { width: 100%; padding: 12px; margin-bottom: 15px; box-sizing: border-box; font-size: 16px; border: 1px solid #ccc; border-radius: 4px; } ";
    html += "input[type='submit'] { background: #007BFF; color: white; border: none; padding: 15px 20px; cursor: pointer; width: 100%; font-size: 16px; margin-bottom: 10px; border-radius: 4px; font-weight: bold; } ";
    html += "input[type='submit']:active { background: #0056b3; } ";
    html += ".btn-danger { background: #dc3545 !important; } .btn-warning { background: #ffc107 !important; color: #000 !important; } ";

    html += "#refresh-status { text-align: center; font-size: 14px; color: green; margin-bottom: 15px; font-weight: bold; padding: 10px; background: #e8f5e9; border-radius: 4px; max-width: 700px; margin-left: auto; margin-right: auto; } ";
    html += "</style>";

    html += "<script>";
    html += "let refreshTimer = setInterval(() => location.reload(), 3000); ";
    html += "function stopRefresh() { ";
    html += "  clearInterval(refreshTimer); ";
    html += "  let statusText = document.getElementById('refresh-status'); ";
    html += "  if(statusText) { statusText.innerText = '⚠️ Auto-refresh: PAUSED (Typing in progress)'; statusText.style.color = 'red'; statusText.style.background = '#ffebee'; } ";
    html += "} ";
    html += "window.onload = () => { ";
    html += "  document.querySelectorAll('input[type=\"password\"], input[type=\"number\"], input[type=\"text\"]').forEach(el => { ";
    html += "    el.addEventListener('focus', stopRefresh); ";
    html += "    el.addEventListener('input', stopRefresh); ";
    html += "  }); ";
    html += "}; ";
    html += "</script>";

    html += "</head><body>";

    html += "<h1>Control Center</h1>";
    html += "<div id='refresh-status'>🔄 Auto-refresh: ACTIVE (3s)</div>";

    html += "<div class='table-wrapper'><table><tr><th>Shield</th><th>Status</th><th>Network</th><th>Last Ping</th><th>Health</th><th>Hits</th></tr>";
    for (int i = 0; i < 5; i++) {
        String disc_class = shields[i].discovered ? "discovered" : "undiscovered";
        html += "<tr class='" + disc_class + "'><td><b>#" + String(i + 1) + "</b></td>";
        html += "<td>" + String(shields[i].discovered ? "Active" : "Hidden") + "</td>";
        html += "<td class='" + String(shields[i].online ? "online" : "offline") + "'>" + (shields[i].online ? "Online" : "Offline") + "</td>";
        html += "<td>" + String(shields[i].last_ping > 0 ? String((millis() - shields[i].last_ping) / 1000) + "s" : "Never") + "</td>";
        html += "<td><strong>" + String(shields[i].heals) + "</strong> / " + String(max_heals) + "</td>";
        html += "<td>" + String(shields[i].hits) + "</td></tr>";
    }
    html += "</table></div>";

    html += "<div class='card'><h2>System Logic</h2>";
    html += "<p><strong>Shield Fall:</strong> <span style='color:" + String(shield_fall?"red":"green") + "; font-weight:bold;'>" + (shield_fall?"ACTIVE":"Standby") + "</span></p>";
    html += "<p><strong>Death:</strong> <span style='color:" + String(death?"red":"green") + "; font-weight:bold;'>" + (death?"DESTROYED":"Alive") + "</span></p>";
    html += "<p><strong>Boat/SOS:</strong> <span style='color:" + String(boat?"purple":"gray") + "; font-weight:bold;'>" + (boat?"ACTIVE":"Inactive") + "</span></p></div>";

    html += "<div class='card'><h2>Engineering Terminal</h2>";
    html += "<form method='POST' action='/submit_code'>";
    html += "<label>Enter System Command Code:</label> <input type='text' name='ship_code' autocomplete='off' required placeholder='CORE-UPG-X'>";
    html += "<input type='submit' value='Execute Command'></form></div>";

    html += "<div class='card'><h2>Game & Network Settings (Admin Only)</h2>";
    html += "<form method='POST' action='/settings'>";
    html += "<label>Admin Password:</label> <input type='password' name='password' required>";
    html += "<label>WiFi AP SSID:</label> <input type='text' name='ap_ssid' value='" + current_ap_ssid + "' required>";
    html += "<label>WiFi AP Password (min 8 chars):</label> <input type='text' name='ap_pass' value='" + current_ap_password + "' minlength='8' required>";
    html += "<hr style='border:0; border-top:1px solid #eee; margin:15px 0;'>";
    html += "<label>Max Heals:</label> <input type='number' name='max_heals' value='" + String(max_heals) + "' min='1'>";
    html += "<label>Regeneration Timer (s):</label> <input type='number' name='regenerate' value='" + String(regenerate) + "' min='1'>";
    html += "<label>Global Shield Fall Duration (s):</label> <input type='number' name='restore' value='" + String(restore) + "' min='1'>";
    html += "<label>Local Shield Immunity/Blink (s):</label> <input type='number' name='shield_immune_duration' value='" + String(shield_immune_duration) + "' min='1' max='60'>";
    html += "<input type='submit' value='Save Settings'></form>";

    html += "<hr style='border:0; border-top:1px dashed #ccc; margin:20px 0;'>";
    html += "<form method='POST' action='/master_reset'>";
    html += "<label>Confirm Admin Password for FULL Reset:</label> <input type='password' name='password' required>";
    html += "<input class='btn-danger' type='submit' value='⚠️ FULL SYSTEM RESET (Clear States, Codes & Network)'></form>";
    html += "</div>";

    html += "</body></html>";
    server.send(200, "text/html", html);
}

void handleSettings() {
    if (server.arg("password") != admin_password) { server.send(401, "text/plain", "Unauthorized"); return; }

    bool wifi_changed = false;

    if (server.hasArg("ap_ssid") && server.arg("ap_ssid").length() > 0) {
        String new_ssid = server.arg("ap_ssid");
        new_ssid.trim();
        if (new_ssid != current_ap_ssid) {
            current_ap_ssid = new_ssid;
            wifi_changed = true;
        }
    }

    if (server.hasArg("ap_pass") && server.arg("ap_pass").length() >= 8) {
        String new_pass = server.arg("ap_pass");
        new_pass.trim();
        if (new_pass != current_ap_password) {
            current_ap_password = new_pass;
            wifi_changed = true;
        }
    }

    if (server.hasArg("max_heals")) max_heals = server.arg("max_heals").toInt();
    if (server.hasArg("regenerate")) regenerate = server.arg("regenerate").toInt();
    if (server.hasArg("restore")) restore = server.arg("restore").toInt();
    if (server.hasArg("shield_immune_duration")) shield_immune_duration = server.arg("shield_immune_duration").toInt();

    saveGameState();

    if (wifi_changed) {
        setupWiFi();
    }

    server.sendHeader("Location", "/main");
    server.send(302, "text/plain", "Settings saved");
}

void handleMasterReset() {
    if (server.arg("password") != admin_password) { server.send(401, "text/plain", "Unauthorized"); return; }

    death = false;
    shield_fall = false;
    sos = false;
    boat = false;
    shield_fall_start = 0;

    max_heals = 3;
    regenerate = 30;
    restore = 10;
    shield_immune_duration = 3;

    current_ap_ssid = getDefaultSSID();
    current_ap_password = "123456789";

    for (int i = 0; i < 3; i++) {
        revive_used[i]  = false;
        upgrade_used[i] = false;
    }

    for (int i = 0; i < 5; i++) {
        shields[i].heals = 1;
        shields[i].hits = 0;
        shields[i].online = false;
        shields[i].discovered = false;
        shields[i].health_changed = true;
    }

    saveGameState();
    setupWiFi();
    sendCommandToAllShields("NORMAL");
    updateDisplay();

    Serial.println("[MASTER_RESET] Game context and Network completely cleared to defaults by GM.");

    server.sendHeader("Location", "/main");
    server.send(302, "text/plain", "Full Reset Executed");
}

void handleSubmitCode() {
    if (!server.hasArg("ship_code")) {
        server.sendHeader("Location", "/main");
        server.send(302, "text/plain", "Bad Request");
        return;
    }

    String input = server.arg("ship_code");
    input.trim();
    bool accepted = false;

    for (int i = 0; i < 3; i++) {
        if (!revive_used[i] && input.equalsIgnoreCase(revive_codes[i])) {
            revive_used[i] = true;

            death = false;
            shield_fall = false;
            sos = false;
            boat = false;

            for (int j = 0; j < 5; j++) {
                shields[j].heals = 1;
                shields[j].health_changed = true;
            }

            sendCommandToAllShields("NORMAL");
            accepted = true;
            Serial.println("[ENGINEERING] Core system REPAIRED via code: " + input);
            break;
        }
    }

    if (!accepted) {
        for (int i = 0; i < 3; i++) {
            if (!upgrade_used[i] && input.equalsIgnoreCase(upgrade_codes[i])) {
                upgrade_used[i] = true;

                if (i == 0) {
                    max_heals += 1;
                    regenerate -= 5;
                    Serial.println("[ENGINEERING] Upgrade Lvl 1 activated");
                }
                else if (i == 1) {
                    restore -= 3;
                    regenerate -= 5;
                    Serial.println("[ENGINEERING] Upgrade Lvl 2 activated");
                }
                else if (i == 2) {
                    max_heals += 1;
                    shield_immune_duration += 1;
                    Serial.println("[ENGINEERING] Upgrade Lvl 3 activated");
                }

                if (regenerate < 2) regenerate = 2;
                if (restore < 2) restore = 2;

                accepted = true;
                break;
            }
        }
    }

    if (accepted) {
        saveGameState();
    } else {
        Serial.println("[ENGINEERING] Code REJECTED or already used: " + input);
    }

    server.sendHeader("Location", "/main");
    server.send(302, "text/plain", accepted ? "Executed" : "Invalid Code");
}

// ==========================================
// ГРАФИКА И ДИСПЛЕЙ
// ==========================================

void initDisplay() {
    if (!gfx->begin()) {
        Serial.println("[ERROR] Display init failed!");
        return;
    }
    #ifdef GFX_BL
    pinMode(GFX_BL, OUTPUT);
    digitalWrite(GFX_BL, HIGH);
    #endif
    gfx->fillScreen(RGB565_BLACK);
}

bool needDisplayUpdate() {
    for (int i = 0; i < 5; i++) {
        if (shields[i].health_changed) return true;
    }
    unsigned long now = millis();
    bool needs_animation = false;

    if (shield_fall && shield_fall_start > 0) {
        needs_animation = true;
    } else {
        for (int i = 0; i < 5; i++) {
            if (!shields[i].discovered) continue;
            if (now - shields[i].hit_immune_start < (shield_immune_duration * 1000UL)) {
                needs_animation = true;
                break;
            }
            if (!death && shields[i].heals < max_heals) {
                needs_animation = true;
                break;
            }
        }
    }
    if (needs_animation && (now - last_display_update) >= 100) {
        return true;
    }
    if ((sos || death) && (now - last_display_update) >= 500) {
        return true;
    }
    return false;
}

void updateDisplay() {
    last_display_update = millis();
    gfx->fillScreen(RGB565_BLACK);
    drawShieldCircles();
    drawStatusMessages();

    for (int i = 0; i < 5; i++) {
        shields[i].health_changed = false;
    }
}

void drawShieldCircles() {
    int cx = gfx->width() / 2;
    int cy = gfx->height() / 2 + 20;
    int r = 30;
    int R = 80;

    int discovered_count = 0;
    for (int i = 0; i < 5; i++) {
        if (shields[i].discovered) discovered_count++;
    }

    if (discovered_count == 0) {
        gfx->setTextColor(RGB565_WHITE);
        gfx->setTextSize(2);
        gfx->setCursor(cx - 60, cy - 10);
        gfx->print("SEARCHING...");
        return;
    }

    int current_index = 0;
    for (int i = 0; i < 5; i++) {
        if (!shields[i].discovered) continue;

        float angle = PI / 2.0 + (float)current_index * (2.0 * PI / (float)discovered_count);
        current_index++;

        int x = cx + cos(angle) * R;
        int y = cy + sin(angle) * R;

        uint16_t color;
        bool is_blink = (millis() / 250) % 2 == 0;

        if (death) {
            color = RGB565_BLACK;
        }
        else if (boat) {
            if (i == 4) color = is_blink ? RGB565_MAGENTA : RGB565_BLACK;
            else color = 0x39E7;
        }
        else if (shield_fall) color = is_blink ? RGB565_RED : RGB565_YELLOW;
        else if (shields[i].heals == 0) color = RGB565_RED;
        else if (shields[i].heals <= max_heals / 2) color = RGB565_YELLOW;
        else color = RGB565_GREEN;

        gfx->fillCircle(x, y, r, color);
        gfx->drawCircle(x, y, r, RGB565_WHITE);

        unsigned long hit_elapsed = millis() - shields[i].hit_immune_start;
        unsigned long hit_total = shield_immune_duration * 1000UL;

        unsigned long sf_elapsed = millis() - shield_fall_start;
        unsigned long sf_total = restore * 1000UL;

        unsigned long regen_elapsed = millis() - last_regen;
        unsigned long regen_total = regenerate * 1000UL;

        if (boat && i != 4) {
            drawArcTimer(x, y, r + 5, 0.0, RGB565_BLACK);
        }
        else if (hit_elapsed < hit_total) {
            float progress = 1.0 - ((float)hit_elapsed / (float)hit_total);
            drawArcTimer(x, y, r + 5, progress, RGB565_RED);
        }
        else if (shield_fall && shield_fall_start > 0 && sf_elapsed < sf_total) {
            float progress = 1.0 - ((float)sf_elapsed / (float)sf_total);
            drawArcTimer(x, y, r + 5, progress, RGB565_YELLOW);
        }
        else if (!death && !boat && shields[i].heals < max_heals) {
            float progress = (float)regen_elapsed / (float)regen_total;
            if (progress > 1.0) progress = 1.0;
            drawArcTimer(x, y, r + 5, progress, RGB565_BLUE);
        }
        else {
            drawArcTimer(x, y, r + 5, 0.0, RGB565_BLACK);
        }

        gfx->setTextColor(death ? RGB565_WHITE : RGB565_BLACK);
        gfx->setTextSize(2);
        String txt = String(shields[i].heals);
        int tw = txt.length() * 12;
        gfx->setCursor(x - tw/2, y - 8);

        if (!(boat && i != 4)) {
            gfx->print(txt);
        }
    }
}

void drawArcTimer(int cx, int cy, int radius, float progress, uint16_t color) {
    if (progress < 0.0) progress = 0.0;
    if (progress > 1.0) progress = 1.0;

    int total_segments = 36;
    int active_segments = (int)(progress * total_segments);
    float angle_step = (2.0 * PI) / total_segments;

    for (int i = 0; i < total_segments; i++) {
        float a1 = -PI/2.0 + (i * angle_step);
        float a2 = -PI/2.0 + ((i + 1) * angle_step);

        int x1 = cx + (int)(cos(a1)*radius);
        int y1 = cy + (int)(sin(a1)*radius);
        int x2 = cx + (int)(cos(a2)*radius);
        int y2 = cy + (int)(sin(a2)*radius);

        if (i < active_segments && progress > 0.0) {
            gfx->drawLine(x1, y1, x2, y2, color);
        }
        else {
            gfx->drawLine(x1, y1, x2, y2, RGB565_BLACK);
        }
    }
}

void drawStatusMessages() {
    gfx->setTextSize(2);
    int y = 15;
    bool blink = (millis() / 500) % 2 == 0;

    if (death && blink) {
        gfx->setTextColor(RGB565_RED);
        gfx->setCursor(20, y);
        gfx->print("DESTROYED");
    }
    else if (shield_fall && shield_fall_start > 0) {
        gfx->setTextColor(RGB565_YELLOW);
        gfx->setCursor(10, y);
        int rem = restore - ((millis() - shield_fall_start) / 1000);
        gfx->print("SHIELDS DOWN "); gfx->print(rem >= 0 ? rem : 0); gfx->print("s");

        if (blink) {
            gfx->setTextColor(RGB565_RED);
            gfx->setCursor(30, y + 25);
            gfx->print("VULNERABLE!");
        }
    }
    else if (sos && blink) {
        gfx->setTextColor(RGB565_MAGENTA);
        gfx->setCursor(20, y);
        gfx->print("LIFEBOAT MODE");
    }
}

// ==========================================
// РАБОТА С ПАМЯТЬЮ (FLASH / PREFERENCES)
// ==========================================

void loadGameState() {
    current_ap_ssid = prefs.getString("ap_ssid", getDefaultSSID());
    current_ap_password = prefs.getString("ap_pass", "123456789");

    shield_fall = prefs.getBool("shield_fall", false);
    death = prefs.getBool("death", false);
    sos = prefs.getBool("sos", false);
    boat = prefs.getBool("boat", false);
    max_heals = prefs.getInt("max_heals", 3);
    regenerate = prefs.getInt("regenerate", 30);
    restore = prefs.getInt("restore", 10);
    shield_fall_start = prefs.getULong("sf_start", 0);
    shield_immune_duration = prefs.getInt("imm_dur", 3);

    for (int i = 0; i < 3; i++) {
        revive_used[i]  = prefs.getBool(("rev_u_" + String(i)).c_str(), false);
        upgrade_used[i] = prefs.getBool(("upg_u_" + String(i)).c_str(), false);
    }

    for (int i = 0; i < 5; i++) {
        String key = "hits_" + String(i);
        shields[i].hits = prefs.getInt(key.c_str(), 0);
        shields[i].hit_immune_start = millis() - 10000;
        shields[i].discovered = false;
    }
}

void saveGameState() {
    prefs.putString("ap_ssid", current_ap_ssid);
    prefs.putString("ap_pass", current_ap_password);

    prefs.putBool("shield_fall", shield_fall);
    prefs.putBool("death", death);
    prefs.putBool("sos", sos);
    prefs.putBool("boat", boat);
    prefs.putInt("max_heals", max_heals);
    prefs.putInt("regenerate", regenerate);
    prefs.putInt("restore", restore);
    prefs.putULong("sf_start", shield_fall_start);
    prefs.putInt("imm_dur", shield_immune_duration);

    for (int i = 0; i < 3; i++) {
        prefs.putBool(("rev_u_" + String(i)).c_str(), revive_used[i]);
        prefs.putBool(("upg_u_" + String(i)).c_str(), upgrade_used[i]);
    }

    for (int i = 0; i < 5; i++) {
        String key = "hits_" + String(i);
        prefs.putInt(key.c_str(), shields[i].hits);
    }
}

void initShields() {
    for (int i = 0; i < 5; i++) {
        shields[i].heals = 1;
        shields[i].online = false;
        shields[i].last_ping = 0;
        shields[i].health_changed = true;
    }
}
