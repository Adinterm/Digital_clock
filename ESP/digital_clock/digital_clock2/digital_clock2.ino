/*
  Wemos D1 mini + I2C 16x2 LCD  --  WiFi Clock + Temperature
  ----------------------------------------------------------------
  LAYOUT (16x2 LCD)
    Columns 0-10 (left):  small text -- "HH:MM Day" / "DD-MM-YYYY"
    Columns 11-15 (right): big 2-digit temperature (4 cols) + a
                            small degree symbol and "C" (1 col)

  BEHAVIOUR
  - First boot (no saved WiFi yet): the board creates its own
    WiFi access point called "ClockSetup". Connect a phone/laptop
    to it, a setup page should pop up automatically (or open
    http://192.168.4.1 manually). Enter your home WiFi SSID +
    password, timezone offset, and latitude/longitude (used for
    the temperature lookup), then Save.
  - From then on the board remembers everything in flash and
    connects to your WiFi automatically on every boot.
  - Time (NTP) is refreshed once every 24 hours -- the WiFi
    radio is switched off between syncs to save power.
  - Temperature (Open-Meteo, free, no API key) is refreshed more
    often (default every 30 minutes) since weather changes faster
    than clock drift. NOTE: this means WiFi wakes up more often
    than the original "once a day" design, which uses more
    battery than the time-only version. Increase
    TEMP_SYNC_INTERVAL_MS below if you'd rather save more power
    and accept a staler temperature reading.
  - A physical button (wired to D5 and GND) re-opens the WiFi
    setup portal at any time. It does NOT erase your saved
    settings -- the portal just lets you re-enter them.
  - Every field is always redrawn as a full fixed-width string
    (padded with spaces) so leftover/ghost characters from a
    previous, longer message can never remain on screen.

  LIBRARIES NEEDED (Arduino IDE > Tools > Manage Libraries):
    - WiFiManager        by tzapu
    - LiquidCrystal_I2C   by Frank de Brabander (or Marco Schwartz)
    - ArduinoJson         by Benoit Blanchon
  (ESP8266WiFi, WiFiUdp, WiFiClientSecure, ESP8266HTTPClient,
   LittleFS ship with the ESP8266 core -- no extra install needed)

  BOARD: LOLIN(WEMOS) D1 R2 & mini  (install "esp8266" by
  ESP8266 Community in Boards Manager first)

  WIRING
    LCD  VCC    -> 5V (or 3V3)
    LCD  GND    -> GND
    LCD  SDA    -> D2
    LCD  SCL    -> D1
    BUTTON pin1 -> D5
    BUTTON pin2 -> GND
*/

#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <WiFiManager.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ---------------- USER-ADJUSTABLE SETTINGS ----------------
#define LCD_ADDR      0x27   // change to 0x3F if your LCD backpack uses that address
#define SDA_PIN       D2
#define SCL_PIN       D1
#define BUTTON_PIN    D5     // press this button to re-open the WiFi/timezone setup portal
#define AP_NAME       "ClockSetup"
#define NTP_SERVER    "pool.ntp.org"
const unsigned long SYNC_INTERVAL_MS      = 24UL * 60UL * 60UL * 1000UL; // time: every 24 hours
const unsigned long TEMP_SYNC_INTERVAL_MS = 30UL * 60UL * 1000UL;        // temperature: every 30 minutes
const unsigned long DEBOUNCE_MS = 50;
// ------------------------------------------------------------

LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
WiFiUDP udp;

int tzOffsetHours = 7;               // default WIB (UTC+7)
char latStr[12] = "-7.97";           // default: Malang, East Java
char lonStr[12] = "112.63";
unsigned long epochAtSync = 0;
unsigned long millisAtSync = 0;
unsigned long lastSyncMillis = 0;
unsigned long lastTempSyncMillis = 0;
bool timeIsValid = false;
float lastTempC = NAN;
bool shouldSaveConfig = false;

int buttonState = HIGH;
int lastButtonReading = HIGH;
unsigned long lastDebounceTime = 0;

const char daysOfWeek[7][4] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

// ---------------- LCD helpers ----------------
// Writes exactly `width` characters starting at (col,row), padding with
// spaces or trimming as needed -- guarantees no leftover/ghost characters
// from whatever was previously drawn in that area.
void lcdPrintField(int row, int col, int width, const String &text) {
  String padded = text;
  while ((int)padded.length() < width) padded += ' ';
  if ((int)padded.length() > width) padded = padded.substring(0, width);
  lcd.setCursor(col, row);
  lcd.print(padded);
}

