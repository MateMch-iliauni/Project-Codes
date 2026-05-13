#include <Wire.h>
#include <Adafruit_MLX90640.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>

// ──────────────────────────────────────────────────────────────
// USER-CONFIGURABLE PARAMETERS
// ──────────────────────────────────────────────────────────────

const float TEMP_THRESHOLD           = 30.0f;
const unsigned long BEEP_INTERVAL_MS = 10000UL;
const unsigned long BEEP_DURATION_MS = 500UL;
const char* TELEGRAM_BOT_TOKEN       = "7655520460:AAG239a4LqNq0acoINI3Pn9RyacG_6AhRUc";
String TELEGRAM_CHAT_ID              = "8154736889";

// ──────────────────────────────────────────────────────────────
// PIN DEFINITIONS
// ──────────────────────────────────────────────────────────────

const int BUZZER_PIN = 4;
const int BUTTON_PIN = 5;   // connect button between GPIO 5 and GND

// ──────────────────────────────────────────────────────────────
// GLOBALS
// ──────────────────────────────────────────────────────────────

Adafruit_MLX90640 mlx;
float frame[32 * 24];

unsigned long lastAlertTime = 0;
bool deviceEnabled          = true;    // button toggles this

bool lastButtonState           = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long DEBOUNCE_MS = 50;

WiFiClientSecure     secureClient;
UniversalTelegramBot telegramBot(TELEGRAM_BOT_TOKEN, secureClient);

// ──────────────────────────────────────────────────────────────
// FORWARD DECLARATIONS
// ──────────────────────────────────────────────────────────────

void setupWiFi();
void sendTelegramAlert(float maxTemp);
void triggerBuzzer();
float getMaxTemperature();
void handleButton();

// ──────────────────────────────────────────────────────────────
// SETUP
// ──────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== Thermal Camera Alarm System ===");

    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    // Wiring: one leg of button → GPIO 5, other leg → GND
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    Wire.begin();
    Wire.setClock(400000);
    mlx.begin(MLX90640_I2CADDR_DEFAULT, &Wire);
    mlx.setMode(MLX90640_CHESS);
    mlx.setResolution(MLX90640_ADC_18BIT);
    mlx.setRefreshRate(MLX90640_2_HZ);

    Serial.printf("[OK] MLX90640 ready. Threshold: %.1f°C | Interval: %lu ms\n",TEMP_THRESHOLD, BEEP_INTERVAL_MS);

    setupWiFi();
    secureClient.setInsecure();

    Serial.println("[OK] Setup complete. Monitoring started.");
}

// ──────────────────────────────────────────────────────────────
// MAIN LOOP
// ──────────────────────────────────────────────────────────────

void loop() {
    handleButton();

    // If device is disabled, keep buzzer off and do nothing
    if (!deviceEnabled) {
        digitalWrite(BUZZER_PIN, LOW);
        delay(100);
        return;
    }

    if (mlx.getFrame(frame) != 0) {
        Serial.println("[WARN] Frame read failed – retrying.");
        delay(200);
        return;
    }

    float maxTemp = getMaxTemperature();
    Serial.printf("[TEMP] Max pixel temperature: %.2f°C\n", maxTemp);

    if (maxTemp > TEMP_THRESHOLD) {
        unsigned long now = millis();

        if (now - lastAlertTime >= BEEP_INTERVAL_MS) {
            lastAlertTime = now;
            Serial.printf("[ALARM] Anomaly detected! Max temp: %.2f°C\n", maxTemp);
            triggerBuzzer();
            sendTelegramAlert(maxTemp);
        }

    } else {
        digitalWrite(BUZZER_PIN, LOW);
    }

    delay(200);
}

// ──────────────────────────────────────────────────────────────
// Button: toggles device ON / OFF only (with debounce)
// ──────────────────────────────────────────────────────────────

void handleButton() {
    bool reading = digitalRead(BUTTON_PIN);

    if (reading != lastButtonState) {
        lastDebounceTime = millis();
    }

    if ((millis() - lastDebounceTime) > DEBOUNCE_MS) {
        static bool stableState = HIGH;
        if (reading == LOW && stableState == HIGH) {
            deviceEnabled = !deviceEnabled;
            digitalWrite(BUZZER_PIN, LOW);   // cut buzzer immediately on toggle
            Serial.printf("[BUTTON] Device %s by user.\n",deviceEnabled ? "ENABLED" : "DISABLED");
        }
        stableState = reading;
    }

    lastButtonState = reading;
}

// ──────────────────────────────────────────────────────────────
// Find the hottest pixel in the frame
// ──────────────────────────────────────────────────────────────

float getMaxTemperature() {
    float maxT = frame[0];
    for (int i = 1; i < 32 * 24; i++) {
        if (frame[i] > maxT) maxT = frame[i];
    }
    return maxT;
}

// ──────────────────────────────────────────────────────────────
// Sound the buzzer for BEEP_DURATION_MS
// ──────────────────────────────────────────────────────────────

void triggerBuzzer() {
    Serial.println("[BUZZER] Beep!");
    digitalWrite(BUZZER_PIN, HIGH);
    delay(BEEP_DURATION_MS);
    digitalWrite(BUZZER_PIN, LOW);
}

// ──────────────────────────────────────────────────────────────
// Wi-Fi via WiFiManager captive portal
// ──────────────────────────────────────────────────────────────

void setupWiFi() {
    WiFiManager wm;
    // ── Remove all menu items except WiFi config ──
    std::vector<const char*> menu = {"wifi"};
    wm.setMenu(menu);

    wm.setTitle("Configure Device");
    wm.setConfigPortalTimeout(600);

    WiFiManagerParameter custom_chat_id("chat", "Telegram Chat ID", "", 30);
    wm.addParameter(&custom_chat_id);

    Serial.println("[WiFi] Attempting connection…");

    if (wm.autoConnect("ThermalSystemSetup")) {
        String entered = String(custom_chat_id.getValue());
        if (entered.length() > 0) TELEGRAM_CHAT_ID = entered;
        Serial.printf("[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("[WiFi] No Wi-Fi – running offline.");
    }
}

// ──────────────────────────────────────────────────────────────
// Send a Telegram alert
// ──────────────────────────────────────────────────────────────

void sendTelegramAlert(float maxTemp) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[Telegram] Skipped – no Wi-Fi.");
        return;
    }

    String message = "🚨 *Thermal Alarm Triggered!*\n";
    message += "Max temperature detected: *" + String(maxTemp, 1) + " °C*\n";

    Serial.println("[Telegram] Sending alert…");
    telegramBot.sendMessage(TELEGRAM_CHAT_ID, message, "Markdown");
}
