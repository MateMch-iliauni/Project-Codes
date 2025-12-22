#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>

const char* ssid     = "WiFi-SSID";
const char* password = "Password";

String botToken = "Telegram_BotToken";
String chatId   = "Telegram_chatId";

WiFiClientSecure client;
UniversalTelegramBot bot(botToken, client);

// LED pin
const int ledPin = 2;

void setup() {
  Serial.begin(115200);

  // Set LED pin as output
  pinMode(ledPin, OUTPUT);

  // Connect to WiFi
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  configTime(0, 0, "pool.ntp.org"); // Set ESP32 time from NTP
  Serial.println("Waiting for time sync...");
  while (time(nullptr) < 100000){
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nTime synced!");

  // Disable certificate verification (Telegram uses HTTPS)
  client.setInsecure();

  // Send Telegram message
  String message = "Hello from ESP32!";
  bool sent = bot.sendMessage(chatId, message, "");
  if(sent){
    Serial.println("Message sent successfully!");
  }else{
    Serial.println("Message failed!");
  }
}

void loop() {
  // Blink the LED infinitely with 1 second interval
  digitalWrite(ledPin, HIGH);
  delay(1000);
  digitalWrite(ledPin, LOW);
  delay(1000);
}
