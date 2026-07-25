/*
  Wemos D1 mini + I2C 16x2 LCD  --  Battery-friendly WiFi Clock
  ----------------------------------------------------------------
  BEHAVIOUR
  - First boot (no saved WiFi yet): the board creates its own
    WiFi access point called "ClockSetup". Connect a phone/laptop
    to it, a setup page should pop up automatically (or open
    http://192.168.4.1 manually). Enter your home WiFi SSID +
    password AND the timezone offset from UTC (e.g. 7 for WIB /
    Malang), then Save.
  - From then on the board remembers everything in flash and
    connects to your WiFi automatically on every boot.
  - Once connected it fetches the exact time from an NTP server
    ONE time, then switches the WiFi radio completely OFF and
    keeps time locally using millis(). The radio is the biggest
    power draw on this board, so this is where the battery is
    saved.
  - Every 24 hours it turns WiFi back on just long enough to
    resync with NTP (corrects any drift), then turns it off again.
  - If WiFi setup ever needs to be redone, erase the flash /
    reflash, or add a reset button pulling a GPIO low (see notes
    at bottom of file).

  LIBRARIES NEEDED (Arduino IDE > Tools > Manage Libraries):
    - WiFiManager        by tzapu
    - LiquidCrystal_I2C   by Frank de Brabander (or Marco Schwartz)
    - ArduinoJson         by Benoit Blanchon
  (ESP8266WiFi, WiFiUdp, LittleFS ship with the ESP8266 core)

  BOARD: LOLIN(WEMOS) D1 R2 & mini  (install "esp8266" by
  ESP8266 Community in Boards Manager first)

  WIRING
    LCD VCC -> 5V (or 3V3)
    LCD GND -> GND
    LCD SDA -> D2
    LCD SCL -> D1
*/

#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <WiFiManager.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ---------------- USER-ADJUSTABLE SETTINGS ----------------
#define LCD_ADDR      0x27   // change to 0x3F if your LCD backpack uses that address
#define SDA_PIN       D2
#define SCL_PIN       D1
#define AP_NAME       "ClockSetup"   // name of the setup WiFi network
#define NTP_SERVER    "pool.ntp.org"
const unsigned long SYNC_INTERVAL_MS = 24UL * 60UL * 60UL * 1000UL; // 24 hours
// ------------------------------------------------------------

LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
WiFiUDP udp;

int tzOffsetHours = 7;            // default WIB (UTC+7); overwritten by saved config
unsigned long epochAtSync = 0;    // unix time (UTC) captured at last successful sync
unsigned long millisAtSync = 0;   // millis() value at that same moment
unsigned long lastSyncMillis = 0;
bool timeIsValid = false;

char tzParamValue[4] = "7";
bool shouldSaveConfig = false;

const char daysOfWeek[7][4] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

// ---------------- config persistence (timezone) ----------------
void loadConfig() {
  if (!LittleFS.begin()) {
    LittleFS.format();
    LittleFS.begin();
  }
  if (LittleFS.exists("/config.json")) {
    File f = LittleFS.open("/config.json", "r");
    StaticJsonDocument<128> doc;
    if (!deserializeJson(doc, f)) {
      tzOffsetHours = doc["tz"] | 7;
    }
    f.close();
  }
}

void saveConfig() {
  StaticJsonDocument<128> doc;
  doc["tz"] = tzOffsetHours;
  File f = LittleFS.open("/config.json", "w");
  serializeJson(doc, f);
  f.close();
}

void saveConfigCallback() {
  shouldSaveConfig = true;
}

// ---------------- NTP (manual, no extra library) ----------------
unsigned long getNtpEpoch() {
  const int NTP_PACKET_SIZE = 48;
  byte packetBuffer[NTP_PACKET_SIZE];
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  packetBuffer[0] = 0b11100011; // LI, Version, Mode

  udp.begin(2390);

  IPAddress ntpIP;
  if (!WiFi.hostByName(NTP_SERVER, ntpIP)) {
    return 0;
  }

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
      return secsSince1900 - seventyYears; // unix epoch, UTC
    }
    delay(10);
  }
  return 0; // failed / timed out
}

