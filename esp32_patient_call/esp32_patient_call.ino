#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <ESP_Mail_Client.h>

// ================= YOUR SETTINGS (edit these) =================
#define WIFI_SSID        "your-wifi-name"
#define WIFI_PASS        "your-wifi-password"

#define SMTP_HOST        "smtp.gmail.com"
#define SMTP_PORT        465                       // SSL
#define SMTP_USER        "your.sender@gmail.com"
#define SMTP_PASSWORD    "xxxx xxxx xxxx xxxx"     // Gmail App Password, not your login password

#define CAREGIVER_EMAIL  "caregiver@example.com"
#define PATIENT_NAME     "Patient Name"
#define LOCATION         "Home - Bedroom"

#define GMT_OFFSET_HOURS 5.5                       // e.g. India = 5.5, UTC = 0
// ==============================================================

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C
#define TOUCH_PIN 4

// Timing (milliseconds)
const unsigned long DEBOUNCE_MS = 50;
const unsigned long TAP_WINDOW_MS = 500;
const unsigned long LONG_PRESS_MS = 800;

// Scrolling
const unsigned long SCROLL_INTERVAL_MS = 30;  // smaller = faster
const int SCROLL_STEP_PX = 3;                 // pixels moved per frame
const uint8_t SCROLL_TEXT_SIZE = 3;           // 1 = small, 3 = big
const int SCROLL_REPEATS = 2;                 // how many times the message passes across, then stops

// Email
const unsigned long ROUTINE_COOLDOWN_MS = 60000;    // min gap between routine emails (per slot)
const unsigned long URGENT_COOLDOWN_MS  = 15000;    // min gap between urgent emails
const uint8_t       MAX_SEND_ATTEMPTS   = 4;
const unsigned long RETRY_BACKOFF_MS    = 5000;     // multiplied by attempt number

Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
WebServer server(80);
Preferences prefs;

// Editable messages (loaded from flash)
String msgTap1, msgTap2, msgTap3, msgLong, msgMore;
String ipText = "No WiFi";

// Touch state
int tapCount = 0;
unsigned long lastTapTime = 0;
bool lastState = LOW;
unsigned long lastChangeTime = 0;
unsigned long pressStartTime = 0;
bool longPressHandled = false;

// Scroll state
String scrollMsg = "";
bool scrolling = false;
int scrollX = SCREEN_WIDTH;
int scrollTextW = 0;
int scrollLoops = 0;
unsigned long lastScrollTime = 0;

// ---------------- Email ----------------
// Plain char arrays only: Arduino String objects are not safe to copy across tasks/queues.
struct MailJob {
  char need[20];
  bool urgent;
};

QueueHandle_t mailQueue;
SMTPSession smtp;

// Cooldown bookkeeping: slots 0..2 = taps 1..3, slot 3 = long press
unsigned long lastMailAt[4] = {0, 0, 0, 0};
bool          mailedOnce[4] = {false, false, false, false};

String nowString() {
  struct tm t;
  if (!getLocalTime(&t, 2000)) return "unknown (clock not synced)";
  char buf[40];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
  return String(buf);
}

// Templates. Edit the wording here.
String mailSubject(const MailJob &j) {
  if (j.urgent) return String("[URGENT] ") + PATIENT_NAME + ": " + j.need;
  return String("[Request] ") + PATIENT_NAME + " needs " + j.need;
}

String mailBody(const MailJob &j) {
  String b;
  if (j.urgent) {
    b  = String(PATIENT_NAME) + " has triggered an EMERGENCY call (" + j.need + ").\n\n";
    b += "Please go to them or call emergency services immediately.\n\n";
  } else {
    b  = String(PATIENT_NAME) + " is asking for: " + j.need + ".\n\n";
    b += "This is a routine request. Please attend when you can.\n\n";
  }
  b += "Time: " + nowString() + "\n";
  b += "Location: " + String(LOCATION) + "\n";
  b += "Wi-Fi signal: " + String(WiFi.RSSI()) + " dBm\n";
  return b;
}

