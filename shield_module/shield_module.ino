#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include <EEPROM.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>

// --- Константы пинов ---
const int SENSOR_PIN = D5;
const int LED_PIN    = D2;

// --- Структура конфигурации (сохраняется в EEPROM) ---
struct ModuleConfig {
    bool configured;
    char ssid[32];
    char pass[64];
    char prefix[16];
    int id;          // 1 - 5
    int led_count;   // 1 - 256
    int resist;      // 1 - 60 секунд
    bool boat;       // Режим шлюпки
};

ModuleConfig config;

// --- Глобальные объекты ---
ESP8266WebServer server(80);
WiFiUDP udp;
Adafruit_NeoPixel strip;

// --- Сетевые настройки ---
IPAddress main_ip(192, 168, 4, 1);
const int udp_port = 1234;

// --- Режимы работы устройства ---
bool is_ap_mode = false;

// --- Состояния светодиодного автомата ---
enum LedMode {
    MODE_NORMAL,
    MODE_HIT,
    MODE_BOAT,
    MODE_OFF,
    MODE_CONFIG,
    MODE_DISCONNECTED
};
LedMode current_mode = MODE_NORMAL;

// --- Таймеры и переменные времени ---
unsigned long last_ping = 0;
unsigned long last_sensor_poll = 0;
unsigned long last_hit_time = 0;
unsigned long last_led_update = 0;
unsigned long hit_start_time = 0;
unsigned long last_wifi_check = 0;

int current_resist_time = 0;
int last_sensor_state = LOW;

// --- Переменные для перезагрузки ---
bool reboot_pending = false;
unsigned long reboot_time = 0;

// Прототипы функций
void startAPMode();
void startClientMode();
void handleConfigPage();
void handleSaveConfig();
void checkWiFiConnection();
void handleUDP();
void handleSensor();
void handlePing();
void handleLEDs();

// ==========================================
// ИНИЦИАЛИЗАЦИЯ
// ==========================================

void setup() {
    Serial.begin(115200);
    Serial.println("\n[SYSTEM] Shield module starting...");

    pinMode(SENSOR_PIN, INPUT);

    EEPROM.begin(512);
    EEPROM.get(0, config);

    if (!config.configured || config.id < 1 || config.id > 5 || config.led_count < 1 || config.led_count > 256) {
        Serial.println("[SYSTEM] Invalid or missing config. Entering AP Mode.");
        startAPMode();
    } else {
        Serial.println("[SYSTEM] Config loaded. Connecting to ship...");
        startClientMode();
    }
}

void loop() {
    if (reboot_pending) {
        if (millis() > reboot_time) {
            ESP.restart();
        }
        return;
    }

    if (is_ap_mode) {
        server.handleClient();
    } else {
        checkWiFiConnection();
        handleUDP();
        handleSensor();
        handlePing();
    }

    handleLEDs();
}

// ==========================================
// КОНТРОЛЬ ПОДКЛЮЧЕНИЯ (АНТИ-ЧИТ)
// ==========================================

void checkWiFiConnection() {
    if (millis() - last_wifi_check >= 1000) {
        last_wifi_check = millis();

        if (WiFi.status() != WL_CONNECTED) {
            if (current_mode != MODE_DISCONNECTED) {
                Serial.println("[WARN] WiFi Link LOST! Entering anti-cheat state.");
                current_mode = MODE_DISCONNECTED;
            }
        } else {
            if (current_mode == MODE_DISCONNECTED) {
                Serial.println("[NET] WiFi reconnected successfully!");
                current_mode = MODE_NORMAL;
            }
        }
    }
}

// ==========================================
// РЕЖИМ ТОЧКИ ДОСТУПА И ВЕБ-СЕРВЕР
// ==========================================

void startAPMode() {
    is_ap_mode = true;
    current_mode = MODE_CONFIG;

    WiFi.disconnect();
    WiFi.mode(WIFI_AP);

    strip.updateLength(config.configured ? config.led_count : 10);
    strip.updateType(NEO_GRB + NEO_KHZ800);
    strip.setPin(LED_PIN);
    strip.begin();
    strip.show();

    String mac = WiFi.macAddress();
    mac.replace(":", "");
    String ap_ssid = "shield_module_" + mac.substring(6);

    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    WiFi.softAP(ap_ssid.c_str(), "123456789");

    Serial.println("[NET] AP Mode Started: " + ap_ssid);

    server.on("/", HTTP_GET, handleConfigPage);
    server.on("/save", HTTP_POST, handleSaveConfig);
    server.begin();
}