void lcdPrintLine(int row, const String &text) {
  lcdPrintField(row, 0, 16, text);
}

// ---------------- Smooth Curved Big-Digit Font (2 cols x 2 rows) ----------------

// Custom character indices (HD44780 supports up to 8 custom characters, 0-7)
#define LT 0 // Top-Left corner
#define UB 1 // Upper Bar
#define RT 2 // Top-Right corner
#define LL 3 // Bottom-Left corner
#define LB 4 // Lower Bar
#define CR 5 // Bottom-Right corner
#define MB 6 // Middle Bar (Thinner)

// 5x8 pixel definitions for smooth shapes
byte customLT[8] = {0x07, 0x0F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}; // Curve Top-Left
byte customUB[8] = {0x1F, 0x1F, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00}; // Top Horizontal Bar
byte customRT[8] = {0x1C, 0x1E, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}; // Curve Top-Right
byte customLL[8] = {0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x0F, 0x07}; // Curve Bottom-Left
byte customLB[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x1F, 0x1F, 0x1F}; // Bottom Horizontal Bar
byte customCR[8] = {0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1E, 0x1C}; // Curve Bottom-Right
byte customMB[8] = {0x00, 0x00, 0x1F, 0x1F, 0x1F, 0x1F, 0x00, 0x00}; // Middle Bar

// Setup custom characters (Call this inside setup())
void initBigDigits() {
  lcd.createChar(LT, customLT);
  lcd.createChar(UB, customUB);
  lcd.createChar(RT, customRT);
  lcd.createChar(LL, customLL);
  lcd.createChar(LB, customLB);
  lcd.createChar(CR, customCR);
  lcd.createChar(MB, customMB);
}

// Map each digit to a 2x2 grid [Row 0 (Col 0, Col 1), Row 1 (Col 0, Col 1)]
const byte bigDigits2[10][4] = {
  { LT, RT, LL, CR }, // 0
  { UB, RT, ' ', CR }, // 1
  { UB, RT, LL, LB }, // 2
  { UB, RT, LB, CR }, // 3
  { LL, CR, ' ', RT }, // 4
  { LT, UB, LB, CR }, // 5
  { LT, UB, LL, CR }, // 6
  { UB, RT, ' ', RT }, // 7
  { LT, RT, LL, CR }, // 8 (Distinct inner spacing via corner angles)
  { LT, RT, LB, CR }  // 9
};

// Function to draw a 2x2 digit at a given column offset
void drawBigDigit2(int digit, int col) {
  if (digit < 0 || digit > 9) return;

  // Row 0 (Top half)
  lcd.setCursor(col, 0);
  lcd.write(bigDigits2[digit][0]);
  lcd.setCursor(col + 1, 0);
  lcd.write(bigDigits2[digit][1]);

  // Row 1 (Bottom half)
  lcd.setCursor(col, 1);
  lcd.write(bigDigits2[digit][2]);
  lcd.setCursor(col + 1, 1);
  lcd.write(bigDigits2[digit][3]);
}
// ---------------- config persistence ----------------
void loadConfig() {
  if (!LittleFS.begin()) {
    LittleFS.format();
    LittleFS.begin();
  }
  if (LittleFS.exists("/config.json")) {
    File f = LittleFS.open("/config.json", "r");
    StaticJsonDocument<192> doc;
    if (!deserializeJson(doc, f)) {
      tzOffsetHours = doc["tz"] | 7;
      strlcpy(latStr, doc["lat"] | "-7.97", sizeof(latStr));
      strlcpy(lonStr, doc["lon"] | "112.63", sizeof(lonStr));
    }
    f.close();
  }
}

void saveConfig() {
  StaticJsonDocument<192> doc;
  doc["tz"] = tzOffsetHours;
  doc["lat"] = latStr;
  doc["lon"] = lonStr;
  File f = LittleFS.open("/config.json", "w");
  serializeJson(doc, f);
  f.close();
}

void saveConfigCallback() {
  shouldSaveConfig = true;
}