bool sendMailOnce(const MailJob &j) {
  Session_Config config;
  config.server.host_name = SMTP_HOST;
  config.server.port      = SMTP_PORT;
  config.login.email      = SMTP_USER;
  config.login.password   = SMTP_PASSWORD;
  config.login.user_domain = "";
  config.time.ntp_server  = "pool.ntp.org,time.nist.gov";
  config.time.gmt_offset  = GMT_OFFSET_HOURS;
  config.time.day_light_offset = 0;

  String subject = mailSubject(j);
  String body    = mailBody(j);

  SMTP_Message message;
  message.sender.name  = "Patient Call System";
  message.sender.email = SMTP_USER;
  message.subject      = subject;
  message.addRecipient("Caregiver", CAREGIVER_EMAIL);
  message.priority     = j.urgent ? esp_mail_smtp_priority_high : esp_mail_smtp_priority_normal;
  message.text.content = body.c_str();
  message.text.charSet = "utf-8";
  message.text.transfer_encoding = Content_Transfer_Encoding::enc_7bit;

  if (!smtp.connect(&config)) {
    Serial.printf("SMTP connect failed: %s\n", smtp.errorReason().c_str());
    return false;
  }
  bool ok = MailClient.sendMail(&smtp, &message);
  if (!ok) Serial.printf("SMTP send failed: %s\n", smtp.errorReason().c_str());
  smtp.closeSession();
  return ok;
}

// Runs on its own task so the touch loop, scrolling and web page never block on the network.
void mailTask(void *) {
  MailJob job;
  for (;;) {
    if (xQueueReceive(mailQueue, &job, portMAX_DELAY) != pdTRUE) continue;
    Serial.printf("Email: sending '%s' (%s)\n", job.need, job.urgent ? "urgent" : "routine");

    bool ok = false;
    for (uint8_t attempt = 1; attempt <= MAX_SEND_ATTEMPTS && !ok; attempt++) {
      if (WiFi.status() != WL_CONNECTED) {
        WiFi.reconnect();
        for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) vTaskDelay(pdMS_TO_TICKS(250));
      }
      if (WiFi.status() == WL_CONNECTED) ok = sendMailOnce(job);
      if (!ok && attempt < MAX_SEND_ATTEMPTS) {
        Serial.printf("Email attempt %u failed, retrying...\n", attempt);
        vTaskDelay(pdMS_TO_TICKS(RETRY_BACKOFF_MS * attempt));
      }
    }
    Serial.println(ok ? "Email: SENT" : "Email: GAVE UP");
  }
}

// slot: 0..2 = tap count-1, 3 = long press
void queueMail(const String &need, bool urgent, int slot) {
  unsigned long now = millis();
  unsigned long cooldown = urgent ? URGENT_COOLDOWN_MS : ROUTINE_COOLDOWN_MS;
  if (mailedOnce[slot] && now - lastMailAt[slot] < cooldown) {
    Serial.println("Email skipped (cooldown)");
    return;
  }
  mailedOnce[slot] = true;
  lastMailAt[slot] = now;

  MailJob job;
  strncpy(job.need, need.c_str(), sizeof(job.need) - 1);
  job.need[sizeof(job.need) - 1] = '\0';
  job.urgent = urgent;
  if (xQueueSend(mailQueue, &job, 0) != pdTRUE) Serial.println("Mail queue full, email dropped");
}

// ---------------- Storage ----------------
void loadMessages() {
  prefs.begin("needs", true);
  msgTap1 = prefs.getString("t1", "WATER");
  msgTap2 = prefs.getString("t2", "FOOD");
  msgTap3 = prefs.getString("t3", "WASHROOM");
  msgLong = prefs.getString("lp", "EMERGENCY");
  msgMore = prefs.getString("more", "IGNORED");
  prefs.end();
}

void saveMessages() {
  prefs.begin("needs", false);
  prefs.putString("t1", msgTap1);
  prefs.putString("t2", msgTap2);
  prefs.putString("t3", msgTap3);
  prefs.putString("lp", msgLong);
  prefs.putString("more", msgMore);
  prefs.end();
}