void handleConfigPage() {
    // Подставляем сохраненные значения, если конфиг уже существует
    String val_ssid   = config.configured ? String(config.ssid) : "space_ship";
    String val_pass   = config.configured ? String(config.pass) : "123456789";
    String val_prefix = config.configured ? String(config.prefix) : "SHIELD";
    int val_id        = config.configured ? config.id : 1;
    int val_leds      = config.configured ? config.led_count : 10;
    int val_resist    = config.configured ? config.resist : 10;
    bool val_boat     = config.configured ? config.boat : false;

    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Shield Config</title>";
    html += "<style>body{font-family:sans-serif; background:#222; color:#fff; padding:20px;} ";
    html += "input[type='text'], input[type='password'], input[type='number'] {width:100%; padding:8px; margin:5px 0 15px; box-sizing:border-box;} ";
    html += "input[type='submit'] {background:#007BFF; color:white; border:none; padding:10px 20px; cursor:pointer; width:100%;} ";
    html += "form {max-width:400px; margin:0 auto; background:#333; padding:20px; border-radius:10px;} </style></head><body>";

    html += "<h2 style='text-align:center;'>Shield Configuration</h2>";
    html += "<form action='/save' method='POST'>";

    html += "<label>Ship SSID:</label><input type='text' name='ssid' value='" + val_ssid + "' required>";
    html += "<label>Ship Password:</label><input type='text' name='pass' value='" + val_pass + "' required>";
    html += "<label>Message Prefix:</label><input type='text' name='prefix' value='" + val_prefix + "' required>";
    html += "<label>Module ID (1-5):</label><input type='number' name='id' min='1' max='5' value='" + String(val_id) + "' required>";
    html += "<label>LED Count (1-256):</label><input type='number' name='led_count' min='1' max='256' value='" + String(val_leds) + "' required>";
    html += "<label>Resist/Blink Time (s):</label><input type='number' name='resist' min='1' max='60' value='" + String(val_resist) + "' required>";

    html += "<label><input type='checkbox' name='boat' value='1' " + String(val_boat ? "checked" : "") + "> Is Lifeboat?</label><br><br>";

    html += "<input type='submit' value='Save & Reboot'></form></body></html>";

    server.send(200, "text/html", html);
}

void handleSaveConfig() {
    config.configured = true;
    strncpy(config.ssid, server.arg("ssid").c_str(), sizeof(config.ssid) - 1);
    strncpy(config.pass, server.arg("pass").c_str(), sizeof(config.pass) - 1);
    strncpy(config.prefix, server.arg("prefix").c_str(), sizeof(config.prefix) - 1);
    config.id = server.arg("id").toInt();
    config.led_count = server.arg("led_count").toInt();
    config.resist = server.arg("resist").toInt();
    config.boat = server.hasArg("boat");

    EEPROM.put(0, config);
    EEPROM.commit();

    server.send(200, "text/plain", "Configuration saved! Rebooting in 3 seconds...");

    reboot_pending = true;
    reboot_time = millis() + 3000;
}

// ==========================================
// ШТАТНЫЙ РЕЖИМ И СВЯЗЬ (STA)
// ==========================================

void startClientMode() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(config.ssid, config.pass);

    Serial.print("[NET] Connecting to ");
    Serial.print(config.ssid);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 5) {
        delay(2000);
        Serial.print(".");
        attempts++;
    }

    // БАГ ИСПРАВЛЕН: Если не смогли подключиться — не затираем конфиг в EEPROM!
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\n[ERROR] Connection failed. Fallback to AP Mode (EEPROM Config preserved).");
        startAPMode();
        return;
    }

    Serial.println("\n[NET] Connected! IP: " + WiFi.localIP().toString());

    udp.begin(udp_port);

    strip.updateLength(config.led_count);
    strip.updateType(NEO_GRB + NEO_KHZ800);
    strip.setPin(LED_PIN);
    strip.begin();
    strip.show();

    current_mode = MODE_NORMAL;
}