// ---------------- WiFi / location / timezone setup portal ----------------
bool doWifiSetup(bool forcePortal) {
  WiFiManager wm;

  char tzBuf[4];
  snprintf(tzBuf, sizeof(tzBuf), "%d", tzOffsetHours);
  WiFiManagerParameter tzParam("tz", "Timezone offset from UTC (e.g. 7 = WIB)", tzBuf, 3);
  WiFiManagerParameter latParam("lat", "Latitude (for temperature)", latStr, 11);
  WiFiManagerParameter lonParam("lon", "Longitude (for temperature)", lonStr, 11);
  wm.addParameter(&tzParam);
  wm.addParameter(&latParam);
  wm.addParameter(&lonParam);
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.setConfigPortalTimeout(180);

  lcdPrintLine(0, "Setup mode");
  lcdPrintLine(1, String("AP: ") + AP_NAME);

  bool connected;
  if (forcePortal) {
    WiFi.forceSleepWake();
    delay(50);
    connected = wm.startConfigPortal(AP_NAME);
  } else {
    connected = wm.autoConnect(AP_NAME);
  }

  tzOffsetHours = atoi(tzParam.getValue());
  strlcpy(latStr, latParam.getValue(), sizeof(latStr));
  strlcpy(lonStr, lonParam.getValue(), sizeof(lonStr));
  if (shouldSaveConfig) {
    saveConfig();
    shouldSaveConfig = false;
  }

  return connected;
}

// ---------------- NTP (manual, no extra library) ----------------
unsigned long getNtpEpoch() {
  const int NTP_PACKET_SIZE = 48;
  byte packetBuffer[NTP_PACKET_SIZE];
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  packetBuffer[0] = 0b11100011;

  udp.begin(2390);
  IPAddress ntpIP;
  if (!WiFi.hostByName(NTP_SERVER, ntpIP)) return 0;

  udp.beginPacket(ntpIP, 123);
  udp.write(packetBuffer, NTP_PACKET_SIZE);
  udp.endPacket();

  unsigned long start = millis();
  while (millis() - start < 3000) {
    if (udp.parsePacket() >= NTP_PACKET_SIZE) {
      udp.read(packetBuffer, NTP_PACKET_SIZE);
      unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
      unsigned long lowWord  = word(packetBuffer[42], packetBuffer[43]);
      unsigned long secsSince1900 = (highWord << 16) | lowWord;
      const unsigned long seventyYears = 2208988800UL;
      return secsSince1900 - seventyYears;
    }
    delay(10);
  }
  return 0;
}

// ---------------- Temperature (Open-Meteo, no API key) ----------------
bool fetchTemperature() {
  WiFiClientSecure client;
  client.setInsecure(); // skip TLS cert validation -- fine for a hobby weather fetch
  HTTPClient http;

  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(latStr) +
               "&longitude=" + String(lonStr) + "&current=temperature_2m";

  if (!http.begin(client, url)) return false;
  int code = http.GET();
  bool ok = false;

  if (code == 200) {
    String payload = http.getString();
    StaticJsonDocument<512> doc;
    if (!deserializeJson(doc, payload)) {
      float t = doc["current"]["temperature_2m"] | NAN;
      if (!isnan(t)) {
        lastTempC = t;
        ok = true;
      }
    }
  }
  http.end();
  return ok;
}

// ---------------- combined WiFi wake / sync / sleep ----------------
void doSync(bool forceTime, bool forceTemp) {
  bool needTime = forceTime || !timeIsValid || (millis() - lastSyncMillis >= SYNC_INTERVAL_MS);
  bool needTemp = forceTemp || isnan(lastTempC) || (millis() - lastTempSyncMillis >= TEMP_SYNC_INTERVAL_MS);
  if (!needTime && !needTemp) return;

  lcdPrintLine(0, "Updating...");
  lcdPrintLine(1, "");

  WiFi.forceSleepWake();
  delay(50);
  WiFi.mode(WIFI_STA);
  WiFi.begin(); // reconnects using credentials already stored in flash

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(200);
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (needTime) {
      unsigned long epoch = getNtpEpoch();
      if (epoch > 0) {
        epochAtSync = epoch;
        millisAtSync = millis();
        lastSyncMillis = millis();
        timeIsValid = true;
      }
    }
    if (needTemp) {
      fetchTemperature();
      lastTempSyncMillis = millis();
    }
  }

  // Radio off until the next sync -- this is the main battery saving.
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();
  delay(50);
}

void handleSetupButton() {
  bool connected = doWifiSetup(true);
  if (connected) {
    doSync(true, true); // refresh both time and temperature right away
  } else {
    WiFi.mode(WIFI_OFF);
    WiFi.forceSleepBegin();
    lcdPrintLine(1, "Setup cancelled");
    delay(1500);
  }
}