String cleanInput(String s) {
  s.trim();
  s.toUpperCase();
  if (s.length() > 16) s = s.substring(0, 16);
  return s;
}

// ---------------- Display ----------------
void centerText(const String &text, uint8_t size, int y) {
  int16_t x1, y1;
  uint16_t w, h;
  display.setTextSize(size);
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  int x = (SCREEN_WIDTH - (int)w) / 2;
  if (x < 0) x = 0;
  display.setCursor(x, y);
  display.print(text);
}

void showStatus(const String &text) {
  scrolling = false;
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  centerText(text, 1, 28);
  display.display();
}

// Shown once after boot (the only place the IP appears)
void showBoot() {
  scrolling = false;
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  centerText("WEB PAGE IP:", 1, 10);
  centerText(ipText, 1, 28);
  centerText("Touch to start", 1, 50);
  display.display();
}

// One scrolling line, vertically centered, nothing else on screen
void drawScrollFrame() {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(SCROLL_TEXT_SIZE);
  display.setTextWrap(false);  // keep it on a single line

  int textHeight = 8 * SCROLL_TEXT_SIZE;
  int y = (SCREEN_HEIGHT - textHeight) / 2;

  display.setCursor(scrollX, y);
  display.print(scrollMsg);
  display.display();
}

void showNeed(const String &need) {
  int16_t x1, y1;
  uint16_t w, h;
  display.setTextSize(SCROLL_TEXT_SIZE);
  display.getTextBounds(need, 0, 0, &x1, &y1, &w, &h);

  scrollMsg = need;
  scrollTextW = w;
  scrollX = SCREEN_WIDTH;  // start from the right edge
  scrollLoops = 0;         // reset pass counter
  scrolling = true;
  lastScrollTime = millis();
  drawScrollFrame();
}

// Stop scrolling and blank the screen
void stopScroll() {
  scrolling = false;
  display.clearDisplay();
  display.display();
}

void updateScroll() {
  if (!scrolling) return;
  unsigned long now = millis();
  if (now - lastScrollTime < SCROLL_INTERVAL_MS) return;
  lastScrollTime = now;

  scrollX -= SCROLL_STEP_PX;
  if (scrollX < -scrollTextW) {
    scrollLoops++;
    if (scrollLoops >= SCROLL_REPEATS) {
      stopScroll();  // finished all passes
      return;
    }
    scrollX = SCREEN_WIDTH;  // scroll again
  }
  drawScrollFrame();
}

// mailSlot: 0..2 = taps 1..3, 3 = long press, -1 = display only (no email)
void triggerNeed(const String &need, int mailSlot) {
  if (need.length() == 0) return;  // empty message = ignore
  showNeed(need);
  if (mailSlot >= 0) queueMail(need, mailSlot == 3, mailSlot);
}

// ---------------- Web page ----------------
String htmlEscape(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  s.replace("'", "&#39;");
  return s;
}

String field(const char *label, const char *name, const String &value) {
  return "<label>" + String(label) + "</label><input name='" + name +
         "' maxlength='16' value='" + htmlEscape(value) + "'>";
}