void handlePing() {
    if (current_mode == MODE_DISCONNECTED) return;

    if (millis() - last_ping >= 2000) {
        last_ping = millis();
        String msg = "PING_" + String(config.id);
        udp.beginPacket(main_ip, udp_port);
        udp.print(msg);
        udp.endPacket();
    }
}

void handleUDP() {
    int packet_size = udp.parsePacket();
    if (packet_size > 0) {
        char buffer[64];
        int len = udp.read(buffer, sizeof(buffer) - 1);
        buffer[len] = '\0';
        String msg = String(buffer);

        if (msg == "NORMAL") {
            current_mode = MODE_NORMAL;
        }
        else if (msg == "BOAT_MODE") {
            current_mode = MODE_BOAT;
        }
        else if (msg == "OFF") {
            current_mode = MODE_OFF;
        }
        else if (msg.startsWith("BLINK_RED_")) {
            int first_underscore = 10;
            int second_underscore = msg.indexOf('_', first_underscore);

            if (second_underscore == -1) {
                current_resist_time = msg.substring(10).toInt();
                hit_start_time = millis();
                current_mode = MODE_HIT;
            } else {
                int target_id = msg.substring(10, second_underscore).toInt();
                int target_time = msg.substring(second_underscore + 1).toInt();

                if (target_id == config.id) {
                    current_resist_time = target_time;
                    hit_start_time = millis();
                    current_mode = MODE_HIT;
                }
            }
        }
    }
}

// ==========================================
// ЛОГИКА ДАТЧИКА ВИБРАЦИИ
// ==========================================

void handleSensor() {
    if (current_mode == MODE_DISCONNECTED) return;

    if (millis() - last_sensor_poll >= 10) {
        last_sensor_poll = millis();
        int state = digitalRead(SENSOR_PIN);

        if (state == HIGH && last_sensor_state == LOW) {
            if (millis() - last_hit_time >= 1000) {
                last_hit_time = millis();

                String msg = "HIT_" + String(config.id);
                udp.beginPacket(main_ip, udp_port);
                udp.print(msg);
                udp.endPacket();
            }
        }
        last_sensor_state = state;
    }
}

// ==========================================
// СВЕТОВОЙ КОНЕЧНЫЙ АВТОМАТ
// ==========================================

void handleLEDs() {
    if (millis() - last_led_update >= 50) {
        last_led_update = millis();
        static float phase = 0;

        switch (current_mode) {

            case MODE_CONFIG: {
                bool blink = (millis() / 500) % 2 == 0;
                strip.setBrightness(255);
                strip.fill(blink ? strip.Color(0, 0, 255) : strip.Color(0, 0, 0));
                strip.show();
                break;
            }

            case MODE_DISCONNECTED: {
                bool blink = (millis() / 1000) % 2 == 0;
                strip.setBrightness(255);
                strip.fill(blink ? strip.Color(0, 180, 255) : strip.Color(0, 0, 0));
                strip.show();
                break;
            }

            case MODE_NORMAL: {
                phase += 0.05;
                if (phase >= 2 * PI) phase -= 2 * PI;

                float multiplier = (sin(phase) + 1.0) / 2.0;
                uint8_t brightness = 127 + (128 * multiplier);

                strip.setBrightness(brightness);
                strip.fill(strip.Color(0, 255, 0));
                strip.show();
                break;
            }

            case MODE_HIT: {
                bool blink = (millis() / 500) % 2 == 0;
                strip.setBrightness(255);
                strip.fill(blink ? strip.Color(255, 0, 0) : strip.Color(0, 0, 0));
                strip.show();

                if (millis() - hit_start_time >= (current_resist_time * 1000UL)) {
                    current_mode = MODE_NORMAL;
                }
                break;
            }

            case MODE_BOAT: {
                if (config.boat) {
                    bool blink = (millis() / 500) % 2 == 0;
                    strip.setBrightness(255);
                    strip.fill(blink ? strip.Color(255, 0, 255) : strip.Color(0, 0, 0));
                } else {
                    strip.fill(strip.Color(0, 0, 0));
                }
                strip.show();
                break;
            }

            case MODE_OFF: {
                strip.fill(strip.Color(0, 0, 0));
                strip.show();
                break;
            }
        }
    }
}