// ---------------- time helpers ----------------
void getLocalParts(int &h, int &m, int &s, int &wday, int &day, int &mon, int &year) {
  unsigned long nowEpoch = epochAtSync + (millis() - millisAtSync) / 1000UL;
  time_t local = (time_t)(nowEpoch + (long)tzOffsetHours * 3600L);
  struct tm *t = gmtime(&local);
  h = t->tm_hour; m = t->tm_min; s = t->tm_sec;
  wday = t->tm_wday; day = t->tm_mday; mon = t->tm_mon + 1; year = t->tm_year + 1900;
}

// ---------------- main display ----------------
void showTimeAndTemp() {
  int h, m, s, wday, day, mon, year;
  getLocalParts(h, m, s, wday, day, mon, year);
  char colon = (s % 2 == 0) ? ':' : ' '; // blink once per second

  char line1[13], line2[13];
  snprintf(line1, sizeof(line1), "%02d%c%02d %s", h, colon, m, daysOfWeek[wday]);
  snprintf(line2, sizeof(line2), "%02d-%02d-%04d", day, mon, year);

  lcdPrintField(0, 0, 11, line1);
  lcdPrintField(1, 0, 11, line2);

  if (!isnan(lastTempC)) {
    int t = (int)roundf(lastTempC);
    if (t < 0) t = 0;   // simple 2-digit font -- see notes at bottom for negative temps
    if (t > 99) t = 99;
    drawBigDigit2(t / 10, 11);
    drawBigDigit2(t % 10, 13);
    lcd.setCursor(15, 0);
    lcd.write((byte)0xDF); // degree symbol (standard HD44780 character set)
    lcd.setCursor(15, 1);
    lcd.print("C");
  } else {
    lcdPrintField(0, 11, 5, "");
    lcdPrintField(1, 11, 5, "");
  }
}

// ---------------- button handling ----------------
void pollButton() {
  int reading = digitalRead(BUTTON_PIN);

  if (reading != lastButtonReading) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > DEBOUNCE_MS) {
    if (reading != buttonState) {
      buttonState = reading;
      if (buttonState == LOW) {
        handleSetupButton();
      }
    }
  }

  lastButtonReading = reading;
}

// ---------------- setup / loop ----------------
void setup() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Wire.begin(SDA_PIN, SCL_PIN);
  lcd.init();
  lcd.backlight();
  initBigDigits();

  lcdPrintLine(0, "Booting clock...");

  loadConfig();

  bool connected = doWifiSetup(false);

  if (connected) {
    doSync(true, true);
  } else {
    lcdPrintLine(1, "WiFi setup fail");
    delay(2000);
  }
}

void loop() {
  pollButton();

  static unsigned long lastDisplayMillis = 0;
  if (millis() - lastDisplayMillis >= 1000) {
    lastDisplayMillis = millis();
    if (timeIsValid) {
      showTimeAndTemp();
    } else {
      lcdPrintLine(0, "No time yet");
      lcdPrintLine(1, "");
    }
  }

  doSync(false, false); // wakes WiFi only when the time or temperature interval is due
}

/*
  NOTES
  ------
  - Negative temperatures: the simple 2-column digit font here only
    covers 0-9 per digit (no minus sign), so readings below 0C are
    clamped to "00". Unlikely to matter for Malang, but if you use
    this somewhere colder, you'd want to add a minus-sign column.

  - Digit readability: at 2 columns wide, "0" and "8" render the
    same (a solid block) since there isn't enough resolution to
    show a hollow center. All other digits (1-7, 9) are distinct.
    If this is confusing in practice, the time display's 3-column
    big-digit style (used in an earlier version of this sketch) is
    easier to read -- happy to swap the temperature font to that
    if you'd prefer clarity over the compact 4-column footprint.

  - Battery trade-off: TEMP_SYNC_INTERVAL_MS (default 30 minutes)
    controls how often WiFi wakes up just for temperature. Time
    sync stays at 24 hours (SYNC_INTERVAL_MS). Increase the temp
    interval (e.g. to 2-4 hours) if you want to prioritize battery
    life over a frequently updated reading.

  - Location: latitude/longitude are set via the same setup portal
    as WiFi/timezone (press the D5 button to reopen it), defaulting
    to Malang, East Java coordinates.

  OPTIONAL: full factory reset button
  -------------------------------------
  The D5 button only re-opens the setup portal (saved settings are
  kept and pre-filled). For a separate "wipe everything" button,
  wire one to D6 and add this near the top of setup(), before
  doWifiSetup():

    pinMode(D6, INPUT_PULLUP);
    if (digitalRead(D6) == LOW) {
      WiFiManager wmReset;
      wmReset.resetSettings();
      LittleFS.remove("/config.json");
    }
*/