void handleRoot() {
  String html = F(
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Patient Call Settings</title><style>"
    "body{font-family:sans-serif;max-width:420px;margin:20px auto;padding:0 16px;background:#f2f5f7;color:#222}"
    "h2{text-align:center}"
    "form{background:#fff;padding:16px;border-radius:10px;box-shadow:0 1px 4px #0003}"
    "label{display:block;margin:12px 0 4px;font-weight:bold}"
    "input{width:100%;box-sizing:border-box;padding:10px;font-size:16px;border:1px solid #aaa;border-radius:6px}"
    "button{width:100%;margin-top:18px;padding:12px;font-size:17px;background:#0a7;color:#fff;border:0;border-radius:6px}"
    ".ok{background:#d7f5e3;padding:10px;border-radius:6px;text-align:center;margin-bottom:12px}"
    "small{color:#666}"
    "</style></head><body><h2>Patient Call Settings</h2>");

  if (server.hasArg("saved")) html += F("<div class='ok'>Saved!</div>");

  html += F("<form method='POST' action='/save'>");
  html += field("1 tap", "t1", msgTap1);
  html += field("2 taps", "t2", msgTap2);
  html += field("3 taps", "t3", msgTap3);
  html += field("Long press (urgent email)", "lp", msgLong);
  html += field("4+ taps (ignored, no email)", "more", msgMore);
  html += F("<button type='submit'>Save</button></form>"
            "<p><small>Max 16 characters. Leave a box empty to show nothing for that touch. "
            "Taps and long press also email the caregiver.</small></p>"
            "</body></html>");

  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("t1"))   msgTap1 = cleanInput(server.arg("t1"));
  if (server.hasArg("t2"))   msgTap2 = cleanInput(server.arg("t2"));
  if (server.hasArg("t3"))   msgTap3 = cleanInput(server.arg("t3"));
  if (server.hasArg("lp"))   msgLong = cleanInput(server.arg("lp"));
  if (server.hasArg("more")) msgMore = cleanInput(server.arg("more"));
  saveMessages();
  Serial.println("Messages updated from web page");

  server.sendHeader("Location", "/?saved=1");
  server.send(303, "text/plain", "");
}

// ---------------- Setup ----------------
void setup() {
  Serial.begin(115200);
  pinMode(TOUCH_PIN, INPUT);

  Wire.begin(21, 22);  // SDA = 21, SCL = 22
  delay(250);

  if (!display.begin(OLED_ADDR, true)) {
    Serial.println("OLED initialization FAILED!");
    while (1);
  }
  Wire.setClock(400000);  // faster I2C = smoother scrolling

  loadMessages();

  // Connect to WiFi
  showStatus("Connecting WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    ipText = WiFi.localIP().toString();
    Serial.print("Open in browser: http://");
    Serial.println(ipText);

    configTime((long)(GMT_OFFSET_HOURS * 3600), 0, "pool.ntp.org", "time.nist.gov");

    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.begin();
    showBoot();
  } else {
    Serial.println("WiFi connection FAILED (touch still works, emails will retry later)");
    showStatus("WiFi failed");
  }

  MailClient.networkReconnect(true);
  mailQueue = xQueueCreate(8, sizeof(MailJob));
  xTaskCreatePinnedToCore(mailTask, "mail", 16384, nullptr, 1, nullptr, 1);
}

// ---------------- Loop ----------------
void loop() {
  server.handleClient();
  updateScroll();

  bool state = digitalRead(TOUCH_PIN);
  unsigned long now = millis();

  // Detect press / release with debounce
  if (state != lastState && (now - lastChangeTime) > DEBOUNCE_MS) {
    lastChangeTime = now;
    lastState = state;

    if (state == HIGH) {
      pressStartTime = now;
      longPressHandled = false;
    } else if (!longPressHandled) {
      tapCount++;
      lastTapTime = now;
      Serial.print("Tap #");
      Serial.println(tapCount);
    }
  }

  // Long press (triggers while finger is still held)
  if (lastState == HIGH && !longPressHandled &&
      (now - pressStartTime) >= LONG_PRESS_MS) {
    longPressHandled = true;
    tapCount = 0;
    triggerNeed(msgLong, 3);
    Serial.println("Long touch -> " + msgLong);
  }

  // Decide after the tap window closes
  if (tapCount > 0 && lastState == LOW && (now - lastTapTime) > TAP_WINDOW_MS) {
    switch (tapCount) {
      case 1: triggerNeed(msgTap1, 0); Serial.println("1 tap -> " + msgTap1); break;
      case 2: triggerNeed(msgTap2, 1); Serial.println("2 taps -> " + msgTap2); break;
      case 3: triggerNeed(msgTap3, 2); Serial.println("3 taps -> " + msgTap3); break;
      default: triggerNeed(msgMore, -1); Serial.println("4+ taps -> " + msgMore); break;
    }
    tapCount = 0;
  }
}