bool syncTime() {
  lcd.setCursor(0, 1);
  lcd.print("Syncing time... ");

  WiFi.forceSleepWake();
  delay(50);
  WiFi.mode(WIFI_STA);
  WiFi.begin(); // reconnects using credentials already stored in flash

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(200);
  }

  bool ok = false;
  if (WiFi.status() == WL_CONNECTED) {
    unsigned long epoch = getNtpEpoch();
    if (epoch > 0) {
      epochAtSync = epoch;
      millisAtSync = millis();
      lastSyncMillis = millis();
      timeIsValid = true;
      ok = true;
    }
  }

  // Radio off until the next sync -- this is the main battery saving.
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();
  delay(50);

  lcd.setCursor(0, 1);
  lcd.print(ok ? "Sync OK         " : "Sync FAILED     ");
  delay(1000);
  return ok;
}

// ---------------- time helpers ----------------
void getLocalParts(int &h, int &m, int &s, int &wday, int &day, int &mon, int &year) {
  unsigned long nowEpoch = epochAtSync + (millis() - millisAtSync) / 1000UL;
  time_t local = (time_t)(nowEpoch + (long)tzOffsetHours * 3600L);
  struct tm *t = gmtime(&local);
  h = t->tm_hour; m = t->tm_min; s = t->tm_sec;
  wday = t->tm_wday; day = t->tm_mday; mon = t->tm_mon + 1; year = t->tm_year + 1900;
}

void printTimeToLCD() {
  int h, m, s, wday, day, mon, year;
  getLocalParts(h, m, s, wday, day, mon, year);

  char line1[17], line2[17];
  snprintf(line1, sizeof(line1), "%02d:%02d:%02d %s", h, m, s, daysOfWeek[wday]);
  snprintf(line2, sizeof(line2), "%02d-%02d-%04d      ", day, mon, year);

  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
}

// ---------------- setup / loop ----------------
void setup() {
  Wire.begin(SDA_PIN, SCL_PIN);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Booting clock...");

  loadConfig();
  snprintf(tzParamValue, sizeof(tzParamValue), "%d", tzOffsetHours);

  WiFiManager wm;
  WiFiManagerParameter tzParam("tz", "Timezone offset from UTC (e.g. 7 = WIB)", tzParamValue, 3);
  wm.addParameter(&tzParam);
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.setConfigPortalTimeout(180); // give up AP mode after 3 min idle, retry next boot

  lcd.setCursor(0, 1);
  lcd.print("AP: ClockSetup  ");

  // Blocks here: tries saved WiFi first, if none/fails it opens the
  // "ClockSetup" access point + web config portal automatically.
  bool connected = wm.autoConnect(AP_NAME);

  tzOffsetHours = atoi(tzParam.getValue());
  if (shouldSaveConfig) saveConfig();

  if (connected) {
    syncTime();
  } else {
    lcd.setCursor(0, 1);
    lcd.print("WiFi setup fail ");
    delay(2000);
  }
}

void loop() {
  if (timeIsValid) {
    printTimeToLCD();
  } else {
    lcd.setCursor(0, 0);
    lcd.print("No time yet     ");
  }

  if (millis() - lastSyncMillis >= SYNC_INTERVAL_MS || !timeIsValid) {
    syncTime();
  }

  delay(1000);
}

/*
  OPTIONAL: reset saved WiFi/timezone
  ------------------------------------
  Wire a push button between D5 (GPIO14) and GND, then add this
  near the top of setup(), before wm.autoConnect():

    pinMode(D5, INPUT_PULLUP);
    if (digitalRead(D5) == LOW) {
      WiFiManager wmReset;
      wmReset.resetSettings();
      LittleFS.remove("/config.json");
    }

  Hold the button while powering on to force the setup portal
  to reappear.

  NOTE ON ACCURACY / FURTHER POWER SAVINGS
  ------------------------------------------
  The ESP8266's internal clock (millis()) can drift by roughly a
  few seconds over 24 hours, which the daily NTP resync corrects.
  If you want to save even more power by putting the whole board
  into deep sleep (not just the radio), you'd need an external
  RTC module (e.g. DS3231) to keep real time while the ESP8266 is
  fully asleep, since deep sleep resets millis(). That's a good
  next upgrade if battery life becomes the top priority over a
  continuously ticking display.
*/